# Tegra PCIe 驱动解析

## 1. 驱动的作用

`pci-tegra.c` 是 **PCIe Host Controller 驱动**（PCIe 主机控制器驱动），用于 NVIDIA Tegra SoC。

### 1.1 核心功能

这个驱动实现了 **PCIe Root Complex（根复合体）** 的功能，使 Tegra SoC 能够：

1. **作为 PCIe 主机**：管理连接到 Tegra 的 PCIe 设备
2. **枚举 PCIe 设备**：扫描 PCIe 总线，发现连接的设备
3. **配置空间访问**：提供对 PCIe 设备配置空间的访问
4. **MSI 支持**：处理 PCIe 设备的 MSI 中断
5. **地址转换**：管理 PCIe 地址空间到系统内存的映射

---

## 2. 与普通 PCIe 设备驱动的区别

### 2.1 PCIe Host Controller 驱动（本驱动）

```
┌─────────────────────────────────┐
│   Tegra SoC (Root Complex)      │
│  ┌───────────────────────────┐  │
│  │  PCIe Host Controller     │  │ ← pci-tegra.c 驱动这里
│  │  (硬件 + 驱动)            │  │
│  └───────────┬───────────────┘  │
└──────────────┼──────────────────┘
               │ PCIe 总线
               │
    ┌──────────┴──────────┐
    │                       │
┌───▼────┐            ┌────▼───┐
│ PCIe   │            │ PCIe   │
│ 设备1  │            │ 设备2  │
└────────┘            └────────┘
```

**作用**：
- 管理 PCIe 总线
- 提供配置空间访问接口
- 处理地址转换
- 管理中断

### 2.2 PCIe 设备驱动（普通设备驱动）

```
┌────────┐
│ PCIe   │ ← 设备驱动（如网卡驱动、显卡驱动）
│ 设备   │
└────────┘
```

**作用**：
- 控制具体的 PCIe 设备
- 实现设备特定的功能

---

## 3. 驱动的主要组件

### 3.1 PCIe 配置空间访问

```c
// 提供配置空间访问接口
static struct pci_ops tegra_pcie_ops = {
    .map_bus = tegra_pcie_map_bus,      // 映射配置空间地址
    .read = tegra_pcie_config_read,      // 读取配置寄存器
    .write = tegra_pcie_config_write,    // 写入配置寄存器
};
```

**作用**：让内核能够访问 PCIe 设备的配置空间（Vendor ID、Device ID、BAR 等）

### 3.2 MSI 中断支持

```c
// MSI 中断处理
static irqreturn_t tegra_pcie_msi_irq(struct irq_desc *desc)
{
    // 处理 MSI 中断
    // 分发到对应的设备驱动
}
```

**作用**：处理 PCIe 设备的 MSI/MSI-X 中断

### 3.3 地址转换（Translation）

```c
static void tegra_pcie_setup_translations(struct tegra_pcie *pcie)
{
    // 设置 PCIe 地址到系统内存地址的转换
    // BAR0: 配置空间
    // BAR1: I/O 空间
    // BAR2: 内存空间（可预取）
    // BAR3: 内存空间（不可预取）
}
```

**作用**：将 PCIe 设备的地址空间映射到系统内存

### 3.4 端口管理

```c
struct tegra_pcie_port {
    struct tegra_pcie *pcie;
    unsigned int index;      // 端口索引
    unsigned int lanes;       // 通道数（x1, x2, x4, x8）
    void __iomem *base;       // 端口寄存器基地址
    // ...
};
```

**作用**：管理多个 PCIe 端口（Tegra 可能有多个 PCIe 端口）

---

## 4. 驱动的工作流程

### 4.1 初始化流程

```
1. tegra_pcie_probe()
   ├─ 解析设备树（获取端口配置、时钟、复位等）
   ├─ 获取资源（寄存器、中断、时钟、电源）
   ├─ 初始化 MSI 支持
   ├─ 上电和初始化硬件
   └─ 注册 PCIe Host Bridge

2. pci_host_probe()
   ├─ 扫描 PCIe 总线
   ├─ 枚举连接的设备
   └─ 为每个设备创建 pci_dev 结构

3. 设备驱动加载
   └─ 内核为每个发现的设备加载对应的驱动
```

### 4.2 配置空间访问流程

```
用户/驱动访问 PCIe 配置空间
    ↓
pci_read_config_byte/word/dword()
    ↓
tegra_pcie_config_read()
    ↓
tegra_pcie_map_bus()  // 映射到硬件地址
    ↓
访问硬件寄存器
```

### 4.3 MSI 中断流程

```
PCIe 设备发送 MSI
    ↓
硬件触发 MSI 中断
    ↓
tegra_pcie_msi_irq()
    ↓
查找对应的 MSI 向量
    ↓
调用设备驱动的中断处理函数
```

---

## 5. Tegra 特定的功能

### 5.1 多端口支持

Tegra SoC 可能有多个 PCIe 端口：

```c
// 支持多个端口配置
static const struct tegra_pcie_soc tegra186_pcie = {
    .num_ports = 3,  // 3 个 PCIe 端口
    // ...
};
```

