# 在配置 BAR 之后修改 ASPM

## 最简单的方法

如果你已经配置了 BAR，并且有配置空间地址，只需要 3 步：

```c
void pcie_modify_aspm(uintptr_t cfg_base)
{
    uint8_t pos;
    uint16_t link_control;
    
    // 1. 查找 PCIe Capability
    pos = pcie_find_capability(cfg_base);
    if (pos == 0)
        return;
    
    // 2. 读取并修改 Link Control 寄存器
    link_control = pcie_read16(cfg_base, pos + PCI_EXP_LNKCTL);
    link_control &= ~0x0003;  // 清除 ASPM 控制位
    link_control |= 0x0003;   // 设置 L0s + L1
    
    // 3. 写回
    pcie_write16(cfg_base, pos + PCI_EXP_LNKCTL, link_control);
}
```

## 使用场景

### 场景1: 有配置空间基址

```c
// 假设你已经有了配置空间基址
uintptr_t cfg_base = 0x40000000;  // 你的配置空间地址

// 直接修改 ASPM
pcie_modify_aspm(cfg_base);
```

### 场景2: 通过 Bus/Dev/Func 计算

```c
// 如果你知道设备的 Bus/Dev/Func
uint8_t bus = 1;
uint8_t dev = 0;
uint8_t func = 0;

// 计算配置空间地址
uintptr_t cfg_base = PCIE_CFG_BASE + 
                     ((bus << 20) | (dev << 15) | (func << 12));

// 修改 ASPM
pcie_modify_aspm(cfg_base);
```

### 场景3: 在设备初始化函数中

```c
void pcie_device_init(uintptr_t cfg_base)
{
    // 1. 配置 BAR
    // ... BAR 配置代码 ...
    
    // 2. 配置完成后，修改 ASPM
    pcie_modify_aspm(cfg_base);
}
```

### 场景4: 指定 ASPM 模式

```c
// 设置特定的 ASPM 模式
void pcie_set_aspm_mode(uintptr_t cfg_base, uint8_t aspm_mode)
{
    uint8_t pos = pcie_find_capability(cfg_base);
    if (pos == 0)
        return;
    
    uint16_t link_control = pcie_read16(cfg_base, pos + PCI_EXP_LNKCTL);
    link_control &= ~0x0003;           // 清除旧设置
    link_control |= (aspm_mode & 0x3); // 设置新模式
    pcie_write16(cfg_base, pos + PCI_EXP_LNKCTL, link_control);
}

// 使用
pcie_set_aspm_mode(cfg_base, 0x03);  // L0s + L1
pcie_set_aspm_mode(cfg_base, 0x01);  // 仅 L0s
pcie_set_aspm_mode(cfg_base, 0x00);  // 禁用
```

## 重要说明

### BAR 和配置空间的区别

- **BAR (Base Address Register)**: 设备的内存/IO 空间，用于访问设备的功能寄存器
- **配置空间**: 设备的配置寄存器，用于配置设备（包括 ASPM）

**ASPM 寄存器在配置空间中，不在 BAR 中！**

```
设备地址空间:
├─> 配置空间 (0x00 - 0xFFF)
│   ├─> Vendor ID, Device ID
│   ├─> BAR0 - BAR5
│   ├─> PCIe Capability
│   │   └─> Link Control (ASPM 在这里) ← 需要访问这里
│   └─> ...
│
└─> BAR 空间 (通过 BAR 映射)
    └─> 设备功能寄存器
```

### 如何获取配置空间地址

1. **如果通过 Bus/Dev/Func 访问**：
   ```c
   uintptr_t cfg_base = PCIE_CFG_BASE + 
                        ((bus << 20) | (dev << 15) | (func << 12));
   ```

2. **如果平台提供了配置空间访问接口**：
   ```c
   // 使用平台提供的函数
   uintptr_t cfg_base = pcie_get_cfg_base(bus, dev, func);
   ```

3. **如果通过 PCIe 控制器访问**：
   ```c
   // 某些平台可能需要通过 PCIe 控制器的配置空间访问接口
   uintptr_t cfg_base = pcie_controller_get_cfg_addr(bus, dev, func);
   ```

## 完整示例

```c
void pcie_device_setup_example(void)
{
    uint8_t bus = 1;
    uint8_t dev = 0;
    uint8_t func = 0;
    
    // 1. 获取配置空间地址
    uintptr_t cfg_base = PCIE_CFG_BASE + 
                         ((bus << 20) | (dev << 15) | (func << 12));
    
    // 2. 配置 BAR（你的现有代码）
    // ... 配置 BAR 的代码 ...
    
    // 3. 配置完成后，修改 ASPM
    pcie_modify_aspm(cfg_base);
    
    // 或者指定模式
    // pcie_set_aspm_mode(cfg_base, 0x03);  // L0s + L1
}
```

## ASPM 模式值

| 值 | 模式 | 说明 |
|---|---|---|
| 0x00 | Disabled | 禁用 ASPM |
| 0x01 | L0s | 仅使能 L0s |
| 0x02 | L1 | 仅使能 L1 |
| 0x03 | L0s + L1 | 使能 L0s 和 L1（推荐） |

## 注意事项

1. **配置空间 vs BAR**: ASPM 在配置空间中，不在 BAR 中
2. **链路训练**: 确保链路训练已完成（通常已经完成）
3. **配置空间地址**: 需要知道如何访问配置空间（Bus/Dev/Func 或直接地址）


