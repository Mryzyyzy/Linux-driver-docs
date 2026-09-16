# PCIe BAR (Base Address Register) 详解

本文档详细解释 PCIe BAR 的作用、工作原理、探测分配过程以及在驱动中的使用方法。

## 目录

1. [BAR 概述](#bar-概述)
2. [BAR 的作用](#bar-的作用)
3. [BAR 的类型](#bar-的类型)
4. [BAR 的探测和分配](#bar-的探测和分配)
5. [BAR 在驱动中的使用](#bar-在驱动中的使用)
6. [实际代码示例](#实际代码示例)
7. [常见问题](#常见问题)

---

## BAR 概述

### 1. 什么是 BAR？

**BAR (Base Address Register) = 基址寄存器**

- **位置**：配置空间中的 0x10-0x27（6个 BAR，每个 4 字节或 8 字节）
- **作用**：告诉系统"我这个设备需要多少地址空间，用来访问我的寄存器/内存"
- **结果**：系统分配一段物理地址空间给设备，BAR 里写入这个基址

### 2. BAR 的基本概念

```
设备内部有寄存器/内存需要被 CPU 访问
    │
    ▼
设备在 BAR 里声明："我需要 1MB 的地址空间"
    │
    ▼
系统分配：物理地址 0xFEA00000 - 0xFEAFFFFF
    │
    ▼
系统把这个基址写入 BAR
    │
    ▼
CPU 访问 0xFEA00000 + offset → 访问设备寄存器
```

---

## BAR 的作用

### 1. 地址空间映射

**BAR 建立了 CPU 地址空间和设备内部资源的映射关系：**

```
CPU 视角:
┌─────────────────────────────────────┐
│ 系统物理地址空间                      │
│                                       │
│  0xFEA00000 ────────────────────────┐│
│  (BAR0 分配的地址)                   ││
│  0xFEAFFFFF                          ││
│                                      ││
│  0xFEB00000 ────────────────────────┐││
│  (BAR1 分配的地址)                   │││
│  0xFEBFFFFF                          │││
└─────────────────────────────────────┘││
                                        ││
设备视角:                                ││
┌──────────────────────────────────────┘│
│ 设备内部资源                           │
│                                       │
│  BAR0 映射到:                         │
│  ┌─────────────────┐                 │
│  │ 控制寄存器      │ ← 0xFEA00000     │
│  │ 状态寄存器      │ ← 0xFEA00004    │
│  │ 数据寄存器      │ ← 0xFEA00008    │
│  │ ...             │                 │
│  └─────────────────┘                 │
│                                       │
│  BAR1 映射到:                         │
│  ┌─────────────────┐                 │
│  │ 设备内存        │ ← 0xFEB00000   │
│  │ (FIFO/Buffer)   │                 │
│  └─────────────────┘                 │
└───────────────────────────────────────┘
```

### 2. 为什么需要 BAR？

**没有 BAR 的问题：**
- CPU 不知道设备内部有什么资源
- CPU 不知道如何访问设备寄存器
- 无法建立地址映射关系

**有了 BAR：**
- 设备声明需要的地址空间大小
- 系统分配物理地址
- CPU 可以通过这个地址访问设备

### 3. BAR 的完整工作流程

```
1. 设备设计时:
   设备硬件设计：我需要 1MB 地址空间来映射我的寄存器
   → BAR0 被设计为 Memory BAR，大小 1MB

2. 系统启动时（BIOS/内核枚举）:
   a. 读取 BAR0 的初始值（通常是全 1 或 0）
   b. 探测 BAR 大小：
      - 写入 0xFFFFFFFF 到 BAR
      - 读回 BAR 值
      - 分析哪些位是可写的（地址位）
      - 计算大小 = 2^(最低可写位)
   c. 分配物理地址：
      - 找到一段空闲的物理地址空间
      - 大小 >= BAR 需要的大小
      - 地址对齐（根据 BAR 类型）
   d. 写入基址到 BAR：
      - 把分配的物理地址写入 BAR

3. 驱动使用时:
   a. 读取 BAR 值（得到物理地址）
   b. 映射到虚拟地址（ioremap）
   c. 通过虚拟地址访问设备寄存器
```

---

## BAR 的类型

### 1. Memory BAR vs I/O BAR

**BAR 有两种类型，由最低位（bit 0）区分：**

```
BAR 寄存器格式:
┌─────────────────────────────────────┐
│ Bit 0: Type                          │
│   0 = Memory BAR                     │
│   1 = I/O BAR                        │
└─────────────────────────────────────┘
```

### 2. Memory BAR（内存 BAR）

**Bit 0 = 0，表示这是内存空间 BAR**

```
Memory BAR 格式 (32-bit):
┌─────────────────────────────────────┐
│ Bit 0:  0 (Memory)                  │
│ Bit 1-2: Locatable                  │
│   00 = 32-bit address               │
│   01 = Reserved                     │
│   10 = 64-bit address (需要 2 个 BAR)│
│   11 = Reserved                     │
│ Bit 3:   Prefetchable                │
│   0 = Non-prefetchable              │
│   1 = Prefetchable                  │
│ Bit 4-31: Base Address              │
└─────────────────────────────────────┘
```

**示例：**

```c
// 32-bit Memory BAR (Non-prefetchable)
BAR0 = 0x00000000
       ││││││││││││││││││││││││││││││││││││││││││││││││││
       └─┘└─┘└─────────────────────────────────────────┘
        │   │                    │
        │   │                    └─> Base Address (28 bits)
        │   └─> Prefetchable = 0
        └─> Type = 0 (Memory), Locatable = 00 (32-bit)

// 64-bit Memory BAR (需要 BAR0 和 BAR1 一起)
BAR0 = 0x00000000  (低 32 位)
BAR1 = 0x00000000  (高 32 位)
```

### 3. I/O BAR（I/O 空间 BAR）

**Bit 0 = 1，表示这是 I/O 空间 BAR**

```
I/O BAR 格式 (32-bit):
┌─────────────────────────────────────┐
│ Bit 0:  1 (I/O)                      │
│ Bit 1:   Reserved                     │
│ Bit 2-31: Base Address               │
└─────────────────────────────────────┘
```

**注意：**
- I/O BAR 主要用于 x86 架构
- ARM 架构通常不支持 I/O 空间
- 现代设备通常使用 Memory BAR

### 4. BAR 类型判断代码

```c
static bool is_memory_bar(u32 bar)
{
    return (bar & 0x01) == 0;  // Bit 0 = 0
}

static bool is_io_bar(u32 bar)
{
    return (bar & 0x01) == 1;  // Bit 0 = 1
}

static bool is_64bit_bar(u32 bar)
{
    return is_memory_bar(bar) && ((bar >> 1) & 0x3) == 0x2;
}

static bool is_prefetchable(u32 bar)
{
    return is_memory_bar(bar) && (bar & 0x08);
}
```

---

## BAR 的探测和分配

### 1. BAR 大小探测

**系统如何知道设备需要多大的地址空间？**

#### 方法：写入全 1，读回分析

```c
// BAR 大小探测算法
static u32 probe_bar_size(struct pci_dev *pdev, int bar)
{
    u32 bar_value, size;
    u32 original_value;
    
    // 1. 保存原始值
    pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0 + bar * 4, &original_value);
    
    // 2. 写入全 1
    pci_write_config_dword(pdev, PCI_BASE_ADDRESS_0 + bar * 4, 0xFFFFFFFF);
    
    // 3. 读回
    pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0 + bar * 4, &bar_value);
    
    // 4. 恢复原始值
    pci_write_config_dword(pdev, PCI_BASE_ADDRESS_0 + bar * 4, original_value);
    
    // 5. 分析大小
    if (is_io_bar(bar_value)) {
        // I/O BAR: 清除 bit 0，取反加 1
        size = ~(bar_value & ~0x03) + 1;
    } else {
        // Memory BAR: 清除低 4 位，取反加 1
        size = ~(bar_value & ~0x0F) + 1;
    }
    
    return size;
}
```

#### 探测过程示例

```
假设设备需要 1MB (0x100000) 地址空间:

1. 原始 BAR 值: 0x00000000

2. 写入 0xFFFFFFFF:
   BAR = 0xFFFFFFFF

3. 设备硬件行为:
   设备只允许低 20 位可写（1MB = 2^20）
   高 12 位被硬件强制为 0
   读回: BAR = 0xFFF00000
        ││││││││││││││││││││││││││││││││││││││││││││││││
        └─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘
        高 12 位 = 0 (硬件强制)
        低 20 位 = 1 (可写)

4. 计算大小:
   size = ~(0xFFF00000 & ~0x0F) + 1
       = ~0xFFF00000 + 1
       = 0x00100000
       = 1MB ✓
```

### 2. BAR 地址分配

**系统如何分配地址给 BAR？**

```c
// 系统分配 BAR 地址的过程（简化）
void allocate_bar_address(struct pci_dev *dev, int bar)
{
    u32 bar_size;
    u32 base_address;
    u32 alignment;
    
    // 1. 探测 BAR 大小
    bar_size = probe_bar_size(dev, bar);
    
    // 2. 计算对齐要求
    // Memory BAR: 必须对齐到大小边界
    // 例如：1MB BAR 必须对齐到 1MB 边界
    alignment = bar_size;
    
    // 3. 在系统地址空间中找空闲区域
    // 假设系统地址空间：0x00000000 - 0xFFFFFFFF
    base_address = find_free_memory_region(bar_size, alignment);
    // 例如找到：0xFEA00000
    
    // 4. 写入 BAR
    pci_write_config_dword(dev, PCI_BASE_ADDRESS_0 + bar * 4, base_address);
    
    // 5. 验证
    u32 verify;
    pci_read_config_dword(dev, PCI_BASE_ADDRESS_0 + bar * 4, &verify);
    if (verify != base_address) {
        pr_err("BAR allocation failed!\n");
    }
}
```

### 3. 64-bit BAR 的特殊处理

**64-bit BAR 需要两个连续的 BAR：**

```c
// 64-bit BAR 使用 BAR0 和 BAR1
// BAR0: 低 32 位
// BAR1: 高 32 位

static void allocate_64bit_bar(struct pci_dev *dev)
{
    u64 bar_size;
    u64 base_address;
    
    // 1. 探测大小（从 BAR0 开始）
    bar_size = probe_bar_size_64bit(dev, 0);
    
    // 2. 分配 64-bit 地址
    base_address = find_free_memory_region_64bit(bar_size);
    
    // 3. 写入 BAR0 (低 32 位)
    pci_write_config_dword(dev, PCI_BASE_ADDRESS_0, 
                           lower_32_bits(base_address));
    
    // 4. 写入 BAR1 (高 32 位)
    pci_write_config_dword(dev, PCI_BASE_ADDRESS_1, 
                           upper_32_bits(base_address));
    
    // 注意：BAR1 被占用，后续 BAR 从 BAR2 开始
}
```

---

## BAR 在驱动中的使用

### 1. 读取 BAR 地址

**驱动如何获取系统分配的 BAR 地址？**

```c
static int pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    resource_size_t bar0_start, bar0_len;
    void __iomem *bar0;
    
    // 方法1: 使用内核 API（推荐）
    bar0_start = pci_resource_start(pdev, 0);  // BAR0 的物理地址
    bar0_len = pci_resource_len(pdev, 0);      // BAR0 的长度
    
    pr_info("BAR0: phys=0x%llx, len=0x%llx\n", 
            (unsigned long long)bar0_start, 
            (unsigned long long)bar0_len);
    
    // 方法2: 直接从配置空间读取
    u32 bar_value;
    pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0, &bar_value);
    
    // 清除低 4 位（类型和对齐位），得到基址
    u32 bar_base = bar_value & ~0x0F;
    pr_info("BAR0 from config: 0x%08x\n", bar_base);
}
```

### 2. 映射 BAR 到虚拟地址

**物理地址 → 虚拟地址映射：**

```c
static int pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    void __iomem *bar0;
    
    // 1. 请求资源（防止其他驱动占用）
    int ret = pci_request_regions(pdev, DRIVER_NAME);
    if (ret) {
        pr_err("Failed to request regions\n");
        return ret;
    }
    
    // 2. 映射 BAR0 到虚拟地址
    bar0 = pci_iomap(pdev, 0, 0);
    // 参数说明：
    //   pdev: PCI 设备
    //   0: BAR 编号（BAR0）
    //   0: 映射长度（0 = 映射整个 BAR）
    
    if (!bar0) {
        pr_err("Failed to map BAR0\n");
        pci_release_regions(pdev);
        return -ENOMEM;
    }
    
    // 3. 现在可以通过 bar0 访问设备寄存器
    u32 control_reg = ioread32(bar0 + 0x00);
    u32 status_reg = ioread32(bar0 + 0x04);
    
    // 4. 写入寄存器
    iowrite32(0x1234, bar0 + 0x08);
    
    // 5. 清理时取消映射
    // pci_iounmap(pdev, bar0);
}
```

### 3. 访问设备寄存器

**通过映射后的虚拟地址访问：**

```c
// 设备寄存器布局示例
#define DEVICE_CONTROL_REG    0x00
#define DEVICE_STATUS_REG     0x04
#define DEVICE_DATA_REG       0x08
#define DEVICE_FIFO_REG       0x0C

static void access_device_registers(void __iomem *bar0)
{
    // 读取控制寄存器
    u32 ctrl = ioread32(bar0 + DEVICE_CONTROL_REG);
    pr_info("Control Register: 0x%08x\n", ctrl);
    
    // 读取状态寄存器
    u32 status = ioread32(bar0 + DEVICE_STATUS_REG);
    pr_info("Status Register: 0x%08x\n", status);
    
    // 写入数据寄存器
    iowrite32(0xDEADBEEF, bar0 + DEVICE_DATA_REG);
    
    // 读取 FIFO
    u32 fifo_data = ioread32(bar0 + DEVICE_FIFO_REG);
    pr_info("FIFO Data: 0x%08x\n", fifo_data);
    
    // 8-bit 访问
    u8 byte_data = ioread8(bar0 + 0x10);
    
    // 16-bit 访问
    u16 word_data = ioread16(bar0 + 0x12);
}
```

### 4. 多个 BAR 的使用

**设备可能有多个 BAR：**

```c
static int pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct my_device *dev;
    void __iomem *bar0, *bar1;
    
    // 映射 BAR0（控制寄存器）
    bar0 = pci_iomap(pdev, 0, 0);
    if (!bar0) {
        pr_err("Failed to map BAR0\n");
        return -ENOMEM;
    }
    
    // 映射 BAR1（数据缓冲区，如果存在）
    if (pci_resource_len(pdev, 1) > 0) {
        bar1 = pci_iomap(pdev, 1, 0);
        if (!bar1) {
            pr_err("Failed to map BAR1\n");
            pci_iounmap(pdev, bar0);
            return -ENOMEM;
        }
    }
    
    dev->bar0 = bar0;  // 控制寄存器
    dev->bar1 = bar1;  // 数据缓冲区
    
    // 使用不同的 BAR 访问不同的资源
    iowrite32(0x1234, bar0 + CONTROL_REG);  // 通过 BAR0 写控制
    ioread32(bar1 + DATA_BUFFER);            // 通过 BAR1 读数据
    
    return 0;
}
```

---

## 实际代码示例

### 1. 完整的 BAR 使用示例

```c
#include <linux/pci.h>
#include <linux/io.h>

struct my_device {
    struct pci_dev *pdev;
    void __iomem *bar0;          // BAR0 虚拟地址
    void __iomem *bar1;          // BAR1 虚拟地址（可选）
    resource_size_t bar0_start;  // BAR0 物理地址
    resource_size_t bar0_len;     // BAR0 长度
};

static int my_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct my_device *dev;
    int ret;
    
    // 1. 分配设备结构
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    dev->pdev = pdev;
    pci_set_drvdata(pdev, dev);
    
    // 2. 使能设备
    ret = pci_enable_device(pdev);
    if (ret)
        goto err_free_dev;
    
    // 3. 获取 BAR 信息（在映射之前）
    dev->bar0_start = pci_resource_start(pdev, 0);
    dev->bar0_len = pci_resource_len(pdev, 0);
    
    pr_info("BAR0: phys=0x%llx, len=0x%llx\n",
            (unsigned long long)dev->bar0_start,
            (unsigned long long)dev->bar0_len);
    
    // 4. 请求资源
    ret = pci_request_regions(pdev, DRIVER_NAME);
    if (ret)
        goto err_disable_device;
    
    // 5. 映射 BAR0
    dev->bar0 = pci_iomap(pdev, 0, 0);
    if (!dev->bar0) {
        pr_err("Failed to map BAR0\n");
        ret = -ENOMEM;
        goto err_release_regions;
    }
    
    pr_info("BAR0 mapped to virtual address: %p\n", dev->bar0);
    
    // 6. 映射 BAR1（如果存在）
    if (pci_resource_len(pdev, 1) > 0) {
        dev->bar1 = pci_iomap(pdev, 1, 0);
        if (dev->bar1) {
            pr_info("BAR1: phys=0x%llx, len=0x%llx, virt=%p\n",
                    (unsigned long long)pci_resource_start(pdev, 1),
                    (unsigned long long)pci_resource_len(pdev, 1),
                    dev->bar1);
        }
    }
    
    // 7. 使用 BAR 访问设备
    // 读取设备 ID 寄存器（假设在 BAR0 + 0x00）
    u32 device_id = ioread32(dev->bar0 + 0x00);
    pr_info("Device ID from register: 0x%08x\n", device_id);
    
    // 写入控制寄存器（假设在 BAR0 + 0x04）
    iowrite32(0x12345678, dev->bar0 + 0x04);
    
    return 0;
    
err_release_regions:
    pci_release_regions(pdev);
err_disable_device:
    pci_disable_device(pdev);
err_free_dev:
    kfree(dev);
    return ret;
}

static void my_remove(struct pci_dev *pdev)
{
    struct my_device *dev = pci_get_drvdata(pdev);
    
    // 取消映射（与映射相反的顺序）
    if (dev->bar1)
        pci_iounmap(pdev, dev->bar1);
    if (dev->bar0)
        pci_iounmap(pdev, dev->bar0);
    
    // 释放资源
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    kfree(dev);
}
```

### 2. BAR 信息打印函数

```c
static void print_bar_info(struct pci_dev *pdev)
{
    int i;
    u32 bar_value;
    resource_size_t start, len;
    
    pr_info("=== BAR Information ===\n");
    
    for (i = 0; i < 6; i++) {
        // 读取 BAR 值
        pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0 + i * 4, &bar_value);
        
        // 获取资源信息
        start = pci_resource_start(pdev, i);
        len = pci_resource_len(pdev, i);
        
        if (len == 0) {
            pr_info("BAR%d: Not used\n", i);
            continue;
        }
        
        pr_info("BAR%d:\n", i);
        pr_info("  Config Value: 0x%08x\n", bar_value);
        
        if (is_io_bar(bar_value)) {
            pr_info("  Type: I/O Space\n");
            pr_info("  Base Address: 0x%04llx\n", (unsigned long long)start);
        } else {
            pr_info("  Type: Memory Space\n");
            if (is_64bit_bar(bar_value)) {
                pr_info("  Width: 64-bit\n");
                pr_info("  Prefetchable: %s\n", 
                        is_prefetchable(bar_value) ? "Yes" : "No");
            } else {
                pr_info("  Width: 32-bit\n");
            }
            pr_info("  Base Address: 0x%08llx\n", (unsigned long long)start);
        }
        
        pr_info("  Length: 0x%llx (%lld bytes)\n", 
                (unsigned long long)len, (unsigned long long)len);
    }
}
```

### 3. 使用 lspci 查看 BAR

**命令行查看 BAR 信息：**

```bash
# 查看所有 BAR
lspci -vvv -s 00:01.0 | grep -A 10 "Region"

# 输出示例：
# Region 0: Memory at f7e00000 (32-bit, non-prefetchable) [size=256K]
# Region 1: Memory at f7d00000 (32-bit, non-prefetchable) [size=64K]
# Region 2: I/O ports at e000 [size=256]
# Region 3: Memory at f7c00000 (64-bit, prefetchable) [size=1M]

# 解读：
# Region 0 = BAR0: 内存空间，32-bit，不可预取，256KB
# Region 1 = BAR1: 内存空间，32-bit，不可预取，64KB
# Region 2 = BAR2: I/O 空间，256 字节
# Region 3 = BAR3: 内存空间，64-bit，可预取，1MB
```

---

## 常见问题

### Q1: BAR 地址是物理地址还是虚拟地址？

**A:** BAR 里存储的是**物理地址**。驱动需要：
1. 读取 BAR 得到物理地址
2. 使用 `pci_iomap()` 映射到虚拟地址
3. 通过虚拟地址访问设备

### Q2: 为什么需要 ioremap？

**A:** 
- CPU 只能访问虚拟地址空间
- BAR 里是物理地址
- `ioremap()` 把物理地址映射到虚拟地址
- 映射后的地址可以用 `ioread32()` / `iowrite32()` 访问

### Q3: BAR 可以动态改变吗？

**A:** 
- **理论上可以**，但不推荐
- BAR 地址在枚举时分配，通常固定
- 改变 BAR 需要重新枚举
- 驱动不应该修改 BAR 值

### Q4: 如何知道设备有几个 BAR？

**A:** 
```c
// 检查每个 BAR 是否被使用
for (i = 0; i < 6; i++) {
    if (pci_resource_len(pdev, i) > 0) {
        pr_info("BAR%d is used, size=0x%llx\n", i,
                (unsigned long long)pci_resource_len(pdev, i));
    }
}
```

### Q5: 64-bit BAR 和 32-bit BAR 有什么区别？

**A:**
- **32-bit BAR**: 只能访问 4GB 以下的地址空间
- **64-bit BAR**: 可以访问整个 64-bit 地址空间
- **64-bit BAR 占用 2 个 BAR**：BAR0（低 32 位）+ BAR1（高 32 位）

---

## 总结

### BAR 的关键点

1. **BAR 的作用**：
   - 声明设备需要的地址空间大小
   - 存储系统分配的物理基址
   - 建立 CPU 地址空间和设备资源的映射

2. **BAR 的工作流程**：
   - 设备声明大小 → 系统探测大小 → 系统分配地址 → 写入 BAR → 驱动映射使用

3. **驱动中的使用**：
   - 读取 BAR 得到物理地址
   - 使用 `pci_iomap()` 映射到虚拟地址
   - 通过虚拟地址访问设备寄存器

4. **注意事项**：
   - BAR 地址是物理地址，需要映射
   - 64-bit BAR 占用 2 个 BAR
   - 不要修改 BAR 值（由系统管理）

**BAR 是 PCIe 设备与系统通信的桥梁，它让 CPU 能够访问设备内部的寄存器、内存等资源。**

