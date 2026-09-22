# PCIe ASPM 设置代码示例

## 快速使用

### 最简单版本（3 步）

```c
void set_aspm_simple(struct pci_dev *pdev)
{
    int pos;
    u16 link_control;
    
    // 1. 查找 PCIe Capability
    pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (!pos)
        return;
    
    // 2. 读取并修改 Link Control 寄存器
    pci_read_config_word(pdev, pos + PCI_EXP_LNKCTL, &link_control);
    link_control &= ~0x0003;  // 清除 ASPM 控制位 (bit 0-1)
    link_control |= 0x0003;   // 设置 L0s + L1
    
    // 3. 写回
    pci_write_config_word(pdev, pos + PCI_EXP_LNKCTL, link_control);
}
```

### 在 probe 函数中使用

```c
static int my_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    // 1. 使能设备
    pci_enable_device(pdev);
    
    // 2. 等待链路训练完成（通常已经完成，但保险起见等待一下）
    msleep(100);
    
    // 3. 设置 ASPM
    set_aspm_simple(pdev);
    
    return 0;
}
```

## ASPM 模式说明

| 值 | 模式 | 说明 |
|---|---|---|
| 0x00 | Disabled | 禁用 ASPM |
| 0x01 | L0s | 仅使能 L0s |
| 0x02 | L1 | 仅使能 L1 |
| 0x03 | L0s + L1 | 使能 L0s 和 L1 |

## 完整版本（带检查）

完整版本包含：
- 检查链路训练是否完成
- 检查设备是否支持 ASPM
- 验证设置结果

详见 `set_aspm.c`

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

1. **链路训练必须完成**：在设置 ASPM 之前，确保链路训练已完成
2. **检查支持能力**：不是所有设备都支持所有 ASPM 模式
3. **两端都要支持**：Root Complex 和设备都必须支持 ASPM 才能生效
4. **系统策略**：某些系统可能通过内核参数禁用 ASPM（如 `pcie_aspm=off`）


