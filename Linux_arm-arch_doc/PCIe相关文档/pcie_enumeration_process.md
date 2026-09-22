# PCIe 设备枚举过程详解

## 核心答案

**是的，PCIe Root Complex 驱动需要进行设备枚举，但通常不是驱动直接枚举，而是通过注册 `pci_host_bridge` 后，由 PCI 核心子系统自动执行枚举。**

---

## 1. 枚举的触发

### 1.1 驱动注册 Host Bridge

```c
static int pcie_mediatek_probe(struct platform_device *pdev)
{
    struct pci_host_bridge *host;
    
    // 创建 Host Bridge
    host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
    
    // 设置操作函数
    host->ops = &pcie_mediatek_ops;  // 配置空间访问
    
    // 注册 Host Bridge - 这会触发枚举！
    err = pci_host_probe(host);  // ← 关键：触发枚举
    if (err < 0) {
        dev_err(dev, "failed to register host: %d\n", err);
        return err;
    }
    
    return 0;
}
```

### 1.2 `pci_host_probe()` 的作用

`pci_host_probe()` 会：

1. **注册 Host Bridge** 到 PCI 核心子系统
2. **触发设备扫描**：自动扫描 PCIe 总线
3. **枚举设备**：发现所有连接的 PCIe 设备
4. **创建 `pci_dev`**：为每个设备创建设备结构
5. **加载设备驱动**：尝试为每个设备加载驱动

---

## 2. 枚举的完整流程

### 2.1 流程图

```
驱动加载
    ↓
pcie_mediatek_probe()
    ↓
创建 pci_host_bridge
    ↓
设置 host->ops (配置空间访问接口)
    ↓
pci_host_probe(host)  ← 触发枚举
    ↓
┌─────────────────────────────────┐
│   PCI 核心子系统                 │
│   (drivers/pci/probe.c)          │
│                                   │
│   1. pci_scan_root_bus()         │
│      ├─ 扫描总线 0                │
│      ├─ 读取每个设备的 Vendor ID  │
│      └─ 创建 pci_dev 结构        │
│                                   │
│   2. pci_bus_add_devices()        │
│      ├─ 为每个设备创建 sysfs      │
│      └─ 触发设备驱动加载          │
└─────────────────────────────────┘
    ↓
设备驱动加载
    ↓
设备开始工作
```

### 2.2 详细步骤

#### 步骤 1: 扫描总线

```c
// drivers/pci/probe.c

int pci_scan_root_bus(struct pci_bus *bus)
{
    // 扫描总线上的所有设备
    for (devfn = 0; devfn < 256; devfn++) {
        // 读取 Vendor ID
        pci_read_config_word(bus, devfn, PCI_VENDOR_ID, &vendor);
        
        if (vendor == 0xffff || vendor == 0)
            continue;  // 设备不存在
        
        // 设备存在！创建 pci_dev
        dev = pci_scan_single_device(bus, devfn);
    }
}
```

#### 步骤 2: 读取配置空间

```c
// 通过 Host Controller 驱动提供的接口读取
static int pcie_mediatek_config_read(struct pci_bus *bus, 
                                     unsigned int devfn,
                                     int where, int size, u32 *val)
{
    // 调用驱动实现的配置空间访问
    // 最终访问硬件寄存器
}
```

#### 步骤 3: 创建设备结构

```c
struct pci_dev *pci_scan_single_device(struct pci_bus *bus, unsigned int devfn)
{
    // 读取设备信息
    pci_read_config_dword(dev, PCI_VENDOR_ID, &dev->vendor);
    pci_read_config_dword(dev, PCI_DEVICE_ID, &dev->device);
    pci_read_config_byte(dev, PCI_CLASS_REVISION, &dev->class);
    
    // 创建 pci_dev 结构
    dev = pci_alloc_dev(bus);
    
    // 添加到总线
    pci_device_add(dev, bus);
    
    return dev;
}
```

---

## 3. 驱动需要提供什么？

### 3.1 配置空间访问接口

驱动必须实现 `pci_ops`，提供配置空间访问：

```c
static struct pci_ops pcie_mediatek_ops = {
    .map_bus = pcie_mediatek_map_bus,      // 映射配置空间地址
    .read = pcie_mediatek_config_read,     // 读取配置寄存器
    .write = pcie_mediatek_config_write,   // 写入配置寄存器
};
```

