/*
 * PCIe ASPM 设置 - ATF (ARM Trusted Firmware) 环境
 * 
 * 在 ATF 环境下设置 PCIe ASPM 的代码示例
 */

#include <stdint.h>
#include <stdbool.h>
#include <common/debug.h>
#include <lib/mmio.h>

/* PCIe 配置空间访问基址（根据平台定义） */
#define PCIE_CFG_BASE    0x40000000  // 示例：根据实际平台修改

/* PCIe Capability ID */
#define PCI_CAP_ID_EXP   0x10

/* PCIe Capability 寄存器偏移 */
#define PCI_EXP_LNKSTA   0x12    // Link Status
#define PCI_EXP_LNKCTL   0x10    // Link Control
#define PCI_EXP_LNKCAP   0x0C    // Link Capabilities

/* ASPM 控制位 */
#define PCI_EXP_LNKCTL_ASPMC     0x0003
#define PCI_EXP_LNKCTL_ASPM_L0s  0x0001
#define PCI_EXP_LNKCTL_ASPM_L1   0x0002
#define PCI_EXP_LNKCTL_ASPM_L0s_L1 0x0003

/* Link Status 位 */
#define PCI_EXP_LNKSTA_DLLLA     0x2000  // Data Link Layer Link Active
#define PCI_EXP_LNKSTA_LT        0x0800  // Link Training

/**
 * 计算 PCIe 配置空间地址
 * @param bus: 总线号
 * @param dev: 设备号
 * @param func: 功能号
 * @param offset: 配置空间偏移
 * @return: 配置空间地址
 */
static uintptr_t pcie_cfg_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    return PCIE_CFG_BASE + ((bus << 20) | (dev << 15) | (func << 12) | offset);
}

/**
 * 读取 PCIe 配置空间（8位）
 */
static uint8_t pcie_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uintptr_t addr = pcie_cfg_addr(bus, dev, func, offset);
    return mmio_read_8(addr);
}

/**
 * 读取 PCIe 配置空间（16位）
 */
static uint16_t pcie_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uintptr_t addr = pcie_cfg_addr(bus, dev, func, offset);
    return mmio_read_16(addr);
}

/**
 * 读取 PCIe 配置空间（32位）
 */
static uint32_t pcie_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uintptr_t addr = pcie_cfg_addr(bus, dev, func, offset);
    return mmio_read_32(addr);
}

/**
 * 写入 PCIe 配置空间（8位）
 */
static void pcie_write8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint8_t value)
{
    uintptr_t addr = pcie_cfg_addr(bus, dev, func, offset);
    mmio_write_8(addr, value);
}

/**
 * 写入 PCIe 配置空间（16位）
 */
static void pcie_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t value)
{
    uintptr_t addr = pcie_cfg_addr(bus, dev, func, offset);
    mmio_write_16(addr, value);
}

/**
 * 写入 PCIe 配置空间（32位）
 */
static void pcie_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value)
{
    uintptr_t addr = pcie_cfg_addr(bus, dev, func, offset);
    mmio_write_32(addr, value);
}

/**
 * 查找 PCIe Capability
 * @param bus: 总线号
 * @param dev: 设备号
 * @param func: 功能号
 * @param cap_id: Capability ID
 * @return: Capability 偏移地址，0 表示未找到
 */
static uint8_t pcie_find_capability(uint8_t bus, uint8_t dev, uint8_t func, uint8_t cap_id)
{
    uint8_t pos;
    uint8_t id;
    
    // 读取 Capability Pointer (0x34)
    pos = pcie_read8(bus, dev, func, 0x34);
    if (pos == 0 || pos == 0xFF)
        return 0;
    
    // 遍历 Capability 链表
    while (pos != 0 && pos < 0xFC) {
        id = pcie_read8(bus, dev, func, pos);
        if (id == cap_id)
            return pos;
        
        pos = pcie_read8(bus, dev, func, pos + 1);
    }
    
    return 0;
}

/**
 * 检查链路训练是否完成
 */
static bool check_link_training_complete(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint8_t pos;
    uint16_t link_status;
    
    // 查找 PCIe Capability
    pos = pcie_find_capability(bus, dev, func, PCI_CAP_ID_EXP);
    if (pos == 0) {
        ERROR("PCIe Capability not found\n");
        return false;
    }
    
    // 读取 Link Status 寄存器
    link_status = pcie_read16(bus, dev, func, pos + PCI_EXP_LNKSTA);
    
    INFO("Link Status: 0x%04X\n", link_status);
    INFO("  - DLLLA (Link Active): %s\n", 
         (link_status & PCI_EXP_LNKSTA_DLLLA) ? "Yes" : "No");
    INFO("  - LT (Link Training): %s\n", 
         (link_status & PCI_EXP_LNKSTA_LT) ? "In Progress" : "Complete");
    
    // 检查链路是否激活且训练完成
    if ((link_status & PCI_EXP_LNKSTA_DLLLA) && 
        !(link_status & PCI_EXP_LNKSTA_LT)) {
        INFO("Link training complete!\n");
        return true;
    }
    
    WARN("Link training not complete yet\n");
    return false;
}

/**
 * 读取 ASPM 支持能力
 */
