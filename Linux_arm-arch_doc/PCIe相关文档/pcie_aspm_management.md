# PCIe ASPM 管理说明

## 核心结论

**大部分情况下，ASPM 不需要 host controller 驱动软件管理**，PCI 核心会自动处理。但某些平台需要在硬件层面使能 ASPM。

## ASPM 自动管理流程

### 1. PCI 核心自动初始化 ASPM

```c
// drivers/pci/probe.c
pci_host_probe()
    ↓
pci_scan_root_bus_bridge()  // 扫描 PCIe 总线
    ↓
pci_bus_add_devices()       // 添加设备
    ↓
pcie_aspm_init_link_state() // ← PCI 核心自动调用！
```

**关键代码：**
```c
// drivers/pci/probe.c:2739
pcie_aspm_init_link_state(bus->self);
```

### 2. ASPM 初始化时机

- **自动调用**：在 PCIe 设备扫描完成后
- **调用位置**：`pci_scan_slot()` 和 `pci_bus_add_devices()` 中
- **无需手动调用**：host controller 驱动不需要手动调用

### 3. ASPM 配置流程

```c
// drivers/pci/pcie/aspm.c
void pcie_aspm_init_link_state(struct pci_dev *pdev)
{
    // 1. 检查是否支持 ASPM
    if (!aspm_support_enabled)
        return;
    
    // 2. 检查设备能力
    pcie_aspm_cap_init(link, blacklist);
    
    // 3. 配置 ASPM 状态
    pcie_config_aspm_path(link);
}
```

## 需要软件管理的情况

### 情况 1：硬件层面需要使能 ASPM

某些平台（如 MediaTek MT7622）需要在硬件寄存器中使能 ASPM：

```c
// drivers/pci/controller/pcie-mediatek.c:674-684
/* MT7622 platforms need to enable LTSSM and ASPM from PCIe subsys */
if (pcie->base) {
    val = readl(pcie->base + PCIE_SYS_CFG_V2);
    val |= PCIE_CSR_LTSSM_EN(port->slot) |
           PCIE_CSR_ASPM_L1_EN(port->slot);  // ← 硬件使能 ASPM
    writel(val, pcie->base + PCIE_SYS_CFG_V2);
}
```

**原因**：硬件设计需要在系统级寄存器中使能 ASPM，而不仅仅是 PCIe 配置空间。

### 情况 2：配置 ASPM 支持能力

某些平台（如 Broadcom）可以在设备树中配置 ASPM 支持：

```c
// drivers/pci/controller/pcie-brcmstb.c:1132-1138
/* Don't advertise L0s capability if 'aspm-no-l0s' */
aspm_support = PCIE_LINK_STATE_L1;
if (!of_property_read_bool(pcie->np, "aspm-no-l0s"))
    aspm_support |= PCIE_LINK_STATE_L0S;

u32p_replace_bits(&tmp, aspm_support,
    PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_ASPM_SUPPORT_MASK);
```

**设备树示例：**
```dts
pcie@f0000000 {
    compatible = "brcm,bcm2711-pcie";
    aspm-no-l0s;  /* 禁用 L0s，只支持 L1 */
    ...
};
```

### 情况 3：禁用 ASPM

如果硬件有问题，可以全局禁用 ASPM：

```c
// 通过内核参数
pcie_aspm=off

// 或在代码中
pcie_no_aspm();  // 全局禁用
```

## 标准 Host Controller 驱动实现

### 不需要特殊处理的情况（大多数）

```c
static int my_pcie_probe(struct platform_device *pdev)
{
    // ... 初始化硬件 ...
    
    // 设置 host bridge
    bridge->sysdata = pcie;
    bridge->ops = &my_pcie_ops;
    
    // 启动 PCIe 枚举（ASPM 会自动初始化）
    pci_host_probe(bridge);  // ← ASPM 在这里自动处理
    
    return 0;
}
```