### 3.2 地址映射

```c
static void __iomem *pcie_mediatek_map_bus(struct pci_bus *bus,
                                          unsigned int devfn,
                                          int where)
{
    // 将 PCIe 配置空间地址映射到硬件地址
    // 返回可访问的虚拟地址
}
```

### 3.3 读取/写入实现

```c
static int pcie_mediatek_config_read(struct pci_bus *bus,
                                     unsigned int devfn,
                                     int where, int size, u32 *val)
{
    void __iomem *addr = pcie_mediatek_map_bus(bus, devfn, where);
    
    // 从硬件读取
    *val = readl(addr);
    
    return PCIBIOS_SUCCESSFUL;
}
```

---

## 4. MediaTek 驱动的枚举实现

### 4.1 查看 MediaTek 驱动

```c
// drivers/pci/controller/pcie-mediatek.c

static int mtk_pcie_probe(struct platform_device *pdev)
{
    struct pci_host_bridge *host;
    struct mtk_pcie *pcie;
    
    // 创建 Host Bridge
    host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
    pcie = pci_host_bridge_priv(host);
    
    // 初始化硬件
    mtk_pcie_setup(pcie);
    
    // 设置操作函数
    host->ops = &mtk_pcie_ops;
    
    // 注册 Host Bridge - 触发枚举
    err = pci_host_probe(host);  // ← 这里触发枚举
    if (err)
        return err;
    
    return 0;
}
```

### 4.2 配置空间访问实现

```c
static struct pci_ops mtk_pcie_ops = {
    .map_bus = mtk_pcie_map_bus,
    .read = pci_generic_config_read,
    .write = pci_generic_config_write,
};

static void __iomem *mtk_pcie_map_bus(struct pci_bus *bus,
                                      unsigned int devfn,
                                      int where)
{
    struct mtk_pcie *pcie = bus->sysdata;
    
    // 计算配置空间地址
    // 映射到硬件寄存器
    return pcie->base + PCIE_CFG_OFFSET(bus->number, devfn, where);
}
```

---

## 5. 枚举过程详解

### 5.1 扫描过程

```
PCI 核心子系统扫描总线：

总线 0，设备 0，功能 0:
  ├─ 读取 Vendor ID (0x00)
  ├─ 如果返回 0xffff 或 0 → 设备不存在，跳过
  └─ 如果返回有效值 → 设备存在！

总线 0，设备 1，功能 0:
  ├─ 读取 Vendor ID
  └─ ...

总线 0，设备 31，功能 7:
  └─ 最后一个可能的设备位置
```

### 5.2 设备发现

```c
// 伪代码：枚举过程

for (bus = 0; bus < max_bus; bus++) {
    for (dev = 0; dev < 32; dev++) {
        for (func = 0; func < 8; func++) {
            devfn = PCI_DEVFN(dev, func);
            
            // 读取 Vendor ID
            pci_read_config_word(bus, devfn, PCI_VENDOR_ID, &vendor);
            
            if (vendor == 0xffff || vendor == 0)
                continue;  // 设备不存在
            
            // 设备存在！
            pci_dev = pci_scan_single_device(bus, devfn);
            
            // 如果是桥接器，递归扫描下级总线
            if (pci_dev->class == PCI_CLASS_BRIDGE_PCI) {
                pci_scan_child_bus(pci_dev->subordinate);
            }
        }
    }
}
```

### 5.3 设备信息读取

枚举时会读取设备的配置空间：

```c
// 读取设备基本信息
pci_read_config_dword(dev, PCI_VENDOR_ID, &dev->vendor);
pci_read_config_dword(dev, PCI_DEVICE_ID, &dev->device);
pci_read_config_byte(dev, PCI_CLASS_REVISION, &dev->class);
pci_read_config_byte(dev, PCI_HEADER_TYPE, &dev->hdr_type);

// 读取 BAR（Base Address Register）
for (i = 0; i < 6; i++) {
    pci_read_config_dword(dev, PCI_BASE_ADDRESS_0 + i*4, &bar);
    // 解析 BAR，获取设备的内存/I/O 地址
}

// 读取中断信息
pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &dev->irq);
```

---

## 6. 枚举后的结果

### 6.1 设备列表

枚举完成后，系统中会有：

