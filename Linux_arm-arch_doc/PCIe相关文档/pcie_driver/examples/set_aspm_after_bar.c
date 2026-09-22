/*
 * PCIe ASPM 设置 - 在配置 BAR 之后
 * 
 * 假设已经完成了 BAR 配置，现在只需要修改 ASPM
 */

#include <stdint.h>
#include <lib/mmio.h>

/* PCIe Capability ID */
#define PCI_CAP_ID_EXP   0x10

/* PCIe Capability 寄存器偏移 */
#define PCI_EXP_LNKCTL   0x10    // Link Control

/**
 * 读取 PCIe 配置空间（16位）
 * @param cfg_base: 配置空间基址（或通过 BAR 访问的地址）
 * @param offset: 配置空间偏移
 */
static uint16_t pcie_read16(uintptr_t cfg_base, uint8_t offset)
{
    return mmio_read_16(cfg_base + offset);
}

/**
 * 写入 PCIe 配置空间（16位）
 */
static void pcie_write16(uintptr_t cfg_base, uint8_t offset, uint16_t value)
{
    mmio_write_16(cfg_base + offset, value);
}

/**
 * 读取 PCIe 配置空间（8位）
 */
static uint8_t pcie_read8(uintptr_t cfg_base, uint8_t offset)
{
    return mmio_read_8(cfg_base + offset);
}

/**
 * 查找 PCIe Capability
 * @param cfg_base: 配置空间基址
 * @return: Capability 偏移地址，0 表示未找到
 */
static uint8_t pcie_find_capability(uintptr_t cfg_base)
{
    uint8_t pos;
    uint8_t id;
    
    // 读取 Capability Pointer (0x34)
    pos = pcie_read8(cfg_base, 0x34);
    if (pos == 0 || pos == 0xFF)
        return 0;
    
    // 遍历 Capability 链表
    while (pos != 0 && pos < 0xFC) {
        id = pcie_read8(cfg_base, pos);
        if (id == PCI_CAP_ID_EXP)
            return pos;
        
        pos = pcie_read8(cfg_base, pos + 1);
    }
    
    return 0;
}

/**
 * 修改 ASPM - 最简单版本
 * @param cfg_base: PCIe 配置空间基址（或设备配置空间地址）
 */
void pcie_modify_aspm(uintptr_t cfg_base)
{
    uint8_t pos;
    uint16_t link_control;
    
    // 1. 查找 PCIe Capability
    pos = pcie_find_capability(cfg_base);
    if (pos == 0) {
        // ERROR: PCIe Capability not found
        return;
    }
    
    // 2. 读取 Link Control 寄存器
    link_control = pcie_read16(cfg_base, pos + PCI_EXP_LNKCTL);
    
    // 3. 修改 ASPM 设置
    link_control &= ~0x0003;  // 清除 ASPM 控制位 (bit 0-1)
    link_control |= 0x0003;   // 设置 L0s + L1 (可以根据需要修改)
    
    // 4. 写回
    pcie_write16(cfg_base, pos + PCI_EXP_LNKCTL, link_control);
}

/**
 * 修改 ASPM - 指定模式版本
 * @param cfg_base: PCIe 配置空间基址
 * @param aspm_mode: ASPM 模式 (0=Disabled, 1=L0s, 2=L1, 3=L0s+L1)
 */
void pcie_set_aspm_mode(uintptr_t cfg_base, uint8_t aspm_mode)
{
    uint8_t pos;
    uint16_t link_control;
    
    // 1. 查找 PCIe Capability
    pos = pcie_find_capability(cfg_base);
    if (pos == 0)
        return;
    
    // 2. 读取 Link Control 寄存器
    link_control = pcie_read16(cfg_base, pos + PCI_EXP_LNKCTL);
    
    // 3. 修改 ASPM 设置
    link_control &= ~0x0003;           // 清除旧设置
    link_control |= (aspm_mode & 0x3); // 设置新模式
    
    // 4. 写回
    pcie_write16(cfg_base, pos + PCI_EXP_LNKCTL, link_control);
}

/**
 * 使用示例1: 如果已经有配置空间地址
 */
void example_with_cfg_base(void)
{
    // 假设你已经有了配置空间基址
    uintptr_t cfg_base = 0x40000000;  // 根据实际情况修改
    
    // 直接修改 ASPM
    pcie_modify_aspm(cfg_base);
}

/**
 * 使用示例2: 如果通过 Bus/Dev/Func 计算配置空间地址
 */
void example_with_bdf(uint8_t bus, uint8_t dev, uint8_t func)
{
    // 计算配置空间地址
    uintptr_t cfg_base = 0x40000000 +  // PCIe 配置空间基址
                         ((bus << 20) | (dev << 15) | (func << 12));
    
    // 修改 ASPM
    pcie_modify_aspm(cfg_base);
}

/**
 * 使用示例3: 如果已经通过 BAR 访问设备
 */
void example_with_bar(void)
{
    // 假设你已经映射了 BAR0
    uintptr_t bar0_base = 0xFEA00000;  // BAR0 映射的地址
    
    // 注意：BAR 是内存空间，不是配置空间
    // 如果需要通过配置空间修改 ASPM，需要：
    // 1. 使用配置空间地址（不是 BAR 地址）
    // 2. 或者通过 PCIe 控制器的配置空间访问接口
    
    // 如果平台提供了从 BAR 访问配置空间的接口，可以使用：
    // uintptr_t cfg_base = get_cfg_space_from_bar(bar0_base);
    // pcie_modify_aspm(cfg_base);
}

/**
 * 使用示例4: 在设备初始化函数中
 */
void pcie_device_init_example(uintptr_t cfg_base)
{
    // ... 其他初始化代码 ...
    // 配置 BAR
    // ...
    
    // 配置完成后，修改 ASPM
    pcie_modify_aspm(cfg_base);
    
    // 或者指定特定的 ASPM 模式
    // pcie_set_aspm_mode(cfg_base, 0x03);  // L0s + L1
}

