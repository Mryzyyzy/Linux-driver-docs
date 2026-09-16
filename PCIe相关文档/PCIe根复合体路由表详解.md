# Root Complex 路由表的创建和配置

## 核心答案

**是的，RC 的路由表是软件写的！**

但是：
- **路由表存储在硬件寄存器中**（Root Complex 的配置寄存器）
- **软件在 PCIe 枚举时配置路由表**
- **配置完成后，硬件自动使用路由表进行地址路由**

---

## 路由表的创建过程

### 1. 完整流程

```
PCIe 枚举和路由表创建流程:
═══════════════════════════════════════════════════════════════════

步骤1: PCIe 枚举（软件）
─────────────────────────────────────
系统扫描 PCIe 总线
├─> 发现设备: Bus 1, Dev 0, Func 0
├─> 读取设备配置空间
└─> 读取 BAR0: 0x00000000 (未分配)

步骤2: 分配物理地址（软件）
─────────────────────────────────────
系统地址分配器:
├─> 查找可用地址空间: 0xFEA00000
├─> 检查冲突: 无冲突 ✓
├─> 分配: 0xFEA00000 - 0xFEAFFFFF (1MB)
└─> 写入 BAR0: 0xFEA00000

步骤3: 创建路由表条目（软件）
─────────────────────────────────────
软件配置路由表:
├─> 读取 BAR0: 0xFEA00000
├─> 计算地址范围: 0xFEA00000 - 0xFEAFFFFF
├─> 创建路由表条目:
│   ├─> 地址基址: 0xFEA00000
│   ├─> 地址掩码: 0xFFF00000 (计算得出)
│   ├─> 目标总线: Bus 1
│   ├─> 目标设备: Dev 0
│   └─> 目标功能: Func 0
└─> 写入 Root Complex 硬件寄存器

步骤4: 硬件自动使用（硬件）
─────────────────────────────────────
CPU 访问 0xFEA00000:
├─> Root Complex 硬件读取路由表寄存器
├─> 硬件并行比较地址
├─> 匹配: 0xFEA00000 在范围内 ✓
├─> 硬件自动生成 TLP
└─> 硬件自动发送到 Bus 1, Dev 0, Func 0
```

### 2. 路由表的结构

**路由表条目结构：**

```c
// 路由表条目（每个设备一个条目）
struct rc_routing_entry {
    uint32_t base;      // 地址基址（例如: 0xFEA00000）
    uint32_t mask;      // 地址掩码（例如: 0xFFF00000）
    uint8_t  bus;       // 目标总线号（例如: 1）
    uint8_t  dev;       // 目标设备号（例如: 0）
    uint8_t  func;      // 目标功能号（例如: 0）
    uint8_t  enable;    // 使能位（1=使能，0=禁用）
};
```

**路由表在硬件中的存储：**

```
Root Complex 硬件寄存器:
═══════════════════════════════════════════════════════════════════

地址: 0xFEC00000 (路由表基址，平台特定)
┌─────────────────────────────────────────────────────────────┐
│ Entry 0:                                                    │
│   0xFEC00000: base  = 0xFEA00000                           │
│   0xFEC00004: mask  = 0xFFF00000                           │
│   0xFEC00008: bus   = 0x01                                 │
│   0xFEC00009: dev   = 0x00                                 │
│   0xFEC0000A: func  = 0x00                                 │
│   0xFEC0000B: enable = 0x01                                │
├─────────────────────────────────────────────────────────────┤
│ Entry 1:                                                    │
│   0xFEC00010: base  = 0xFEB00000                           │
│   0xFEC00014: mask  = 0xFFF00000                           │
│   0xFEC00018: bus   = 0x01                                 │
│   0xFEC00019: dev   = 0x01                                 │
│   0xFEC0001A: func  = 0x00                                 │
│   0xFEC0001B: enable = 0x01                                │
├─────────────────────────────────────────────────────────────┤
│ Entry 2:                                                    │
│   ...                                                       │
└─────────────────────────────────────────────────────────────┘
```

---

## 软件如何创建路由表

### 1. ATF/BIOS/UEFI 环境

**在系统初始化时创建路由表：**

