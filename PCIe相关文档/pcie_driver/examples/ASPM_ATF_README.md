# PCIe ASPM 设置 - ATF 环境

## 快速使用（最简单版本）

```c
void pcie_set_aspm_simple(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint8_t pos;
    uint16_t link_control;
    
    // 1. 查找 PCIe Capability
    pos = pcie_find_capability(bus, dev, func, PCI_CAP_ID_EXP);
    if (pos == 0)
        return;
    
    // 2. 读取并修改 Link Control 寄存器
    link_control = pcie_read16(bus, dev, func, pos + PCI_EXP_LNKCTL);
    link_control &= ~0x0003;  // 清除 ASPM 控制位 (bit 0-1)
    link_control |= 0x0003;   // 设置 L0s + L1
    
    // 3. 写回
    pcie_write16(bus, dev, func, pos + PCI_EXP_LNKCTL, link_control);
}
```

## 使用示例

```c
// 在 ATF 初始化函数中调用
void pcie_init(void)
{
    uint8_t bus = 1;   // PCIe 总线号
    uint8_t dev = 0;   // 设备号
    uint8_t func = 0;  // 功能号
    
    // 等待链路训练完成（如果需要）
    // mdelay(100);
    
    // 设置 ASPM
    pcie_set_aspm_simple(bus, dev, func);
}
```

## 关键点

### 1. PCIe 配置空间访问

ATF 中使用 `mmio_read_*` 和 `mmio_write_*` 函数访问配置空间：

```c
// 读取配置空间
uint16_t value = mmio_read_16(cfg_addr);

// 写入配置空间
mmio_write_16(cfg_addr, value);
```

### 2. 配置空间地址计算

```c
// 根据 Bus/Dev/Func 计算配置空间地址
uintptr_t cfg_addr = PCIE_CFG_BASE + 
                     ((bus << 20) | (dev << 15) | (func << 12) | offset);
```

### 3. 查找 PCIe Capability

```c
// 从 Capability Pointer (0x34) 开始遍历链表
uint8_t pos = pcie_read8(bus, dev, func, 0x34);
while (pos != 0 && pos < 0xFC) {
    if (pcie_read8(bus, dev, func, pos) == PCI_CAP_ID_EXP)
        return pos;  // 找到 PCIe Capability
    pos = pcie_read8(bus, dev, func, pos + 1);
}
```

## ASPM 模式

| 值 | 模式 | 说明 |
|---|---|---|
| 0x00 | Disabled | 禁用 ASPM |
| 0x01 | L0s | 仅使能 L0s |
| 0x02 | L1 | 仅使能 L1 |
| 0x03 | L0s + L1 | 使能 L0s 和 L1（推荐） |

## 寄存器说明

- **PCI_EXP_LNKCTL (0x10)**: Link Control 寄存器
  - Bit 0-1: ASPM Control
  - 0 = Disabled, 1 = L0s, 2 = L1, 3 = L0s + L1

- **PCI_EXP_LNKCAP (0x0C)**: Link Capabilities 寄存器
  - Bit 10-11: ASPM Support（只读，表示设备支持的能力）

- **PCI_EXP_LNKSTA (0x12)**: Link Status 寄存器
  - Bit 11: Link Training (LT)
  - Bit 13: Data Link Layer Link Active (DLLLA)

## 注意事项

1. **配置空间基址**：需要根据实际平台修改 `PCIE_CFG_BASE`
2. **链路训练**：确保在设置 ASPM 之前链路训练已完成
3. **总线号**：需要知道目标设备的 Bus/Dev/Func
4. **头文件**：需要包含 ATF 的头文件（`lib/mmio.h` 等）

## 完整版本 vs 简化版本

- **`set_aspm_atf.c`**: 完整版本，包含链路训练检查、ASPM 支持检查等
- **`set_aspm_atf_simple.c`**: 简化版本，直接设置，适合快速使用

## 平台相关配置

根据不同的 ARM 平台，可能需要：

1. **修改配置空间基址**：
   ```c
   #define PCIE_CFG_BASE    0x40000000  // 根据平台修改
   ```

2. **使用平台特定的配置空间访问函数**：
   ```c
   // 某些平台可能有封装好的函数
   pcie_read_config16(bus, dev, func, offset);
   ```

3. **添加平台初始化**：
   ```c
   // 某些平台需要先初始化 PCIe 控制器
   pcie_controller_init();
   ```