```
/sys/bus/pci/devices/
├── 0000:00:00.0  (Root Complex)
├── 0000:01:00.0  (PCIe 设备 1，如网卡)
├── 0000:01:00.1  (PCIe 设备 1 的第二个功能)
└── 0000:02:00.0  (PCIe 设备 2，如存储卡)
```

### 6.2 设备驱动加载

```c
// 内核为每个设备尝试加载驱动
for_each_pci_dev(dev) {
    // 查找匹配的驱动
    driver = pci_find_driver(dev);
    
    if (driver) {
        // 加载驱动
        driver->probe(dev);
    }
}
```

---

## 7. 驱动中的枚举相关代码

### 7.1 Tegra 驱动

```c
// drivers/pci/controller/pci-tegra.c

static int tegra_pcie_probe(struct platform_device *pdev)
{
    // ...
    
    host->ops = &tegra_pcie_ops;
    host->map_irq = tegra_pcie_map_irq;
    
    // 注册 Host Bridge - 触发枚举
    err = pci_host_probe(host);  // ← 枚举在这里触发
    if (err < 0) {
        dev_err(dev, "failed to register host: %d\n", err);
        goto pm_runtime_put;
    }
    
    return 0;
}
```

### 7.2 MediaTek 驱动

```c
// drivers/pci/controller/pcie-mediatek.c

static int mtk_pcie_probe(struct platform_device *pdev)
{
    // ...
    
    host->ops = &mtk_pcie_ops;
    
    // 注册 Host Bridge - 触发枚举
    ret = pci_host_probe(host);  // ← 枚举在这里触发
    if (ret)
        return ret;
    
    return 0;
}
```

---

## 8. 枚举的时机

### 8.1 何时触发枚举？

枚举在以下时机触发：

1. **驱动加载时**：
   ```c
   pci_host_probe(host);  // 在 probe() 中调用
   ```

2. **系统启动时**：
   - 如果驱动编译进内核，在系统启动时自动加载
   - 如果驱动是模块，在 `modprobe` 时加载

3. **热插拔时**：
   - 如果支持热插拔，插入设备时也会触发扫描

### 8.2 枚举的条件

枚举需要满足：

1. ✅ **硬件已初始化**：PCIe 控制器已上电并配置
2. ✅ **链路已建立**：PCIe 链路训练成功
3. ✅ **配置空间可访问**：驱动提供了有效的 `pci_ops`

---

## 9. 验证枚举结果

### 9.1 查看枚举的设备

```bash
# 查看所有 PCIe 设备
lspci

# 查看详细信息
lspci -v

# 查看设备树
ls /sys/bus/pci/devices/
```

### 9.2 查看驱动日志

```bash
# 查看内核日志
dmesg | grep -i pcie

# 应该看到：
# [   1.234567] pcie-mediatek: PCIe link up
# [   1.234568] pci 0000:00:00.0: [xxxx:xxxx] type 01 class 0x060400
# [   1.234569] pci 0000:01:00.0: [xxxx:xxxx] type 00 class 0x020000
```

---

## 10. 总结

### 10.1 关键点

1. ✅ **驱动需要触发枚举**：通过 `pci_host_probe()` 注册 Host Bridge
2. ✅ **枚举由 PCI 核心子系统执行**：驱动不需要自己实现枚举逻辑
3. ✅ **驱动只需提供配置空间访问**：实现 `pci_ops` 接口
4. ✅ **枚举自动发现设备**：扫描总线，读取配置空间，创建设备结构

### 10.2 驱动的工作

```
驱动的工作：
├─ 初始化硬件
├─ 提供配置空间访问接口 (pci_ops)
├─ 注册 Host Bridge (pci_host_probe)
└─ 枚举由 PCI 核心子系统自动完成 ✓
```

### 10.3 枚举流程

```
pci_host_probe()
    ↓
PCI 核心子系统
    ├─ 扫描总线
    ├─ 读取配置空间（通过驱动提供的接口）
    ├─ 创建 pci_dev
    └─ 加载设备驱动
```

---

## 11. 参考资料

- [Linux PCI 子系统文档](https://www.kernel.org/doc/html/latest/PCI/index.html)
- [PCIe 设备枚举](https://www.kernel.org/doc/html/latest/PCI/pci.html#device-enumeration)
- [PCI Host Controller 驱动编写指南](https://www.kernel.org/doc/html/latest/PCI/pci.html#host-bridge-drivers)

