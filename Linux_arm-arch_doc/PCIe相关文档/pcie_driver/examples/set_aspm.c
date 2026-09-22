/*
 * PCIe ASPM 设置示例
 * 
 * 演示如何在链路训练完成后设置 ASPM（Active State Power Management）
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>

#define PCI_CAP_ID_EXP    0x10    // PCIe Capability ID
#define PCI_EXP_LNKSTA    0x12    // Link Status 寄存器偏移
#define PCI_EXP_LNKCTL    0x10    // Link Control 寄存器偏移
#define PCI_EXP_LNKCAP    0x0C    // Link Capabilities 寄存器偏移

// ASPM 控制位定义
#define PCI_EXP_LNKCTL_ASPMC     0x0003  // ASPM Control 位 (bit 0-1)
#define PCI_EXP_LNKCTL_ASPM_L0s  0x0001  // L0s 使能
#define PCI_EXP_LNKCTL_ASPM_L1   0x0002  // L1 使能
#define PCI_EXP_LNKCTL_ASPM_L0s_L1 0x0003  // L0s + L1 使能

// Link Status 位定义
#define PCI_EXP_LNKSTA_DLLLA     0x2000  // Data Link Layer Link Active
#define PCI_EXP_LNKSTA_LT         0x0800  // Link Training

/**
 * 检查链路训练是否完成
 */
static int check_link_training_complete(struct pci_dev *pdev)
{
    u16 link_status;
    int pos;
    
    // 查找 PCIe Capability
    pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (!pos) {
        pr_err("PCIe Capability not found\n");
        return -ENODEV;
    }
    
    // 读取 Link Status 寄存器
    pci_read_config_word(pdev, pos + PCI_EXP_LNKSTA, &link_status);
    
    pr_info("Link Status: 0x%04X\n", link_status);
    pr_info("  - DLLLA (Link Active): %s\n", 
            (link_status & PCI_EXP_LNKSTA_DLLLA) ? "Yes" : "No");
    pr_info("  - LT (Link Training): %s\n", 
            (link_status & PCI_EXP_LNKSTA_LT) ? "In Progress" : "Complete");
    
    // 检查链路是否激活且训练完成
    if ((link_status & PCI_EXP_LNKSTA_DLLLA) && 
        !(link_status & PCI_EXP_LNKSTA_LT)) {
        pr_info("Link training complete!\n");
        return 0;
    }
    
    pr_warn("Link training not complete yet\n");
    return -EAGAIN;
}

/**
 * 读取 ASPM 支持能力
 */
static u8 read_aspm_support(struct pci_dev *pdev, int pos)
{
    u32 link_cap;
    
    // 读取 Link Capabilities 寄存器
    pci_read_config_dword(pdev, pos + PCI_EXP_LNKCAP, &link_cap);
    
    // 提取 ASPM Support 位 (bit 10-11)
    u8 aspm_support = (link_cap >> 10) & 0x3;
    
    pr_info("ASPM Support: ");
    switch (aspm_support) {
        case 0:
            pr_cont("None\n");
            break;
        case 1:
            pr_cont("L0s\n");
            break;
        case 2:
            pr_cont("L1\n");
            break;
        case 3:
            pr_cont("L0s + L1\n");
            break;
    }
    
    return aspm_support;
}

/**
 * 设置 ASPM
 * @param pdev: PCIe 设备
 * @param aspm_mode: ASPM 模式
 *                   0 = Disabled
 *                   1 = L0s Enabled
 *                   2 = L1 Enabled
 *                   3 = L0s + L1 Enabled
 */