```c
/**
 * 配置 Root Complex 路由表（ATF 环境）
 */
void configure_rc_routing_table(uint8_t bus, uint8_t dev, uint8_t func,
                                 uint32_t bar_base, uint32_t bar_size)
{
    // 1. 计算路由表条目索引
    int index = (bus << 8) | (dev << 3) | func;
    
    // 2. 计算路由表条目地址
    uintptr_t entry_base = RC_ROUTING_TABLE_BASE + (index * 16);
    
    // 3. 计算地址掩码
    uint32_t mask = ~(bar_size - 1);
    
    // 4. 写入路由表条目（软件配置）
    mmio_write_32(entry_base + 0x00, bar_base);  // 地址基址
    mmio_write_32(entry_base + 0x04, mask);     // 地址掩码
    mmio_write_8(entry_base + 0x08, bus);        // 总线号
    mmio_write_8(entry_base + 0x09, dev);        // 设备号
    mmio_write_8(entry_base + 0x0A, func);       // 功能号
    mmio_write_8(entry_base + 0x0B, 0x01);       // 使能
    
    INFO("Routing table entry configured:\n");
    INFO("  Address: 0x%08X - 0x%08X\n", bar_base, bar_base + bar_size - 1);
    INFO("  Target: Bus %d, Dev %d, Func %d\n", bus, dev, func);
}
```

### 2. Linux 内核环境

**在内核 PCIe 枚举时创建路由表：**

```c
/**
 * 配置 Root Complex 路由表（Linux 内核）
 */
void configure_rc_routing_table(struct pci_dev *pdev)
{
    struct rc_routing_entry entry;
    void __iomem *rc_config;
    
    // 1. 映射 Root Complex 配置寄存器
    rc_config = ioremap(RC_ROUTING_TABLE_BASE, 0x1000);
    if (!rc_config)
        return;
    
    // 2. 读取设备的 BAR0
    u32 bar0 = pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0);
    u32 bar0_size = pci_resource_len(pdev, 0);
    
    // 3. 计算路由表条目索引
    int index = (pdev->bus->number << 8) | 
                (PCI_SLOT(pdev->devfn) << 3) | 
                PCI_FUNC(pdev->devfn);
    
    // 4. 配置路由表条目
    entry.base = bar0;
    entry.mask = ~(bar0_size - 1);
    entry.bus  = pdev->bus->number;
    entry.dev  = PCI_SLOT(pdev->devfn);
    entry.func = PCI_FUNC(pdev->devfn);
    entry.enable = 1;
    
    // 5. 写入 Root Complex 硬件寄存器（软件写入）
    uintptr_t entry_base = (uintptr_t)rc_config + (index * 16);
    writel(entry.base, entry_base + 0x00);
    writel(entry.mask, entry_base + 0x04);
    writeb(entry.bus,  entry_base + 0x08);
    writeb(entry.dev,  entry_base + 0x09);
    writeb(entry.func, entry_base + 0x0A);
    writeb(entry.enable, entry_base + 0x0B);
    
    pr_info("RC routing table configured for %04x:%02x:%02x.%x\n",
            pci_domain_nr(pdev->bus), entry.bus, entry.dev, entry.func);
    
    iounmap(rc_config);
}
```

### 3. 完整的枚举和路由表创建流程

```c
/**
 * PCIe 枚举并创建路由表（完整流程）
 */
void pcie_enumerate_and_create_routing_table(void)
{
    uint8_t bus, dev, func;
    uint32_t bar_base, bar_size;
    
    // 1. 扫描 PCIe 总线
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (func = 0; func < 8; func++) {
                // 2. 检查设备是否存在
                if (!pcie_device_exists(bus, dev, func))
                    continue;
                
                // 3. 读取设备配置空间
                uint16_t vendor_id = pcie_read_config16(bus, dev, func, 0x00);
                uint16_t device_id = pcie_read_config16(bus, dev, func, 0x02);
                
                INFO("Found device: %04X:%04X at %02X:%02X.%X\n",
                     vendor_id, device_id, bus, dev, func);
                
                // 4. 读取 BAR0
                uint32_t bar0 = pcie_read_config32(bus, dev, func, 0x10);
                
                // 5. 分配物理地址（如果 BAR 未分配）
                if ((bar0 & 0x01) == 0) {  // Memory BAR
                    bar_size = pcie_get_bar_size(bus, dev, func, 0);
                    bar_base = allocate_pci_address(bar_size);
                    
                    // 写入 BAR0
                    pcie_write_config32(bus, dev, func, 0x10, bar_base);
                } else {
                    bar_base = bar0 & ~0x0F;
                    bar_size = pcie_get_bar_size(bus, dev, func, 0);
                }
                
                // 6. 创建路由表条目（软件写入）
                configure_rc_routing_table(bus, dev, func, bar_base, bar_size);
            }
        }
    }
}
```

---

## 路由表的存储位置

### 1. 硬件寄存器

**路由表存储在 Root Complex 的硬件寄存器中：**

