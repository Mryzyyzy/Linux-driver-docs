/*
 * 手动读取PCIe链路状态示例程序
 * 演示如何直接读取PCIe配置空间获取链路信息
 * 
 * 编译: gcc -o read_link_status read_link_status.c
 * 使用: ./read_link_status 0000:00:01.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#define PCI_CAP_ID_EXP    0x10    /* PCIe Capability ID */
#define PCI_EXP_LNKSTA    0x12    /* Link Status寄存器偏移 */
#define PCI_EXP_LNKCAP    0x0c    /* Link Capabilities寄存器偏移 */
#define PCI_EXP_LNKCTL    0x10    /* Link Control寄存器偏移 */

/* PCI配置空间标准偏移 */
#define PCI_CAPABILITY_LIST    0x34

/* 读取配置空间字节 */
static int read_config_byte(int fd, int offset, uint8_t *value)
{
    if (lseek(fd, offset, SEEK_SET) < 0) {
        perror("lseek");
        return -1;
    }
    
    if (read(fd, value, 1) != 1) {
        perror("read");
        return -1;
    }
    
    return 0;
}

/* 读取配置空间字（16位） */
static int read_config_word(int fd, int offset, uint16_t *value)
{
    uint8_t low, high;
    
    if (read_config_byte(fd, offset, &low) < 0)
        return -1;
    if (read_config_byte(fd, offset + 1, &high) < 0)
        return -1;
    
    *value = low | (high << 8);
    return 0;
}

/* 读取配置空间双字（32位） */
static int read_config_dword(int fd, int offset, uint32_t *value)
{
    uint16_t low, high;
    
    if (read_config_word(fd, offset, &low) < 0)
        return -1;
    if (read_config_word(fd, offset + 2, &high) < 0)
        return -1;
    
    *value = low | (high << 16);
    return 0;
}

/* 查找PCIe Capability */
static int find_pcie_capability(int fd)
{
    uint8_t cap_ptr;
    uint8_t cap_id;
    int max_iterations = 48;  /* 防止无限循环 */
    
    /* 读取Capabilities Pointer */
    if (read_config_byte(fd, PCI_CAPABILITY_LIST, &cap_ptr) < 0)
        return 0;
    
    if (cap_ptr == 0 || cap_ptr == 0xff || cap_ptr < 0x40)
        return 0;
    
    /* 遍历Capability链表 */
    while (cap_ptr && max_iterations--) {
        /* 读取Capability ID */
        if (read_config_byte(fd, cap_ptr, &cap_id) < 0)
            break;
        
        if (cap_id == PCI_CAP_ID_EXP)
            return cap_ptr;
        
        /* 读取Next Pointer */
        if (read_config_byte(fd, cap_ptr + 1, &cap_ptr) < 0)
            break;
        
        if (cap_ptr == 0 || cap_ptr == 0xff)
            break;
    }
    
    return 0;
}

/* 解析链路速度 */
static const char *speed_to_string(int speed)
{
    switch (speed) {
        case 1: return "2.5 GT/s (Gen1)";
        case 2: return "5.0 GT/s (Gen2)";
        case 3: return "8.0 GT/s (Gen3)";
        case 4: return "16.0 GT/s (Gen4)";
        case 5: return "32.0 GT/s (Gen5)";
        default: return "Unknown";
    }
}

int main(int argc, char *argv[])
{
    char path[256];
    int fd;
    int pcie_cap;
    uint16_t link_status, link_control;
    uint32_t link_cap;
    uint16_t vendor_id, device_id;
    uint8_t class_code[3];
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pci_device>\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:00:01.0\n", argv[0]);
        fprintf(stderr, "\nFind your device with: lspci\n");
        return 1;
    }
    
    /* 构建配置空间文件路径 */
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", argv[1]);
    
    printf("Opening: %s\n", path);
    
    /* 打开配置空间文件 */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open config file: %s\n", strerror(errno));
        fprintf(stderr, "Make sure the device exists: lspci | grep %s\n", argv[1]);
        return 1;
    }
    
    /* 读取基本设备信息 */
    if (read_config_word(fd, 0x00, &vendor_id) < 0) {
        close(fd);
        return 1;
    }
    
    if (read_config_word(fd, 0x02, &device_id) < 0) {
        close(fd);
        return 1;
    }
    
    printf("\n=== Device Information ===\n");
    printf("Vendor ID: 0x%04x\n", vendor_id);
    printf("Device ID: 0x%04x\n", device_id);
    
    /* 读取Class Code */
    if (read_config_byte(fd, 0x0b, &class_code[0]) == 0 &&
        read_config_byte(fd, 0x0a, &class_code[1]) == 0 &&
        read_config_byte(fd, 0x09, &class_code[2]) == 0) {
        printf("Class Code: 0x%02x%02x%02x\n",
               class_code[0], class_code[1], class_code[2]);
    }
    
    /* 查找PCIe Capability */
    printf("\n=== Searching for PCIe Capability ===\n");
    pcie_cap = find_pcie_capability(fd);
    
    if (!pcie_cap) {
        printf("PCIe Capability not found. This may not be a PCIe device.\n");
        close(fd);
        return 1;
    }
    
    printf("PCIe Capability found at offset 0x%02x\n", pcie_cap);
    
    /* 读取Link Capabilities */
    if (read_config_dword(fd, pcie_cap + PCI_EXP_LNKCAP, &link_cap) < 0) {
        close(fd);
        return 1;
    }
    
    /* 读取Link Status */
    if (read_config_word(fd, pcie_cap + PCI_EXP_LNKSTA, &link_status) < 0) {
        close(fd);
        return 1;
    }
    
    /* 读取Link Control */
    if (read_config_word(fd, pcie_cap + PCI_EXP_LNKCTL, &link_control) < 0) {
        close(fd);
        return 1;
    }
    
    /* 解析并显示信息 */
    printf("\n=== PCIe Link Information ===\n");
    printf("Link Status Register: 0x%04x\n", link_status);
    printf("Link Control Register: 0x%04x\n", link_control);
    printf("Link Capabilities Register: 0x%08x\n", link_cap);
    
    printf("\n--- Link Capabilities ---\n");
    int max_speed = link_cap & 0x0f;
    int max_width = (link_cap >> 4) & 0x3f;
    printf("Maximum Speed: %s\n", speed_to_string(max_speed));
    printf("Maximum Width: x%d\n", max_width);
    
    printf("\n--- Current Link Status ---\n");
    int current_speed = link_status & 0x0f;
    int current_width = (link_status >> 4) & 0x3f;
    int link_training = (link_status >> 11) & 0x1;
    int slot_clock = (link_status >> 15) & 0x1;
    
    printf("Current Speed: %s\n", speed_to_string(current_speed));
    printf("Current Width: x%d\n", current_width);
    printf("Link Training: %s\n", link_training ? "In Progress" : "Complete");
    printf("Slot Clock: %s\n", slot_clock ? "Active" : "Inactive");
    
    /* 检查是否有速度/宽度降级 */
    if (current_speed < max_speed || current_width < max_width) {
        printf("\n⚠ Warning: Link is operating below maximum capability!\n");
        printf("  Max: %s x%d\n", speed_to_string(max_speed), max_width);
        printf("  Current: %s x%d\n", speed_to_string(current_speed), current_width);
    }
    
    close(fd);
    return 0;
}

