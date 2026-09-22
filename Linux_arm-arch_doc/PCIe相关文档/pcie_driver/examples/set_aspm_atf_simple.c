/*
 * PCIe ASPM 设置 - ATF 简化版
 * 
 * 最简单的代码：链路训练完成后设置 ASPM
 */

#include <stdint.h>
#include <lib/mmio.h>

/* PCIe 配置空间访问基址（根据实际平台修改） */
#define PCIE_CFG_BASE    0x40000000

/* PCIe Capability ID */
#define PCI_CAP_ID_EXP   0x10

/* PCIe Capability 寄存器偏移 */
#define PCI_EXP_LNKCTL   0x10    // Link Control

/* ASPM 控制位 */
#define PCI_EXP_LNKCTL_ASPM_L0s_L1  0x0003

/**
 * 计算 PCIe 配置空间地址
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
    return mmio_read_8(pcie_cfg_addr(bus, dev, func, offset));
}

/**
 * 读取 PCIe 配置空间（16位）
 */
static uint16_t pcie_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    return mmio_read_16(pcie_cfg_addr(bus, dev, func, offset));
}

/**
 * 写入 PCIe 配置空间（16位）
 */
static void pcie_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t value)
{
    mmio_write_16(pcie_cfg_addr(bus, dev, func, offset), value);
}

/**
 * 查找 PCIe Capability
 */
static uint8_t pcie_find_capability(uint8_t bus, uint8_t dev, uint8_t func, uint8_t cap_id)
{
    uint8_t pos = pcie_read8(bus, dev, func, 0x34);  // Capability Pointer
    
    if (pos == 0 || pos == 0xFF)
        return 0;
    
    while (pos != 0 && pos < 0xFC) {
        if (pcie_read8(bus, dev, func, pos) == cap_id)
            return pos;
        pos = pcie_read8(bus, dev, func, pos + 1);
    }
    
    return 0;
}

/**
 * 设置 ASPM - 最简版本（3步）
 */
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

/**
 * 使用示例
 */
void example_usage(void)
{
    // 设置总线 1，设备 0，功能 0 的 ASPM
    pcie_set_aspm_simple(1, 0, 0);
}


