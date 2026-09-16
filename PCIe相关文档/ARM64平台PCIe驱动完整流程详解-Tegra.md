# ARM64平台PCIe驱动完整流程详解
## 以NVIDIA Tegra PCIe控制器为例

---

## 目录

1. [概述](#概述)
2. [系统架构](#系统架构)
3. [阶段1: 设备树解析](#阶段1-设备树解析)
4. [阶段2: Platform驱动初始化](#阶段2-platform驱动初始化)
5. [阶段3: PCIe控制器硬件初始化](#阶段3-pcie控制器硬件初始化)
6. [阶段4: PCIe总线扫描和设备发现](#阶段4-pcie总线扫描和设备发现)
7. [阶段5: 资源分配](#阶段5-资源分配)
8. [阶段6: 设备驱动匹配和加载](#阶段6-设备驱动匹配和加载)
9. [关键数据结构](#关键数据结构)
10. [代码流程详解](#代码流程详解)

---

## 概述

本文档以**NVIDIA Tegra210**平台的PCIe控制器驱动为例，详细讲解ARM64平台上PCIe设备驱动的完整流程，从设备树配置到设备驱动加载的每一个步骤。

**驱动位置：**
- 控制器驱动：`drivers/pci/controller/pci-tegra.c`
- 设备树：`arch/arm64/boot/dts/nvidia/tegra210.dtsi`

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                    ARM64平台启动流程                          │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  阶段1: 设备树解析 (Device Tree)                            │
│  - 解析pcie@1003000节点                                      │
│  - 读取reg、interrupts、clocks等属性                        │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  阶段2: Platform驱动匹配和初始化                             │
│  drivers/pci/controller/pci-tegra.c                         │
│  - tegra_pcie_probe()                                       │
│  - 解析设备树资源                                            │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  阶段3: PCIe控制器硬件初始化                                 │
│  - 电源管理（regulator、powergate）                         │
│  - 时钟配置（clk_prepare_enable）                           │
│  - 复位控制（reset_control_deassert）                      │
│  - PHY初始化（phy_power_on）                                │
│  - 配置空间映射（ioremap）                                  │
│  - MSI初始化                                                │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  阶段4: PCIe总线扫描和设备发现                               │
│  drivers/pci/probe.c                                        │
│  - pci_host_probe()                                         │
│  - pci_scan_root_bus_bridge()                               │
│  - pci_scan_child_bus()                                     │
│  - 读取Vendor ID和Device ID                                 │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  阶段5: 资源分配                                             │
│  drivers/pci/setup-res.c                                    │
│  - pci_assign_unassigned_root_bus_resources()              │
│  - 分配BAR、中断等资源                                       │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  阶段6: 设备添加到系统                                       │
│  drivers/pci/bus.c                                          │
│  - pci_bus_add_devices()                                    │
│  - device_attach() → 触发驱动匹配                           │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│  阶段7: 设备驱动匹配和加载                                   │
│  drivers/pci/pci-driver.c                                   │
│  - pci_match_device()                                       │
│  - pci_call_probe()                                         │
│  - 调用设备驱动的probe()函数                                 │
└─────────────────────────────────────────────────────────────┘
```

---

## 阶段1: 设备树解析

### 1.1 设备树节点定义

**位置：** `arch/arm64/boot/dts/nvidia/tegra210.dtsi`

```dts
pcie@1003000 {
    compatible = "nvidia,tegra210-pcie";
    device_type = "pci";
    
    /* 寄存器基地址 */
    reg = <0x0 0x01003000 0x0 0x00000800>, /* PADS registers */
          <0x0 0x01003800 0x0 0x00000800>, /* AFI registers */
          <0x0 0x02000000 0x0 0x10000000>; /* configuration space */
    reg-names = "pads", "afi", "cs";
    
    /* 中断定义 */
    interrupts = <GIC_SPI 98 IRQ_TYPE_LEVEL_HIGH>, /* controller interrupt */
                 <GIC_SPI 99 IRQ_TYPE_LEVEL_HIGH>; /* MSI interrupt */
    interrupt-names = "intr", "msi";
    
    /* 中断映射 */
    #interrupt-cells = <1>;
    interrupt-map-mask = <0 0 0 0>;
    interrupt-map = <0 0 0 0 &gic GIC_SPI 98 IRQ_TYPE_LEVEL_HIGH>;
    
    /* 总线范围 */
    bus-range = <0x00 0xff>;
    
    /* 地址格式 */
    #address-cells = <3>;
    #size-cells = <2>;
    
    /* 地址映射（ranges） */
    ranges = <0x02000000 0 0x01000000 0x0 0x01000000 0 0x00001000>, /* port 0配置空间 */
             <0x02000000 0 0x01001000 0x0 0x01001000 0 0x00001000>, /* port 1配置空间 */
             <0x01000000 0 0x0        0x0 0x12000000 0 0x00010000>, /* I/O空间 (64 KiB) */
             <0x02000000 0 0x13000000 0x0 0x13000000 0 0x0d000000>, /* 非预取内存 (208 MiB) */
             <0x42000000 0 0x20000000 0x0 0x20000000 0 0x20000000>; /* 预取内存 (512 MiB) */
    
    /* 时钟定义 */
    clocks = <&tegra_car TEGRA210_CLK_PCIE>,
             <&tegra_car TEGRA210_CLK_AFI>,
             <&tegra_car TEGRA210_CLK_PLL_E>,
             <&tegra_car TEGRA210_CLK_CML0>;
    clock-names = "pex", "afi", "pll_e", "cml";
    
    /* 复位定义 */
    resets = <&tegra_car 70>,
             <&tegra_car 72>,
             <&tegra_car 74>;
    reset-names = "pex", "afi", "pcie_x";
    
    /* 引脚控制 */
    pinctrl-names = "default", "idle";
    pinctrl-0 = <&pex_dpd_disable>;
    pinctrl-1 = <&pex_dpd_enable>;
    
    status = "disabled";
    
    /* PCIe端口定义 */
    pci@1,0 {
        device_type = "pci";
        assigned-addresses = <0x82000800 0 0x01000000 0 0x1000>;
        reg = <0x000800 0 0 0 0>;
        bus-range = <0x00 0xff>;
        status = "disabled";
        #address-cells = <3>;
        #size-cells = <2>;
        ranges;
        nvidia,num-lanes = <4>;
    };
    
    pci@2,0 {
        device_type = "pci";
        assigned-addresses = <0x82001000 0 0x01001000 0 0x1000>;
        reg = <0x001000 0 0 0 0>;
        bus-range = <0x00 0xff>;
        status = "disabled";
        #address-cells = <3>;
        #size-cells = <2>;
        ranges;
        nvidia,num-lanes = <1>;
    };
};
```

### 1.2 设备树属性说明

| 属性 | 说明 |
|------|------|
| `compatible` | 用于匹配Platform驱动：`"nvidia,tegra210-pcie"` |
| `reg` | 三个寄存器区域：PADS、AFI、配置空间 |
| `interrupts` | 控制器中断和MSI中断（GIC SPI 98和99） |
| `ranges` | 地址映射，定义PCIe地址空间到CPU地址空间的映射 |
| `clocks` | PCIe相关的时钟源 |
| `resets` | PCIe相关的复位信号 |

---

## 阶段2: Platform驱动初始化

### 2.1 驱动注册

**位置：** `drivers/pci/controller/pci-tegra.c`

```c
static struct platform_driver tegra_pcie_driver = {
    .driver = {
        .name = "tegra-pcie",
        .of_match_table = tegra_pcie_of_match,  // 匹配设备树
        .pm = &tegra_pcie_pm_ops,
    },
    .probe = tegra_pcie_probe,
    .remove_new = tegra_pcie_remove,
};

module_platform_driver(tegra_pcie_driver);
```

### 2.2 设备树匹配表

```c
static const struct of_device_id tegra_pcie_of_match[] = {
    { .compatible = "nvidia,tegra210-pcie", .data = &tegra210_pcie_soc },
    { .compatible = "nvidia,tegra186-pcie", .data = &tegra186_pcie_soc },
    { .compatible = "nvidia,tegra194-pcie", .data = &tegra194_pcie_soc },
    { },
};
MODULE_DEVICE_TABLE(of, tegra_pcie_of_match);
```

### 2.3 Probe函数入口

```c
static int tegra_pcie_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct pci_host_bridge *host;
    struct tegra_pcie *pcie;
    int err;
    
    // 1. 分配Host Bridge结构
    host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
    if (!host)
        return -ENOMEM;
    
    pcie = pci_host_bridge_priv(host);
    host->sysdata = pcie;
    platform_set_drvdata(pdev, pcie);
    
    // 2. 获取SoC特定数据
    pcie->soc = of_device_get_match_data(dev);
    INIT_LIST_HEAD(&pcie->ports);
    pcie->dev = dev;
    
    // 3. 解析设备树
    err = tegra_pcie_parse_dt(pcie);
    if (err < 0)
        return err;
    
    // 4. 获取资源（时钟、复位、寄存器等）
    err = tegra_pcie_get_resources(pcie);
    if (err < 0) {
        dev_err(dev, "failed to request resources: %d\n", err);
        return err;
    }
    
    // 5. 初始化MSI
    err = tegra_pcie_msi_setup(pcie);
    if (err < 0) {
        dev_err(dev, "failed to enable MSI support: %d\n", err);
        goto put_resources;
    }
    
    // 6. 电源管理
    pm_runtime_enable(pcie->dev);
    err = pm_runtime_get_sync(pcie->dev);
    if (err < 0) {
        dev_err(dev, "fail to enable pcie controller: %d\n", err);
        goto pm_runtime_put;
    }
    
    // 7. 设置配置空间访问函数
    host->ops = &tegra_pcie_ops;  // ← 关键！
    host->map_irq = tegra_pcie_map_irq;
    
    // 8. 开始PCIe总线扫描（进入阶段4）
    err = pci_host_probe(host);
    if (err < 0) {
        dev_err(dev, "failed to register host: %d\n", err);
        goto pm_runtime_put;
    }
    
    return 0;
    
pm_runtime_put:
    pm_runtime_put_sync(pcie->dev);
    pm_runtime_disable(pcie->dev);
    tegra_pcie_msi_teardown(pcie);
put_resources:
    tegra_pcie_put_resources(pcie);
    return err;
}
```

---

## 阶段3: PCIe控制器硬件初始化

### 3.1 解析设备树资源

```c
static int tegra_pcie_parse_dt(struct tegra_pcie *pcie)
{
    struct device *dev = pcie->dev;
    struct device_node *np = dev->of_node;
    struct resource res;
    int err;
    
    // 1. 解析寄存器资源
    err = of_address_to_resource(np, 0, &res);
    pcie->pads = devm_ioremap_resource(dev, &res);
    
    err = of_address_to_resource(np, 1, &res);
    pcie->afi = devm_ioremap_resource(dev, &res);
    
    err = of_address_to_resource(np, 2, &res);
    pcie->cfg = devm_ioremap_resource(dev, &res);
    
    // 2. 解析中断
    pcie->irq = platform_get_irq_byname(pdev, "intr");
    pcie->msi_irq = platform_get_irq_byname(pdev, "msi");
    
    // 3. 解析时钟
    pcie->pex_clk = devm_clk_get(dev, "pex");
    pcie->afi_clk = devm_clk_get(dev, "afi");
    pcie->pll_e = devm_clk_get(dev, "pll_e");
    pcie->cml_clk = devm_clk_get(dev, "cml");
    
    // 4. 解析复位
    pcie->pex_rst = devm_reset_control_get(dev, "pex");
    pcie->afi_rst = devm_reset_control_get(dev, "afi");
    pcie->pcie_xrst = devm_reset_control_get(dev, "pcie_x");
    
    // 5. 解析PCIe端口
    for_each_child_of_node(np, port_np) {
        err = tegra_pcie_port_parse_dt(port_np, pcie);
    }
    
    return 0;
}
```

### 3.2 电源和时钟初始化

```c
static int tegra_pcie_power_on(struct tegra_pcie *pcie)
{
    struct device *dev = pcie->dev;
    int err;
    
    // 1. 断言所有复位
    reset_control_assert(pcie->pcie_xrst);
    reset_control_assert(pcie->afi_rst);
    reset_control_assert(pcie->pex_rst);
    
    // 2. 使能电源域
    if (!dev->pm_domain)
        tegra_powergate_power_off(TEGRA_POWERGATE_PCIE);
    
    // 3. 使能regulator
    err = regulator_bulk_enable(pcie->num_supplies, pcie->supplies);
    
    // 4. 使能电源门控
    if (!dev->pm_domain) {
        err = tegra_powergate_power_on(TEGRA_POWERGATE_PCIE);
        err = tegra_powergate_remove_clamping(TEGRA_POWERGATE_PCIE);
    }
    
    // 5. 使能时钟
    err = clk_prepare_enable(pcie->pll_e);
    if (pcie->cml_clk)
        err = clk_prepare_enable(pcie->cml_clk);
    err = clk_prepare_enable(pcie->afi_clk);
    
    // 6. 解除复位
    reset_control_deassert(pcie->afi_rst);
    
    return 0;
}
```

### 3.3 控制器使能

```c
static void tegra_pcie_enable_controller(struct tegra_pcie *pcie)
{
    const struct tegra_pcie_soc *soc = pcie->soc;
    struct tegra_pcie_port *port;
    unsigned long value;
    
    // 1. 配置PLL
    if (pcie->phy) {
        value = afi_readl(pcie, AFI_PLLE_CONTROL);
        value &= ~AFI_PLLE_CONTROL_BYPASS_PADS2PLLE_CONTROL;
        value |= AFI_PLLE_CONTROL_PADS2PLLE_CONTROL_EN;
        afi_writel(pcie, value, AFI_PLLE_CONTROL);
    }
    
    // 2. 配置PCIe模式
    value = afi_readl(pcie, AFI_PCIE_CONFIG);
    value &= ~AFI_PCIE_CONFIG_SM2TMS0_XBAR_CONFIG_MASK;
    value |= AFI_PCIE_CONFIG_PCIE_DISABLE_ALL | pcie->xbar_config;
    
    // 3. 使能所有端口
    list_for_each_entry(port, &pcie->ports, list) {
        value &= ~AFI_PCIE_CONFIG_PCIE_DISABLE(port->index);
    }
    
    afi_writel(pcie, value, AFI_PCIE_CONFIG);
    
    // 4. 配置Gen2支持
    if (soc->has_gen2) {
        value = afi_readl(pcie, AFI_FUSE);
        value &= ~AFI_FUSE_PCIE_T0_GEN2_DIS;
        afi_writel(pcie, value, AFI_FUSE);
    }
    
    // 5. 使能FPCI（FPCI = Fast PCI）
    value = afi_readl(pcie, AFI_CONFIGURATION);
    value |= AFI_CONFIGURATION_EN_FPCI;
    value |= AFI_CONFIGURATION_CLKEN_OVERRIDE;
    afi_writel(pcie, value, AFI_CONFIGURATION);
    
    // 6. 使能中断
    value = AFI_INTR_EN_INI_SLVERR | AFI_INTR_EN_INI_DECERR |
            AFI_INTR_EN_TGT_SLVERR | AFI_INTR_EN_TGT_DECERR |
            AFI_INTR_EN_TGT_WRERR | AFI_INTR_EN_DFPCI_DECERR;
    
    if (soc->has_intr_prsnt_sense)
        value |= AFI_INTR_EN_PRSNT_SENSE;
    
    afi_writel(pcie, value, AFI_AFI_INTR_ENABLE);
    afi_writel(pcie, 0xffffffff, AFI_SM_INTR_ENABLE);
    
    // 7. 使能中断掩码
    afi_writel(pcie, AFI_INTR_MASK_INT_MASK, AFI_INTR_MASK);
    
    // 8. 禁用所有异常
    afi_writel(pcie, 0, AFI_FPCI_ERROR_MASKS);
}
```

### 3.4 配置空间访问函数

```c
// 配置空间映射函数
static void __iomem *tegra_pcie_map_bus(struct pci_bus *bus,
                                        unsigned int devfn, int where)
{
    struct tegra_pcie *pcie = bus->sysdata;
    void __iomem *addr = NULL;
    
    if (pci_is_root_bus(bus)) {
        // Root Bus：直接访问配置空间
        if (PCI_SLOT(devfn) == 0) {
            addr = pcie->cfg + (PCI_FUNC(devfn) << 8);
        }
    } else {
        // 子总线：通过Type 1配置访问
        struct tegra_pcie_port *port;
        
        list_for_each_entry(port, &pcie->ports, list) {
            if (port->root_bus_nr != bus->number)
                continue;
            
            addr = tegra_pcie_conf_address(port, bus, devfn, where);
            break;
        }
    }
    
    return addr;
}

// PCIe操作结构
static struct pci_ops tegra_pcie_ops = {
    .map_bus = tegra_pcie_map_bus,
    .read = pci_generic_config_read,
    .write = pci_generic_config_write,
};
```

### 3.5 PHY和端口初始化

```c
static void tegra_pcie_port_enable(struct tegra_pcie_port *port)
{
    unsigned long ctrl = tegra_pcie_port_get_pex_ctrl(port);
    unsigned long value;
    
    // 1. 使能参考时钟
    value = afi_readl(port->pcie, ctrl);
    value |= AFI_PEX_CTRL_REFCLK_EN;
    value |= AFI_PEX_CTRL_CLKREQ_EN;
    value |= AFI_PEX_CTRL_OVERRIDE_EN;
    afi_writel(port->pcie, value, ctrl);
    
    // 2. 复位端口
    tegra_pcie_port_reset(port);
    
    // 3. 使能Root Port特性
    tegra_pcie_enable_rp_features(port);
    
    // 4. 配置ECTL（Equalization Control）
    if (soc->ectl.enable)
        tegra_pcie_program_ectl_settings(port);
    
    // 5. 应用软件修复
    tegra_pcie_apply_sw_fixup(port);
}
```

---

## 阶段4: PCIe总线扫描和设备发现

### 4.1 总线扫描入口

**位置：** `drivers/pci/probe.c`

```c
int pci_host_probe(struct pci_host_bridge *bridge)
{
    struct pci_bus *bus, *child;
    int ret;
    
    // 1. 扫描根总线
    pci_lock_rescan_remove();
    ret = pci_scan_root_bus_bridge(bridge);
    pci_unlock_rescan_remove();
    
    if (ret < 0) {
        dev_err(bridge->dev.parent, "Scanning root bridge failed");
        return ret;
    }
    
    bus = bridge->bus;
    
    // 2. 分配资源
    if (bridge->preserve_config)
        pci_bus_claim_resources(bus);
    
    pci_assign_unassigned_root_bus_resources(bus);
    
    // 3. 配置PCIe设置
    list_for_each_entry(child, &bus->children, node)
        pcie_bus_configure_settings(child);
    
    // 4. 添加设备到系统（触发驱动匹配）
    pci_lock_rescan_remove();
    pci_bus_add_devices(bus);
    pci_unlock_rescan_remove();
    
    return 0;
}
```

### 4.2 扫描根总线

```c
int pci_scan_root_bus_bridge(struct pci_host_bridge *bridge)
{
    struct resource_entry *window;
    bool found = false;
    struct pci_bus *b;
    int max, bus, ret;
    
    // 1. 查找总线号资源
    resource_list_for_each_entry(window, &bridge->windows)
        if (window->res->flags & IORESOURCE_BUS) {
            bridge->busnr = window->res->start;
            found = true;
            break;
        }
    
    // 2. 注册Host Bridge
    ret = pci_register_host_bridge(bridge);
    if (ret < 0)
        return ret;
    
    b = bridge->bus;
    bus = bridge->busnr;
    
    // 3. 如果没有指定总线号，使用默认值
    if (!found) {
        dev_info(&b->dev,
                 "No busn resource found for root bus, will use [bus %02x-ff]\n",
                 bus);
        pci_bus_insert_busn_res(b, bus, 255);
    }
    
    // 4. 扫描子总线（递归扫描所有设备）
    max = pci_scan_child_bus(b);
    
    if (!found)
        pci_bus_update_busn_res_end(b, max);
    
    return 0;
}
```

### 4.3 扫描子总线

```c
unsigned int pci_scan_child_bus(struct pci_bus *bus)
{
    unsigned int devfn, cmax, max = bus->busn_res.start;
    struct pci_dev *dev;
    
    dev_dbg(&bus->dev, "scanning bus\n");
    
    // 1. 扫描所有可能的设备（devfn = 0-255，步进8）
    for (devfn = 0; devfn < 256; devfn += 8)
        pci_scan_slot(bus, devfn);
    
    // 2. 扫描桥接器后面的设备（递归）
    for_each_pci_bridge(dev, bus) {
        cmax = max;
        max = pci_scan_bridge_extend(bus, dev, max, 0, 0);
    }
    
    dev_dbg(&bus->dev, "bus scan returning with max=%02x\n", max);
    return max;
}
```

### 4.4 扫描单个设备

```c
static struct pci_dev *pci_scan_device(struct pci_bus *bus, int devfn)
{
    struct pci_dev *dev;
    u32 l;
    
    // 1. 读取Vendor ID和Device ID
    if (!pci_bus_read_dev_vendor_id(bus, devfn, &l, 60*1000))
        return NULL;  // 设备不存在
    
    // 2. 分配pci_dev结构
    dev = pci_alloc_dev(bus);
    if (!dev)
        return NULL;
    
    dev->devfn = devfn;
    dev->vendor = l & 0xffff;        // Vendor ID
    dev->device = (l >> 16) & 0xffff; // Device ID
    
    // 3. 设置设备（读取完整配置空间）
    if (pci_setup_device(dev)) {
        pci_bus_put(dev->bus);
        kfree(dev);
        return NULL;
    }
    
    return dev;
}
```

### 4.5 设备发现流程

```
pci_scan_child_bus()
    ↓
for (devfn = 0; devfn < 256; devfn += 8)
    ↓
pci_scan_slot(bus, devfn)
    ↓
pci_scan_device(bus, devfn)
    ↓
pci_bus_read_dev_vendor_id()  // 通过tegra_pcie_ops.read读取配置空间0x00
    ↓
如果返回有效ID (不是0xFFFF或0x0000)
    ↓
pci_alloc_dev()              // 分配pci_dev结构
    ↓
pci_setup_device()            // 读取完整配置空间
    - 读取Header Type
    - 读取Class Code
    - 读取BARs
    - 读取Capabilities
    - 初始化PCIe能力
```

---

## 阶段5: 资源分配

### 5.1 资源分配流程

```c
void pci_assign_unassigned_root_bus_resources(struct pci_bus *bus)
{
    // 1. 先分配关键资源（桥接器窗口）
    pci_bus_size_bridges(bus);
    pci_bus_assign_resources(bus);
    
    // 2. 分配设备资源
    pci_assign_unassigned_bus_resources(bus);
}
```

### 5.2 BAR资源分配

```c
int pci_assign_resource(struct pci_dev *dev, int resno)
{
    struct resource *res = &dev->resource[resno];
    struct pci_bus *bus;
    resource_size_t size, align, min;
    int ret;
    
    // 1. 计算所需大小
    size = pci_calc_resource_flags(dev, res);
    
    // 2. 从父总线分配资源
    bus = dev->bus;
    ret = pci_bus_alloc_resource(bus, res, size, align, min, 
                                  type_mask, alignf, alignf_data);
    
    // 3. 写入BAR寄存器
    if (ret == 0)
        pci_update_resource(dev, resno);
    
    return ret;
}
```

---

## 阶段6: 设备驱动匹配和加载

### 6.1 设备添加到系统

**位置：** `drivers/pci/bus.c`

```c
void pci_bus_add_devices(const struct pci_bus *bus)
{
    struct pci_dev *dev;
    struct pci_bus *child;
    
    // 1. 遍历总线上的所有设备
    list_for_each_entry(dev, &bus->devices, bus_list) {
        pci_bus_add_device(dev);
    }
    
    // 2. 递归添加子总线上的设备
    list_for_each_entry(child, &bus->children, node) {
        pci_bus_add_devices(child);
    }
}

void pci_bus_add_device(struct pci_dev *dev)
{
    // 1. 平台特定处理
    pcibios_bus_add_device(dev);
    
    // 2. 设备修复（quirks）
    pci_fixup_device(pci_fixup_final, dev);
    
    // 3. 创建sysfs文件
    pci_create_sysfs_dev_files(dev);
    
    // 4. 创建proc文件
    pci_proc_attach_device(dev);
    
    // 5. 触发驱动匹配 ← 关键！
    dev->match_driver = !dn || of_device_is_available(dn);
    retval = device_attach(&dev->dev);  // ← 进入驱动匹配流程
}
```

### 6.2 驱动匹配流程

**位置：** `drivers/pci/pci-driver.c`

```c
static int pci_device_probe(struct device *dev)
{
    struct pci_dev *pci_dev = to_pci_dev(dev);
    struct pci_driver *drv = to_pci_driver(dev->driver);
    
    // 1. 分配中断
    pci_assign_irq(pci_dev);
    pcibios_alloc_irq(pci_dev);
    
    // 2. 匹配设备ID
    id = pci_match_device(drv, pci_dev);
    if (id) {
        // 3. 调用驱动的probe函数
        error = pci_call_probe(drv, pci_dev, id);
    }
    
    return error;
}

static const struct pci_device_id *pci_match_device(struct pci_driver *drv,
                                                     struct pci_dev *dev)
{
    // 1. 先检查动态ID列表（sysfs添加的）
    list_for_each_entry(dynid, &drv->dynids.list, node) {
        if (pci_match_one_device(&dynid->id, dev))
            return &dynid->id;
    }
    
    // 2. 检查静态ID表
    for (ids = drv->id_table; (found_id = pci_match_id(ids, dev)); ids++) {
        return found_id;
    }
    
    return NULL;  // 不匹配
}
```

---

## 关键数据结构

### 1. tegra_pcie结构

```c
struct tegra_pcie {
    struct device *dev;
    const struct tegra_pcie_soc *soc;
    
    // 寄存器基地址
    void __iomem *pads;  // PADS寄存器
    void __iomem *afi;   // AFI寄存器
    void __iomem *cfg;   // 配置空间
    
    // 中断
    int irq;      // 控制器中断
    int msi_irq;  // MSI中断
    
    // 时钟
    struct clk *pex_clk;
    struct clk *afi_clk;
    struct clk *pll_e;
    struct clk *cml_clk;
    
    // 复位
    struct reset_control *pex_rst;
    struct reset_control *afi_rst;
    struct reset_control *pcie_xrst;
    
    // PCIe端口列表
    struct list_head ports;
    
    // MSI
    struct tegra_msi msi;
    
    // 配置
    u32 xbar_config;
};
```

### 2. pci_host_bridge结构

```c
struct pci_host_bridge {
    struct device dev;
    struct pci_bus *bus;        // 根总线
    struct pci_ops *ops;        // 配置空间访问函数
    struct list_head windows;    // 资源窗口（I/O、Memory）
    void *sysdata;               // 私有数据（tegra_pcie）
    int (*map_irq)(const struct pci_dev *dev, u8 slot, u8 pin);
    // ...
};
```

### 3. pci_ops结构

```c
struct pci_ops {
    void __iomem *(*map_bus)(struct pci_bus *bus, 
                             unsigned int devfn, int where);
    int (*read)(struct pci_bus *bus, unsigned int devfn, 
                int where, int size, u32 *val);
    int (*write)(struct pci_bus *bus, unsigned int devfn, 
                 int where, int size, u32 val);
};
```

---

## 代码流程详解

### 完整调用链

```
系统启动
    ↓
1. 设备树解析
   of_platform_populate()
    ↓
2. Platform驱动匹配
   platform_driver_register(&tegra_pcie_driver)
   → 匹配 "nvidia,tegra210-pcie"
    ↓
3. tegra_pcie_probe()
   ├─ tegra_pcie_parse_dt()          // 解析设备树
   ├─ tegra_pcie_get_resources()     // 获取时钟、复位等
   ├─ tegra_pcie_msi_setup()         // 初始化MSI
   ├─ tegra_pcie_power_on()          // 电源管理
   ├─ tegra_pcie_enable_controller() // 使能控制器
   ├─ host->ops = &tegra_pcie_ops    // 设置配置空间访问函数
   └─ pci_host_probe(host)           // ← 进入PCI核心
       ↓
4. pci_host_probe()
   ├─ pci_scan_root_bus_bridge()
   │   ├─ pci_register_host_bridge()
   │   └─ pci_scan_child_bus()
   │       ├─ pci_scan_slot()        // 扫描每个slot
   │       │   └─ pci_scan_device()  // 扫描设备
   │       │       └─ pci_bus_read_dev_vendor_id()  // 读取Vendor ID
   │       │           └─ tegra_pcie_ops.read()    // 通过配置空间访问
   │       └─ pci_scan_bridge_extend() // 扫描桥接器
   ├─ pci_assign_unassigned_root_bus_resources()  // 分配资源
   └─ pci_bus_add_devices()         // 添加设备
       └─ pci_bus_add_device()
           └─ device_attach()       // 触发驱动匹配
               ↓
5. pci_device_probe()
   ├─ pci_match_device()            // 匹配设备ID
   └─ pci_call_probe()
       └─ drv->probe(dev, id)       // 调用设备驱动probe
```

---

## 总结

ARM64平台上的PCIe驱动流程包括：

1. **设备树配置**：定义PCIe控制器的硬件资源
2. **Platform驱动**：解析设备树，初始化硬件
3. **PCI核心**：扫描总线，发现设备
4. **资源分配**：为设备分配BAR和中断
5. **驱动匹配**：根据Vendor ID和Device ID匹配驱动
6. **设备驱动**：初始化设备功能

整个流程是分层的：
- **底层**：Platform驱动（平台相关，如Tegra）
- **中层**：PCI核心（通用功能）
- **上层**：设备驱动（设备相关）

这种设计实现了硬件抽象和代码复用，使得不同ARM64平台的PCIe控制器可以共享相同的PCI核心代码。