```
存储位置:
═══════════════════════════════════════════════════════════════════

Root Complex 硬件芯片内部
    │
    ├─> 配置寄存器区域
    │   │
    │   └─> 地址路由表寄存器 (0xFEC00000 - 0xFEC0FFFF)
    │       │
    │       ├─> Entry 0: 设备 0 的路由信息
    │       ├─> Entry 1: 设备 1 的路由信息
    │       └─> Entry N: 设备 N 的路由信息
    │
    └─> 地址解码器硬件电路
        └─> 自动读取路由表寄存器进行地址匹配
```

### 2. 寄存器地址（平台特定）

**不同平台的寄存器地址可能不同：**

```c
// 平台 A 的寄存器地址
#define RC_ROUTING_TABLE_BASE_A  0xFEC00000

// 平台 B 的寄存器地址
#define RC_ROUTING_TABLE_BASE_B  0xFE800000

// 平台 C 的寄存器地址（通过设备树/ACPI 获取）
// 从设备树读取: pcie@fe000000/routing-table@0
```

---

## 软件配置 vs 硬件使用

### 1. 软件配置（一次）

```
软件配置路由表:
═══════════════════════════════════════════════════════════════════

时间: 系统初始化时（PCIe 枚举阶段）
执行者: BIOS/UEFI/ATF/内核
操作: 写入 Root Complex 硬件寄存器

流程:
1. PCIe 枚举设备
2. 分配地址给设备（写入 BAR）
3. 创建路由表条目
4. 写入 Root Complex 硬件寄存器
5. 完成
```

### 2. 硬件使用（每次访问）

```
硬件使用路由表:
═══════════════════════════════════════════════════════════════════

时间: CPU 每次访问 PCIe 设备时
执行者: Root Complex 硬件电路
操作: 自动读取路由表寄存器并匹配

流程:
1. CPU 访问 0xFEA00000
2. Root Complex 硬件读取路由表寄存器
3. 硬件并行比较所有条目
4. 匹配到对应的设备
5. 硬件自动生成 TLP
6. 硬件自动发送到目标设备
```

---

## 路由表的生命周期

### 1. 创建时机

```
路由表创建时机:
═══════════════════════════════════════════════════════════════════

系统启动
    │
    ├─> 硬件上电
    │   └─> Root Complex 硬件初始化
    │
    ├─> 链路训练（硬件自动）
    │   └─> 建立物理连接
    │
    ├─> PCIe 枚举（软件）
    │   ├─> 扫描总线
    │   ├─> 发现设备
    │   └─> 分配地址
    │
    └─> 创建路由表（软件） ← 这里！
        └─> 写入 Root Complex 硬件寄存器
```

### 2. 更新时机

**路由表在以下情况需要更新：**

- **新设备插入**：热插拔时，需要添加新的路由表条目
- **设备移除**：需要禁用或删除对应的路由表条目
- **地址重新分配**：如果设备地址改变，需要更新路由表

```c
/**
 * 更新路由表（设备热插拔时）
 */
void update_rc_routing_table(uint8_t bus, uint8_t dev, uint8_t func,
                             bool add_device)
{
    int index = (bus << 8) | (dev << 3) | func;
    uintptr_t entry_base = RC_ROUTING_TABLE_BASE + (index * 16);
    
    if (add_device) {
        // 添加设备：配置路由表条目
        // ... 配置代码 ...
        mmio_write_8(entry_base + 0x0B, 0x01);  // 使能
    } else {
        // 移除设备：禁用路由表条目
        mmio_write_8(entry_base + 0x0B, 0x00);  // 禁用
    }
}
```

---

## 总结

### 关键点

1. **路由表是软件创建的**：
   - 软件在 PCIe 枚举时创建路由表
   - 软件写入 Root Complex 硬件寄存器

2. **路由表存储在硬件中**：
   - 存储在 Root Complex 的配置寄存器中
   - 不是存储在内存中

3. **硬件自动使用**：
   - 配置完成后，硬件自动读取路由表
   - CPU 访问时，硬件自动匹配并路由

4. **配置时机**：
   - 系统初始化时（PCIe 枚举阶段）
   - 设备热插拔时（动态更新）

### 流程图

```
软件配置路由表（一次）
    │
    ├─> PCIe 枚举设备
    ├─> 分配地址给设备
    ├─> 创建路由表条目
    └─> 写入硬件寄存器
        │
        ▼
硬件自动使用（每次访问）
    │
    ├─> CPU 访问设备
    ├─> Root Complex 读取路由表
    ├─> 硬件匹配地址
    └─> 硬件自动路由
```

**简单记忆：软件写一次，硬件用一辈子！**