### 5.2 硬件特定初始化

```c
// Tegra 特定的寄存器配置
static void tegra_pcie_enable_rp_features(struct tegra_pcie_port *port)
{
    // 配置 Root Port 特性
    // 优化带宽设置
    // 配置时钟钳制
}
```

### 5.3 电源管理

```c
// Tegra 特定的电源管理
static int tegra_pcie_power_on(struct tegra_pcie *pcie)
{
    // 启用电源域
    // 配置时钟
    // 解除复位
}
```

---

## 6. 设备树绑定

### 6.1 设备树节点示例

```dts
pcie@14100000 {
    compatible = "nvidia,tegra194-pcie";
    reg = <0x00 0x14100000 0x0 0x00020000>,  // appl registers
          <0x00 0x30000000 0x0 0x00040000>,  // config space
          <0x00 0x30040000 0x0 0x00040000>,  // iATU_DMA
          <0x00 0x30080000 0x0 0x00040000>;  // DBI
    reg-names = "appl", "config", "atu_dma", "dbi";
    
    #address-cells = <3>;
    #size-cells = <2>;
    device_type = "pci";
    
    // 子节点：PCIe 端口
    pcie@0,0 {
        reg = <0x00010000 0x0 0x00000000 0x0 0x00100000>;
        nvidia,num-lanes = <1>;
    };
};
```

### 6.2 驱动如何解析

```c
static int tegra_pcie_parse_dt(struct tegra_pcie *pcie)
{
    // 解析设备树
    // 获取端口配置
    // 获取时钟和复位信息
    // 获取电源配置
}
```

---

## 7. 与其他驱动的关系

### 7.1 与 PCI 核心子系统

```
┌─────────────────────────────────┐
│   PCI 核心子系统                │
│   (drivers/pci/)                │
│   - pci.c                       │
│   - probe.c                     │
│   - bus.c                       │
└────────────┬────────────────────┘
             │
             │ 调用
             │
┌────────────▼────────────────────┐
│   PCIe Host Controller 驱动     │
│   (pci-tegra.c)                 │
│   - 提供配置空间访问             │
│   - 提供地址转换                 │
│   - 提供中断支持                 │
└────────────┬────────────────────┘
             │
             │ 控制
             │
┌────────────▼────────────────────┐
│   Tegra PCIe 硬件               │
└─────────────────────────────────┘
```

### 7.2 与设备驱动

```
PCI 核心子系统
    ↓ 枚举设备
发现 PCIe 设备（如网卡）
    ↓
加载设备驱动（如网卡驱动）
    ↓
设备驱动通过 PCI 核心访问设备
    ↓
PCI 核心调用 Host Controller 驱动
    ↓
访问硬件
```

---

## 8. 关键数据结构

### 8.1 tegra_pcie

```c
struct tegra_pcie {
    struct device *dev;
    void __iomem *pads;      // PADS 寄存器
    void __iomem *afi;       // AFI 寄存器
    void __iomem *cfg;       // 配置空间
    struct tegra_msi msi;    // MSI 支持
    struct list_head ports;  // PCIe 端口列表
    const struct tegra_pcie_soc *soc;  // SoC 特定配置
    // ...
};
```

### 8.2 tegra_pcie_port

```c
struct tegra_pcie_port {
    struct tegra_pcie *pcie;
    unsigned int index;      // 端口索引
    unsigned int lanes;       // 通道数
    void __iomem *base;      // 端口寄存器
    struct phy **phys;       // PHY 接口
    // ...
};
```

---

## 9. 总结

### 9.1 这个驱动是什么？

- ✅ **PCIe Host Controller 驱动**
- ✅ **Root Complex 驱动**
- ✅ **使 Tegra SoC 能够作为 PCIe 主机**

### 9.2 主要功能

1. **配置空间访问**：让内核能够读取/写入 PCIe 设备配置寄存器
2. **设备枚举**：扫描 PCIe 总线，发现连接的设备
3. **地址转换**：将 PCIe 地址空间映射到系统内存
4. **中断处理**：支持 Legacy 和 MSI/MSI-X 中断
5. **电源管理**：管理 PCIe 控制器的电源状态

### 9.3 与其他驱动的区别

| 驱动类型 | 作用 | 示例 |
|---------|------|------|
| **Host Controller 驱动** | 管理 PCIe 总线，提供基础设施 | `pci-tegra.c` |
| **设备驱动** | 控制具体的 PCIe 设备 | 网卡驱动、显卡驱动 |

### 9.4 类比

- **Host Controller 驱动** = "高速公路管理系统"
  - 管理道路（PCIe 总线）
  - 提供基础设施（配置空间、地址转换）
  
- **设备驱动** = "汽车司机"
  - 控制具体的车辆（PCIe 设备）
  - 使用道路系统提供的服务

---

## 10. 参考资料

- [PCIe 规范](https://pcisig.com/)
- [Linux PCI 子系统文档](https://www.kernel.org/doc/html/latest/PCI/index.html)
- [Tegra PCIe 驱动源码](https://elixir.bootlin.com/linux/latest/source/drivers/pci/controller/pci-tegra.c)

