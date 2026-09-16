# PCIe Host Controller 驱动编写指南

## 核心概念

PCIe Host Controller 驱动实际上是一个**平台驱动（Platform Driver）**，它：
1. 通过 `platform_bus_type` 注册（因为它是 SoC 内部的平台设备）
2. 初始化 PCIe 硬件控制器
3. 提供 PCI 配置空间访问接口
4. 调用 `pci_host_probe()` 启动 PCIe 设备枚举

## 关键步骤

### 1. 定义驱动私有数据结构

```c
struct my_pcie {
    struct device *dev;
    void __iomem *base;        // 寄存器基地址
    struct clk *clk;           // 时钟
    struct reset_control *rst; // 复位
    struct phy *phy;           // PHY
    struct irq_domain *irq_domain;
    int irq;
    u32 busnr;                 // 总线号
};
```

### 2. 实现 PCI 配置空间访问操作（核心！）

必须实现 `struct pci_ops`：

```c
static struct pci_ops my_pcie_ops = {
    .read = my_pcie_rd_conf,   // 读取配置空间
    .write = my_pcie_wr_conf,  // 写入配置空间
};
```

**关键点：**
- `read/write` 函数会被 PCI 核心调用来访问配置空间
- 需要区分 Root Complex 自己的配置空间和下游设备的配置空间
- Root Complex: `pci_is_root_bus(bus) && PCI_SLOT(devfn) == 0`
- 下游设备: 通过 PCIe 配置事务访问

### 3. 硬件初始化

```c
static int my_pcie_init_hw(struct my_pcie *pcie)
{
    // 1. 使能时钟
    clk_prepare_enable(pcie->clk);
    
    // 2. 释放复位
    reset_control_deassert(pcie->rst);
    
    // 3. 初始化 PHY
    phy_init(pcie->phy);
    phy_power_on(pcie->phy);
    
    // 4. 配置 PCIe 控制器寄存器
    // 设置链路宽度、速度、ATU（地址转换单元）等
    
    return 0;
}
```

### 4. Probe 函数流程

```c
static int my_pcie_probe(struct platform_device *pdev)
{
    // 1. 分配 host bridge（包含私有数据）
    bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
    pcie = pci_host_bridge_priv(bridge);
    
    // 2. 解析设备树（获取资源）
    my_pcie_parse_dt(pcie);
    
    // 3. 初始化硬件
    my_pcie_init_hw(pcie);
    
    // 4. 设置 host bridge
    bridge->sysdata = pcie;      // 保存私有数据指针
    bridge->ops = &my_pcie_ops;  // 设置配置空间访问操作
    
    // 5. 解析资源窗口（IO、Memory）
    pci_parse_request_of_pci_ranges(...);
    
    // 6. 启动 PCIe 枚举（关键！）
    pci_host_probe(bridge);
    
    return 0;
}
```

### 5. 注册为平台驱动

```c
static struct platform_driver my_pcie_driver = {
    .driver = {
        .name = "my-pcie",
        .of_match_table = my_pcie_of_match,  // 设备树匹配
    },
    .probe = my_pcie_probe,
    .remove_new = my_pcie_remove,
};

module_platform_driver(my_pcie_driver);
```

## 设备树示例

```dts
pcie: pcie@f0000000 {
    compatible = "my-company,my-pcie";
    reg = <0xf0000000 0x1000000>;        // 寄存器地址
    interrupts = <GIC_SPI 100 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&pcie_clk>;
    resets = <&pcie_rst>;
    phy-names = "pcie-phy";
    phys = <&pcie_phy>;
    
    // PCIe 资源窗口
    ranges = <0x81000000 0x0 0x00000000 0x10000000 0x0 0x00010000   /* I/O */
              0x82000000 0x0 0x20000000 0x20000000 0x0 0x10000000>; /* Memory */
    
    bus-range = <0x0 0xff>;
    #address-cells = <3>;
    #size-cells = <2>;
    #interrupt-cells = <1>;
};
```

## 关键 API 说明

### `devm_pci_alloc_host_bridge()`
- 分配 `pci_host_bridge` 结构
- 同时分配私有数据空间
- 使用 `devm_` 前缀，自动管理内存

### `pci_host_bridge_priv()`
- 从 `pci_host_bridge` 获取私有数据指针
- 私有数据紧跟在 `pci_host_bridge` 后面

### `pci_host_probe()`
- **这是 PCIe 枚举的入口**
- 内部会调用：
  - `pci_scan_root_bus_bridge()` - 扫描 PCIe 总线
  - `pci_assign_unassigned_root_bus_resources()` - 分配资源
  - `pci_bus_add_devices()` - 添加设备并绑定驱动

### `pci_parse_request_of_pci_ranges()`
- 从设备树解析 `ranges` 属性
- 设置 IO、Memory、Prefetch Memory 窗口

## 与 PCI 总线的关系

```
platform_bus_type (平台总线)
    ↓
my_pcie_driver.probe()  ← 通过设备树匹配
    ↓
初始化 PCIe 硬件控制器
    ↓
pci_host_probe()
    ↓
pci_scan_root_bus_bridge()
    ↓
发现 PCIe 设备
    ↓
pci_bus_type (PCI 总线)  ← 管理 PCIe 设备
    ↓
PCI 设备与 PCI 驱动匹配
```

## 常见问题

### Q: 为什么是 platform driver 而不是 pci driver？
A: PCIe host controller 是 SoC 内部的平台设备，通过设备树描述，所以用 platform driver。

### Q: 配置空间访问如何实现？
A: 根据硬件实现：
- **ECAM (Enhanced Configuration Access Mechanism)**: 直接内存映射
- **Type1 配置**: 通过配置地址/数据寄存器
- **自定义方式**: 根据硬件文档实现

### Q: 如何调试？
A:
1. 检查硬件初始化是否成功
2. 检查配置空间读写是否正确
3. 使用 `lspci` 查看设备
4. 检查 `/sys/bus/pci/devices/` 下的设备

## 完整示例

参考 `/home/fbt1709/pcie_host_driver_example.c`