**不需要做：**
- ❌ 手动调用 `pcie_aspm_init_link_state()`
- ❌ 手动配置 ASPM 寄存器（除非硬件要求）
- ❌ 手动设置 PCIe 配置空间的 ASPM 位

### 需要特殊处理的情况

```c
static int my_pcie_probe(struct platform_device *pdev)
{
    // ... 初始化硬件 ...
    
    // 情况 1：硬件层面使能 ASPM（如 MediaTek）
    if (needs_hw_aspm_enable) {
        val = readl(pcie->base + PCIE_SYS_CFG);
        val |= PCIE_ASPM_ENABLE;
        writel(val, pcie->base + PCIE_SYS_CFG);
    }
    
    // 情况 2：配置 ASPM 支持能力（如 Broadcom）
    if (of_property_read_bool(np, "aspm-no-l0s")) {
        // 修改 Link Capability 寄存器，移除 L0s 支持
        configure_aspm_capability(pcie, PCIE_LINK_STATE_L1);
    }
    
    // 启动 PCIe 枚举
    pci_host_probe(bridge);
    
    return 0;
}
```

## ASPM 状态管理

### ASPM 状态类型

```c
// drivers/pci/pcie/aspm.c
#define PCIE_LINK_STATE_L0S     0x01  // L0s 状态
#define PCIE_LINK_STATE_L1      0x02  // L1 状态
#define PCIE_LINK_STATE_L1_1    0x04  // L1.1 子状态
#define PCIE_LINK_STATE_L1_2    0x08  // L1.2 子状态
```

### ASPM 策略

通过内核配置或参数控制：

```bash
# 内核配置
CONFIG_PCIEASPM_DEFAULT=y      # BIOS 默认
CONFIG_PCIEASPM_PERFORMANCE=y   # 高性能（禁用 ASPM）
CONFIG_PCIEASPM_POWERSAVE=y     # 省电模式（启用 ASPM）

# 内核参数
pcie_aspm=off          # 禁用 ASPM
pcie_aspm=performance  # 性能模式
pcie_aspm=powersave    # 省电模式
```

## 检查 ASPM 状态

### 用户空间检查

```bash
# 查看 ASPM 状态
cat /sys/module/pcie_aspm/parameters/policy

# 查看设备的 ASPM 能力
lspci -vvv | grep -i aspm

# 查看 Link Control 寄存器
setpci -s 00:00.0 CAP_EXP+0x10.L
```

### 代码中检查

```c
// 检查设备是否支持 ASPM
u16 lnkcap;
pcie_capability_read_word(dev, PCI_EXP_LNKCAP, &lnkcap);
if (lnkcap & PCI_EXP_LNKCAP_ASPM) {
    // 支持 ASPM
}

// 检查当前 ASPM 状态
u16 lnkctl;
pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &lnkctl);
u8 aspm_state = (lnkctl & PCI_EXP_LNKCTL_ASPMC) >> 10;
```

## 总结

| 情况 | 是否需要软件管理 | 说明 |
|------|----------------|------|
| **标准平台** | ❌ **不需要** | PCI 核心自动管理 |
| **MediaTek MT7622** | ✅ **需要** | 硬件寄存器使能 |
| **Broadcom** | ✅ **可选** | 配置 ASPM 支持能力 |
| **有问题的硬件** | ✅ **需要** | 禁用 ASPM |

## 建议

1. **默认情况**：不需要在 host controller 驱动中处理 ASPM
2. **特殊平台**：查看硬件文档，确认是否需要硬件层面使能
3. **调试**：使用 `lspci -vvv` 检查 ASPM 状态
4. **问题排查**：如果遇到电源管理问题，尝试 `pcie_aspm=off`

## 相关文件

- `drivers/pci/pcie/aspm.c` - ASPM 核心实现
- `drivers/pci/probe.c` - PCIe 设备扫描和 ASPM 初始化
- `drivers/pci/controller/pcie-mediatek.c` - MediaTek 平台示例
- `drivers/pci/controller/pcie-brcmstb.c` - Broadcom 平台示例