static int set_aspm(struct pci_dev *pdev, u8 aspm_mode)
{
    u16 link_control;
    int pos;
    
    // 查找 PCIe Capability
    pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (!pos) {
        pr_err("PCIe Capability not found\n");
        return -ENODEV;
    }
    
    // 1. 检查链路训练是否完成
    if (check_link_training_complete(pdev) != 0) {
        pr_err("Link training not complete, cannot set ASPM\n");
        return -EAGAIN;
    }
    
    // 2. 读取 ASPM 支持能力
    u8 aspm_support = read_aspm_support(pdev, pos);
    if (aspm_support == 0) {
        pr_warn("Device does not support ASPM\n");
        return -ENOTSUPP;
    }
    
    // 3. 检查请求的 ASPM 模式是否被支持
    if (aspm_mode == 1 && !(aspm_support & 0x1)) {
        pr_err("Device does not support L0s\n");
        return -ENOTSUPP;
    }
    if (aspm_mode == 2 && !(aspm_support & 0x2)) {
        pr_err("Device does not support L1\n");
        return -ENOTSUPP;
    }
    
    // 4. 读取当前的 Link Control 寄存器
    pci_read_config_word(pdev, pos + PCI_EXP_LNKCTL, &link_control);
    pr_info("Current Link Control: 0x%04X\n", link_control);
    pr_info("Current ASPM Control: 0x%02X\n", link_control & PCI_EXP_LNKCTL_ASPMC);
    
    // 5. 清除旧的 ASPM 设置
    link_control &= ~PCI_EXP_LNKCTL_ASPMC;
    
    // 6. 设置新的 ASPM 模式
    link_control |= (aspm_mode & PCI_EXP_LNKCTL_ASPMC);
    
    // 7. 写入 Link Control 寄存器
    pci_write_config_word(pdev, pos + PCI_EXP_LNKCTL, link_control);
    
    // 8. 验证设置
    pci_read_config_word(pdev, pos + PCI_EXP_LNKCTL, &link_control);
    u8 new_aspm = link_control & PCI_EXP_LNKCTL_ASPMC;
    
    pr_info("New Link Control: 0x%04X\n", link_control);
    pr_info("New ASPM Control: ");
    switch (new_aspm) {
        case 0:
            pr_cont("Disabled\n");
            break;
        case 1:
            pr_cont("L0s Enabled\n");
            break;
        case 2:
            pr_cont("L1 Enabled\n");
            break;
        case 3:
            pr_cont("L0s + L1 Enabled\n");
            break;
    }
    
    return 0;
}

/**
 * 示例：在 probe 函数中设置 ASPM
 */
static int my_pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int ret;
    
    pr_info("=== PCIe Device Probe ===\n");
    
    // 1. 使能设备
    ret = pci_enable_device(pdev);
    if (ret) {
        pr_err("Failed to enable device\n");
        return ret;
    }
    
    // 2. 等待链路训练完成（通常已经完成，但可以检查）
    msleep(100);  // 等待 100ms 确保训练完成
    
    // 3. 检查链路训练状态
    if (check_link_training_complete(pdev) != 0) {
        pr_warn("Link training may not be complete, retrying...\n");
        msleep(200);
        if (check_link_training_complete(pdev) != 0) {
            pr_err("Link training failed\n");
            return -EAGAIN;
        }
    }
    
    // 4. 设置 ASPM（例如：使能 L0s + L1）
    ret = set_aspm(pdev, PCI_EXP_LNKCTL_ASPM_L0s_L1);
    if (ret) {
        pr_warn("Failed to set ASPM: %d\n", ret);
        // 继续执行，ASPM 设置失败不是致命错误
    }
    
    pr_info("=== Device Initialized ===\n");
    
    return 0;
}

/**
 * 示例：简单的使用函数
 */
void example_set_aspm_simple(struct pci_dev *pdev)
{
    int pos;
    u16 link_control;
    
    // 1. 查找 PCIe Capability
    pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (!pos)
        return;
    
    // 2. 读取 Link Control
    pci_read_config_word(pdev, pos + PCI_EXP_LNKCTL, &link_control);
    
    // 3. 设置 ASPM 为 L0s + L1
    link_control &= ~PCI_EXP_LNKCTL_ASPMC;  // 清除旧设置
    link_control |= PCI_EXP_LNKCTL_ASPM_L0s_L1;  // 设置新值
    
    // 4. 写回
    pci_write_config_word(pdev, pos + PCI_EXP_LNKCTL, link_control);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PCIe ASPM Setting Example");