static uint8_t read_aspm_support(uint8_t bus, uint8_t dev, uint8_t func, uint8_t pos)
{
    uint32_t link_cap;
    
    // 读取 Link Capabilities 寄存器
    link_cap = pcie_read32(bus, dev, func, pos + PCI_EXP_LNKCAP);
    
    // 提取 ASPM Support 位 (bit 10-11)
    uint8_t aspm_support = (link_cap >> 10) & 0x3;
    
    INFO("ASPM Support: ");
    switch (aspm_support) {
        case 0:
            INFO("None\n");
            break;
        case 1:
            INFO("L0s\n");
            break;
        case 2:
            INFO("L1\n");
            break;
        case 3:
            INFO("L0s + L1\n");
            break;
    }
    
    return aspm_support;
}

/**
 * 设置 ASPM
 * @param bus: 总线号
 * @param dev: 设备号
 * @param func: 功能号
 * @param aspm_mode: ASPM 模式 (0=Disabled, 1=L0s, 2=L1, 3=L0s+L1)
 * @return: 0 成功，-1 失败
 */
int pcie_set_aspm(uint8_t bus, uint8_t dev, uint8_t func, uint8_t aspm_mode)
{
    uint8_t pos;
    uint16_t link_control;
    uint8_t aspm_support;
    
    // 1. 查找 PCIe Capability
    pos = pcie_find_capability(bus, dev, func, PCI_CAP_ID_EXP);
    if (pos == 0) {
        ERROR("PCIe Capability not found\n");
        return -1;
    }
    
    // 2. 检查链路训练是否完成
    if (!check_link_training_complete(bus, dev, func)) {
        ERROR("Link training not complete, cannot set ASPM\n");
        return -1;
    }
    
    // 3. 读取 ASPM 支持能力
    aspm_support = read_aspm_support(bus, dev, func, pos);
    if (aspm_support == 0) {
        WARN("Device does not support ASPM\n");
        return -1;
    }
    
    // 4. 检查请求的 ASPM 模式是否被支持
    if (aspm_mode == 1 && !(aspm_support & 0x1)) {
        ERROR("Device does not support L0s\n");
        return -1;
    }
    if (aspm_mode == 2 && !(aspm_support & 0x2)) {
        ERROR("Device does not support L1\n");
        return -1;
    }
    
    // 5. 读取当前的 Link Control 寄存器
    link_control = pcie_read16(bus, dev, func, pos + PCI_EXP_LNKCTL);
    INFO("Current Link Control: 0x%04X\n", link_control);
    INFO("Current ASPM Control: 0x%02X\n", link_control & PCI_EXP_LNKCTL_ASPMC);
    
    // 6. 清除旧的 ASPM 设置
    link_control &= ~PCI_EXP_LNKCTL_ASPMC;
    
    // 7. 设置新的 ASPM 模式
    link_control |= (aspm_mode & PCI_EXP_LNKCTL_ASPMC);
    
    // 8. 写入 Link Control 寄存器
    pcie_write16(bus, dev, func, pos + PCI_EXP_LNKCTL, link_control);
    
    // 9. 验证设置
    link_control = pcie_read16(bus, dev, func, pos + PCI_EXP_LNKCTL);
    uint8_t new_aspm = link_control & PCI_EXP_LNKCTL_ASPMC;
    
    INFO("New Link Control: 0x%04X\n", link_control);
    INFO("New ASPM Control: ");
    switch (new_aspm) {
        case 0:
            INFO("Disabled\n");
            break;
        case 1:
            INFO("L0s Enabled\n");
            break;
        case 2:
            INFO("L1 Enabled\n");
            break;
        case 3:
            INFO("L0s + L1 Enabled\n");
            break;
    }
    
    return 0;
}

/**
 * 简化版本：直接设置 ASPM（不检查）
 */
void pcie_set_aspm_simple(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint8_t pos;
    uint16_t link_control;
    
    // 1. 查找 PCIe Capability
    pos = pcie_find_capability(bus, dev, func, PCI_CAP_ID_EXP);
    if (pos == 0)
        return;
    
    // 2. 读取 Link Control 寄存器
    link_control = pcie_read16(bus, dev, func, pos + PCI_EXP_LNKCTL);
    
    // 3. 设置 ASPM 为 L0s + L1
    link_control &= ~PCI_EXP_LNKCTL_ASPMC;  // 清除旧设置
    link_control |= PCI_EXP_LNKCTL_ASPM_L0s_L1;  // 设置新值
    
    // 4. 写回
    pcie_write16(bus, dev, func, pos + PCI_EXP_LNKCTL, link_control);
}

/**
 * 使用示例：在 ATF 初始化函数中调用
 */
void pcie_init_aspm_example(void)
{
    uint8_t bus = 1;   // PCIe 总线号
    uint8_t dev = 0;   // 设备号
    uint8_t func = 0;  // 功能号
    
    // 等待链路训练完成（通常已经完成）
    // 如果需要等待，可以使用延时函数
    // mdelay(100);
    
    // 方法1: 使用完整版本（带检查）
    if (pcie_set_aspm(bus, dev, func, PCI_EXP_LNKCTL_ASPM_L0s_L1) != 0) {
        WARN("Failed to set ASPM\n");
    }
    
    // 方法2: 使用简化版本（不检查，直接设置）
    // pcie_set_aspm_simple(bus, dev, func);
}


