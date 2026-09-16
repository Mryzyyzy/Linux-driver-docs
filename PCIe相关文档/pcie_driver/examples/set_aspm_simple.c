/*
 * PCIe ASPM 设置 - 简化版
 * 
 * 最简单的代码示例：链路训练完成后设置 ASPM
 */

#include <linux/pci.h>

/**
 * 设置 ASPM - 最简版本
 */
void set_aspm_simple(struct pci_dev *pdev)
{
    int pos;
    u16 link_control;
    
    // 1. 查找 PCIe Capability
    pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (!pos)
        return;
    
    // 2. 读取 Link Control 寄存器
    pci_read_config_word(pdev, pos + PCI_EXP_LNKCTL, &link_control);
    
    // 3. 设置 ASPM 为 L0s + L1（清除旧值，设置新值）
    link_control &= ~0x0003;  // 清除 bit 0-1 (ASPM Control)
    link_control |= 0x0003;   // 设置 L0s + L1
    
    // 4. 写回寄存器
    pci_write_config_word(pdev, pos + PCI_EXP_LNKCTL, link_control);
}

/**
 * 在 probe 函数中使用
 */
static int my_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    // ... 其他初始化代码 ...
    
    // 使能设备
    pci_enable_device(pdev);
    
    // 等待链路训练完成（通常已经完成）
    msleep(100);
    
    // 设置 ASPM
    set_aspm_simple(pdev);
    
    return 0;
}


