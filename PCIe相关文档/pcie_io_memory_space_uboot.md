# PCIe IO 空间和 Memory 空间的区别及 U-Boot 页表设置

## 一、为什么要分出 IO 空间和 Memory 空间？

### 1. 历史原因（兼容性）

PCI/PCIe 规范继承自早期的 x86 架构，x86 有独立的 I/O 地址空间：

```
x86 地址空间：
├── Memory 空间（0x00000000 - 0xFFFFFFFF）
│   └── 用于内存映射的设备和 RAM
│
└── I/O 空间（0x0000 - 0xFFFF）
    └── 用于端口 I/O 访问（in/out 指令）
```

### 2. 访问方式不同

#### I/O 空间（Port I/O）
- **访问方式**: 使用专门的 I/O 指令（`in/out` 或 `inl/outl`）
- **地址范围**: 通常 64KB（0x0000 - 0xFFFF）
- **特点**:
  - 独立的地址空间，不与内存地址冲突
  - 访问速度较慢（需要专门的 I/O 周期）
  - 主要用于简单的寄存器访问
  - **不能缓存**

#### Memory 空间（Memory-Mapped I/O, MMIO）
- **访问方式**: 使用普通的内存访问指令（`readl/writel`）
- **地址范围**: 可以映射到系统内存空间的任意位置
- **特点**:
  - 与系统内存共享地址空间
  - 访问速度快（可以使用缓存）
  - 可以映射大块内存
  - 支持 DMA 操作

### 3. 硬件实现差异

```
PCIe 设备 BAR（Base Address Register）:
├── BAR[0-5] 可以是：
│   ├── I/O 空间 BAR
│   │   └── bit[0] = 1 (PCI_BASE_ADDRESS_SPACE_IO)
│   │
│   └── Memory 空间 BAR
│       ├── bit[0] = 0 (PCI_BASE_ADDRESS_SPACE_MEMORY)
│       ├── bit[1] = 0 → 32-bit 地址
│       ├── bit[1] = 1 → 64-bit 地址
│       └── bit[3] = 1 → Prefetchable（可预取）
```

### 4. 实际应用场景

#### I/O 空间适用场景
- 简单的控制寄存器
- 状态寄存器
- 不需要缓存的小块寄存器
- 需要严格顺序访问的寄存器

#### Memory 空间适用场景
- 大块内存缓冲区
- 帧缓冲区（Frame Buffer）
- DMA 缓冲区
- 需要高性能访问的寄存器组

### 5. 现代趋势

**注意**: 在现代系统中，特别是 ARM 架构：
- **ARM 架构没有独立的 I/O 空间**
- 所有设备都使用 Memory-Mapped I/O (MMIO)
- PCIe I/O 空间通过**地址转换**映射到 Memory 空间

## 二、PCIe 地址空间映射

### 1. 设备树中的 ranges 属性

```dts
pcie@f0000000 {
    compatible = "my-company,my-pcie";
    reg = <0xf0000000 0x1000000>;
    
    /* ranges 定义地址空间映射 */
    ranges = <
        /* 格式: <child_addr parent_addr size> */
        
        /* I/O 空间映射 */
        0x81000000 0x0 0x00000000 0x10000000 0x0 0x00010000
        /* ↑        ↑  ↑          ↑          ↑  ↑
         * │        │  │          │          │  └─ 大小 (64KB)
         * │        │  │          │          └─ 父地址高位
         * │        │  │          └─ 父地址低位 (0x10000000)
         * │        │  └─ 子地址高位
         * │        └─ 子地址中位
         * └─ 标志: 0x81000000 = I/O 空间
         *           0x82000000 = Memory 空间
         *           0xC0000000 = Prefetchable Memory
         */
        
        /* Memory 空间映射 */
        0x82000000 0x0 0x20000000 0x20000000 0x0 0x10000000
    >;
    
    bus-range = <0x0 0xff>;
};
```

### 2. ranges 标志位说明

| 标志值 | 含义 | 说明 |
|--------|------|------|
| `0x81000000` | I/O 空间 | PCIe I/O 空间映射 |
| `0x82000000` | Memory 空间 | 非预取 Memory 空间 |
| `0xC0000000` | Prefetchable Memory | 可预取 Memory 空间 |

## 三、U-Boot 中页表设置

### 1. ARM 架构页表属性

在 ARM 架构中，页表项（Page Table Entry）包含以下关键属性：

```c
// ARM64 页表属性位定义
#define PTE_TYPE_MASK       (3 << 0)    // 页表类型掩码（bit[1:0]）
#define PTE_TYPE_PAGE       (3 << 0)    // 页表项（0b11）- 指向下一级页表
#define PTE_TYPE_BLOCK      (1 << 0)    // 块表项（0b01）- 直接映射大块内存

/*
 * ARM64 页表项类型区分（通过 bit[1:0]）：
 * 
 * bit[1:0] = 0b00: 无效表项（Invalid）
 * bit[1:0] = 0b01: 块表项（Block entry）- PTE_TYPE_BLOCK
 * bit[1:0] = 0b11: 页表项（Page entry）- PTE_TYPE_PAGE
 * 
 * 区分方法：
 * 1. 读取页表项的低 2 位：type = pte & PTE_TYPE_MASK
 * 2. 判断：
 *    - if (type == PTE_TYPE_BLOCK) -> 块表项，直接映射物理地址
 *    - if (type == PTE_TYPE_PAGE) -> 页表项，指向下一级页表
 * 
 * 使用场景：
 * - 块表项：用于映射大块连续内存（如 2MB/1GB 块），减少页表层级
 * - 页表项：用于映射小粒度内存（如 4KB 页），需要多级页表
 */

/*
 * ARM64 页表项低 12 位（bit[11:0]）详细说明
 * 
 * ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
 * │ 11  │ 10  │  9  │  8  │  7  │  6  │  5  │  4  │  3  │  2  │  1  │  0  │
 * ├─────┼─────┼─────┼─────┼─────┼─────┼─────┼─────┼─────┼─────┼─────┼─────┤
 * │ NG  │ AF  │ SH[1]│SH[0]│ AP[1]│AP[0]│ NS │  0  │ATTR[2]│ATTR[1]│ATTR[0]│TYPE[1]│TYPE[0]│
 * └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
 * 
 * 位域说明：
 * 
 * bit[1:0]  - TYPE: 页表项类型
 *   - 0b00: 无效表项（Invalid）
 *   - 0b01: 块表项（Block entry）- PTE_TYPE_BLOCK
 *   - 0b11: 页表项（Page entry）- PTE_TYPE_PAGE
 * 
 * bit[4:2]  - ATTRINDX: 内存属性索引（MAIR Index）
 *   - 3 位，可索引 0-7，对应 MAIR 寄存器中的 8 种内存属性
 *   - 例如：0 = Device nGnRnE, 1 = Device nGnRE, 2 = Normal NC, 3 = Normal WB
 * 
 * bit[5]    - NS: Non-Secure（非安全）
 *   - 0: Secure（安全世界）
 *   - 1: Non-Secure（非安全世界，EL0/EL1 通常使用）
 * 
 * bit[7:6]  - AP: Access Permission（访问权限）
 *   - 0b00 (0): 读写权限（EL0 可读写）
 *   - 0b01 (1): 只读权限（EL0 只读）
 *   - 0b10 (2): 用户可访问（EL0 可访问）
 *   - 0b11 (3): 特权级访问（仅 EL1+ 可访问）
 * 
 * bit[9:8]  - SH: Shareability（共享属性）
 *   - 0b00 (0): Non-Shareable（不共享，设备内存常用）
 *   - 0b10 (2): Outer Shareable（外部共享，多核系统）
 *   - 0b11 (3): Inner Shareable（内部共享，同一簇内共享）
 * 
 * bit[10]   - AF: Access Flag（访问标志）
 *   - 0: 未访问（首次访问会触发异常，由 OS 处理）
 *   - 1: 已访问（表示该页已被访问过）
 * 
 * bit[11]   - NG: Non-Global（非全局）
 *   - 0: Global（全局，ASID 切换时不清除 TLB）
 *   - 1: Non-Global（非全局，ASID 切换时清除 TLB）
 */

// 内存属性
#define PTE_TYPE_MASK       (3 << 0)    // bit[1:0] - 页表类型
#define PTE_TYPE_PAGE       (3 << 0)    // 0b11 - 页表项
#define PTE_TYPE_BLOCK      (1 << 0)    // 0b01 - 块表项

#define PTE_ATTRINDX_MASK   (7 << 2)    // bit[4:2] - 内存属性索引（MAIR Index）
#define PTE_NS               (1 << 5)    // bit[5] - 非安全
#define PTE_AP_MASK          (3 << 6)    // bit[7:6] - 访问权限
#define PTE_AP_RW            (0 << 6)    // 0b00 - 读写
#define PTE_AP_RO            (1 << 6)    // 0b01 - 只读
#define PTE_AP_USER          (2 << 6)    // 0b10 - 用户可访问
#define PTE_AP_PRIV          (3 << 6)    // 0b11 - 仅特权级
#define PTE_SH_MASK         (3 << 8)    // bit[9:8] - 共享属性
#define PTE_SH_NS            (0 << 8)    // 0b00 - Non-Shareable
#define PTE_SH_OS            (2 << 8)    // 0b10 - Outer Shareable
#define PTE_SH_IS            (3 << 8)    // 0b11 - Inner Shareable
#define PTE_AF               (1 << 10)   // bit[10] - 访问标志
#define PTE_NG               (1 << 11)   // bit[11] - 非全局

// 高位属性（不在低 12 位内，但常用）
#define PTE_GP              (1 << 50)   // bit[50] - BTI guarded (Branch Target Identification)
#define PTE_DBM             (1 << 51)   // bit[51] - 脏位（Dirty Bit Modifier）
#define PTE_CONT             (1 << 52)   // bit[52] - 连续页（Contiguous range）
#define PTE_PXN             (1 << 53)   // bit[53] - 特权执行禁止（Privileged XN）
#define PTE_UXN             (1 << 54)   // bit[54] - 用户执行禁止（User XN）
#define PTE_DIRTY            (1 << 55)   // bit[55] - 脏页标志（软件定义）
#define PTE_SPECIAL          (1 << 56)   // bit[56] - 特殊页标志（软件定义）
#define PTE_DEVMAP           (1 << 57)   // bit[57] - 设备映射标志（软件定义）
#define PTE_UFFD_WP          (1 << 58)   // bit[58] - Userfaultfd write-protect（软件定义）
// bit[59]: 保留
#define PTE_PO_IDX_0         (1 << 60)   // bit[60] - Permission Overlay Index[0]
#define PTE_PO_IDX_1         (1 << 61)   // bit[61] - Permission Overlay Index[1]
#define PTE_PO_IDX_2         (1 << 62)   // bit[62] - Permission Overlay Index[2]
// bit[63]: 软件使用位（SW bit）

/*
 * ARM64 页表项完整位域分布（64 位）
 * 
 * ┌─────────────────────────────────────────────────────────────────┐
 * │ bit[63:0] - 完整的 64 位页表项                                  │
 * ├─────────────────────────────────────────────────────────────────┤
 * │ bit[63]    - SW bit（软件使用位）                                │
 * │ bit[62:60] - Permission Overlay Index（权限覆盖索引）          │
 * │ bit[59]    - 保留                                               │
 * │ bit[58]    - PTE_UFFD_WP（用户空间缺页处理写保护）             │
 * │ bit[57]    - PTE_DEVMAP（设备映射标志）                         │
 * │ bit[56]    - PTE_SPECIAL（特殊页标志）                          │
 * │ bit[55]    - PTE_DIRTY（脏页标志）                              │
 * │ bit[54]    - PTE_UXN（用户执行禁止）                            │
 * │ bit[53]    - PTE_PXN（特权执行禁止）                            │
 * │ bit[52]    - PTE_CONT（连续页）                                 │
 * │ bit[51]    - PTE_DBM（脏位管理）                                │
 * │ bit[50]    - PTE_GP（BTI 保护）                                 │
 * │ bit[49:12] - 物理地址（PA，取决于配置）                        │
 * │ bit[11:0]  - 页表属性（见上文详细说明）                        │
 * └─────────────────────────────────────────────────────────────────┘
 * 
 * 物理地址空间：
 * - 48 位 PA：bit[47:12] 用于物理地址（36 位，支持 64GB）
 * - 52 位 PA：bit[51:12] 用于物理地址（40 位，支持 1TB）
 *             需要 CONFIG_ARM64_PA_BITS_52
 */
```

### 2. 低 12 位详细位域表

| 位域 | 位号 | 名称 | 值 | 含义 |
|------|------|------|-----|------|
| TYPE | [1:0] | 页表类型 | 0b00 | 无效表项 |
|      |       |          | 0b01 | 块表项（Block） |
|      |       |          | 0b11 | 页表项（Page） |
| ATTRINDX | [4:2] | 内存属性索引 | 0-7 | MAIR 寄存器索引（0=Device, 2=Normal NC, 3=Normal WB） |
| NS | [5] | 非安全 | 0 | Secure（安全世界） |
|    |     |        | 1 | Non-Secure（非安全世界） |
| AP | [7:6] | 访问权限 | 0b00 | 读写（EL0 可读写） |
|    |       |          | 0b01 | 只读（EL0 只读） |
|    |       |          | 0b10 | 用户可访问 |
|    |       |          | 0b11 | 仅特权级访问 |
| SH | [9:8] | 共享属性 | 0b00 | Non-Shareable |
|    |       |          | 0b10 | Outer Shareable |
|    |       |          | 0b11 | Inner Shareable |
| AF | [10] | 访问标志 | 0 | 未访问 |
|    |      |          | 1 | 已访问 |
| NG | [11] | 非全局 | 0 | Global（ASID 切换时保留） |
|    |      |          | 1 | Non-Global（ASID 切换时清除） |

### 3. 实际使用示例：构建页表项

```c
// 示例：为 PCIe 设备内存构建页表项
u64 build_pcie_pte(phys_addr_t pa)
{
    u64 pte = pa;  // 物理地址（高位）
    
    // 低 12 位属性设置
    pte |= PTE_TYPE_PAGE;        // bit[1:0] = 0b11，页表项
    pte |= (0 << 2);              // bit[4:2] = 0，MAIR Index 0 (Device)
    pte |= PTE_NS;                // bit[5] = 1，非安全
    pte |= PTE_AP_RW;             // bit[7:6] = 0b00，读写权限
    pte |= PTE_SH_NS;             // bit[9:8] = 0b00，Non-Shareable
    pte |= PTE_AF;                // bit[10] = 1，已访问标志
    pte |= PTE_NG;                // bit[11] = 1，非全局
    
    // 高位属性
    pte |= PTE_PXN;               // bit[53]，禁止特权执行
    pte |= PTE_UXN;               // bit[54]，禁止用户执行
    
    return pte;
}

// 解析页表项低 12 位
void parse_pte_low12(u64 pte)
{
    printf("页表项低 12 位解析：\n");
    printf("  TYPE[1:0] = 0x%x: %s\n", 
           pte & PTE_TYPE_MASK,
           (pte & PTE_TYPE_MASK) == PTE_TYPE_PAGE ? "页表项" : 
           (pte & PTE_TYPE_MASK) == PTE_TYPE_BLOCK ? "块表项" : "无效");
    
    printf("  ATTRINDX[4:2] = %lu (MAIR Index)\n", 
           (pte & PTE_ATTRINDX_MASK) >> 2);
    
    printf("  NS[5] = %lu: %s\n", 
           (pte >> 5) & 1,
           (pte & PTE_NS) ? "Non-Secure" : "Secure");
    
    printf("  AP[7:6] = 0x%lx: ", (pte & PTE_AP_MASK) >> 6);
    switch ((pte & PTE_AP_MASK) >> 6) {
        case 0: printf("读写\n"); break;
        case 1: printf("只读\n"); break;
        case 2: printf("用户可访问\n"); break;
        case 3: printf("仅特权级\n"); break;
    }
    
    printf("  SH[9:8] = 0x%lx: ", (pte & PTE_SH_MASK) >> 8);
    switch ((pte & PTE_SH_MASK) >> 8) {
        case 0: printf("Non-Shareable\n"); break;
        case 2: printf("Outer Shareable\n"); break;
        case 3: printf("Inner Shareable\n"); break;
    }
    
    printf("  AF[10] = %lu: %s\n", 
           (pte >> 10) & 1,
           (pte & PTE_AF) ? "已访问" : "未访问");
    
    printf("  NG[11] = %lu: %s\n", 
           (pte >> 11) & 1,
           (pte & PTE_NG) ? "Non-Global" : "Global");
}
```

### 5. ARM64 页表项完整 64 位位域总结

| 位域 | 位号 | 名称 | 说明 |
|------|------|------|------|
| **物理地址** | [51:12] 或 [47:12] | PA | 物理地址（48位PA或52位PA） |
| **软件位** | [63] | SW | 软件使用位 |
| **权限覆盖** | [62:60] | PO Index | Permission Overlay 索引 |
| **保留** | [59] | Reserved | 保留位 |
| **用户缺页** | [58] | UFFD_WP | Userfaultfd write-protect |
| **设备映射** | [57] | DEVMAP | 设备映射标志 |
| **特殊页** | [56] | SPECIAL | 特殊页标志 |
| **脏页** | [55] | DIRTY | 脏页标志（软件） |
| **用户执行禁止** | [54] | UXN | User eXecute Never |
| **特权执行禁止** | [53] | PXN | Privileged eXecute Never |
| **连续页** | [52] | CONT | Contiguous range |
| **脏位管理** | [51] | DBM | Dirty Bit Modifier |
| **BTI 保护** | [50] | GP | Guarded Page (BTI) |
| **非全局** | [11] | NG | Non-Global |
| **访问标志** | [10] | AF | Access Flag |
| **共享属性** | [9:8] | SH | Shareability |
| **访问权限** | [7:6] | AP | Access Permission |
| **非安全** | [5] | NS | Non-Secure |
| **内存属性** | [4:2] | ATTRINDX | MAIR Index |
| **页表类型** | [1:0] | TYPE | Page/Block/Invalid |

**总结：**
- **页表项总大小：64 位（8 字节）**
- **最高使用位：bit[63]**
- **物理地址支持：48 位（默认）或 52 位（需要 CONFIG_ARM64_PA_BITS_52）**
- **属性位：bit[11:0]（低 12 位）+ bit[50:52, 54, 55:58, 60:63]（高位属性）**

### 7. 页表属性的硬件/软件处理机制

**重要：页表属性分为三类：硬件自动处理、软件设置、硬件软件协作**

#### 7.1 硬件自动处理的属性

| 属性 | 位号 | 处理方式 | 说明 |
|------|------|---------|------|
| **AF (Access Flag)** | [10] | **硬件自动设置** | MMU 首次访问时自动置位，触发异常由 OS 处理 |
| **DBM (Dirty Bit)** | [51] | **硬件自动管理** | 写操作时硬件自动更新（如果支持） |
| **页表遍历** | - | **硬件自动** | MMU 自动解析页表，进行地址转换 |
| **权限检查** | [7:6], [53:54] | **硬件自动** | MMU 自动检查访问权限，违规触发异常 |
| **内存属性应用** | [4:2] | **硬件自动** | MMU 根据 MAIR 索引自动应用缓存策略 |

**硬件自动处理流程：**
```
CPU 访问内存
  ↓
MMU 自动查找页表
  ↓
硬件检查权限（AP, PXN, UXN）
  ↓
硬件应用内存属性（根据 ATTRINDX 查 MAIR）
  ↓
硬件自动设置 AF（如果未设置，触发异常）
  ↓
硬件自动更新 DBM（写操作时）
  ↓
完成地址转换和访问
```

#### 7.2 软件设置的属性

| 属性 | 位号 | 设置者 | 说明 |
|------|------|--------|------|
| **TYPE** | [1:0] | **软件** | 内核/U-Boot 创建页表时设置 |
| **ATTRINDX** | [4:2] | **软件** | 软件根据内存类型选择 MAIR 索引 |
| **NS** | [5] | **软件** | 软件根据安全需求设置 |
| **AP** | [7:6] | **软件** | 软件根据访问权限需求设置 |
| **SH** | [9:8] | **软件** | 软件根据共享需求设置 |
| **NG** | [11] | **软件** | 软件根据 ASID 需求设置 |
| **PXN/UXN** | [53:54] | **软件** | 软件根据执行权限需求设置 |
| **CONT** | [52] | **软件** | 软件标记连续页 |
| **DIRTY** | [55] | **软件** | Linux 内核软件维护的脏页标志 |
| **SPECIAL** | [56] | **软件** | Linux 内核软件使用的特殊页标志 |
| **DEVMAP** | [57] | **软件** | Linux 内核设备映射标志 |
| **物理地址** | [47:12] 或 [51:12] | **软件** | 软件设置物理地址映射 |

**软件设置流程（U-Boot/Linux 内核）：**
```c
// 软件创建页表项
u64 pte = phys_addr;                    // 设置物理地址
pte |= PTE_TYPE_PAGE;                   // 软件设置类型
pte |= PTE_AF;                          // 软件初始设置 AF=1（或让硬件设置）
pte |= (mair_index << 2);              // 软件选择内存属性索引
pte |= PTE_NS;                          // 软件设置安全属性
pte |= PTE_AP_RW;                       // 软件设置访问权限
pte |= PTE_SH_NS;                       // 软件设置共享属性
pte |= PTE_PXN | PTE_UXN;              // 软件设置执行权限
// 写入页表
*pte_ptr = pte;
```

#### 7.3 硬件软件协作的属性

| 属性 | 位号 | 协作方式 | 说明 |
|------|------|---------|------|
| **AF (Access Flag)** | [10] | **硬件设置，软件处理异常** | 硬件首次访问时置位，如果未设置触发异常，软件处理 |
| **DBM (Dirty Bit)** | [51] | **硬件更新，软件读取** | 硬件写操作时更新，软件读取判断是否脏页 |

**AF 的硬件软件协作流程：**
```
1. 软件创建页表项时，AF 可以设置为 0 或 1
   - AF=0: 让硬件自动设置（首次访问触发异常）
   - AF=1: 软件预设置（避免首次访问异常）

2. 如果 AF=0，首次访问时：
   CPU 访问内存
     ↓
   MMU 发现 AF=0
     ↓
   硬件触发 Access Flag Fault 异常
     ↓
   软件异常处理程序：
     - 读取页表项
     - 设置 AF=1
     - 写回页表项
     - 刷新 TLB
     - 返回，重新执行访问
     ↓
   硬件自动完成访问
```

#### 7.4 实际示例：PCIe 设备内存映射

```c
// U-Boot 中设置 PCIe 设备内存页表
void setup_pcie_device_mapping(phys_addr_t pcie_base, size_t size)
{
    // ===== 软件设置部分 =====
    u64 pte = pcie_base;                // 软件：设置物理地址
    pte |= PTE_TYPE_PAGE;               // 软件：设置页表类型
    pte |= (0 << 2);                    // 软件：选择 MAIR Index 0 (Device)
    pte |= PTE_NS;                      // 软件：设置为非安全
    pte |= PTE_AP_RW;                   // 软件：设置读写权限
    pte |= PTE_SH_NS;                   // 软件：设置 Non-Shareable
    pte |= PTE_AF;                      // 软件：预设置 AF=1（避免首次访问异常）
    pte |= PTE_PXN | PTE_UXN;          // 软件：禁止执行
    
    // 写入页表（软件操作）
    set_pte(pcie_base, pte, size);
    
    // ===== 硬件自动处理部分 =====
    // 之后当 CPU 访问该内存时：
    // 1. MMU 硬件自动查找页表
    // 2. 硬件自动检查权限（AP, PXN, UXN）
    // 3. 硬件自动应用内存属性（根据 ATTRINDX 查 MAIR，应用 Device 属性）
    // 4. 硬件自动完成地址转换
    // 5. 硬件自动应用缓存策略（Non-Cacheable, Non-Bufferable）
}
```

#### 7.5 总结对比表

| 处理方式 | 属性示例 | 特点 |
|---------|---------|------|
| **纯硬件** | 页表遍历、权限检查、地址转换 | 完全由 MMU 硬件自动完成，软件无需干预 |
| **纯软件** | TYPE, AP, SH, NS, 物理地址 | 软件创建页表时设置，硬件只读取使用 |
| **硬件软件协作** | AF, DBM | 硬件自动更新，软件可以读取或处理异常 |

**关键理解：**
- **软件负责"配置"**：设置页表项的各个属性位
- **硬件负责"执行"**：根据页表项属性自动进行地址转换、权限检查、内存属性应用
- **协作机制**：某些属性（如 AF）可以由硬件自动设置，但需要软件处理异常或预设置

#### 7.6 硬件如何处理软件设置的属性位（详细流程）

**问题：软件设置了 bit[1:0] 等属性位后，硬件是如何处理的？**

##### 7.6.1 硬件 MMU 的完整处理流程

```
┌─────────────────────────────────────────────────────────────┐
│ 步骤 1: CPU 发起内存访问（虚拟地址 VA）                      │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 步骤 2: MMU 硬件自动查找页表                                │
│  - 根据 VA 的 bit[47:39] 索引 PGD (Level 0)                 │
│  - 读取页表项 (64位，8字节)                                  │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 步骤 3: 硬件解析 bit[1:0] - TYPE 字段                       │
│                                                              │
│  硬件逻辑：                                                  │
│  type = pte & 0x3;  // 提取 bit[1:0]                        │
│                                                              │
│  if (type == 0b00) {                                         │
│      → 无效表项，触发 Page Fault 异常                       │
│      → 软件异常处理程序接管                                 │
│  }                                                           │
│  else if (type == 0b01) {                                   │
│      → 块表项（Block entry）                                │
│      → 硬件直接提取物理地址（bit[47:21] 或 bit[51:21]）    │
│      → 跳转到步骤 6（权限检查）                             │
│  }                                                           │
│  else if (type == 0b11) {                                   │
│      → 页表项（Page entry）                                  │
│      → 硬件提取下一级页表地址（bit[47:12]）                 │
│      → 继续遍历下一级页表（Level 1）                        │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 步骤 4: 硬件解析 bit[4:2] - ATTRINDX 字段                   │
│                                                              │
│  硬件逻辑：                                                  │
│  attr_idx = (pte >> 2) & 0x7;  // 提取 bit[4:2]            │
│                                                              │
│  → 硬件读取 MAIR_ELx 寄存器                                  │
│  → 根据 attr_idx 索引 MAIR 寄存器（8个属性槽，每个8位）     │
│  → 获取内存属性：Device/Normal, Cacheable/Non-Cacheable 等 │
│  → 硬件自动应用这些属性到内存访问                           │
│                                                              │
│  例如：attr_idx = 0                                         │
│    → MAIR[0] = 0x00 (Device nGnRnE)                        │
│    → 硬件自动设置：Non-Cacheable, Non-Bufferable           │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 步骤 5: 硬件解析 bit[7:6] - AP 字段（访问权限）             │
│                                                              │
│  硬件逻辑：                                                  │
│  ap = (pte >> 6) & 0x3;  // 提取 bit[7:6]                  │
│  current_el = get_current_el();  // 获取当前异常级别       │
│                                                              │
│  if (ap == 0b00) {  // 读写权限                              │
│      if (current_el == EL0 && !is_privileged()) {          │
│          → 允许读写                                          │
│      }                                                       │
│  }                                                           │
│  else if (ap == 0b01) {  // 只读权限                         │
│      if (is_write_access) {                                 │
│          → 触发 Permission Fault 异常                       │
│      }                                                       │
│  }                                                           │
│  else if (ap == 0b11) {  // 仅特权级                         │
│      if (current_el == EL0) {                               │
│          → 触发 Permission Fault 异常                       │
│      }                                                       │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 步骤 6: 硬件解析 bit[53:54] - PXN/UXN 字段（执行权限）      │
│                                                              │
│  硬件逻辑：                                                  │
│  if (is_instruction_fetch) {  // 指令获取                   │
│      if (current_el == EL0 && (pte & PTE_UXN)) {          │
│          → 触发 Permission Fault 异常                       │
│      }                                                       │
│      if (current_el >= EL1 && (pte & PTE_PXN)) {          │
│          → 触发 Permission Fault 异常                       │
│      }                                                       │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 步骤 7: 硬件解析 bit[9:8] - SH 字段（共享属性）             │
│                                                              │
│  硬件逻辑：                                                  │
│  sh = (pte >> 8) & 0x3;  // 提取 bit[9:8]                   │
│                                                              │
│  if (sh == 0b00) {  // Non-Shareable                        │
│      → 硬件不进行缓存一致性操作                             │
│      → 适用于设备内存                                        │
│  }                                                           │
│  else if (sh == 0b10) {  // Outer Shareable                │
│      → 硬件进行外部缓存一致性操作                           │
│  }                                                           │
│  else if (sh == 0b11) {  // Inner Shareable                 │
│      → 硬件进行内部缓存一致性操作                           │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 步骤 8: 硬件处理 bit[10] - AF 字段（访问标志）             │
│                                                              │
│  硬件逻辑：                                                  │
│  if (!(pte & PTE_AF)) {  // AF = 0                          │
│      → 硬件触发 Access Flag Fault 异常                      │
│      → 软件异常处理程序：                                   │
│        1. 读取页表项                                         │
│        2. 设置 AF = 1                                       │
│        3. 写回页表项                                        │
│        4. 刷新 TLB                                         │
│        5. 返回，硬件重新执行访问                            │
│  }                                                           │
│  else {  // AF = 1                                          │
│      → 硬件继续执行，无需异常处理                            │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 步骤 9: 硬件提取物理地址并完成转换                          │
│                                                              │
│  硬件逻辑：                                                  │
│  if (type == PTE_TYPE_BLOCK) {                              │
│      pa = (pte & 0x0000fffffffff000ULL) | (va & 0x1fffff); │
│  }                                                           │
│  else {  // PTE_TYPE_PAGE                                    │
│      pa = (pte & 0x0000fffffffff000ULL) | (va & 0xfff);     │
│  }                                                           │
│                                                              │
│  → 硬件将虚拟地址 VA 转换为物理地址 PA                      │
│  → 硬件应用内存属性（从 MAIR 获取）                         │
│  → 硬件完成内存访问                                         │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 步骤 10: 硬件自动更新 bit[51] - DBM（如果是写操作）        │
│                                                              │
│  硬件逻辑：                                                  │
│  if (is_write_access && (pte & PTE_DBM)) {                  │
│      → 硬件自动更新 DBM 位                                  │
│      → 标记页面为脏页                                       │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘
```

##### 7.6.2 硬件处理 bit[1:0] 的具体示例

```c
/*
 * 硬件 MMU 内部逻辑（伪代码，实际是硬件电路实现）
 */

// 硬件读取页表项（64位）
u64 pte = read_pte_from_memory(pte_addr);

// 硬件提取 bit[1:0]
u8 type = pte & 0x3;

// 硬件根据 type 执行不同逻辑
switch (type) {
    case 0b00:  // 无效表项
        // 硬件触发 Page Fault 异常
        raise_exception(PAGE_FAULT);
        // 软件异常处理程序接管
        break;
        
    case 0b01:  // 块表项（Block entry）
        // 硬件直接提取物理地址（大块映射）
        phys_addr_t pa = (pte & 0x0000fffffffff000ULL) | 
                         (va & 0x1fffff);  // 2MB 块偏移
        // 硬件继续处理权限检查、内存属性等
        process_block_entry(pa, pte);
        break;
        
    case 0b11:  // 页表项（Page entry）
        // 硬件提取下一级页表地址
        u64 *next_level_pt = (u64 *)(pte & 0x0000fffffffff000ULL);
        // 硬件继续遍历下一级页表
        walk_next_level(next_level_pt, va);
        break;
}
```

##### 7.6.3 硬件处理其他属性位的示例

```c
/*
 * 硬件处理 ATTRINDX (bit[4:2]) 的流程
 */

// 硬件提取 ATTRINDX
u8 attr_idx = (pte >> 2) & 0x7;

// 硬件读取 MAIR 寄存器（系统寄存器）
u64 mair = read_system_register(MAIR_EL1);

// 硬件根据索引获取内存属性（每个属性 8 位）
u8 memory_attr = (mair >> (attr_idx * 8)) & 0xFF;

// 硬件解析内存属性并应用
if (memory_attr == 0x00) {  // Device nGnRnE
    // 硬件自动设置：
    // - Non-Cacheable（不缓存）
    // - Non-Bufferable（不缓冲）
    // - 严格顺序访问
    set_cache_policy(NON_CACHEABLE);
    set_buffer_policy(NON_BUFFERABLE);
    set_ordering(STRICT_ORDERING);
}
else if (memory_attr == 0xFF) {  // Normal Write-Back
    // 硬件自动设置：
    // - Cacheable（可缓存）
    // - Write-Back（写回策略）
    set_cache_policy(WRITE_BACK);
}
```

##### 7.6.4 硬件权限检查流程

```c
/*
 * 硬件检查访问权限 (bit[7:6] AP, bit[53:54] PXN/UXN)
 */

// 硬件提取权限位
u8 ap = (pte >> 6) & 0x3;
bool pxn = pte & PTE_PXN;
bool uxn = pte & PTE_UXN;
u8 current_el = get_current_exception_level();
bool is_write = is_write_access();
bool is_instruction = is_instruction_fetch();

// 硬件检查执行权限
if (is_instruction) {
    if (current_el == 0 && uxn) {
        raise_exception(PERMISSION_FAULT);
        return;
    }
    if (current_el >= 1 && pxn) {
        raise_exception(PERMISSION_FAULT);
        return;
    }
}

// 硬件检查访问权限
if (is_write && ap == 0b01) {  // 只读权限
    raise_exception(PERMISSION_FAULT);
    return;
}

if (current_el == 0 && ap == 0b11) {  // 仅特权级
    raise_exception(PERMISSION_FAULT);
    return;
}

// 权限检查通过，硬件继续执行
```

##### 7.6.5 总结：硬件如何处理软件设置的属性位

| 属性位 | 硬件处理方式 | 硬件操作 |
|--------|------------|---------|
| **bit[1:0] TYPE** | 硬件读取并解析 | 决定是继续遍历页表还是直接映射 |
| **bit[4:2] ATTRINDX** | 硬件读取并查 MAIR | 自动应用内存属性（缓存策略） |
| **bit[7:6] AP** | 硬件读取并检查 | 自动检查访问权限，违规触发异常 |
| **bit[9:8] SH** | 硬件读取并应用 | 自动应用共享属性（缓存一致性） |
| **bit[10] AF** | 硬件读取并检查 | 如果为 0，触发异常让软件处理 |
| **bit[53:54] PXN/UXN** | 硬件读取并检查 | 自动检查执行权限，违规触发异常 |
| **bit[51] DBM** | 硬件读取并更新 | 写操作时自动更新 |

**关键点：**
1. **硬件是"读取者"**：硬件从内存中读取软件设置的页表项
2. **硬件是"执行者"**：硬件根据属性位自动执行相应的操作（地址转换、权限检查、属性应用）
3. **硬件是"检查者"**：硬件自动检查权限，违规时触发异常
4. **硬件是"应用者"**：硬件自动应用内存属性（缓存策略、共享属性等）

**整个流程是：软件设置 → 硬件读取 → 硬件解析 → 硬件执行**

### 6. 如何判断页表项类型（实际代码示例）

```c
// 判断页表项类型的函数
static inline int is_pte_type_page(u64 pte)
{
    return (pte & PTE_TYPE_MASK) == PTE_TYPE_PAGE;
}

static inline int is_pte_type_block(u64 pte)
{
    return (pte & PTE_TYPE_MASK) == PTE_TYPE_BLOCK;
}

static inline int is_pte_valid(u64 pte)
{
    return (pte & PTE_TYPE_MASK) != 0;
}

// 使用示例：遍历页表并区分类型
void walk_page_table(u64 *pgd, unsigned long va)
{
    u64 pte = pgd[va >> 39 & 0x1ff];  // 获取页表项
    
    if (!is_pte_valid(pte)) {
        printf("无效表项\n");
        return;
    }
    
    if (is_pte_type_block(pte)) {
        // 块表项：直接映射，物理地址在 pte 的高位
        phys_addr_t pa = pte & 0x0000fffffffff000ULL;
        printf("块表项：VA 0x%lx -> PA 0x%llx (2MB/1GB 块)\n", va, pa);
    } else if (is_pte_type_page(pte)) {
        // 页表项：指向下一级页表
        u64 *next_level = (u64 *)(pte & 0x0000fffffffff000ULL);
        printf("页表项：指向下一级页表 0x%p\n", next_level);
        // 继续遍历下一级...
    }
}
```

### 3. 内存类型（Memory Type）

ARM 使用 MAIR（Memory Attribute Indirection Register）定义内存类型：

```c
// U-Boot 中常见的 MAIR 设置
#define MAIR_ATTR_DEVICE_nGnRnE   0x00  // Device, Non-Gathering, Non-Reordering, No Early Write Acknowledgment
#define MAIR_ATTR_DEVICE_nGnRE    0x04  // Device, Non-Gathering, Non-Reordering, Early Write Acknowledgment
#define MAIR_ATTR_DEVICE_GRE      0x0c  // Device, Gathering, Reordering, Early Write Acknowledgment
#define MAIR_ATTR_NORMAL_NC       0x44  // Normal, Non-Cacheable
#define MAIR_ATTR_NORMAL_WT       0xbb  // Normal, Write-Through
#define MAIR_ATTR_NORMAL_WB      0xff  // Normal, Write-Back

// MAIR 寄存器设置（8 个属性，每个 8 位）
#define MAIR_EL2_SET \
    (MAIR_ATTR_DEVICE_nGnRnE << 0) |   /* Index 0: Device */ \
    (MAIR_ATTR_DEVICE_nGnRE << 8) |    /* Index 1: Device (with Early Write) */ \
    (MAIR_ATTR_NORMAL_NC << 16) |      /* Index 2: Normal Non-Cacheable */ \
    (MAIR_ATTR_NORMAL_WB << 24)        /* Index 3: Normal Write-Back */
```

### 3. PCIe 设备内存的页表设置

#### 对于 I/O 空间（映射到 Memory）

```c
// U-Boot 中设置 PCIe I/O 空间的页表
void setup_pcie_io_mapping(phys_addr_t pcie_io_base, size_t size)
{
    // 使用 Device 内存类型（Index 0 或 1）
    // 属性：Non-Cacheable, Non-Bufferable, Device
    u64 pte = pcie_io_base | 
              PTE_TYPE_PAGE |           // 页表项类型
              PTE_AF |                  // 访问标志
              (0 << 2) |                // MAIR Index 0 (Device nGnRnE)
              PTE_SH_NS |               // Non-Shareable
              PTE_AP_RW |               // 读写权限
              PTE_PXN |                 // 禁止执行
              PTE_UXN;                  // 禁止用户执行
    
    // 设置页表项
    set_pte(pcie_io_base, pte, size);
}
```

#### 对于 Memory 空间（非预取）

```c
// U-Boot 中设置 PCIe Memory 空间的页表
void setup_pcie_mem_mapping(phys_addr_t pcie_mem_base, size_t size)
{
    // 使用 Device 内存类型或 Normal Non-Cacheable
    // 属性：Non-Cacheable, Non-Bufferable
    u64 pte = pcie_mem_base | 
              PTE_TYPE_PAGE |
              PTE_AF |
              (0 << 2) |                // MAIR Index 0 (Device)
              PTE_SH_NS |
              PTE_AP_RW |
              PTE_PXN |
              PTE_UXN;
    
    set_pte(pcie_mem_base, pte, size);
}
```

#### 对于 Prefetchable Memory 空间

```c
// U-Boot 中设置 PCIe Prefetchable Memory 空间的页表
void setup_pcie_prefetch_mapping(phys_addr_t pcie_prefetch_base, size_t size)
{
    // 可以使用 Normal Non-Cacheable 或 Write-Through
    // 取决于硬件要求
    u64 pte = pcie_prefetch_base | 
              PTE_TYPE_PAGE |
              PTE_AF |
              (2 << 2) |                // MAIR Index 2 (Normal NC)
              PTE_SH_IS |               // Inner Shareable（如果多核）
              PTE_AP_RW |
              PTE_PXN |
              PTE_UXN;
    
    set_pte(pcie_prefetch_base, pte, size);
}
```

### 4. 关键属性说明

#### 必须设置的属性

1. **内存类型（MAIR Index）**
   - **I/O 空间**: 使用 `MAIR_ATTR_DEVICE_nGnRnE` (Index 0)
   - **Memory 空间**: 使用 `MAIR_ATTR_DEVICE_nGnRnE` 或 `MAIR_ATTR_NORMAL_NC`
   - **Prefetchable Memory**: 可以使用 `MAIR_ATTR_NORMAL_NC` 或 `MAIR_ATTR_NORMAL_WT`

2. **Non-Cacheable（必须）**
   - PCIe 设备寄存器**绝对不能缓存**
   - 缓存会导致数据不一致
   - 使用 Device 类型自动禁用缓存

3. **Non-Bufferable（推荐）**
   - 写操作必须立即到达设备
   - 不能缓冲写操作
   - Device nGnRnE 类型自动禁用缓冲

4. **禁止执行（必须）**
   - `PTE_PXN`: 禁止特权执行
   - `PTE_UXN`: 禁止用户执行
   - 设备内存不能作为代码执行

5. **访问权限**
   - `PTE_AP_RW`: 读写权限
   - 根据需求设置用户/特权访问

6. **共享属性**
   - `PTE_SH_NS`: Non-Shareable（单核或不需要共享）
   - `PTE_SH_IS`: Inner Shareable（多核系统）
   - `PTE_SH_OS`: Outer Shareable（多级缓存系统）

### 5. U-Boot 中的实际代码示例

```c
// arch/arm/mach-xxx/pcie.c (示例)

#include <asm/armv8/mmu.h>
#include <asm/io.h>

#define PCIE_IO_BASE    0x10000000
#define PCIE_IO_SIZE    0x00010000  // 64KB

#define PCIE_MEM_BASE   0x20000000
#define PCIE_MEM_SIZE   0x10000000  // 256MB

void setup_pcie_mmu(void)
{
    struct mm_region *mm = mem_map;
    
    // 设置 PCIe I/O 空间映射
    mm->virt = PCIE_IO_BASE;
    mm->phys = PCIE_IO_BASE;
    mm->size = PCIE_IO_SIZE;
    mm->attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |  // Device 类型
                PTE_BLOCK_NON_SHARE |                   // Non-Shareable
                PTE_BLOCK_PXN |                         // 禁止执行
                PTE_BLOCK_UXN;
    mm++;
    
    // 设置 PCIe Memory 空间映射
    mm->virt = PCIE_MEM_BASE;
    mm->phys = PCIE_MEM_BASE;
    mm->size = PCIE_MEM_SIZE;
    mm->attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |  // Device 类型
                PTE_BLOCK_NON_SHARE |
                PTE_BLOCK_PXN |
                PTE_BLOCK_UXN;
    mm++;
    
    // 更新内存映射表
    mmu_setup();
}
```

### 6. 内存类型选择指南

| 内存类型 | MAIR Index | 适用场景 | 特点 |
|---------|-----------|---------|------|
| **Device nGnRnE** | 0 | I/O 空间、控制寄存器 | 最严格，无缓存、无缓冲、无重排序 |
| **Device nGnRE** | 1 | 状态寄存器 | 允许 Early Write Acknowledgment |
| **Device GRE** | 2 | 某些特殊设备 | 允许 Gathering 和 Reordering |
| **Normal NC** | 2 | Prefetchable Memory | 非缓存，但允许预取 |
| **Normal WT** | 3 | 帧缓冲区 | 写通缓存 |
| **Normal WB** | 4 | 系统内存 | 写回缓存 |

## 四、常见错误和注意事项

### 1. 错误：使用缓存属性

```c
// ❌ 错误：PCIe 设备内存不能使用缓存
mm->attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL);  // 错误！

// ✅ 正确：使用 Device 类型
mm->attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE);
```

### 2. 错误：允许执行

```c
// ❌ 错误：设备内存不能执行代码
mm->attrs = ...;  // 缺少 PTE_BLOCK_PXN

// ✅ 正确：禁止执行
mm->attrs = ... | PTE_BLOCK_PXN | PTE_BLOCK_UXN;
```

### 3. 注意事项

- **地址对齐**: 确保映射地址按页大小对齐（通常 4KB）
- **大小对齐**: 映射大小必须是页大小的倍数
- **TLB 刷新**: 修改页表后需要刷新 TLB
- **多核系统**: 考虑共享属性设置

## 五、总结

### IO 空间 vs Memory 空间

| 特性 | I/O 空间 | Memory 空间 |
|------|---------|------------|
| **地址范围** | 64KB | 可映射到任意位置 |
| **访问方式** | I/O 指令 | 内存访问指令 |
| **缓存** | 不能缓存 | 可以缓存（但通常不缓存） |
| **用途** | 简单寄存器 | 大块内存、缓冲区 |
| **ARM 架构** | 映射到 Memory | 直接使用 Memory |

### U-Boot 页表设置要点

1. **内存类型**: 使用 `MT_DEVICE_NGNRNE`（Device nGnRnE）
2. **禁止缓存**: 必须设置 Non-Cacheable
3. **禁止执行**: 设置 `PTE_BLOCK_PXN | PTE_BLOCK_UXN`
4. **访问权限**: 设置 `PTE_BLOCK_AP_RW`（读写）
5. **共享属性**: 根据系统需求设置（单核用 Non-Shareable）

### 关键代码模板

```c
mm->attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |  // Device 类型
            PTE_BLOCK_NON_SHARE |                   // 共享属性
            PTE_BLOCK_PXN |                         // 禁止特权执行
            PTE_BLOCK_UXN;                          // 禁止用户执行
```

## 六、SCR_EL3.NS 位切换机制（Linux ↔ ATF）

### 1. SCR_EL3 寄存器概述

**SCR_EL3 (Secure Configuration Register, EL3)** 是 ARM TrustZone 的关键寄存器，控制安全状态：

```c
// SCR_EL3 关键位定义
#define SCR_EL3_NS          (1UL << 0)   // bit[0] - Non-Secure bit
#define SCR_EL3_IRQ         (1UL << 1)   // bit[1] - IRQ routing
#define SCR_EL3_FIQ         (1UL << 2)   // bit[2] - FIQ routing
#define SCR_EL3_EA          (1UL << 3)   // bit[3] - External Abort routing
#define SCR_EL3_SMD         (1UL << 7)   // bit[7] - Secure Monitor Disable
#define SCR_EL3_HCE         (1UL << 8)   // bit[8] - HVC enable
#define SCR_EL3_SIF         (1UL << 9)   // bit[9] - Secure Instruction Fetch
```

**NS 位（bit[0]）的含义：**
- `NS = 0`: Secure 世界（安全世界，ATF 运行）
- `NS = 1`: Non-Secure 世界（非安全世界，Linux 运行）

### 2. 从 Linux 跳转到 ATF 时的 NS 位切换

#### 2.1 切换方式：**软件切换（不是硬件自动）**

**关键点：SCR_EL3.NS 位必须由软件（ATF）显式切换，硬件不会自动切换。**

#### 2.2 切换流程

```
┌─────────────────────────────────────────────────────────────┐
│ Linux (EL1, Non-Secure)                                     │
│ SCR_EL3.NS = 1                                              │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ Linux 触发异常或调用 SMC/HVC                                 │
│ - SMC (Secure Monitor Call)                                 │
│ - HVC (Hypervisor Call)                                     │
│ - 外部中断/异常                                              │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 硬件自动行为（异常路由）                                     │
│ - 硬件检测到 SMC/HVC 指令或异常                             │
│ - 硬件自动切换到 EL3（异常级别切换）                         │
│ - 硬件跳转到 ATF 的异常向量表                                │
│ - **但 NS 位保持不变！**                                     │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ ATF 异常处理程序（EL3）                                      │
│ - ATF 读取当前 SCR_EL3.NS = 1（仍然是 Non-Secure）         │
│ - ATF **软件**判断需要切换到 Secure                          │
│ - ATF **软件**执行：                                         │
│   mrs x0, SCR_EL3                                           │
│   bic x0, x0, #(1 << 0)  // 清除 NS 位                      │
│   msr SCR_EL3, x0                                           │
│ - 现在 SCR_EL3.NS = 0（Secure 世界）                        │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ ATF 继续执行（EL3, Secure）                                 │
│ SCR_EL3.NS = 0                                               │
└─────────────────────────────────────────────────────────────┘
```

### 3. 详细代码示例

#### 3.1 Linux 侧（触发 SMC）

```c
// Linux 内核中调用 SMC
void linux_call_atf(void)
{
    // Linux 运行在 EL1, Non-Secure
    // 此时 SCR_EL3.NS = 1
    
    // 触发 SMC 调用
    asm volatile(
        "mov x0, #0x1234\n"      // 传递参数
        "smc #0\n"               // Secure Monitor Call
        :
        :
        : "x0", "memory"
    );
    
    // 硬件自动行为：
    // 1. 检测到 SMC 指令
    // 2. 硬件自动切换到 EL3
    // 3. 硬件跳转到 ATF 的异常向量表
    // 4. **但 SCR_EL3.NS 位仍然保持为 1（硬件不自动切换）**
}
```

#### 3.2 ATF 侧（处理 SMC 并切换 NS 位）

```c
// ATF 中的 SMC 处理程序
void smc_handler(uint64_t smc_fid, uint64_t x1, uint64_t x2, uint64_t x3)
{
    uint64_t scr_el3;
    
    // 1. ATF 读取当前 SCR_EL3（此时 NS 可能仍然是 1）
    asm volatile("mrs %0, SCR_EL3" : "=r" (scr_el3));
    
    // 2. ATF 软件判断：需要切换到 Secure 世界
    if (scr_el3 & SCR_EL3_NS) {
        // 3. ATF **软件**清除 NS 位（切换到 Secure）
        scr_el3 &= ~SCR_EL3_NS;
        
        // 4. ATF **软件**写回 SCR_EL3
        asm volatile("msr SCR_EL3, %0" : : "r" (scr_el3));
        
        // 5. 现在 SCR_EL3.NS = 0，进入 Secure 世界
    }
    
    // 6. ATF 继续执行 Secure 世界的代码
    secure_world_function();
}
```

### 4. 从 ATF 返回 Linux 时的切换

```
┌─────────────────────────────────────────────────────────────┐
│ ATF (EL3, Secure)                                           │
│ SCR_EL3.NS = 0                                               │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ ATF 准备返回 Linux                                           │
│ - ATF **软件**设置 SCR_EL3.NS = 1                           │
│   mrs x0, SCR_EL3                                           │
│   orr x0, x0, #(1 << 0)  // 设置 NS 位                      │
│   msr SCR_EL3, x0                                           │
│ - 执行 ERET（异常返回）                                       │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 硬件自动行为（ERET）                                         │
│ - 硬件从 EL3 返回到 EL1                                      │
│ - 硬件恢复 Linux 的上下文                                    │
│ - **NS 位已经是 1（由软件设置）**                             │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ Linux (EL1, Non-Secure)                                     │
│ SCR_EL3.NS = 1                                               │
└─────────────────────────────────────────────────────────────┘
```

### 5. 关键总结

| 项目 | 说明 |
|------|------|
| **切换方式** | **软件切换**（ATF 显式设置） |
| **硬件行为** | 硬件只负责异常级别切换（EL1 ↔ EL3），**不自动切换 NS 位** |
| **切换时机** | ATF 异常处理程序中，根据业务逻辑决定是否切换 |
| **切换指令** | `msr SCR_EL3, x0`（软件写入） |
| **硬件自动** | ❌ 硬件不会自动切换 NS 位 |

### 6. 为什么需要软件切换？

1. **灵活性**：软件可以根据业务逻辑决定是否切换安全世界
2. **安全性**：防止意外切换，需要显式控制
3. **性能**：不是所有 SMC 调用都需要切换安全世界
4. **兼容性**：某些 SMC 调用可以在 Non-Secure 状态下处理

### 7. 实际 ATF 代码示例

```c
// ATF 中的典型处理流程
void handle_smc(uint64_t smc_fid)
{
    switch (smc_fid) {
        case SMC_FID_SECURE_FUNCTION:
            // 需要切换到 Secure 世界
            switch_to_secure_world();  // 软件清除 NS 位
            secure_function();
            switch_to_non_secure_world();  // 软件设置 NS 位
            break;
            
        case SMC_FID_NON_SECURE_FUNCTION:
            // 不需要切换，保持在 Non-Secure
            non_secure_function();
            break;
    }
}

void switch_to_secure_world(void)
{
    uint64_t scr_el3;
    asm volatile("mrs %0, SCR_EL3" : "=r" (scr_el3));
    scr_el3 &= ~SCR_EL3_NS;  // 软件清除 NS 位
    asm volatile("msr SCR_EL3, %0" : : "r" (scr_el3));
}

void switch_to_non_secure_world(void)
{
    uint64_t scr_el3;
    asm volatile("mrs %0, SCR_EL3" : "=r" (scr_el3));
    scr_el3 |= SCR_EL3_NS;  // 软件设置 NS 位
    asm volatile("msr SCR_EL3, %0" : : "r" (scr_el3));
}
```

**结论：SCR_EL3.NS 位必须由软件（ATF）显式切换，硬件不会自动切换。硬件只负责异常级别切换和异常路由。**

这样设置可以确保 PCIe 设备内存被正确访问，避免缓存导致的数据一致性问题。

## 七、PCIe Slot Capabilities Register 详解

### 1. Slot Capabilities Register 概述

**PCIe Slot Capabilities Register (SLTCAP)** 是 PCIe 扩展能力寄存器组中的一个重要寄存器，用于描述 PCIe 插槽的硬件能力和特性。

**重要：这是 PCIe Bridge 的配置寄存器！**

**设备类型：**
- **PCIe Root Port**：CPU 或芯片组提供的根端口（有物理插槽）
- **PCIe Switch Downstream Port**：PCIe 交换机的下游端口（有物理插槽）
- **PCIe-to-PCIe Bridge**：PCIe 到 PCIe 的桥接设备

**注意：**
- 不是所有 PCIe 设备都有 Slot Capabilities
- 只有**有物理插槽的 PCIe Bridge** 才有这个寄存器
- Endpoint 设备（如网卡、显卡）通常没有这个寄存器

**寄存器位置：**
- **偏移地址**：`0x14`（从 PCIe Capability 结构基址开始）
- **寄存器大小**：32 位（4 字节）
- **访问权限**：只读（Read-Only）
- **设备类型**：PCIe Bridge（Root Port 或 Switch Port）

### 2. Slot Capabilities Register 位域定义

```c
// Linux 内核中的定义 (include/uapi/linux/pci_regs.h)
#define PCI_EXP_SLTCAP        0x14    // Slot Capabilities Register

// 位域定义
#define PCI_EXP_SLTCAP_ABP     0x00000001  // bit[0]  - Attention Button Present
#define PCI_EXP_SLTCAP_PCP     0x00000002  // bit[1]  - Power Controller Present
#define PCI_EXP_SLTCAP_MRLSP   0x00000004  // bit[2]  - MRL Sensor Present
#define PCI_EXP_SLTCAP_AIP     0x00000008  // bit[3]  - Attention Indicator Present
#define PCI_EXP_SLTCAP_PIP     0x00000010  // bit[4]  - Power Indicator Present
#define PCI_EXP_SLTCAP_HPS     0x00000020  // bit[5]  - Hot-Plug Surprise
#define PCI_EXP_SLTCAP_HPC     0x00000040  // bit[6]  - Hot-Plug Capable
#define PCI_EXP_SLTCAP_SPLV    0x00007f80  // bit[14:7] - Slot Power Limit Value
#define PCI_EXP_SLTCAP_SPLS    0x00018000  // bit[16:15] - Slot Power Limit Scale
#define PCI_EXP_SLTCAP_EIP     0x00020000  // bit[17] - Electromechanical Interlock Present
#define PCI_EXP_SLTCAP_NCCS    0x00040000  // bit[18] - No Command Completed Support
#define PCI_EXP_SLTCAP_PSN     0xfff80000  // bit[31:19] - Physical Slot Number
```

### 3. 各字段详细说明

#### 3.1 硬件指示器字段

| 位域 | 名称 | 说明 |
|------|------|------|
| **bit[0] ABP** | Attention Button Present | 插槽是否有 Attention 按钮<br/>- `1`: 有 Attention 按钮<br/>- `0`: 无 Attention 按钮 |
| **bit[1] PCP** | Power Controller Present | 插槽是否有电源控制器<br/>- `1`: 有电源控制器（可软件控制电源）<br/>- `0`: 无电源控制器 |
| **bit[2] MRLSP** | MRL Sensor Present | 插槽是否有 MRL（Manually-operated Retention Latch）传感器<br/>- `1`: 有 MRL 传感器<br/>- `0`: 无 MRL 传感器 |
| **bit[3] AIP** | Attention Indicator Present | 插槽是否有 Attention 指示灯<br/>- `1`: 有 Attention 指示灯<br/>- `0`: 无 Attention 指示灯 |
| **bit[4] PIP** | Power Indicator Present | 插槽是否有电源指示灯<br/>- `1`: 有电源指示灯<br/>- `0`: 无电源指示灯 |

#### 3.2 热插拔相关字段

| 位域 | 名称 | 说明 |
|------|------|------|
| **bit[5] HPS** | Hot-Plug Surprise | 是否支持热插拔 Surprise（意外移除）<br/>- `1`: 支持 Surprise 热插拔（设备可在运行时移除）<br/>- `0`: 不支持 Surprise 热插拔 |
| **bit[6] HPC** | Hot-Plug Capable | 是否支持热插拔<br/>- `1`: 支持热插拔<br/>- `0`: 不支持热插拔 |

#### 3.3 电源限制字段

| 位域 | 名称 | 说明 |
|------|------|------|
| **bit[14:7] SPLV** | Slot Power Limit Value | 插槽功率限制值（0-127）<br/>实际功率 = SPLV × SPLS |
| **bit[16:15] SPLS** | Slot Power Limit Scale | 插槽功率限制比例<br/>- `00b`: 1.0x<br/>- `01b`: 0.1x<br/>- `10b`: 0.01x<br/>- `11b`: 0.001x |

**功率计算示例：**
```
如果 SPLV = 75, SPLS = 01b (0.1x)
实际功率限制 = 75 × 0.1 = 7.5W

如果 SPLV = 100, SPLS = 00b (1.0x)
实际功率限制 = 100 × 1.0 = 100W
```

#### 3.4 其他字段

| 位域 | 名称 | 说明 |
|------|------|------|
| **bit[17] EIP** | Electromechanical Interlock Present | 是否有机电互锁机构<br/>- `1`: 有互锁机构（防止带电插拔）<br/>- `0`: 无互锁机构 |
| **bit[18] NCCS** | No Command Completed Support | 是否不支持命令完成信号<br/>- `1`: 不支持命令完成<br/>- `0`: 支持命令完成 |
| **bit[31:19] PSN** | Physical Slot Number | 物理插槽编号（0-8191）<br/>用于标识物理插槽位置<br/>**通常由固件/硬件设置，软件只读** |

### 4. Slot Capabilities Setting 的含义

**"Slot Capabilities Setting"** 通常指的是：

1. **读取 Slot Capabilities Register**：读取插槽的能力寄存器，了解插槽支持哪些功能
2. **配置 Slot Control Register**：根据能力设置 Slot Control Register，启用相应的功能
3. **设置插槽参数**：根据硬件能力配置插槽的工作参数（如功率限制）

### 5. 实际使用示例

#### 5.1 读取 Slot Capabilities

```c
// 读取 Slot Capabilities Register
u32 slot_cap = pci_read_config_dword(dev, 
    PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCAP);

// 检查是否支持热插拔
if (slot_cap & PCI_EXP_SLTCAP_HPC) {
    printk("Slot supports Hot-Plug\n");
}

// 检查是否有电源控制器
if (slot_cap & PCI_EXP_SLTCAP_PCP) {
    printk("Slot has Power Controller\n");
}

// 读取物理插槽编号
u16 slot_number = (slot_cap & PCI_EXP_SLTCAP_PSN) >> 19;
printk("Physical Slot Number: %d\n", slot_number);

// 读取功率限制
u8 splv = (slot_cap & PCI_EXP_SLTCAP_SPLV) >> 7;
u8 spls = (slot_cap & PCI_EXP_SLTCAP_SPLS) >> 15;
float power_limit = splv * (spls == 0 ? 1.0 : 
                            (spls == 1 ? 0.1 : 
                            (spls == 2 ? 0.01 : 0.001)));
printk("Slot Power Limit: %.2fW\n", power_limit);
```

#### 5.2 根据能力配置 Slot Control

```c
// 读取 Slot Capabilities
u32 slot_cap = pci_read_config_dword(dev, 
    PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCAP);

// 读取 Slot Control Register
u16 slot_ctl = pci_read_config_word(dev, 
    PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCTL);

// 根据能力启用相应的功能
if (slot_cap & PCI_EXP_SLTCAP_HPC) {
    // 启用热插拔中断
    slot_ctl |= PCI_EXP_SLTCTL_HPIE;
}

if (slot_cap & PCI_EXP_SLTCAP_ABP) {
    // 启用 Attention Button 中断
    slot_ctl |= PCI_EXP_SLTCTL_ABPE;
}

if (slot_cap & PCI_EXP_SLTCAP_PCP) {
    // 如果支持电源控制器，可以控制电源
    // slot_ctl |= PCI_EXP_SLTCTL_PWR_ON;  // 上电
    // slot_ctl &= ~PCI_EXP_SLTCTL_PCC;    // 或下电
}

// 写回 Slot Control Register
pci_write_config_word(dev, 
    PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCTL, slot_ctl);
```

### 6. Slot Capabilities vs Slot Control

| 寄存器 | 类型 | 说明 |
|--------|------|------|
| **Slot Capabilities (SLTCAP)** | 只读 | 描述插槽的硬件能力（支持哪些功能） |
| **Slot Control (SLTCTL)** | 读写 | 控制插槽的功能（启用/禁用功能） |
| **Slot Status (SLTSTA)** | 读写 | 显示插槽的当前状态（事件标志） |

**关系：**
- **Capabilities** 告诉软件"插槽能做什么"
- **Control** 让软件"控制插槽做什么"
- **Status** 告诉软件"插槽当前状态如何"

### 7. 典型应用场景

#### 7.1 热插拔支持检测

```c
bool is_hotplug_supported(struct pci_dev *dev)
{
    u32 slot_cap = pci_read_config_dword(dev, 
        PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCAP);
    
    return (slot_cap & PCI_EXP_SLTCAP_HPC) != 0;
}
```

#### 7.2 电源管理

```c
void configure_slot_power(struct pci_dev *dev)
{
    u32 slot_cap = pci_read_config_dword(dev, 
        PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCAP);
    
    if (slot_cap & PCI_EXP_SLTCAP_PCP) {
        // 插槽有电源控制器，可以控制电源
        u16 slot_ctl = pci_read_config_word(dev, 
            PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCTL);
        
        // 上电
        slot_ctl &= ~PCI_EXP_SLTCTL_PCC;  // 清除 Power Controller Control
        pci_write_config_word(dev, 
            PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCTL, slot_ctl);
    }
}
```

### 8. Physical Slot Number (PSN) 详细说明

#### 8.1 PSN 字段概述

**PSN (Physical Slot Number)** 是 bit[31:19] 字段，用于标识物理插槽的唯一编号。

**字段特性：**
- **位域**：bit[31:19]，共 13 位
- **取值范围**：0 - 8191（2^13 - 1）
- **访问权限**：**只读**（Read-Only）
- **设置方式**：通常由**固件/硬件**设置，操作系统只读取

#### 8.2 PSN 的设置方式

**PSN 不是由操作系统软件设置的，而是由以下方式设置：**

##### 方式 1：硬件自动设置（推荐）

某些 PCIe Root Port 或 Switch 硬件会自动从硬件引脚或配置读取插槽编号：

```c
// 硬件自动设置示例（伪代码）
// 硬件在初始化时从 GPIO 或配置寄存器读取插槽编号
u16 physical_slot = read_hardware_slot_id();  // 从硬件读取
slot_cap_reg = (physical_slot << 19) | other_bits;  // 硬件设置
```

##### 方式 2：固件（BIOS/UEFI）设置

系统固件在启动时根据主板设计设置 PSN：

```c
// UEFI/BIOS 代码示例
void configure_pcie_slot_capab                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          ilities(struct pcie_port *port)
{
    u32 slot_cap = 0;
    
    // 根据主板设计设置物理插槽编号
    // 例如：Slot 1 = 1, Slot 2 = 2, 等等
    u16 slot_number = get_slot_number_from_board_config(port);
    
    // 设置 PSN 字段
    slot_cap |= (slot_number << 19);  // bit[31:19]
    
    // 设置其他能力位...
    slot_cap |= PCI_EXP_SLTCAP_HPC;   // 热插拔能力
    slot_cap |= PCI_EXP_SLTCAP_PCP;   // 电源控制器
    
    // 写入寄存器（通常在初始化阶段）
    pci_write_config_dword(port->dev, 
        PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCAP, slot_cap);
}
```

##### 方式 3：设备树/ACPI 配置

在某些系统中，插槽编号可以通过设备树或 ACPI 表配置：

```dts
// 设备树示例
pcie@0 {
    slot-number = <1>;  // 物理插槽编号
    ...
};
```

#### 8.3 PSN 的作用和用途

**PSN 的主要用途：**

1. **物理位置标识**
   - 唯一标识物理插槽位置
   - 帮助用户和管理员识别哪个物理插槽对应哪个设备

2. **热插拔管理**
   - 操作系统使用 PSN 来管理热插拔事件
   - 当设备插入/移除时，系统可以通过 PSN 识别是哪个插槽

3. **系统管理**
   - 系统管理工具（如 IPMI、Redfish）使用 PSN 来报告插槽状态
   - 远程管理时可以准确指示物理位置

4. **日志和调试**
   - 系统日志中使用 PSN 来标识问题插槽
   - 便于硬件故障定位

#### 8.4 实际使用示例

```c
// Linux 内核中读取 PSN
void print_slot_info(struct pci_dev *dev)
{
    u32 slot_cap;
    u16 slot_number;
    
    // 读取 Slot Capabilities Register
    pci_read_config_dword(dev, 
        PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCAP, &slot_cap);
    
    // 提取 PSN (bit[31:19])
    slot_number = (slot_cap & PCI_EXP_SLTCAP_PSN) >> 19;
    
    printk("PCIe Device: %04x:%04x\n", 
           dev->vendor, dev->device);
    printk("Physical Slot Number: %d\n", slot_number);
    printk("Bus: %02x, Device: %02x, Function: %02x\n",
           dev->bus->number, PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));
}

// 示例输出：
// PCIe Device: 8086:1234
// Physical Slot Number: 3
// Bus: 01, Device: 00, Function: 0
// 
// 这表示设备在物理插槽 3 上
```

#### 8.5 PSN 与逻辑设备编号的区别

| 类型 | 说明 | 示例 |
|------|------|------|
| **Physical Slot Number (PSN)** | 物理插槽编号，标识硬件位置 | Slot 1, Slot 2, Slot 3 |
| **Bus/Device/Function** | 逻辑设备编号，PCIe 枚举后分配 | Bus 01, Device 00, Function 0 |
| **关系** | PSN 是物理标识，BDF 是逻辑标识 | 一个 PSN 可能对应多个 BDF（多功能设备） |

**示例场景：**
```
物理插槽 3 (PSN=3)
  ↓ 插入多功能网卡
  ↓ PCIe 枚举
逻辑设备：
  - Bus 01, Device 00, Function 0 (网卡功能 1)
  - Bus 01, Device 00, Function 1 (网卡功能 2)
```

#### 8.6 为什么 PSN 是只读的？

1. **硬件特性**：PSN 反映的是物理硬件位置，不应该被软件改变
2. **系统稳定性**：防止软件错误修改导致物理位置识别错误
3. **一致性**：确保整个系统生命周期内插槽编号保持一致

#### 8.7 设置 PSN 的时机

**PSN 通常在以下时机设置：**

1. **硬件初始化**：PCIe Root Port 或 Switch 硬件初始化时
2. **固件启动**：BIOS/UEFI 启动时根据主板配置设置
3. **设备树解析**：Linux 内核解析设备树时（如果支持）

**操作系统运行时：**
- 操作系统**只读取** PSN，不修改
- 如果 PSN 为 0 或无效，操作系统会忽略或使用默认值

### 9. PCIe Bridge 配置说明

#### 9.1 Slot Capabilities 与 PCIe Bridge 的关系

**是的，Slot Capabilities Register 是 PCIe Bridge 的配置寄存器！**

**PCIe Bridge 类型：**

```
┌─────────────────────────────────────────────────────────┐
│ PCIe 系统架构                                            │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  CPU/Root Complex                                        │
│    │                                                      │
│    ├─ Root Port (Bridge) ← Slot Capabilities 在这里    │
│    │    │                                                  │
│    │    └─ Physical Slot 1                               │
│    │                                                       │
│    ├─ Root Port (Bridge) ← Slot Capabilities 在这里    │
│    │    │                                                  │
│    │    └─ Physical Slot 2                               │
│    │                                                       │
│    └─ PCIe Switch (Bridge)                             │
│         │                                                 │
│         ├─ Downstream Port (Bridge) ← Slot Capabilities │
│         │    └─ Physical Slot 3                          │
│         │                                                 │
│         └─ Downstream Port (Bridge) ← Slot Capabilities │
│              └─ Physical Slot 4                          │
│                                                           │
└─────────────────────────────────────────────────────────┘
```

#### 9.2 哪些设备有 Slot Capabilities？

| 设备类型 | 是否有 Slot Capabilities | 说明 |
|---------|-------------------------|------|
| **Root Port** | ✅ 有 | CPU/芯片组提供的根端口，连接物理插槽 |
| **Switch Downstream Port** | ✅ 有 | PCIe 交换机的下游端口，连接物理插槽 |
| **Switch Upstream Port** | ❌ 无 | PCIe 交换机的上游端口，不连接插槽 |
| **Endpoint** | ❌ 无 | 终端设备（网卡、显卡等），不是 Bridge |
| **Legacy PCIe Bridge** | ✅ 可能有 | 传统 PCIe 桥接设备 |

#### 9.3 PCIe Bridge 配置空间中的位置

**PCIe Bridge 的配置空间结构：**

```
PCIe Bridge 配置空间 (256 字节)
├─ Standard PCI Header (0x00 - 0x3F)
│   ├─ Vendor ID, Device ID
│   ├─ Command, Status
│   └─ ...
│
└─ PCIe Extended Capabilities (从 0x100 开始)
    ├─ PCIe Capability Structure (Offset 0x00)
    │   ├─ PCIe Capability ID (0x10)
    │   ├─ PCIe Capability Version
    │   ├─ Device Capabilities (0x04)
    │   ├─ Device Control/Status (0x08)
    │   ├─ Link Capabilities (0x0C)
    │   ├─ Link Control/Status (0x10)
    │   ├─ Slot Capabilities (0x14) ← 这里！
    │   ├─ Slot Control (0x18)
    │   ├─ Slot Status (0x1A)
    │   └─ ...
    │
    └─ 其他扩展能力...
```

#### 9.4 如何识别 PCIe Bridge？

```c
// 检查设备是否是 PCIe Bridge
bool is_pcie_bridge(struct pci_dev *dev)
{
    u8 header_type;
    
    pci_read_config_byte(dev, PCI_HEADER_TYPE, &header_type);
    header_type &= 0x7f;
    
    // PCIe Bridge 通常是 Header Type 1 (PCI-to-PCI Bridge)
    // 或者有 PCIe Capability
    if (header_type == PCI_HEADER_TYPE_BRIDGE) {
        // 检查是否有 PCIe Capability
        if (pci_find_capability(dev, PCI_CAP_ID_EXP)) {
            return true;  // 是 PCIe Bridge
        }
    }
    
    return false;
}

// 检查 Bridge 是否有 Slot Capabilities
bool has_slot_capabilities(struct pci_dev *dev)
{
    int pos;
    u32 slot_cap;
    
    // 查找 PCIe Capability
    pos = pci_find_capability(dev, PCI_CAP_ID_EXP);
    if (!pos)
        return false;
    
    // 读取 Slot Capabilities Register
    pci_read_config_dword(dev, pos + PCI_EXP_SLTCAP, &slot_cap);
    
    // 检查 Slot Implemented 位（在 PCIe Capability 寄存器中）
    // 如果 Slot Implemented = 1，说明有 Slot Capabilities
    // 这需要检查 PCIe Capability 寄存器的 Slot Implemented 位
    
    return true;  // 简化示例
}
```

#### 9.5 PCIe Bridge 中的其他相关寄存器

**除了 Slot Capabilities，PCIe Bridge 还有：**

| 寄存器 | 偏移 | 说明 |
|--------|------|------|
| **Slot Capabilities** | 0x14 | 插槽能力（只读） |
| **Slot Control** | 0x18 | 插槽控制（读写） |
| **Slot Status** | 0x1A | 插槽状态（读写） |
| **Root Control** | 0x1C | 根端口控制（仅 Root Port） |
| **Root Capabilities** | 0x1E | 根端口能力（仅 Root Port） |
| **Root Status** | 0x20 | 根端口状态（仅 Root Port） |

#### 9.6 实际应用：在 PCIe Bridge 中配置 Slot

```c
// 在 PCIe Root Port 中配置 Slot Capabilities
void configure_root_port_slot(struct pci_dev *root_port)
{
    int pos;
    u32 slot_cap;
    u16 slot_ctl;
    
    // 1. 查找 PCIe Capability
    pos = pci_find_capability(root_port, PCI_CAP_ID_EXP);
    if (!pos) {
        printk("Not a PCIe device\n");
        return;
    }
    
    // 2. 读取 Slot Capabilities（只读，由硬件/固件设置）
    pci_read_config_dword(root_port, pos + PCI_EXP_SLTCAP, &slot_cap);
    
    printk("Root Port Slot Capabilities: 0x%08x\n", slot_cap);
    
    // 3. 根据能力配置 Slot Control
    pci_read_config_word(root_port, pos + PCI_EXP_SLTCTL, &slot_ctl);
    
    // 如果支持热插拔，启用热插拔中断
    if (slot_cap & PCI_EXP_SLTCAP_HPC) {
        slot_ctl |= PCI_EXP_SLTCTL_HPIE;
        printk("Enabled Hot-Plug interrupt\n");
    }
    
    // 如果支持 Attention Button，启用按钮中断
    if (slot_cap & PCI_EXP_SLTCAP_ABP) {
        slot_ctl |= PCI_EXP_SLTCTL_ABPE;
        printk("Enabled Attention Button interrupt\n");
    }
    
    // 4. 写回 Slot Control
    pci_write_config_word(root_port, pos + PCI_EXP_SLTCTL, slot_ctl);
}
```

#### 9.7 总结：Slot Capabilities 与 PCIe Bridge

**关键点：**

1. ✅ **Slot Capabilities 是 PCIe Bridge 的配置寄存器**
2. ✅ **只有有物理插槽的 Bridge 才有**（Root Port、Switch Downstream Port）
3. ✅ **Endpoint 设备没有**（网卡、显卡等）
4. ✅ **在 PCIe Extended Capabilities 中**（从 PCIe Capability 基址偏移 0x14）
5. ✅ **通常由硬件/固件设置**，操作系统只读取

### 10. PCIe 插槽配置阶段详解

#### 10.1 可以设置的寄存器

**重要区分：**

| 寄存器 | 是否可设置 | 设置阶段 | 说明 |
|--------|----------|---------|------|
| **Slot Capabilities (SLTCAP)** | ✅ **可以设置** | 硬件初始化/固件启动 | 由硬件或固件设置，操作系统只读 |
| **Slot Control (SLTCTL)** | ✅ **可以设置** | 固件启动/操作系统运行时 | 软件可以读写，控制插槽功能 |
| **Slot Status (SLTSTA)** | ⚠️ **部分可设置** | 操作系统运行时 | 状态位，某些位可写（清除） |

#### 10.2 PCIe 插槽配置的各个阶段

```
┌─────────────────────────────────────────────────────────────┐
│ 阶段 1: 硬件复位/上电                                       │
│ - 硬件自动初始化 PCIe 控制器                                │
│ - Slot Capabilities 寄存器可能由硬件自动设置（如果支持）   │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 阶段 2: BootROM / 早期固件（Pre-Boot）                      │
│ - BootROM 或早期固件初始化 PCIe                            │
│ - 可以设置 Slot Capabilities（如果硬件未自动设置）         │
│ - 设置物理插槽编号（PSN）                                    │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 阶段 3: BIOS/UEFI 固件启动                                  │
│ - BIOS/UEFI 读取/设置 Slot Capabilities                     │
│ - 根据主板配置设置插槽参数                                   │
│ - 初始化 Slot Control Register                             │
│ - 配置热插拔、电源管理等                                     │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 阶段 4: Bootloader（如 U-Boot）                             │
│ - U-Boot 可以读取 Slot Capabilities                        │
│ - U-Boot 可以配置 Slot Control                             │
│ - 为操作系统准备 PCIe 环境                                  │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 阶段 5: 操作系统启动（Linux 内核）                          │
│ - 内核 PCIe 子系统读取 Slot Capabilities（只读）           │
│ - 内核配置 Slot Control Register                           │
│ - 启用热插拔中断、电源管理等                                 │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│ 阶段 6: 操作系统运行时                                       │
│ - 操作系统动态配置 Slot Control                            │
│ - 处理热插拔事件                                            │
│ - 管理电源状态                                              │
└─────────────────────────────────────────────────────────────┘
```

#### 10.3 各阶段详细说明

##### 阶段 1: 硬件复位/上电

```c
// 硬件自动行为（不可编程）
// 某些 PCIe 控制器硬件会自动从 GPIO 或配置读取插槽信息
// 并设置 Slot Capabilities 寄存器
```

##### 阶段 2: BootROM / 早期固件

```c
// BootROM 或早期固件代码
void early_pcie_slot_init(void)
{
    // 1. 读取硬件配置（如从 GPIO、EEPROM 等）
    u16 slot_number = read_slot_number_from_hw();
    u8 power_limit = read_power_limit_from_hw();
    
    // 2. 设置 Slot Capabilities Register
    u32 slot_cap = 0;
    slot_cap |= (slot_number << 19);  // PSN
    slot_cap |= (power_limit << 7);   // SPLV
    slot_cap |= PCI_EXP_SLTCAP_HPC;   // 热插拔能力
    slot_cap |= PCI_EXP_SLTCAP_PCP;   // 电源控制器
    
    // 3. 写入寄存器（如果硬件允许）
    pci_write_config_dword(pcie_root_port, 
        PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCAP, slot_cap);
}
```

##### 阶段 3: BIOS/UEFI 固件

```c
// UEFI/BIOS 代码（通常在 DXE 阶段）
EFI_STATUS ConfigurePcieSlotCapabilities(
    IN PCIE_ROOT_PORT *RootPort
)
{
    UINT32 SlotCap;
    UINT16 SlotCtl;
    
    // 1. 读取当前 Slot Capabilities（可能已被早期固件设置）
    PciRead32(RootPort->Base + PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCAP, &SlotCap);
    
    // 2. 如果需要，可以修改（某些实现允许）
    // 注意：根据 PCIe 规范，Slot Capabilities 应该是只读的
    // 但某些平台可能在固件阶段允许修改
    
    // 3. 根据主板配置设置插槽参数
    // 例如：从 ACPI 表或设备树读取配置
    UINT16 SlotNumber = GetSlotNumberFromAcpiTable(RootPort);
    SlotCap = (SlotCap & ~PCI_EXP_SLTCAP_PSN) | (SlotNumber << 19);
    
    // 4. 配置 Slot Control Register
    PciRead16(RootPort->Base + PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCTL, &SlotCtl);
    
    if (SlotCap & PCI_EXP_SLTCAP_HPC) {
        SlotCtl |= PCI_EXP_SLTCTL_HPIE;  // 启用热插拔中断
    }
    
    PciWrite16(RootPort->Base + PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCTL, SlotCtl);
    
    return EFI_SUCCESS;
}
```

##### 阶段 4: U-Boot

```c
// U-Boot PCIe 初始化代码
int pcie_slot_init(struct pcie_port *port)
{
    u32 slot_cap;
    u16 slot_ctl;
    int pos;
    
    // 1. 查找 PCIe Capability
    pos = pci_find_capability(port->dev, PCI_CAP_ID_EXP);
    if (!pos)
        return -ENODEV;
    
    // 2. 读取 Slot Capabilities（只读，由固件设置）
    pci_read_config_dword(port->dev, pos + PCI_EXP_SLTCAP, &slot_cap);
    
    printf("PCIe Slot Capabilities: 0x%08x\n", slot_cap);
    
    // 3. 配置 Slot Control（U-Boot 可以设置）
    pci_read_config_word(port->dev, pos + PCI_EXP_SLTCTL, &slot_ctl);
    
    // 启用热插拔中断（如果需要）
    if (slot_cap & PCI_EXP_SLTCAP_HPC) {
        slot_ctl |= PCI_EXP_SLTCTL_HPIE;
    }
    
    // 设置电源状态
    if (slot_cap & PCI_EXP_SLTCAP_PCP) {
        slot_ctl &= ~PCI_EXP_SLTCTL_PCC;  // 上电
    }
    
    pci_write_config_word(port->dev, pos + PCI_EXP_SLTCTL, slot_ctl);
    
    return 0;
}
```

##### 阶段 5: Linux 内核启动

```c
// Linux 内核 PCIe 热插拔驱动
static int pciehp_probe(struct pcie_device *dev)
{
    struct controller *ctrl;
    u32 slot_cap;
    u16 slot_ctl;
    int pos;
    
    // 1. 读取 Slot Capabilities（只读）
    pos = pci_find_capability(dev->port, PCI_CAP_ID_EXP);
    pci_read_config_dword(dev->port, pos + PCI_EXP_SLTCAP, &slot_cap);
    
    // 2. 检查是否支持热插拔
    if (!(slot_cap & PCI_EXP_SLTCAP_HPC)) {
        return -ENODEV;  // 不支持热插拔
    }
    
    // 3. 配置 Slot Control
    pci_read_config_word(dev->port, pos + PCI_EXP_SLTCTL, &slot_ctl);
    
    // 启用所有中断
    slot_ctl |= PCI_EXP_SLTCTL_HPIE |    // 热插拔中断
                PCI_EXP_SLTCTL_ABPE |    // Attention Button 中断
                PCI_EXP_SLTCTL_PDCE |    // Presence Detect 中断
                PCI_EXP_SLTCTL_MRLSCE |  // MRL Sensor 中断
                PCI_EXP_SLTCTL_CCIE;     // Command Completed 中断
    
    pci_write_config_word(dev->port, pos + PCI_EXP_SLTCTL, slot_ctl);
    
    return 0;
}
```

##### 阶段 6: 操作系统运行时

```c
// 运行时动态配置（例如：通过 sysfs 或 ioctl）
static int pciehp_set_power_state(struct controller *ctrl, u8 state)
{
    u16 slot_ctl;
    int pos;
    
    pos = pci_find_capability(ctrl->pcie->port, PCI_CAP_ID_EXP);
    pci_read_config_word(ctrl->pcie->port, pos + PCI_EXP_SLTCTL, &slot_ctl);
    
    if (state == PCIEHP_POWER_ON) {
        slot_ctl &= ~PCI_EXP_SLTCTL_PCC;  // 上电
    } else {
        slot_ctl |= PCI_EXP_SLTCTL_PCC;   // 下电
    }
    
    pci_write_config_word(ctrl->pcie->port, pos + PCI_EXP_SLTCTL, slot_ctl);
    
    return 0;
}
```

#### 10.4 配置阶段总结表

| 阶段 | 可以设置 Slot Capabilities? | 可以设置 Slot Control? | 典型操作 |
|------|---------------------------|---------------------|---------|
| **硬件复位** | 硬件自动（如果支持） | ❌ | 硬件初始化 |
| **BootROM** | ✅ 可以 | ✅ 可以 | 早期初始化 |
| **BIOS/UEFI** | ✅ 可以（某些平台） | ✅ 可以 | 根据主板配置设置 |
| **U-Boot** | ⚠️ 通常只读 | ✅ 可以 | 为操作系统准备环境 |
| **Linux 内核** | ❌ 只读 | ✅ 可以 | 启用热插拔等功能 |
| **运行时** | ❌ 只读 | ✅ 可以 | 动态管理插槽 |

#### 10.5 关键要点

1. **Slot Capabilities**：
   - **理论上只读**（根据 PCIe 规范）
   - **实际中**：某些平台在固件阶段可能允许设置
   - **操作系统**：只能读取，不能修改

2. **Slot Control**：
   - **始终可读写**
   - **任何阶段**都可以设置
   - **操作系统**：主要使用这个寄存器控制插槽

3. **最佳实践**：
   - **固件阶段**：设置 Slot Capabilities（如果平台支持）
   - **操作系统**：只读取 Slot Capabilities，配置 Slot Control

### 11. 总结

**PCIe Slot Capabilities Setting** 是指：

1. **读取能力寄存器**：了解插槽支持哪些硬件功能
2. **配置控制寄存器**：根据能力启用相应的功能
3. **设置工作参数**：配置功率限制、指示灯等参数

**关键点：**
- Slot Capabilities 是**只读**的，由硬件决定
- 软件只能**读取**能力，不能修改
- 软件根据能力**配置** Slot Control Register 来使用这些功能
- 主要用于**热插拔**、**电源管理**、**状态指示**等功能

## 十二、HPB_BASE 与 PCIe Base 地址的差异

### 1. 术语定义

#### 1.1 HPB_BASE

**HPB = Hot Plug Bus（热插拔总线）**

**HPB_BASE** 是指**热插拔总线的基址**，通常用于标识支持热插拔功能的 PCIe 总线。

**特点：**
- 指向支持热插拔的总线（Bus）
- 用于热插拔控制器（HPC - Hot Plug Controller）管理
- 在 UEFI/ACPI 中用于标识热插拔总线位置

#### 1.2 PCIe Base

**PCIe Base** 是指**PCIe 控制器的基址**，可以是：

1. **PCIe 配置空间基址**：PCIe 设备的配置寄存器基址
2. **PCIe 内存空间基址**：PCIe 设备的 Memory/IO 空间基址
3. **PCIe Root Port 基址**：Root Port 控制器的寄存器基址

### 2. 主要差异对比

| 特性 | HPB_BASE | PCIe Base |
|------|----------|-----------|
| **全称** | Hot Plug Bus Base | PCIe Controller Base |
| **指向对象** | 热插拔总线（Bus Number） | PCIe 控制器/设备 |
| **地址类型** | 总线编号（Bus Number）或设备路径 | 物理/虚拟内存地址 |
| **用途** | 热插拔管理、资源分配 | 寄存器访问、配置空间访问 |
| **使用场景** | UEFI/ACPI 热插拔支持 | 驱动程序、固件初始化 |
| **地址范围** | 总线编号（0-255） | 内存地址（物理/虚拟） |

### 3. 详细说明

#### 3.1 HPB_BASE 的含义

**HPB_BASE 通常指的是总线编号（Bus Number），而不是内存地址：**

```c
// UEFI 代码示例
typedef struct {
    UINT8   BusBase;        // HPB_BASE: 热插拔总线的起始总线编号
    UINT8   BusLimit;        // 热插拔总线的结束总线编号
    UINT16  Segment;         // PCIe Segment 编号
} PCIE_HPB_INFO;

// 示例
PCIE_HPB_INFO hpb_info = {
    .BusBase = 0x10,    // HPB_BASE = 总线 0x10
    .BusLimit = 0x1F,   // 总线范围：0x10 - 0x1F
    .Segment = 0
};
```

**在 ACPI/UEFI 中的使用：**

```c
// ACPI 热插拔支持
// HPB_BASE 用于标识哪个总线支持热插拔
#define HPB_BASE    0x10  // 热插拔总线起始编号

// 检查设备是否在热插拔总线上
bool is_on_hotplug_bus(struct pci_dev *dev)
{
    return (dev->bus->number >= HPB_BASE && 
            dev->bus->number <= HPB_LIMIT);
}
```

#### 3.2 PCIe Base 的含义

**PCIe Base 通常指的是内存映射地址：**

```c
// PCIe Root Port 寄存器基址
#define PCIE_ROOT_PORT_BASE    0xF8000000  // 物理地址

// PCIe 配置空间基址
#define PCIE_CFG_BASE          0xE0000000  // 配置空间基址

// 访问 PCIe 寄存器
void __iomem *pcie_base = ioremap(PCIE_ROOT_PORT_BASE, 0x100000);
u32 reg_value = readl(pcie_base + PCIE_REG_OFFSET);
```

**不同类型的 PCIe Base：**

```c
// 1. PCIe Root Port 控制器基址
#define PCIE_RC_BASE           0xF8000000  // Root Complex 基址

// 2. PCIe 配置空间基址
#define PCIE_CFG_BASE          0xE0000000  // 配置空间基址

// 3. PCIe Memory 空间基址
#define PCIE_MEM_BASE          0x80000000  // Memory 空间基址

// 4. PCIe IO 空间基址
#define PCIE_IO_BASE           0x10000000  // IO 空间基址
```

### 4. 实际应用场景

#### 4.1 HPB_BASE 的使用场景

```c
// UEFI 热插拔初始化
EFI_STATUS InitializeHotPlugBus(
    IN UINT8 HpbBase,      // HPB_BASE: 热插拔总线起始编号
    IN UINT8 HpbLimit      // 热插拔总线结束编号
)
{
    // 1. 标识热插拔总线范围
    // 2. 为热插拔总线分配资源（Memory/IO）
    // 3. 初始化热插拔控制器（HPC）
    
    for (UINT8 Bus = HpbBase; Bus <= HpbLimit; Bus++) {
        // 为每个热插拔总线分配资源
        AllocateResourcesForHotPlugBus(Bus);
    }
    
    return EFI_SUCCESS;
}

// 使用示例
InitializeHotPlugBus(0x10, 0x1F);  // HPB_BASE = 0x10
```

#### 4.2 PCIe Base 的使用场景

```c
// PCIe 控制器初始化
void pcie_controller_init(void)
{
    // 1. 映射 PCIe Root Port 寄存器
    void __iomem *pcie_base = ioremap(PCIE_RC_BASE, 0x100000);
    
    // 2. 配置 PCIe Root Port
    writel(0x12345678, pcie_base + PCIE_RC_CONFIG);
    
    // 3. 初始化 PCIe 链路
    pcie_link_init(pcie_base);
    
    // 4. 扫描 PCIe 总线
    pcie_scan_bus(pcie_base);
}
```

### 5. 关系图

```
┌─────────────────────────────────────────────────────────┐
│ PCIe 系统架构                                            │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ PCIe Root Complex (PCIe Base = 0xF8000000)            │
│    │                                                      │
│    ├─ Root Port 0                                        │
│    │    │                                                  │
│    │    └─ Bus 0x00 (普通总线)                            │
│    │                                                       │
│    ├─ Root Port 1                                        │
│    │    │                                                  │
│    │    └─ Bus 0x10 (HPB_BASE = 0x10) ← 热插拔总线      │
│    │         │                                             │
│    │         ├─ Bus 0x10 (热插拔插槽 1)                  │
│    │         ├─ Bus 0x11 (热插拔插槽 2)                  │
│    │         └─ Bus 0x12 (热插拔插槽 3)                  │
│    │                                                       │
│    └─ Root Port 2                                        │
│         │                                                  │
│         └─ Bus 0x20 (普通总线)                           │
│                                                           │
└─────────────────────────────────────────────────────────┘

说明：
- PCIe Base (0xF8000000): Root Complex 寄存器基址
- HPB_BASE (0x10): 热插拔总线起始编号
- 热插拔总线范围: 0x10 - 0x1F
```

### 6. 代码示例对比

#### 6.1 使用 HPB_BASE

```c
// 检查设备是否在热插拔总线上
bool is_hotplug_device(struct pci_dev *dev)
{
    // HPB_BASE 是总线编号
    #define HPB_BASE    0x10
    #define HPB_LIMIT   0x1F
    
    return (dev->bus->number >= HPB_BASE && 
            dev->bus->number <= HPB_LIMIT);
}

// 为热插拔总线分配资源
void allocate_hpb_resources(void)
{
    // HPB_BASE 指定总线范围
    for (int bus = HPB_BASE; bus <= HPB_LIMIT; bus++) {
        allocate_bus_resources(bus);
    }
}
```

#### 6.2 使用 PCIe Base

```c
// 访问 PCIe 控制器寄存器
void configure_pcie_controller(void)
{
    // PCIe Base 是内存地址
    #define PCIE_BASE   0xF8000000
    
    void __iomem *base = ioremap(PCIE_BASE, 0x100000);
    
    // 配置寄存器
    writel(0x12345678, base + PCIE_REG_OFFSET);
    
    iounmap(base);
}

// 访问 PCIe 配置空间
void access_pcie_config_space(void)
{
    // PCIe 配置空间基址
    #define PCIE_CFG_BASE   0xE0000000
    
    void __iomem *cfg_base = ioremap(PCIE_CFG_BASE, 0x10000000);
    
    // 读取设备配置
    u32 vendor_id = readl(cfg_base + (bus << 20) + (dev << 15) + 0x00);
    
    iounmap(cfg_base);
}
```

### 7. 总结

| 项目 | HPB_BASE | PCIe Base |
|------|----------|-----------|
| **本质** | 总线编号（Bus Number） | 内存地址（Memory Address） |
| **单位** | 总线编号（0-255） | 字节地址（32/64位） |
| **用途** | 标识热插拔总线范围 | 访问 PCIe 寄存器/配置空间 |
| **设置者** | 固件/BIOS | 硬件设计/固件映射 |
| **使用场景** | 热插拔管理、资源分配 | 寄存器访问、设备配置 |

**关键区别：**
- **HPB_BASE** = **总线编号**（逻辑概念）
- **PCIe Base** = **内存地址**（物理/虚拟地址）

**关系：**
- HPB_BASE 标识哪些总线支持热插拔
- PCIe Base 用于访问这些总线的硬件寄存器
- 两者配合使用：HPB_BASE 指定范围，PCIe Base 提供访问方式

## 十三、pcie_pex_slot 基地址说明

### 1. pcie_pex_slot 基地址的含义

**`pcie_pex_slot`** 通常指的是 **PCIe Express Slot 相关的基地址**，用于访问 PCIe 插槽的配置和控制寄存器。

**可能的基地址类型：**

1. **PCIe Root Port 寄存器基址**：用于访问 Root Port 控制器的寄存器
2. **PCIe 配置空间基址**：用于通过配置空间访问 Slot 寄存器
3. **PCIe 扩展能力基址**：PCIe Capability 结构的基址

### 2. 常见的基地址类型

#### 2.1 PCIe Root Port 控制器基址（最常见）

**这是最常用的基地址，用于访问 Slot Capabilities/Control/Status 寄存器：**

```c
// 典型的 PCIe Root Port 基址（平台相关）
#define PCIE_ROOT_PORT_BASE    0xF8000000  // 物理地址
// 或
#define PCIE_RC_BASE          0xF8000000  // Root Complex 基址

// 使用示例
void __iomem *pcie_base = ioremap(PCIE_ROOT_PORT_BASE, 0x100000);

// 访问 Slot Capabilities（通过配置空间）
// 但通常需要通过 PCIe 配置空间访问，而不是直接内存映射
```

#### 2.2 PCIe 配置空间基址

**通过 PCIe 配置空间访问 Slot 寄存器：**

```c
// PCIe 配置空间基址（平台相关）
#define PCIE_CFG_BASE          0xE0000000  // 配置空间基址

// 访问 Slot Capabilities Register
u32 slot_cap;
pci_read_config_dword(root_port_dev, 
    PCI_CAP_ID_EXP_OFFSET + PCI_EXP_SLTCAP, &slot_cap);
```

#### 2.3 PCIe Capability 结构基址

**PCIe Capability 结构在配置空间中的偏移：**

```c
// 1. 查找 PCIe Capability
int pos = pci_find_capability(dev, PCI_CAP_ID_EXP);
// pos 就是 PCIe Capability 的基址（在配置空间中的偏移）

// 2. 访问 Slot Capabilities（偏移 0x14）
u32 slot_cap;
pci_read_config_dword(dev, pos + PCI_EXP_SLTCAP, &slot_cap);
// pos + 0x14 就是 Slot Capabilities 的地址
```

### 3. 典型的基地址值（平台相关）

#### 3.1 ARM 平台示例

```c
// ARM 平台常见的 PCIe 基址
#define PCIE_RC_BASE           0x40000000  // Root Complex 基址
#define PCIE_CFG_BASE           0x50000000  // 配置空间基址
#define PCIE_MEM_BASE          0x60000000  // Memory 空间基址
#define PCIE_IO_BASE           0x70000000  // IO 空间基址
```

#### 3.2 x86 平台示例

```c
// x86 平台常见的 PCIe 基址
#define PCIE_RC_BASE           0xF8000000  // Root Complex 基址
#define PCIE_CFG_BASE          0xE0000000  // 配置空间基址（ECAM）
#define PCIE_MEM_BASE          0x80000000  // Memory 空间基址
#define PCIE_IO_BASE           0x10000000   // IO 空间基址
```

#### 3.3 嵌入式平台示例

```c
// 嵌入式 SoC 平台
#define PCIE_RC_BASE           0x18000000  // Root Complex 基址
#define PCIE_CFG_BASE          0x19000000  // 配置空间基址
```

### 4. 如何确定 pcie_pex_slot 的基地址

#### 4.1 从设备树获取

```dts
// 设备树示例
pcie@0 {
    compatible = "vendor,pcie-controller";
    reg = <0x0 0x40000000 0x0 0x1000000>;  // 基址和大小
    // ...
    
    pcie-slot@0 {
        reg = <0x0 0x40000000 0x0 0x100000>;  // Slot 基址
        // ...
    };
};
```

#### 4.2 从 ACPI 表获取

```c
// ACPI 表中有 PCIe 基址信息
// 通过 ACPI 解析获取
```

#### 4.3 从硬件手册获取

**通常需要查阅 SoC/芯片组的数据手册，找到 PCIe Root Port 的寄存器基址。**

### 5. 实际使用示例

#### 5.1 通过配置空间访问（推荐）

```c
// 这是最常用的方式
void configure_pcie_slot(struct pci_dev *root_port)
{
    int pos;
    u32 slot_cap;
    u16 slot_ctl;
    
    // 1. 查找 PCIe Capability（这就是"基址"）
    pos = pci_find_capability(root_port, PCI_CAP_ID_EXP);
    if (!pos)
        return;
    
    // pos 就是 PCIe Capability 的基址（配置空间偏移）
    // 例如：pos = 0x100（PCIe Capability 在配置空间偏移 0x100）
    
    // 2. 访问 Slot Capabilities（基址 + 偏移）
    pci_read_config_dword(root_port, pos + PCI_EXP_SLTCAP, &slot_cap);
    // 实际地址 = root_port 配置空间基址 + pos + 0x14
    
    // 3. 访问 Slot Control（基址 + 偏移）
    pci_read_config_word(root_port, pos + PCI_EXP_SLTCTL, &slot_ctl);
    // 实际地址 = root_port 配置空间基址 + pos + 0x18
}
```

#### 5.2 通过内存映射访问（某些平台）

```c
// 某些平台支持直接内存映射访问
void configure_pcie_slot_mmio(void)
{
    // PCIe Root Port 寄存器基址（从设备树或硬件手册获取）
    #define PCIE_SLOT_BASE  0xF8000000
    
    void __iomem *base = ioremap(PCIE_SLOT_BASE, 0x100000);
    
    // 直接访问寄存器（平台特定）
    u32 slot_cap = readl(base + PCIE_SLOT_CAP_OFFSET);
    u16 slot_ctl = readw(base + PCIE_SLOT_CTL_OFFSET);
    
    iounmap(base);
}
```

### 6. 基地址的层次结构

```
┌─────────────────────────────────────────────────────────┐
│ PCIe 地址层次结构                                        │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ PCIe 配置空间基址 (PCIE_CFG_BASE = 0xE0000000)         │
│    │                                                      │
│    └─ Root Port 配置空间 (Bus 0, Dev 0, Func 0)        │
│         │                                                  │
│         └─ PCIe Capability 基址 (pos = 0x100)           │
│              │                                              │
│              ├─ Device Capabilities (pos + 0x04)          │
│              ├─ Device Control (pos + 0x08)               │
│              ├─ Link Capabilities (pos + 0x0C)            │
│              ├─ Link Control (pos + 0x10)                │
│              ├─ Slot Capabilities (pos + 0x14) ← 这里！  │
│              ├─ Slot Control (pos + 0x18)                 │
│              └─ Slot Status (pos + 0x1A)                  │
│                                                           │
└─────────────────────────────────────────────────────────┘

实际访问地址计算：
Slot Capabilities 地址 = PCIE_CFG_BASE + (Bus << 20) + (Dev << 15) + (Func << 12) + pos + 0x14
```

### 7. 总结：pcie_pex_slot 基地址

**`pcie_pex_slot` 的基地址通常是：**

1. **PCIe Capability 结构的基址**（最常见）
   - 通过 `pci_find_capability()` 获取
   - 返回配置空间中的偏移（如 0x100）
   - 然后通过 `pos + PCI_EXP_SLTCAP` 访问 Slot 寄存器

2. **PCIe Root Port 寄存器基址**（某些平台）
   - 从设备树或硬件手册获取
   - 通常是物理地址（如 0xF8000000）
   - 需要 `ioremap()` 映射后访问

3. **PCIe 配置空间基址**（系统级）
   - 整个 PCIe 配置空间的基址
   - 用于计算具体设备的配置空间地址

**推荐方式：**
- **使用 `pci_find_capability()` 获取 PCIe Capability 基址**
- **然后通过配置空间访问 Slot 寄存器**
- **这是最标准和跨平台的方式**

## 十四、PCIe Capability 结构基址的设置者

### 1. 答案：硬件设置（由 PCIe 设备硬件决定）

**PCIe Capability 结构的基址是由硬件（PCIe 设备）设置的，不是软件设置的。**

### 2. 详细说明

#### 2.1 硬件设置机制

**PCIe Capability 结构的位置由以下因素决定：**

1. **硬件设计**：PCIe 设备硬件在制造时就确定了 Capability 结构的位置
2. **Capability List**：设备通过配置空间中的 Capability List 链表组织各种 Capability
3. **固定偏移**：Capability List 的起始位置在配置空间中有固定偏移

#### 2.2 PCIe 配置空间布局

```
┌─────────────────────────────────────────────────────────┐
│ PCIe 配置空间（256 字节或 4KB）                         │
├─────────────────────────────────────────────────────────┤
│ 0x00 - 0x3F: Standard PCI Header                       │
│   0x00: Vendor ID, Device ID                           │
│   0x04: Command, Status                                │
│   0x06: Status Register                                 │
│      bit[4]: Capabilities List (CAP) ← 硬件设置        │
│   0x34: Capability Pointer (如果 CAP=1)                │
│      ← 硬件设置，指向第一个 Capability 的偏移           │
│                                                          │
│ 0x40 - 0xFF: Capability List (如果 CAP=1)              │
│   └─ 由硬件组织的 Capability 链表                      │
│                                                          │
│ 0x100 - 0xFFF: Extended Capabilities (PCIe)           │
│   └─ PCIe Extended Capabilities                        │
└─────────────────────────────────────────────────────────┘
```

#### 2.3 Capability List 的组织方式

**硬件通过链表组织 Capability：**

```
配置空间偏移 0x34 (Capability Pointer)
  ↓ 硬件设置的值（如 0x80）
  ↓
配置空间偏移 0x80 (第一个 Capability)
  ├─ Byte 0: Capability ID (如 0x10 = PCIe)
  ├─ Byte 1: Next Capability Pointer (如 0xA0)
  └─ Byte 2-3: Capability 特定数据
       ↓
配置空间偏移 0xA0 (第二个 Capability)
  ├─ Byte 0: Capability ID
  ├─ Byte 1: Next Capability Pointer (如 0xC0)
  └─ ...
       ↓
配置空间偏移 0xC0 (第三个 Capability)
  ├─ Byte 0: Capability ID
  ├─ Byte 1: Next Capability Pointer (0x00 = 结束)
  └─ ...
```

### 3. 软件如何查找 Capability 基址

#### 3.1 Linux 内核的查找流程

```c
// Linux 内核查找 PCIe Capability 的流程
u8 pci_find_capability(struct pci_dev *dev, int cap)
{
    u8 pos;
    
    // 1. 查找 Capability List 的起始位置
    pos = __pci_bus_find_cap_start(dev->bus, dev->devfn, dev->hdr_type);
    // 返回配置空间偏移 0x34 的值（硬件设置）
    
    // 2. 遍历 Capability List，查找指定的 Capability ID
    if (pos)
        pos = __pci_find_next_cap(dev->bus, dev->devfn, pos, cap);
    // 遍历链表，查找 Capability ID = 0x10 (PCIe)
    
    return pos;  // 返回 PCIe Capability 的基址（配置空间偏移）
}
```

#### 3.2 查找过程详解

```c
// 步骤 1: 检查设备是否支持 Capability List
static u8 __pci_bus_find_cap_start(struct pci_bus *bus,
                                    unsigned int devfn, u8 hdr_type)
{
    u16 status;
    
    // 读取 Status Register (0x06)
    pci_bus_read_config_word(bus, devfn, PCI_STATUS, &status);
    
    // 检查 Capabilities List 位（硬件设置）
    if (!(status & PCI_STATUS_CAP_LIST))
        return 0;  // 设备不支持 Capability List
    
    // 返回 Capability Pointer 的偏移（硬件固定位置）
    switch (hdr_type) {
        case PCI_HEADER_TYPE_NORMAL:
        case PCI_HEADER_TYPE_BRIDGE:
            return PCI_CAPABILITY_LIST;  // 0x34
        case PCI_HEADER_TYPE_CARDBUS:
            return PCI_CB_CAPABILITY_LIST;  // 0x14
    }
    
    return 0;
}

// 步骤 2: 遍历 Capability List，查找指定的 Capability
static u8 __pci_find_next_cap_ttl(struct pci_bus *bus, unsigned int devfn,
                                   u8 pos, int cap, int *ttl)
{
    u8 id;
    u16 ent;
    
    // 从 Capability Pointer 开始（硬件设置的值，如 0x80）
    pci_bus_read_config_byte(bus, devfn, pos, &pos);
    
    // 遍历链表
    while ((*ttl)--) {
        if (pos < 0x40)
            break;
        
        pos &= ~3;  // 对齐到 4 字节边界
        
        // 读取 Capability 条目（2 字节）
        pci_bus_read_config_word(bus, devfn, pos, &ent);
        
        id = ent & 0xff;  // Capability ID
        if (id == 0xff)
            break;  // 无效条目
        if (id == cap)
            return pos;  // 找到！返回基址
        
        // 移动到下一个 Capability（硬件设置的 Next Pointer）
        pos = (ent >> 8);  // Next Capability Pointer
    }
    
    return 0;  // 未找到
}
```

### 4. 硬件设置的具体内容

#### 4.1 硬件在配置空间中设置的值

**硬件在设备制造时或初始化时设置：**

| 配置空间位置 | 字段 | 设置者 | 说明 |
|------------|------|--------|------|
| **0x06 bit[4]** | Capabilities List (CAP) | **硬件** | 是否支持 Capability List |
| **0x34** | Capability Pointer | **硬件** | 指向第一个 Capability 的偏移 |
| **Capability 偏移 + 0** | Capability ID | **硬件** | Capability 类型（0x10 = PCIe） |
| **Capability 偏移 + 1** | Next Pointer | **硬件** | 指向下一个 Capability 的偏移 |

#### 4.2 硬件设置的时机

**硬件设置 Capability 结构的时机：**

1. **设备制造时**：
   - 硬件设计时确定 Capability 结构的位置
   - 固件/ROM 中可能包含初始值

2. **设备复位时**：
   - PCIe 设备复位后，硬件自动初始化配置空间
   - Capability List 由硬件自动设置

3. **设备初始化时**：
   - 某些设备可能在初始化时动态设置
   - 但通常由硬件自动完成

### 5. 软件的角色

#### 5.1 软件只能读取，不能设置

**软件（操作系统/固件）的角色：**

```c
// ✅ 软件可以做的：
// 1. 读取硬件设置的 Capability Pointer
u8 cap_ptr;
pci_read_config_byte(dev, PCI_CAPABILITY_LIST, &cap_ptr);

// 2. 遍历 Capability List
u8 pos = cap_ptr;
while (pos) {
    u16 cap_entry;
    pci_read_config_word(dev, pos, &cap_entry);
    u8 cap_id = cap_entry & 0xFF;
    pos = (cap_entry >> 8) & 0xFF;
    
    if (cap_id == PCI_CAP_ID_EXP) {
        // 找到 PCIe Capability，pos 就是基址
        break;
    }
}

// ❌ 软件不能做的：
// 1. 不能修改 Capability Pointer（硬件设置）
// 2. 不能修改 Capability ID（硬件设置）
// 3. 不能修改 Next Pointer（硬件设置）
// 4. 不能改变 Capability 结构的位置（硬件决定）
```

#### 5.2 软件可以修改的内容

**虽然 Capability 基址由硬件设置，但软件可以修改 Capability 结构中的某些字段：**

```c
// 软件可以修改 PCIe Capability 结构中的某些寄存器：
// 1. Device Control Register (pos + 0x08)
u16 dev_ctl;
pci_read_config_word(dev, pos + PCI_EXP_DEVCTL, &dev_ctl);
dev_ctl |= PCI_EXP_DEVCTL_BCR_FLR;  // 软件可以设置
pci_write_config_word(dev, pos + PCI_EXP_DEVCTL, dev_ctl);

// 2. Link Control Register (pos + 0x10)
u16 lnk_ctl;
pci_read_config_word(dev, pos + PCI_EXP_LNKCTL, &lnk_ctl);
lnk_ctl |= PCI_EXP_LNKCTL_ASPM_L0S;  // 软件可以设置
pci_write_config_word(dev, pos + PCI_EXP_LNKCTL, lnk_ctl);

// 3. Slot Control Register (pos + 0x18)
u16 slot_ctl;
pci_read_config_word(dev, pos + PCI_EXP_SLTCTL, &slot_ctl);
slot_ctl |= PCI_EXP_SLTCTL_HPIE;  // 软件可以设置
pci_write_config_word(dev, pos + PCI_EXP_SLTCTL, slot_ctl);
```

### 6. 总结

| 项目 | 设置者 | 说明 |
|------|--------|------|
| **Capability List 支持位** | **硬件** | Status Register bit[4] |
| **Capability Pointer** | **硬件** | 配置空间 0x34 的值 |
| **Capability ID** | **硬件** | Capability 类型标识 |
| **Next Pointer** | **硬件** | 链表中的下一个 Capability 位置 |
| **Capability 基址** | **硬件** | 通过遍历链表找到的位置 |
| **Capability 寄存器内容** | **硬件/软件** | 某些寄存器软件可以修改 |

**关键点：**
- **PCIe Capability 结构的基址由硬件设置**
- **软件只能读取和查找，不能设置基址**
- **软件可以修改 Capability 结构中的某些控制寄存器**
- **基址的位置由硬件设计决定，软件通过遍历 Capability List 找到**

**查找流程：**
```
硬件设置 Capability Pointer (0x34) → 软件读取
  ↓
硬件组织 Capability List → 软件遍历
  ↓
硬件设置每个 Capability 的 ID 和 Next Pointer → 软件查找
  ↓
找到 PCIe Capability (ID = 0x10) → 返回基址
```

## 十六、PCIe 控制器与 AXI Bridge 基址关系

### 1. 问题：AXI Bridge 的基址是 PCIe 控制器的基址吗？

**答案：取决于硬件设计，可能是同一个，也可能是不同的。**

### 2. 两种常见的硬件设计

#### 2.1 集成设计（共享基址）

**某些 SoC 将 PCIe 控制器和 AXI Bridge 集成在一起：**

```
┌─────────────────────────────────────────────────────────┐
│ PCIe 控制器 + AXI Bridge（集成设计）                    │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ PCIe Controller Base (0xF8000000)                      │
│    ├─ PCIe Core Registers (0x900000 - 0x9FFFFF)        │
│    ├─ AXI Bridge Registers (0x0 - 0xFFFFF)              │
│    └─ PCIe Config Space (通过 AXI Bridge 访问)          │
│                                                          │
│ AXI Bridge Base = PCIe Controller Base                 │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

**特点：**
- AXI Bridge 和 PCIe 控制器共享同一个基址
- 通过不同的偏移访问不同的功能模块
- 例如：Rockchip 某些平台的设计

#### 2.2 分离设计（独立基址）

**某些 SoC 将 PCIe 控制器和 AXI Bridge 分离：**

```
┌─────────────────────────────────────────────────────────┐
│ PCIe 控制器和 AXI Bridge（分离设计）                    │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ PCIe Controller Base (0xF8000000)                      │
│    └─ PCIe Core Registers                               │
│                                                          │
│ AXI Bridge Base (0xF9000000)                            │
│    └─ AXI Bridge Registers                              │
│    └─ PCIe Config Space Access                          │
│                                                          │
│ 两个独立的基址                                           │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

**特点：**
- AXI Bridge 有独立的基址
- 需要分别映射两个地址空间
- 例如：某些 Xilinx、Altera 平台的设计

### 3. 如何确定 AXI Bridge 的基址

#### 3.1 从设备树获取（推荐）

```dts
// 设备树示例 - 集成设计
pcie@f8000000 {
    compatible = "vendor,pcie-controller";
    reg = <0x0 0xf8000000 0x0 0x1000000>;  // PCIe 控制器基址
    
    // AXI Bridge 使用同一个基址，通过偏移访问
    // 或者明确指定 AXI Bridge 基址
    axi-base = <0x0 0xf8000000 0x0 0x100000>;  // AXI Bridge 基址
    apb-base = <0x0 0xf8100000 0x0 0x10000>;   // APB 基址
};

// 设备树示例 - 分离设计
pcie@f8000000 {
    compatible = "vendor,pcie-controller";
    reg = <0x0 0xf8000000 0x0 0x100000>;  // PCIe 控制器基址
};

axi-bridge@f9000000 {
    compatible = "vendor,axi-bridge";
    reg = <0x0 0xf9000000 0x0 0x100000>;  // AXI Bridge 独立基址
};
```

#### 3.2 从硬件手册获取

**查阅 SoC 数据手册，查找：**
- PCIe Controller Register Map
- AXI Bridge Register Map
- 确认它们是否共享基址

#### 3.3 代码中的处理方式

```c
// Rockchip PCIe 控制器的处理方式（集成设计）
int rockchip_pcie_parse_dt(struct rockchip_pcie *rockchip)
{
    struct resource *regs;
    
    if (rockchip->is_rc) {
        // 从设备树获取 "axi-base" 资源
        regs = platform_get_resource_byname(pdev,
                                            IORESOURCE_MEM,
                                            "axi-base");
        // AXI Bridge 基址映射为 reg_base
        rockchip->reg_base = devm_pci_remap_cfg_resource(dev, regs);
        // reg_base 既是 PCIe 控制器基址，也是 AXI Bridge 基址
    }
    
    // APB 基址是独立的
    rockchip->apb_base =
        devm_platform_ioremap_resource_byname(pdev, "apb-base");
    
    return 0;
}
```

### 4. AXI Bridge 的作用

**AXI Bridge 的主要功能：**

1. **地址转换**：PCIe 地址 ↔ AXI 地址
2. **配置空间访问**：通过 AXI 总线访问 PCIe 配置空间
3. **事务转换**：PCIe 事务 ↔ AXI 事务
4. **窗口管理**：管理地址转换窗口（ATR - Address Translation Register）

### 5. 基址使用示例

#### 5.1 集成设计（共享基址）

```c
// AXI Bridge 和 PCIe 控制器共享基址
#define PCIE_CONTROLLER_BASE    0xF8000000
#define AXI_BRIDGE_BASE         PCIE_CONTROLLER_BASE  // 同一个基址

// 访问 PCIe Core 寄存器
void __iomem *pcie_base = ioremap(PCIE_CONTROLLER_BASE, 0x1000000);
writel(value, pcie_base + PCIE_CORE_CTRL_OFFSET);

// 访问 AXI Bridge 寄存器（使用同一个基址，不同偏移）
writel(value, pcie_base + AXI_BRIDGE_CTRL_OFFSET);

// 访问 PCIe 配置空间（通过 AXI Bridge）
pci_read_config_dword(dev, offset, &value);
// 内部会通过 AXI Bridge 访问配置空间
```

#### 5.2 分离设计（独立基址）

```c
// AXI Bridge 和 PCIe 控制器有独立基址
#define PCIE_CONTROLLER_BASE    0xF8000000
#define AXI_BRIDGE_BASE         0xF9000000  // 不同的基址

// 访问 PCIe Core 寄存器
void __iomem *pcie_base = ioremap(PCIE_CONTROLLER_BASE, 0x100000);
writel(value, pcie_base + PCIE_CORE_CTRL_OFFSET);

// 访问 AXI Bridge 寄存器（使用独立基址）
void __iomem *axi_base = ioremap(AXI_BRIDGE_BASE, 0x100000);
writel(value, axi_base + AXI_BRIDGE_CTRL_OFFSET);

// 访问 PCIe 配置空间（通过 AXI Bridge）
// 需要通过 AXI Bridge 基址访问
```

### 6. 在 ATF 中的处理

#### 6.1 ATF 中获取基址

```c
// ATF 中获取 PCIe 控制器和 AXI Bridge 基址
void atf_get_pcie_axi_bridge_base(struct pcie_controller *ctrl)
{
    // 方法 1: 从设备树获取
    #ifdef DT_SUPPORT
    ctrl->pcie_base = dt_get_pcie_base(ctrl->node);
    ctrl->axi_bridge_base = dt_get_axi_bridge_base(ctrl->node);
    // 如果设备树中没有 axi-bridge-base，可能和 pcie_base 相同
    if (ctrl->axi_bridge_base == 0) {
        ctrl->axi_bridge_base = ctrl->pcie_base;  // 共享基址
    }
    #endif
    
    // 方法 2: 从平台配置获取（硬编码）
    #ifdef PLATFORM_CONFIG
    ctrl->pcie_base = PLAT_PCIE_CONTROLLER_BASE;
    ctrl->axi_bridge_base = PLAT_PCIE_AXI_BRIDGE_BASE;
    // 如果未定义 AXI_BRIDGE_BASE，使用 PCIe 基址
    if (ctrl->axi_bridge_base == 0) {
        ctrl->axi_bridge_base = ctrl->pcie_base;
    }
    #endif
}
```

#### 6.2 ATF 中配置 AXI Bridge

```c
// ATF 中配置 AXI Bridge（使用正确的基址）
void atf_configure_axi_bridge(struct pcie_controller *ctrl)
{
    void __iomem *axi_base;
    
    // 使用 AXI Bridge 基址（可能与 PCIe 基址相同或不同）
    if (ctrl->axi_bridge_base == ctrl->pcie_base) {
        // 集成设计：使用同一个基址
        axi_base = ioremap(ctrl->pcie_base, 0x1000000);
    } else {
        // 分离设计：使用独立基址
        axi_base = ioremap(ctrl->axi_bridge_base, 0x100000);
    }
    
    // 配置 AXI Bridge 寄存器
    // 例如：配置地址转换窗口
    writel(0x80000000, axi_base + AXI_BRIDGE_WINDOW0_BASE);
    writel(0x10000000, axi_base + AXI_BRIDGE_WINDOW0_SIZE);
    
    // 配置 PCIe 配置空间访问
    writel(0xE0000000, axi_base + AXI_BRIDGE_CFG_BASE);
}
```

### 7. 判断方法

#### 7.1 检查设备树

```dts
// 检查设备树中是否有独立的 axi-bridge 节点
// 如果有 → 分离设计，独立基址
// 如果没有 → 集成设计，共享基址
```

#### 7.2 检查寄存器映射

```c
// 读取寄存器，检查地址空间
// 如果 PCIe 控制器基址 + 偏移可以访问 AXI Bridge 寄存器
// → 集成设计，共享基址
// 如果必须使用不同的基址
// → 分离设计，独立基址
```

#### 7.3 检查硬件手册

**查阅 SoC 数据手册：**
- 查看 PCIe Controller Register Map
- 查看 AXI Bridge Register Map
- 确认地址空间是否重叠

### 8. 总结

| 设计类型 | AXI Bridge 基址 | 说明 |
|---------|----------------|------|
| **集成设计** | **与 PCIe 控制器基址相同** | 共享地址空间，通过偏移区分 |
| **分离设计** | **独立的基址** | 需要分别映射两个地址空间 |

**判断方法：**
1. 查看设备树配置
2. 查看硬件手册
3. 测试寄存器访问

**在 ATF 中的处理：**
- 优先从设备树获取基址
- 如果没有独立的 AXI Bridge 基址，使用 PCIe 控制器基址
- 根据实际硬件设计选择正确的基址

**关键点：**
- **不是所有平台都相同**：不同 SoC 的设计可能不同
- **需要查看硬件手册**：确认具体的地址映射
- **设备树是权威来源**：设备树中的配置最准确

## 十五、多 PCIe 控制器环境下的 Slot 配置（ATF 初始化）

### 1. 问题：6 个 PCIe 控制器是否都需要设置 Slot？

**答案：不是所有控制器都需要设置 Slot！**

**需要设置 Slot 的控制器：**
- ✅ **Root Port**（根端口）：有物理插槽，需要设置 Slot Capabilities
- ✅ **Switch Downstream Port**（交换机下游端口）：有物理插槽，需要设置 Slot Capabilities

**不需要设置 Slot 的控制器：**
- ❌ **Switch Upstream Port**（交换机上游端口）：无物理插槽，不需要设置
- ❌ **Endpoint**（终端设备）：不是 Bridge，不需要设置

### 2. 如何识别需要设置 Slot 的控制器

#### 2.1 检查控制器类型

```c
// ATF 中检查 PCIe 控制器类型
bool needs_slot_configuration(struct pcie_controller *ctrl)
{
    u16 vendor_id, device_id;
    u8 header_type;
    u32 slot_cap;
    int pos;
    
    // 1. 读取 Vendor ID 和 Device ID
    pci_read_config_word(ctrl->dev, PCI_VENDOR_ID, &vendor_id);
    pci_read_config_word(ctrl->dev, PCI_DEVICE_ID, &device_id);
    
    // 2. 读取 Header Type
    pci_read_config_byte(ctrl->dev, PCI_HEADER_TYPE, &header_type);
    header_type &= 0x7F;
    
    // 3. 检查是否是 Bridge（Root Port 或 Switch Port）
    if (header_type != PCI_HEADER_TYPE_BRIDGE) {
        return false;  // 不是 Bridge，不需要设置 Slot
    }
    
    // 4. 查找 PCIe Capability
    pos = pci_find_capability(ctrl->dev, PCI_CAP_ID_EXP);
    if (!pos) {
        return false;  // 不是 PCIe 设备
    }
    
    // 5. 检查 Slot Implemented 位（在 PCIe Capability Register 中）
    u16 pcie_cap;
    pci_read_config_word(ctrl->dev, pos + PCI_EXP_FLAGS, &pcie_cap);
    
    // Slot Implemented 位在 PCIe Capability Register 的 bit[8]
    if (!(pcie_cap & PCI_EXP_FLAGS_SLOT)) {
        return false;  // 没有实现 Slot，不需要设置
    }
    
    // 6. 读取 Slot Capabilities，检查是否有 Slot
    pci_read_config_dword(ctrl->dev, pos + PCI_EXP_SLTCAP, &slot_cap);
    
    // 如果 Slot Capabilities 全为 0，可能没有 Slot
    if (slot_cap == 0) {
        return false;
    }
    
    return true;  // 需要设置 Slot
}
```

#### 2.2 识别 Root Port vs Switch Port

```c
// 识别 Root Port（通常需要设置 Slot）
bool is_root_port(struct pcie_controller *ctrl)
{
    u16 vendor_id, device_id;
    
    pci_read_config_word(ctrl->dev, PCI_VENDOR_ID, &vendor_id);
    pci_read_config_word(ctrl->dev, PCI_DEVICE_ID, &device_id);
    
    // Root Port 通常由 SoC 厂商提供
    // 例如：Intel、AMD、ARM SoC 厂商的 Root Port
    // 需要根据你的平台判断
    
    // 检查是否是 Root Port（通常在 Bus 0）
    if (ctrl->dev->bus->number == 0) {
        // 可能是 Root Port
        return true;
    }
    
    return false;
}

// 识别 Switch Downstream Port（需要设置 Slot）
bool is_switch_downstream_port(struct pcie_controller *ctrl)
{
    // Switch Downstream Port 通常：
    // 1. 是 Bridge（Header Type = 1）
    // 2. 有 Slot Implemented
    // 3. 在 Switch 的下游
    
    u8 header_type;
    pci_read_config_byte(ctrl->dev, PCI_HEADER_TYPE, &header_type);
    
    if ((header_type & 0x7F) != PCI_HEADER_TYPE_BRIDGE) {
        return false;
    }
    
    // 检查是否有 Slot
    int pos = pci_find_capability(ctrl->dev, PCI_CAP_ID_EXP);
    if (!pos)
        return false;
    
    u16 pcie_cap;
    pci_read_config_word(ctrl->dev, pos + PCI_EXP_FLAGS, &pcie_cap);
    
    return (pcie_cap & PCI_EXP_FLAGS_SLOT) != 0;
}
```

### 3. ATF 中初始化多个 PCIe 控制器的 Slot

#### 3.1 遍历所有 PCIe 控制器

```c
// ATF 中初始化所有 PCIe 控制器的 Slot
void atf_init_all_pcie_slots(void)
{
    struct pcie_controller controllers[6];
    int i;
    
    // 1. 枚举所有 PCIe 控制器
    // 假设你已经识别了 6 个控制器
    for (i = 0; i < 6; i++) {
        controllers[i] = get_pcie_controller(i);
    }
    
    // 2. 为每个控制器配置 Slot（如果需要）
    for (i = 0; i < 6; i++) {
        if (needs_slot_configuration(&controllers[i])) {
            INFO("Configuring Slot for PCIe Controller %d\n", i);
            configure_pcie_slot(&controllers[i], i);
        } else {
            INFO("Skipping Slot configuration for PCIe Controller %d (no slot)\n", i);
        }
    }
}
```

#### 3.2 为单个控制器配置 Slot

```c
// ATF 中配置单个 PCIe 控制器的 Slot
void configure_pcie_slot(struct pcie_controller *ctrl, int controller_id)
{
    int pos;
    u32 slot_cap;
    u16 slot_ctl;
    u16 pcie_cap;
    
    // 1. 查找 PCIe Capability
    pos = pci_find_capability(ctrl->dev, PCI_CAP_ID_EXP);
    if (!pos) {
        ERROR("PCIe Capability not found for controller %d\n", controller_id);
        return;
    }
    
    // 2. 检查 Slot Implemented
    pci_read_config_word(ctrl->dev, pos + PCI_EXP_FLAGS, &pcie_cap);
    if (!(pcie_cap & PCI_EXP_FLAGS_SLOT)) {
        WARN("Controller %d does not implement Slot\n", controller_id);
        return;
    }
    
    // 3. 读取当前 Slot Capabilities（只读，由硬件/固件设置）
    pci_read_config_dword(ctrl->dev, pos + PCI_EXP_SLTCAP, &slot_cap);
    
    INFO("Controller %d Slot Capabilities: 0x%08x\n", controller_id, slot_cap);
    
    // 4. 如果需要，可以设置 Slot Capabilities（某些平台允许）
    // 注意：根据 PCIe 规范，Slot Capabilities 应该是只读的
    // 但某些平台在固件阶段可能允许修改
    if (platform_allows_slot_cap_modification()) {
        // 设置物理插槽编号（PSN）
        slot_cap = (slot_cap & ~PCI_EXP_SLTCAP_PSN) | 
                   ((controller_id + 1) << 19);  // PSN = controller_id + 1
        
        // 设置其他能力位（根据硬件支持）
        slot_cap |= PCI_EXP_SLTCAP_HPC;   // 热插拔能力
        slot_cap |= PCI_EXP_SLTCAP_PCP;   // 电源控制器
        
        pci_write_config_dword(ctrl->dev, pos + PCI_EXP_SLTCAP, slot_cap);
    }
    
    // 5. 配置 Slot Control Register（软件可以设置）
    pci_read_config_word(ctrl->dev, pos + PCI_EXP_SLTCTL, &slot_ctl);
    
    // 根据 Slot Capabilities 启用相应的功能
    if (slot_cap & PCI_EXP_SLTCAP_HPC) {
        slot_ctl |= PCI_EXP_SLTCTL_HPIE;  // 启用热插拔中断
    }
    
    if (slot_cap & PCI_EXP_SLTCAP_ABP) {
        slot_ctl |= PCI_EXP_SLTCTL_ABPE;  // 启用 Attention Button 中断
    }
    
    if (slot_cap & PCI_EXP_SLTCAP_PCP) {
        slot_ctl &= ~PCI_EXP_SLTCTL_PCC;  // 上电（如果需要）
    }
    
    pci_write_config_word(ctrl->dev, pos + PCI_EXP_SLTCTL, slot_ctl);
    
    INFO("Controller %d Slot configured successfully\n", controller_id);
}
```

### 4. 完整的 ATF 初始化流程

#### 4.1 推荐的初始化顺序

```c
// ATF 中完整的 PCIe 初始化流程
void atf_pcie_init_all(void)
{
    struct pcie_controller controllers[6];
    int i;
    
    INFO("Initializing 6 PCIe controllers...\n");
    
    // 步骤 1: 枚举所有 PCIe 控制器
    for (i = 0; i < 6; i++) {
        controllers[i] = discover_pcie_controller(i);
        if (!controllers[i].dev) {
            ERROR("Failed to discover PCIe controller %d\n", i);
            continue;
        }
        
        INFO("Found PCIe controller %d: Bus %d, Dev %d, Func %d\n",
             i, controllers[i].dev->bus->number,
             PCI_SLOT(controllers[i].dev->devfn),
             PCI_FUNC(controllers[i].dev->devfn));
    }
    
    // 步骤 2: 初始化每个控制器的基础功能
    for (i = 0; i < 6; i++) {
        if (controllers[i].dev) {
            pcie_controller_init(&controllers[i]);
        }
    }
    
    // 步骤 3: 配置 Slot（仅对有 Slot 的控制器）
    for (i = 0; i < 6; i++) {
        if (controllers[i].dev && needs_slot_configuration(&controllers[i])) {
            INFO("Configuring Slot for controller %d\n", i);
            configure_pcie_slot(&controllers[i], i);
        } else {
            INFO("Skipping Slot config for controller %d (no slot)\n", i);
        }
    }
    
    // 步骤 4: 扫描 PCIe 总线
    for (i = 0; i < 6; i++) {
        if (controllers[i].dev) {
            pcie_bus_scan(&controllers[i]);
        }
    }
    
    INFO("PCIe initialization completed\n");
}
```

### 5. 判断是否需要设置 Slot 的决策流程

```
┌─────────────────────────────────────────────────────────┐
│ 对于每个 PCIe 控制器                                     │
└─────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────┐
│ 步骤 1: 检查 Header Type                                │
│ - Header Type = Bridge?                                 │
│   ├─ 是 → 继续                                          │
│   └─ 否 → 跳过（不是 Bridge，不需要 Slot）              │
└─────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────┐
│ 步骤 2: 检查 PCIe Capability                            │
│ - 有 PCIe Capability?                                   │
│   ├─ 是 → 继续                                          │
│   └─ 否 → 跳过（不是 PCIe 设备）                        │
└─────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────┐
│ 步骤 3: 检查 Slot Implemented                          │
│ - PCIe Capability Register bit[8] = 1?                 │
│   ├─ 是 → 继续                                          │
│   └─ 否 → 跳过（没有实现 Slot）                         │
└─────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────┐
│ 步骤 4: 检查 Slot Capabilities                          │
│ - Slot Capabilities Register != 0?                      │
│   ├─ 是 → ✅ 需要设置 Slot                              │
│   └─ 否 → 跳过（Slot 未实现）                           │
└─────────────────────────────────────────────────────────┘
```

### 6. 实际代码示例（ATF 风格）

```c
// ATF 风格的 PCIe Slot 配置
#include <common/debug.h>
#include <drivers/pci/pci.h>

// 检查控制器是否需要 Slot 配置
static bool pcie_controller_has_slot(struct pcie_controller *ctrl)
{
    uint8_t header_type;
    uint16_t pcie_cap;
    uint32_t slot_cap;
    int pos;
    
    // 1. 检查 Header Type
    pci_read_config_byte(ctrl->dev, PCI_HEADER_TYPE, &header_type);
    if ((header_type & 0x7F) != PCI_HEADER_TYPE_BRIDGE) {
        return false;
    }
    
    // 2. 查找 PCIe Capability
    pos = pci_find_capability(ctrl->dev, PCI_CAP_ID_EXP);
    if (!pos) {
        return false;
    }
    
    // 3. 检查 Slot Implemented
    pci_read_config_word(ctrl->dev, pos + PCI_EXP_FLAGS, &pcie_cap);
    if (!(pcie_cap & PCI_EXP_FLAGS_SLOT)) {
        return false;
    }
    
    // 4. 检查 Slot Capabilities
    pci_read_config_dword(ctrl->dev, pos + PCI_EXP_SLTCAP, &slot_cap);
    if (slot_cap == 0) {
        return false;
    }
    
    return true;
}

// 配置单个控制器的 Slot
static void pcie_configure_slot(struct pcie_controller *ctrl, int id)
{
    int pos;
    uint32_t slot_cap;
    uint16_t slot_ctl;
    
    pos = pci_find_capability(ctrl->dev, PCI_CAP_ID_EXP);
    if (!pos) {
        ERROR("PCIe Capability not found for controller %d\n", id);
        return;
    }
    
    // 读取 Slot Capabilities
    pci_read_config_dword(ctrl->dev, pos + PCI_EXP_SLTCAP, &slot_cap);
    INFO("Controller %d: Slot Capabilities = 0x%08x\n", id, slot_cap);
    
    // 配置 Slot Control
    pci_read_config_word(ctrl->dev, pos + PCI_EXP_SLTCTL, &slot_ctl);
    
    if (slot_cap & PCI_EXP_SLTCAP_HPC) {
        slot_ctl |= PCI_EXP_SLTCTL_HPIE;
    }
    
    pci_write_config_word(ctrl->dev, pos + PCI_EXP_SLTCTL, slot_ctl);
    
    INFO("Controller %d: Slot configured\n", id);
}

// 初始化所有 PCIe 控制器的 Slot
void plat_pcie_slot_init(void)
{
    struct pcie_controller *controllers;
    int num_controllers = 6;
    int i;
    
    // 获取所有 PCIe 控制器（假设你已经有了这个列表）
    controllers = get_pcie_controllers();
    
    for (i = 0; i < num_controllers; i++) {
        if (pcie_controller_has_slot(&controllers[i])) {
            INFO("Configuring Slot for PCIe Controller %d\n", i);
            pcie_configure_slot(&controllers[i], i);
        } else {
            VERBOSE("Skipping Slot config for Controller %d (no slot)\n", i);
        }
    }
}
```

### 7. 总结和建议

**对于你的 6 个 PCIe 控制器：**

1. **不是所有都需要设置 Slot**
   - 只有有物理插槽的 Root Port 或 Switch Downstream Port 需要
   - Switch Upstream Port 不需要

2. **判断方法**
   - 检查 Header Type（必须是 Bridge）
   - 检查 Slot Implemented 位
   - 检查 Slot Capabilities 寄存器

3. **在 ATF 中的处理**
   - 遍历所有控制器
   - 对每个控制器检查是否需要 Slot
   - 只为需要的控制器配置 Slot

4. **推荐流程**
   ```
   枚举 6 个控制器
     ↓
   对每个控制器：
     ├─ 检查是否有 Slot → 是 → 配置 Slot
     └─ 检查是否有 Slot → 否 → 跳过
   ```

**关键点：**
- **先检查，再配置**：不是所有控制器都需要 Slot 配置
- **根据硬件特性**：只有实现了 Slot 的控制器才需要配置
- **避免错误配置**：不要为没有 Slot 的控制器设置 Slot 寄存器

## 十七、PCIe 配置空间的固定布局

### 1. 答案：是的，前面 64 字节是固定的

**PCIe 配置空间的前 64 字节（0x00 - 0x3F）是 PCI/PCIe 规范标准化的固定布局。**

### 2. PCIe 配置空间布局

```
┌─────────────────────────────────────────────────────────┐
│ PCIe 配置空间（256 字节或 4KB）                         │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ 0x00 - 0x3F: Standard PCI Header（固定布局）            │
│   ├─ 0x00: Vendor ID, Device ID                        │
│   ├─ 0x04: Command, Status                              │
│   ├─ 0x08: Revision ID, Class Code                      │
│   ├─ 0x0C: Cache Line Size, Latency Timer              │
│   ├─ 0x0E: Header Type                                  │
│   ├─ 0x0F: BIST                                         │
│   ├─ 0x10 - 0x27: Base Address Registers (BARs)        │
│   ├─ 0x28 - 0x2B: CardBus CIS Pointer                   │
│   ├─ 0x2C - 0x2F: Subsystem Vendor/Device ID            │
│   ├─ 0x30 - 0x33: Expansion ROM Base Address            │
│   ├─ 0x34: Capability Pointer（固定位置）               │
│   ├─ 0x35 - 0x3B: Reserved                              │
│   └─ 0x3C - 0x3F: Interrupt Line/Pin, Min_Gnt, Max_Lat │
│                                                          │
│ 0x40 - 0xFF: Device-Specific（设备特定，不固定）       │
│                                                          │
│ 0x100 - 0xFFF: Extended Capabilities（PCIe 扩展能力）  │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### 3. 固定布局的详细说明

#### 3.1 标准 PCI Header（0x00 - 0x3F）

**这 64 字节是 PCI/PCIe 规范强制要求的固定布局：**

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| **0x00** | 2 | Vendor ID | 厂商 ID（固定位置） |
| **0x02** | 2 | Device ID | 设备 ID（固定位置） |
| **0x04** | 2 | Command | 命令寄存器（固定位置） |
| **0x06** | 2 | Status | 状态寄存器（固定位置） |
| **0x08** | 1 | Revision ID | 修订版本（固定位置） |
| **0x09** | 1 | Class Code[0] | 类代码（固定位置） |
| **0x0A** | 1 | Class Code[1] | 类代码（固定位置） |
| **0x0B** | 1 | Class Code[2] | 类代码（固定位置） |
| **0x0C** | 1 | Cache Line Size | 缓存行大小（固定位置） |
| **0x0D** | 1 | Latency Timer | 延迟定时器（固定位置） |
| **0x0E** | 1 | Header Type | 头部类型（固定位置） |
| **0x0F** | 1 | BIST | 内置自检（固定位置） |
| **0x10** | 4 | BAR 0 | 基址寄存器 0（固定位置） |
| **0x14** | 4 | BAR 1 | 基址寄存器 1（固定位置） |
| **0x18** | 4 | BAR 2 | 基址寄存器 2（固定位置） |
| **0x1C** | 4 | BAR 3 | 基址寄存器 3（固定位置） |
| **0x20** | 4 | BAR 4 | 基址寄存器 4（固定位置） |
| **0x24** | 4 | BAR 5 | 基址寄存器 5（固定位置） |
| **0x28** | 4 | CardBus CIS | CardBus CIS 指针（固定位置） |
| **0x2C** | 2 | Subsystem Vendor ID | 子系统厂商 ID（固定位置） |
| **0x2E** | 2 | Subsystem ID | 子系统 ID（固定位置） |
| **0x30** | 4 | Expansion ROM | 扩展 ROM 基址（固定位置） |
| **0x34** | 1 | Capability Pointer | 能力列表指针（固定位置） |
| **0x35** | 1 | Reserved | 保留（固定位置） |
| **0x36** | 1 | Reserved | 保留（固定位置） |
| **0x37** | 1 | Reserved | 保留（固定位置） |
| **0x38** | 1 | Reserved | 保留（固定位置） |
| **0x39** | 1 | Reserved | 保留（固定位置） |
| **0x3A** | 1 | Reserved | 保留（固定位置） |
| **0x3B** | 1 | Reserved | 保留（固定位置） |
| **0x3C** | 1 | Interrupt Line | 中断线（固定位置） |
| **0x3D** | 1 | Interrupt Pin | 中断引脚（固定位置） |
| **0x3E** | 1 | Min_Gnt | 最小授权（固定位置） |
| **0x3F** | 1 | Max_Lat | 最大延迟（固定位置） |

#### 3.2 为什么前面是固定的？

**PCI/PCIe 规范要求前 64 字节固定布局的原因：**

1. **兼容性**：所有 PCI/PCIe 设备都有相同的标准头部
2. **枚举**：系统可以通过固定位置快速识别设备
3. **标准化**：软件可以依赖固定的偏移访问标准寄存器
4. **向后兼容**：PCI 和 PCIe 设备共享相同的标准头部

### 4. 固定布局的代码定义

```c
// Linux 内核中的定义（include/uapi/linux/pci_regs.h）

// 标准头部大小
#define PCI_STD_HEADER_SIZEOF    64  // 前 64 字节是固定的

// 固定位置的寄存器定义
#define PCI_VENDOR_ID            0x00  // 固定位置
#define PCI_DEVICE_ID            0x02  // 固定位置
#define PCI_COMMAND              0x04  // 固定位置
#define PCI_STATUS               0x06  // 固定位置
#define PCI_REVISION_ID          0x08  // 固定位置
#define PCI_CLASS_PROG           0x09  // 固定位置
#define PCI_CLASS_DEVICE         0x0a  // 固定位置
#define PCI_CACHE_LINE_SIZE      0x0c  // 固定位置
#define PCI_LATENCY_TIMER        0x0d  // 固定位置
#define PCI_HEADER_TYPE          0x0e  // 固定位置
#define PCI_BIST                 0x0f  // 固定位置

// BARs（固定位置）
#define PCI_BASE_ADDRESS_0       0x10  // 固定位置
#define PCI_BASE_ADDRESS_1       0x14  // 固定位置
#define PCI_BASE_ADDRESS_2       0x18  // 固定位置
#define PCI_BASE_ADDRESS_3       0x1c  // 固定位置
#define PCI_BASE_ADDRESS_4       0x20  // 固定位置
#define PCI_BASE_ADDRESS_5       0x24  // 固定位置

// Capability Pointer（固定位置）
#define PCI_CAPABILITY_LIST      0x34  // 固定位置！

// 中断相关（固定位置）
#define PCI_INTERRUPT_LINE       0x3c  // 固定位置
#define PCI_INTERRUPT_PIN        0x3d  // 固定位置
```

### 5. Capability Pointer 的位置是固定的

**关键点：Capability Pointer 在 0x34 是固定的！**

```c
// Capability Pointer 的位置是 PCI 规范强制要求的
#define PCI_CAPABILITY_LIST      0x34  // 固定位置

// 软件总是从这个固定位置读取 Capability Pointer
u8 cap_ptr;
pci_read_config_byte(dev, PCI_CAPABILITY_LIST, &cap_ptr);
// 如果 cap_ptr != 0，说明有 Capability List
// cap_ptr 指向第一个 Capability 的偏移（如 0x80）
```

### 6. 固定布局 vs 可变布局

#### 6.1 固定部分（0x00 - 0x3F）

```
✅ 所有 PCI/PCIe 设备都相同
✅ 偏移位置固定
✅ 字段含义固定
✅ 软件可以依赖这些固定位置
```

#### 6.2 可变部分（0x40 - 0xFF）

```
⚠️ 设备特定
⚠️ 不同设备可能不同
⚠️ 某些设备可能为空
```

#### 6.3 扩展能力（0x100 - 0xFFF）

```
⚠️ PCIe 设备特有
⚠️ 通过链表组织
⚠️ 位置不固定（通过链表查找）
```

### 7. 实际应用

#### 7.1 为什么 Capability 基址需要查找？

**虽然 Capability Pointer 的位置是固定的（0x34），但 Capability 结构本身的位置不固定：**

```c
// 1. Capability Pointer 的位置是固定的（0x34）
u8 cap_ptr;
pci_read_config_byte(dev, PCI_CAPABILITY_LIST, &cap_ptr);
// cap_ptr = 0x80（例如，由硬件设置）

// 2. 但 Capability 结构的位置不固定（由硬件决定）
// PCIe Capability 可能在 0x80、0xA0、0xC0 等位置
// 需要通过遍历链表找到

// 3. 查找 PCIe Capability
int pos = pci_find_capability(dev, PCI_CAP_ID_EXP);
// pos = 0x80（例如，通过遍历链表找到）
```

#### 7.2 固定布局的优势

```c
// 因为前 64 字节是固定的，软件可以：
// 1. 直接访问标准寄存器
u16 vendor_id;
pci_read_config_word(dev, PCI_VENDOR_ID, &vendor_id);  // 固定位置 0x00

u16 device_id;
pci_read_config_word(dev, PCI_DEVICE_ID, &device_id);  // 固定位置 0x02

u8 header_type;
pci_read_config_byte(dev, PCI_HEADER_TYPE, &header_type);  // 固定位置 0x0E

// 2. 从固定位置读取 Capability Pointer
u8 cap_ptr;
pci_read_config_byte(dev, PCI_CAPABILITY_LIST, &cap_ptr);  // 固定位置 0x34

// 3. 不需要查找，直接访问
```

### 8. 总结

| 配置空间区域 | 是否固定 | 说明 |
|------------|---------|------|
| **0x00 - 0x3F** | ✅ **固定** | 标准 PCI Header，所有设备相同 |
| **0x34** | ✅ **固定** | Capability Pointer 位置固定 |
| **0x40 - 0xFF** | ❌ 不固定 | 设备特定区域 |
| **0x100 - 0xFFF** | ❌ 不固定 | PCIe 扩展能力（通过链表） |

**关键点：**
- **前 64 字节（0x00 - 0x3F）是固定的**：PCI/PCIe 规范强制要求
- **Capability Pointer 位置固定（0x34）**：但指向的 Capability 位置不固定
- **软件可以依赖固定位置**：直接访问标准寄存器，无需查找
- **Capability 结构位置不固定**：需要通过链表查找

**所以：**
- ✅ **配置空间前面是固定的**（0x00 - 0x3F）
- ✅ **Capability Pointer 位置是固定的**（0x34）
- ❌ **但 Capability 结构本身的位置不固定**（需要通过链表查找）

## 十八、PCIe Bridge 内部寄存器 vs PCIe 配置空间

### 1. 重要发现：两个不同的地址空间

**你发现的是正确的！PCIe Root Port/Bridge 确实有两个不同的地址空间：**

```
┌─────────────────────────────────────────────────────────┐
│ PCIe Root Port / Bridge 地址空间                        │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ 0x0000 - 0x0FFF: Bridge 内部寄存器（控制寄存器）       │
│   ├─ PCIe Core 控制寄存器                               │
│   ├─ AXI Bridge 寄存器                                  │
│   ├─ Link 控制寄存器                                    │
│   ├─ 中断控制寄存器                                      │
│   └─ 其他 Bridge 特定寄存器                             │
│                                                          │
│ 0x1000 - 0x1FFF: PCIe 配置空间（标准 PCIe 配置空间）   │
│   ├─ 0x1000 + 0x00: Vendor ID, Device ID               │
│   ├─ 0x1000 + 0x04: Command, Status                    │
│   ├─ 0x1000 + 0x34: Capability Pointer                  │
│   └─ 0x1000 + 0x14: Slot Capabilities（如果实现）      │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### 2. 两种地址空间的区别

#### 2.1 Bridge 内部寄存器（0x0000 - 0x0FFF）

**这是 Root Port/Bridge 的控制寄存器，不是 PCIe 配置空间：**

| 地址范围 | 用途 | 访问方式 | 说明 |
|---------|------|---------|------|
| **0x0000 - 0x0FFF** | Bridge 内部寄存器 | 直接内存映射 | Root Port/Switch Port 的控制寄存器 |
| **0x0000 - 0x00FF** | PCIe Core 寄存器 | 直接访问 | PCIe 核心控制寄存器 |
| **0x0100 - 0x01FF** | AXI Bridge 寄存器 | 直接访问 | AXI Bridge 控制寄存器 |
| **0x0200 - 0x02FF** | Link 控制寄存器 | 直接访问 | 链路训练和控制寄存器 |

**特点：**
- ❌ **不是 PCIe 配置空间**
- ✅ **Bridge 硬件控制寄存器**
- ✅ **直接内存映射访问**
- ✅ **平台/厂商特定**

#### 2.2 PCIe 配置空间（0x1000 - 0x1FFF）

**这是标准的 PCIe 配置空间，符合 PCIe 规范：**

| 地址范围 | 用途 | 访问方式 | 说明 |
|---------|------|---------|------|
| **0x1000 - 0x1FFF** | PCIe 配置空间 | 通过配置空间访问 | 标准 PCIe 配置空间 |
| **0x1000 + 0x00** | Vendor ID, Device ID | 配置空间访问 | 标准 PCI Header |
| **0x1000 + 0x34** | Capability Pointer | 配置空间访问 | 固定位置 |
| **0x1000 + 0x14** | Slot Capabilities | 配置空间访问 | PCIe Capability 结构 |

**特点：**
- ✅ **标准 PCIe 配置空间**
- ✅ **符合 PCIe 规范**
- ✅ **通过 PCIe 配置空间访问机制**
- ✅ **前 64 字节固定布局**

### 3. 实际地址映射示例

#### 3.1 典型的 Root Port 地址映射

```c
// Root Port 的完整地址空间
#define ROOT_PORT_BASE           0xF8000000

// Bridge 内部寄存器（0x0000 - 0x0FFF）
#define BRIDGE_CORE_REG_BASE     (ROOT_PORT_BASE + 0x0000)
#define BRIDGE_AXI_REG_BASE      (ROOT_PORT_BASE + 0x0100)
#define BRIDGE_LINK_REG_BASE     (ROOT_PORT_BASE + 0x0200)

// PCIe 配置空间（0x1000 - 0x1FFF）
#define PCIE_CFG_SPACE_BASE      (ROOT_PORT_BASE + 0x1000)

// 访问示例
void configure_root_port(void)
{
    void __iomem *bridge_base = ioremap(ROOT_PORT_BASE, 0x2000);
    
    // 1. 访问 Bridge 内部寄存器（直接内存映射）
    writel(0x12345678, bridge_base + 0x0000);  // Bridge Core 寄存器
    writel(0x87654321, bridge_base + 0x0100);  // AXI Bridge 寄存器
    
    // 2. 访问 PCIe 配置空间（通过配置空间访问）
    // 注意：不能直接访问 bridge_base + 0x1000
    // 需要通过 PCIe 配置空间访问机制
    u16 vendor_id;
    pci_read_config_word(root_port_dev, PCI_VENDOR_ID, &vendor_id);
    // 内部会映射到 0x1000 + 0x00
}
```

### 4. 如何访问这两种地址空间

#### 4.1 访问 Bridge 内部寄存器

```c
// Bridge 内部寄存器：直接内存映射访问
void configure_bridge_internal_regs(void)
{
    void __iomem *bridge_base = ioremap(ROOT_PORT_BASE, 0x1000);
    
    // 直接访问 Bridge 内部寄存器
    writel(value, bridge_base + BRIDGE_CORE_CTRL_OFFSET);
    writel(value, bridge_base + BRIDGE_AXI_CTRL_OFFSET);
    writel(value, bridge_base + BRIDGE_LINK_CTRL_OFFSET);
    
    iounmap(bridge_base);
}
```

#### 4.2 访问 PCIe 配置空间

```c
// PCIe 配置空间：通过配置空间访问机制
void access_pcie_config_space(struct pci_dev *root_port)
{
    // 通过 PCIe 配置空间访问 API
    u16 vendor_id;
    pci_read_config_word(root_port, PCI_VENDOR_ID, &vendor_id);
    // 内部会访问 0x1000 + 0x00
    
    u32 slot_cap;
    int pos = pci_find_capability(root_port, PCI_CAP_ID_EXP);
    pci_read_config_dword(root_port, pos + PCI_EXP_SLTCAP, &slot_cap);
    // 内部会访问 0x1000 + pos + 0x14
}
```

### 5. 为什么有两个地址空间？

#### 5.1 Bridge 内部寄存器的作用

**Bridge 内部寄存器用于：**
- 控制 PCIe 核心功能（链路训练、电源管理等）
- 控制 AXI Bridge 功能（地址转换、窗口管理等）
- 控制中断和错误处理
- 平台特定的控制功能

#### 5.2 PCIe 配置空间的作用

**PCIe 配置空间用于：**
- 标准的 PCIe 设备识别和配置
- Capability 结构（Slot、Link、Device 等）
- 符合 PCIe 规范的配置接口

### 6. 在 ATF 中的处理

#### 6.1 区分两种地址空间

```c
// ATF 中处理 Root Port 的两种地址空间
void atf_configure_pcie_root_port(struct pcie_controller *ctrl)
{
    void __iomem *bridge_base;
    void __iomem *cfg_space_base;
    
    // 1. 映射 Bridge 内部寄存器空间（0x0000 - 0x0FFF）
    bridge_base = ioremap(ctrl->base, 0x1000);
    
    // 配置 Bridge 内部寄存器
    writel(0x12345678, bridge_base + BRIDGE_CORE_CTRL);
    writel(0x87654321, bridge_base + BRIDGE_AXI_CTRL);
    
    // 2. 映射 PCIe 配置空间（0x1000 - 0x1FFF）
    cfg_space_base = ioremap(ctrl->base + 0x1000, 0x1000);
    
    // 或者通过配置空间访问机制
    // 注意：通常不直接映射配置空间，而是通过 PCIe 配置空间访问
    
    // 3. 访问 PCIe 配置空间（通过配置空间访问）
    // 需要使用 PCIe 配置空间访问函数
    // 而不是直接访问 cfg_space_base
}
```

#### 6.2 正确的访问方式

```c
// ATF 中正确的访问方式
void atf_pcie_root_port_init(struct pcie_controller *ctrl)
{
    // 1. Bridge 内部寄存器：直接内存映射
    void __iomem *bridge_base = ioremap(ctrl->base, 0x1000);
    
    // 配置 Bridge 内部功能
    configure_bridge_core(bridge_base);
    configure_axi_bridge(bridge_base);
    
    // 2. PCIe 配置空间：通过配置空间访问机制
    // 需要先初始化 PCIe 配置空间访问
    init_pcie_config_space_access(ctrl);
    
    // 然后通过配置空间访问 API
    u16 vendor_id;
    pcie_config_read(ctrl, 0, 0, 0, PCI_VENDOR_ID, &vendor_id);
    // 内部会访问 0x1000 + 0x00
    
    // 访问 Slot Capabilities
    int pos = pcie_find_capability(ctrl, 0, 0, 0, PCI_CAP_ID_EXP);
    u32 slot_cap;
    pcie_config_read(ctrl, 0, 0, 0, pos + PCI_EXP_SLTCAP, &slot_cap);
    // 内部会访问 0x1000 + pos + 0x14
}
```

### 7. 地址空间映射图

```
┌─────────────────────────────────────────────────────────┐
│ Root Port 完整地址空间（从基址开始）                    │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ Base + 0x0000 - 0x0FFF                                  │
│ ┌─────────────────────────────────────┐                │
│ │ Bridge 内部寄存器                    │                │
│ │ - PCIe Core 寄存器                   │                │
│ │ - AXI Bridge 寄存器                  │                │
│ │ - Link 控制寄存器                    │                │
│ │ - 中断控制寄存器                      │                │
│ │ 访问方式：直接内存映射                │                │
│ └─────────────────────────────────────┘                │
│                                                          │
│ Base + 0x1000 - 0x1FFF                                  │
│ ┌─────────────────────────────────────┐                │
│ │ PCIe 配置空间                        │                │
│ │ - 0x1000 + 0x00: Vendor/Device ID   │                │
│ │ - 0x1000 + 0x34: Capability Pointer │                │
│ │ - 0x1000 + 0x14: Slot Capabilities  │                │
│ │ 访问方式：PCIe 配置空间访问机制       │                │
│ └─────────────────────────────────────┘                │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### 8. 关键区别总结

| 特性 | Bridge 内部寄存器 | PCIe 配置空间 |
|------|------------------|--------------|
| **地址范围** | 0x0000 - 0x0FFF | 0x1000 - 0x1FFF |
| **访问方式** | 直接内存映射 | PCIe 配置空间访问 |
| **用途** | Bridge 硬件控制 | 标准 PCIe 配置 |
| **规范** | 平台/厂商特定 | PCIe 规范标准 |
| **固定布局** | ❌ 平台特定 | ✅ 前 64 字节固定 |

### 9. 实际代码示例

```c
// 完整的 Root Port 初始化
void init_pcie_root_port(void)
{
    #define ROOT_PORT_BASE       0xF8000000
    
    // 1. 映射 Bridge 内部寄存器
    void __iomem *bridge_base = ioremap(ROOT_PORT_BASE, 0x1000);
    
    // 配置 Bridge 内部功能
    writel(0x12345678, bridge_base + 0x0000);  // Core Control
    writel(0x87654321, bridge_base + 0x0100);  // AXI Control
    
    // 2. PCIe 配置空间访问（通过配置空间机制）
    // 注意：不能直接访问 bridge_base + 0x1000
    // 需要通过 PCIe 配置空间访问函数
    
    struct pci_dev *root_port = get_root_port_dev();
    
    // 访问标准 PCIe 配置空间
    u16 vendor_id;
    pci_read_config_word(root_port, PCI_VENDOR_ID, &vendor_id);
    // 内部访问：ROOT_PORT_BASE + 0x1000 + 0x00
    
    // 访问 Slot Capabilities
    int pos = pci_find_capability(root_port, PCI_CAP_ID_EXP);
    // pos 是相对于配置空间基址的偏移（如 0x100）
    // 实际访问：ROOT_PORT_BASE + 0x1000 + pos + 0x14
    
    u32 slot_cap;
    pci_read_config_dword(root_port, pos + PCI_EXP_SLTCAP, &slot_cap);
    // 实际访问：ROOT_PORT_BASE + 0x1000 + 0x100 + 0x14 = 0x1114
}
```

### 10. 总结

**你的发现是正确的！**

1. **0x0000 - 0x0FFF**：Bridge 内部寄存器（Root Port 控制寄存器）
   - 直接内存映射访问
   - 平台/厂商特定

2. **0x1000 - 0x1FFF**：PCIe 配置空间（标准 PCIe 配置空间）
   - 通过 PCIe 配置空间访问机制
   - 符合 PCIe 规范
   - 前 64 字节（相对于配置空间）是固定的

**关键点：**
- **Bridge 内部寄存器 ≠ PCIe 配置空间**
- **两个不同的地址空间，用途不同**
- **访问方式不同：直接映射 vs 配置空间访问**
- **Slot Capabilities 在 PCIe 配置空间中（0x1000 + 偏移）**

## 十九、Slot Capabilities Register 的位置确认

### 1. 答案：是的，Slot Capabilities Register 是 PCIe 配置空间的寄存器

**Slot Capabilities Register (SLTCAP) 确实位于 PCIe 配置空间中，是 PCIe Extended Capabilities 的一部分。**

### 2. Slot Capabilities Register 的完整地址路径

```
┌─────────────────────────────────────────────────────────┐
│ Root Port 地址空间                                       │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ Base + 0x0000 - 0x0FFF                                  │
│ └─ Bridge 内部寄存器（不是配置空间）                    │
│                                                          │
│ Base + 0x1000 - 0x1FFF                                  │
│ └─ PCIe 配置空间（标准 PCIe 配置空间）                 │
│    │                                                      │
│    ├─ 0x1000 + 0x00: Vendor ID, Device ID               │
│    ├─ 0x1000 + 0x34: Capability Pointer                │
│    │    └─ 指向 PCIe Capability（例如：0x100）          │
│    │                                                      │
│    └─ PCIe Capability 结构（例如：0x1000 + 0x100）     │
│         ├─ 0x1000 + 0x100: PCIe Capability ID          │
│         ├─ 0x1000 + 0x104: Device Capabilities           │
│         ├─ 0x1000 + 0x108: Device Control               │
│         ├─ 0x1000 + 0x10C: Link Capabilities            │
│         ├─ 0x1000 + 0x110: Link Control                 │
│         ├─ 0x1000 + 0x114: Slot Capabilities ← 这里！    │
│         ├─ 0x1000 + 0x118: Slot Control                 │
│         └─ 0x1000 + 0x11A: Slot Status                  │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### 3. 详细地址计算

#### 3.1 完整地址路径

```c
// Slot Capabilities Register 的完整地址计算

// 1. Root Port 基址
#define ROOT_PORT_BASE           0xF8000000

// 2. PCIe 配置空间基址（从 Root Port 基址偏移 0x1000）
#define PCIE_CFG_SPACE_BASE      (ROOT_PORT_BASE + 0x1000)

// 3. 查找 PCIe Capability（在配置空间中）
int pos = pci_find_capability(root_port, PCI_CAP_ID_EXP);
// pos = 0x100（例如，这是配置空间内的偏移）

// 4. Slot Capabilities Register 地址
// 完整地址 = ROOT_PORT_BASE + 0x1000 + pos + PCI_EXP_SLTCAP
//          = 0xF8000000 + 0x1000 + 0x100 + 0x14
//          = 0xF8001114

// 但通常不直接计算，而是通过配置空间访问：
u32 slot_cap;
pci_read_config_dword(root_port, pos + PCI_EXP_SLTCAP, &slot_cap);
// 内部会访问：PCIE_CFG_SPACE_BASE + pos + PCI_EXP_SLTCAP
```

#### 3.2 地址层次

```
物理地址层次：
┌─────────────────────────────────────────────────────────┐
│ 0xF8000000 (Root Port Base)                            │
│    │                                                      │
│    ├─ 0xF8000000 - 0xF8000FFF: Bridge 内部寄存器        │
│    │                                                      │
│    └─ 0xF8001000 - 0xF8001FFF: PCIe 配置空间            │
│         │                                                  │
│         ├─ 0xF8001000 + 0x00: Vendor ID                  │
│         ├─ 0xF8001000 + 0x34: Capability Pointer         │
│         │    └─ 值 = 0x100（指向 PCIe Capability）       │
│         │                                                  │
│         └─ 0xF8001000 + 0x100: PCIe Capability 开始      │
│              ├─ 0xF8001000 + 0x100: Capability ID        │
│              ├─ 0xF8001000 + 0x114: Slot Capabilities ←  │
│              └─ 0xF8001000 + 0x118: Slot Control         │
│                                                           │
└─────────────────────────────────────────────────────────┘

配置空间内偏移（相对于 0x1000）：
┌─────────────────────────────────────────────────────────┐
│ 配置空间偏移（从 0x1000 开始）                          │
├─────────────────────────────────────────────────────────┤
│ 0x00: Vendor ID, Device ID                             │
│ 0x34: Capability Pointer = 0x100                        │
│ 0x100: PCIe Capability 开始                             │
│   0x100 + 0x00: PCIe Capability ID                       │
│   0x100 + 0x14: Slot Capabilities ← 这里！              │
│   0x100 + 0x18: Slot Control                            │
└─────────────────────────────────────────────────────────┘
```

### 4. 确认：Slot Capabilities 是配置空间寄存器

#### 4.1 证据 1：PCIe 规范定义

**根据 PCIe 规范：**
- Slot Capabilities Register 是 **PCIe Extended Capabilities** 的一部分
- PCIe Extended Capabilities 位于 **PCIe 配置空间**中
- 因此 Slot Capabilities 在 **PCIe 配置空间**中

#### 4.2 证据 2：Linux 内核代码

```c
// Linux 内核中访问 Slot Capabilities
// drivers/pci/hotplug/pciehp_core.c

static int pciehp_probe(struct pcie_device *dev)
{
    struct controller *ctrl;
    u32 slot_cap;
    int pos;
    
    // 1. 查找 PCIe Capability（在配置空间中）
    pos = pci_find_capability(dev->port, PCI_CAP_ID_EXP);
    // pos 是配置空间内的偏移
    
    // 2. 读取 Slot Capabilities（通过配置空间访问）
    pci_read_config_dword(dev->port, pos + PCI_EXP_SLTCAP, &slot_cap);
    // 这是配置空间访问，不是直接内存映射
    
    // 3. 配置 Slot Control（也是配置空间访问）
    pci_read_config_word(dev->port, pos + PCI_EXP_SLTCTL, &slot_ctl);
    pci_write_config_word(dev->port, pos + PCI_EXP_SLTCTL, slot_ctl);
}
```

#### 4.3 证据 3：访问方式

**Slot Capabilities 必须通过 PCIe 配置空间访问机制：**

```c
// ✅ 正确：通过配置空间访问
u32 slot_cap;
pci_read_config_dword(root_port, pos + PCI_EXP_SLTCAP, &slot_cap);

// ❌ 错误：不能直接内存映射访问
// writel(value, bridge_base + 0x1114);  // 错误！
```

### 5. 完整的地址映射关系

```
┌─────────────────────────────────────────────────────────┐
│ 地址映射关系                                             │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ 物理地址：0xF8000000 (Root Port Base)                   │
│    │                                                      │
│    ├─ 0xF8000000 - 0xF8000FFF                           │
│    │   └─ Bridge 内部寄存器                              │
│    │      访问方式：直接内存映射                          │
│    │      writel(value, bridge_base + offset)            │
│    │                                                      │
│    └─ 0xF8001000 - 0xF8001FFF                           │
│        └─ PCIe 配置空间                                  │
│           访问方式：PCIe 配置空间访问                      │
│           pci_read_config_dword(dev, offset, &value)    │
│            │                                              │
│            ├─ 0xF8001000 + 0x00: Vendor ID               │
│            ├─ 0xF8001000 + 0x34: Capability Pointer     │
│            └─ 0xF8001000 + 0x100: PCIe Capability        │
│                ├─ 0xF8001000 + 0x114: Slot Capabilities │
│                └─ 0xF8001000 + 0x118: Slot Control       │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### 6. 在 ATF 中的正确访问方式

```c
// ATF 中访问 Slot Capabilities（正确方式）
void atf_access_slot_capabilities(struct pcie_controller *ctrl)
{
    u32 slot_cap;
    u16 slot_ctl;
    int pos;
    
    // 方法 1: 通过 PCIe 配置空间访问（推荐）
    // 1. 查找 PCIe Capability（在配置空间中）
    pos = pcie_find_capability(ctrl, 0, 0, 0, PCI_CAP_ID_EXP);
    // pos 是配置空间内的偏移（如 0x100）
    
    // 2. 读取 Slot Capabilities（配置空间访问）
    pcie_config_read(ctrl, 0, 0, 0, 
                     pos + PCI_EXP_SLTCAP, &slot_cap);
    // 实际物理地址：base + 0x1000 + pos + 0x14
    
    // 3. 配置 Slot Control（配置空间访问）
    pcie_config_read(ctrl, 0, 0, 0, 
                     pos + PCI_EXP_SLTCTL, &slot_ctl);
    slot_ctl |= PCI_EXP_SLTCTL_HPIE;
    pcie_config_write(ctrl, 0, 0, 0, 
                      pos + PCI_EXP_SLTCTL, slot_ctl);
    
    // 方法 2: 直接计算地址（不推荐，但某些平台可能需要）
    // 注意：这需要知道确切的地址映射
    void __iomem *cfg_base = ioremap(ctrl->base + 0x1000, 0x1000);
    slot_cap = readl(cfg_base + pos + PCI_EXP_SLTCAP);
    // 但通常不推荐，应该使用配置空间访问机制
}
```

### 7. 总结

**Slot Capabilities Register 的位置：**

| 项目 | 说明 |
|------|------|
| **是否在配置空间** | ✅ **是**，在 PCIe 配置空间中 |
| **配置空间基址** | Base + 0x1000 |
| **PCIe Capability 偏移** | 配置空间内偏移（如 0x100） |
| **Slot Capabilities 偏移** | PCIe Capability + 0x14 |
| **完整地址** | Base + 0x1000 + PCIe_Cap_offset + 0x14 |
| **访问方式** | PCIe 配置空间访问机制 |

**关键点：**
- ✅ **Slot Capabilities Register 是 PCIe 配置空间的寄存器**
- ✅ **位于 PCIe 配置空间（0x1000 + 偏移）中**
- ✅ **不是 Bridge 内部寄存器（0x0000 - 0x0FFF）**
- ✅ **必须通过 PCIe 配置空间访问机制访问**
- ✅ **不能直接内存映射访问（虽然可以计算地址）**

**访问方式对比：**

```c
// Bridge 内部寄存器（0x0000 - 0x0FFF）
writel(value, bridge_base + 0x0000);  // 直接内存映射

// PCIe 配置空间（0x1000 - 0x1FFF）
pci_read_config_dword(dev, offset, &value);  // 配置空间访问
// Slot Capabilities 在这里面
```

## 二十、AXI Bridge 与 PCIe 控制器的关系

### 1. 关系概述

**AXI Bridge 是 PCIe 控制器的一个关键组件，负责在 AXI 总线和 PCIe 总线之间进行协议转换和地址映射。**

### 2. 架构关系图

```
┌─────────────────────────────────────────────────────────┐
│ SoC 系统架构                                             │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ CPU / 其他 Master                                        │
│    │                                                      │
│    └─ AXI 总线                                           │
│         │                                                  │
│         ├─ AXI Bridge ← 协议转换和地址映射               │
│         │    │                                              │
│         │    └─ PCIe 控制器                                │
│         │         │                                          │
│         │         ├─ PCIe Core（链路层、物理层）          │
│         │         ├─ PCIe 配置空间                         │
│         │         └─ PCIe 链路（连接到 PCIe 设备）        │
│         │                                                  │
│         └─ 其他 AXI 设备                                  │
│                                                           │
└─────────────────────────────────────────────────────────┘
```

### 3. AXI Bridge 的作用

#### 3.1 主要功能

**AXI Bridge 在 PCIe 控制器中的核心作用：**

1. **协议转换**
   - AXI 事务 → PCIe 事务
   - PCIe 事务 → AXI 事务

2. **地址转换（ATR - Address Translation Register）**
   - AXI 地址 → PCIe 地址
   - PCIe 地址 → AXI 地址

3. **配置空间访问**
   - 通过 AXI 总线访问 PCIe 配置空间
   - 提供配置空间的 AXI 接口

4. **窗口管理**
   - 管理地址转换窗口（Outbound/Inbound）
   - 配置地址映射范围

#### 3.2 详细功能说明

```
┌─────────────────────────────────────────────────────────┐
│ AXI Bridge 功能模块                                      │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ 1. 协议转换模块                                          │
│    - AXI Read/Write → PCIe Memory Read/Write            │
│    - AXI Burst → PCIe 事务拆分/合并                      │
│    - AXI 响应 → PCIe Completion                         │
│                                                          │
│ 2. 地址转换模块（ATR）                                   │
│    - Outbound 窗口：CPU → PCIe 设备                      │
│      AXI 地址 0x80000000 → PCIe 地址 0x00000000         │
│    - Inbound 窗口：PCIe 设备 → CPU                       │
│      PCIe 地址 0x00000000 → AXI 地址 0x80000000         │
│                                                          │
│ 3. 配置空间访问模块                                      │
│    - 通过 AXI 访问 PCIe 配置空间                         │
│    - 提供配置空间的 AXI Slave 接口                       │
│                                                          │
│ 4. 窗口管理模块                                          │
│    - 配置地址转换窗口                                     │
│    - 管理窗口大小和属性                                   │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### 4. 在 PCIe 控制器中的位置

#### 4.1 集成设计（常见）

```
┌─────────────────────────────────────────────────────────┐
│ PCIe 控制器（集成 AXI Bridge）                          │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ PCIe Controller Base (0xF8000000)                      │
│    │                                                      │
│    ├─ AXI Bridge (0x0000 - 0x00FF)                      │
│    │    ├─ 地址转换寄存器（ATR）                         │
│    │    ├─ 窗口配置寄存器                                 │
│    │    └─ 配置空间访问接口                               │
│    │                                                      │
│    ├─ PCIe Core (0x0100 - 0x0FFF)                      │
│    │    ├─ 链路训练控制                                   │
│    │    ├─ 物理层控制                                     │
│    │    └─ 错误处理                                       │
│    │                                                      │
│    └─ PCIe 配置空间 (0x1000 - 0x1FFF)                   │
│         └─ 标准 PCIe 配置空间                            │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

#### 4.2 分离设计（某些平台）

```
┌─────────────────────────────────────────────────────────┐
│ PCIe 控制器和 AXI Bridge（分离设计）                    │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ AXI Bridge Base (0xF8000000)                            │
│    └─ AXI Bridge 寄存器                                  │
│                                                          │
│ PCIe Controller Base (0xF9000000)                       │
│    └─ PCIe Core 寄存器                                  │
│                                                          │
│ PCIe Config Space Base (0xFA000000)                     │
│    └─ PCIe 配置空间                                      │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### 5. AXI Bridge 的关键寄存器

#### 5.1 地址转换寄存器（ATR）

```c
// AXI Bridge 中的地址转换寄存器
#define AXI_BRIDGE_OB_WINDOW0_BASE    0x0000  // Outbound 窗口 0 基址
#define AXI_BRIDGE_OB_WINDOW0_SIZE    0x0004  // Outbound 窗口 0 大小
#define AXI_BRIDGE_OB_WINDOW0_ATTR    0x0008  // Outbound 窗口 0 属性

#define AXI_BRIDGE_IB_WINDOW0_BASE    0x0100  // Inbound 窗口 0 基址
#define AXI_BRIDGE_IB_WINDOW0_SIZE    0x0104  // Inbound 窗口 0 大小
#define AXI_BRIDGE_IB_WINDOW0_ATTR    0x0108  // Inbound 窗口 0 属性

// 配置示例
void configure_axi_bridge_windows(void)
{
    void __iomem *axi_base = ioremap(AXI_BRIDGE_BASE, 0x1000);
    
    // 配置 Outbound 窗口：CPU → PCIe
    // AXI 地址 0x80000000 → PCIe 地址 0x00000000
    writel(0x80000000, axi_base + AXI_BRIDGE_OB_WINDOW0_BASE);
    writel(0x10000000, axi_base + AXI_BRIDGE_OB_WINDOW0_SIZE);  // 256MB
    writel(0x00000000, axi_base + AXI_BRIDGE_OB_WINDOW0_ATTR);
    
    // 配置 Inbound 窗口：PCIe → CPU
    // PCIe 地址 0x00000000 → AXI 地址 0x80000000
    writel(0x80000000, axi_base + AXI_BRIDGE_IB_WINDOW0_BASE);
    writel(0x10000000, axi_base + AXI_BRIDGE_IB_WINDOW0_SIZE);  // 256MB
    writel(0x00000000, axi_base + AXI_BRIDGE_IB_WINDOW0_ATTR);
}
```

#### 5.2 配置空间访问接口

```c
// AXI Bridge 提供配置空间访问接口
#define AXI_BRIDGE_CFG_BASE           0x0200  // 配置空间基址寄存器
#define AXI_BRIDGE_CFG_ACCESS         0x0204  // 配置空间访问控制

// 通过 AXI Bridge 访问 PCIe 配置空间
void access_pcie_config_via_axi_bridge(void)
{
    void __iomem *axi_base = ioremap(AXI_BRIDGE_BASE, 0x1000);
    
    // 设置配置空间基址
    writel(0xF8001000, axi_base + AXI_BRIDGE_CFG_BASE);
    
    // 通过 AXI Bridge 访问配置空间
    // CPU 通过 AXI 总线访问 → AXI Bridge 转换 → PCIe 配置空间
}
```

### 6. 数据流示例

#### 6.1 CPU 访问 PCIe 设备内存

```
CPU 发起内存访问
  ↓
AXI 总线（地址：0x80000000）
  ↓
AXI Bridge
  ├─ 地址转换：0x80000000 → 0x00000000（PCIe 地址）
  ├─ 协议转换：AXI Write → PCIe Memory Write
  └─ 窗口匹配：找到对应的 Outbound 窗口
  ↓
PCIe 控制器
  ├─ 生成 PCIe 事务
  └─ 发送到 PCIe 链路
  ↓
PCIe 设备
```

#### 6.2 PCIe 设备访问 CPU 内存

```
PCIe 设备发起内存访问
  ↓
PCIe 链路
  ↓
PCIe 控制器
  ├─ 接收 PCIe 事务
  └─ 传递给 AXI Bridge
  ↓
AXI Bridge
  ├─ 地址转换：0x00000000 → 0x80000000（AXI 地址）
  ├─ 协议转换：PCIe Memory Read → AXI Read
  └─ 窗口匹配：找到对应的 Inbound 窗口
  ↓
AXI 总线（地址：0x80000000）
  ↓
CPU 内存
```

### 7. 在 ATF 中的配置

#### 7.1 初始化 AXI Bridge

```c
// ATF 中初始化 AXI Bridge
void atf_init_axi_bridge(struct pcie_controller *ctrl)
{
    void __iomem *axi_base;
    
    // 1. 获取 AXI Bridge 基址
    // 可能是 PCIe 控制器基址（集成设计）
    // 或独立基址（分离设计）
    if (ctrl->axi_bridge_base == 0) {
        axi_base = ioremap(ctrl->pcie_base, 0x1000);  // 集成设计
    } else {         
        axi_base = ioremap(ctrl->axi_bridge_base, 0x1000);  // 分离设计
    }
    
    // 2. 配置地址转换窗口
    configure_axi_outbound_windows(axi_base);
    configure_axi_inbound_windows(axi_base);
    
    // 3. 配置配置空间访问
    configure_axi_config_space_access(axi_base, ctrl);
    
    // 4. 使能 AXI Bridge
    writel(0x1, axi_base + AXI_BRIDGE_CTRL);
}
```

#### 7.2 配置地址转换窗口

```c
// 配置 AXI Bridge 的地址转换窗口
void configure_axi_outbound_windows(void __iomem *axi_base)
{
    // Outbound 窗口：CPU → PCIe 设备
    // 窗口 0：Memory 空间
    writel(0x80000000, axi_base + AXI_BRIDGE_OB_WIN0_BASE);   // AXI 基址
    writel(0x00000000, axi_base + AXI_BRIDGE_OB_WIN0_PCI_BASE); // PCIe 基址
    writel(0x10000000, axi_base + AXI_BRIDGE_OB_WIN0_SIZE);   // 256MB
    writel(0x2, axi_base + AXI_BRIDGE_OB_WIN0_TYPE);          // Memory Write
    
    // 窗口 1：IO 空间
    writel(0x10000000, axi_base + AXI_BRIDGE_OB_WIN1_BASE);   // AXI 基址
    writel(0x00000000, axi_base + AXI_BRIDGE_OB_WIN1_PCI_BASE); // PCIe 基址
    writel(0x00010000, axi_base + AXI_BRIDGE_OB_WIN1_SIZE);   // 64KB
    writel(0x6, axi_base + AXI_BRIDGE_OB_WIN1_TYPE);          // IO Write
    
    // 窗口 2：配置空间
    writel(0xE0000000, axi_base + AXI_BRIDGE_OB_WIN2_BASE);   // AXI 基址
    writel(0x00000000, axi_base + AXI_BRIDGE_OB_WIN2_PCI_BASE); // PCIe 基址
    writel(0x10000000, axi_base + AXI_BRIDGE_OB_WIN2_SIZE);   // 256MB
    writel(0xA, axi_base + AXI_BRIDGE_OB_WIN2_TYPE);          // Type0 Config
}
```

### 8. 关系总结

| 项目 | 说明 |
|------|------|
| **关系类型** | **组件关系**：AXI Bridge 是 PCIe 控制器的组件 |
| **位置** | 通常在 PCIe 控制器内部（集成设计）或外部（分离设计） |
| **作用** | 协议转换、地址转换、配置空间访问 |
| **基址关系** | 可能共享基址（集成）或独立基址（分离） |
| **依赖关系** | PCIe 控制器依赖 AXI Bridge 进行总线转换 |

### 9. 关键理解

**AXI Bridge 和 PCIe 控制器的关系：**

1. **AXI Bridge 是 PCIe 控制器的接口组件**
   - 连接 AXI 总线和 PCIe 控制器
   - 提供协议和地址转换功能

2. **两者协同工作**
   - AXI Bridge 处理总线协议转换
   - PCIe 控制器处理 PCIe 协议

3. **地址空间关系**
   - AXI Bridge 基址可能 = PCIe 控制器基址（集成设计）
   - 或 AXI Bridge 有独立基址（分离设计）

4. **功能分工**
   - **AXI Bridge**：总线接口、地址转换、窗口管理                                                                                                                                                                 
   - **PCIe 控制器**：PCIe 协议、链路管理、配置空间

**类比理解：**
- **AXI Bridge** = 翻译官（协议和地址转换）
- **PCIe 控制器** = 执行者（PCIe 协议处理）
 
·## 二十一、PLDA 在 PCIe 中的含义

### 1. PLDA 是什么？

**PLDA 是一家 PCIe IP 核（Intellectual Property Core）供应商，提供 PCIe 控制器 IP 核。**

**PLDA = PCIe IP 核供应商**

### 2. PLDA 提供的 PCIe IP 核

#### 2.1 PLDA XpressRich

**PLDA 最著名的 PCIe IP 核是 XpressRich：**
 
- **产品名称**：PLDA XpressRich PCIe Controller
- **功能**：完整的 PCIe Root Port / Endpoint IP 核
- **支持标准**：PCIe 1.0/2.0/3.0/4.0/5.0
- **应用**：SoC 设计、FPGA 设计

#### 2.2 主要特性

**PLDA XpressRich PCIe IP 核的主要特性：**

1. **Root Port 模式**
   - 支持 PCIe Root Port 功能
   - 支持热插拔
   - 支持 MSI/MSI-X

2. **Endpoint 模式**
   - 支持 PCIe Endpoint 功能
   - 支持多个 Function

3. **地址转换（ATR）**
   - 支持 AXI 到 PCIe 的地址转换
   - 支持多个地址转换窗口

4. **配置空间**
   - 完整的 PCIe 配置空间支持
   - 支持扩展能力

### 3. PLDA PCIe IP 核在 Linux 内核中的使用

#### 3.1 Linux 内核驱动

**Linux 内核中有 PLDA PCIe 控制器的驱动：**

```c
// drivers/pci/controller/plda/pcie-plda-host.c
// PLDA PCIe XpressRich host controller driver

// 驱动支持的平台：
// - Microchip SoC
// - StarFive SoC
// - 其他使用 PLDA IP 核的平台
```

#### 3.2 驱动功能

**PLDA PCIe 驱动提供：**

1. **Root Port 支持**
   - PCIe 总线扫描
   - 设备枚举
   - 配置空间访问

2. **MSI/MSI-X 支持**
   - MSI 中断处理
   - MSI-X 中断处理

3. **地址转换（ATR）**
   - Outbound 窗口配置
   - Inbound 窗口配置
   - AXI 到 PCIe 地址转换

### 4. PLDA IP 核的架构

```
┌─────────────────────────────────────────────────────────┐
│ PLDA XpressRich PCIe IP 核架构                          │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ PCIe 接口                                                │
│    │                                                      │
│    └─ PCIe Core                                          │
│         ├─ PCIe 物理层（PHY）                            │
│         ├─ PCIe 数据链路层（DLL）                        │
│         └─ PCIe 事务层（TL）                             │
│              │                                            │
│              └─ AXI Bridge                               │
│                   ├─ AXI Master 接口                    │
│                   ├─ AXI Slave 接口                     │
│                   ├─ 地址转换（ATR）                      │
│                   └─ 配置空间访问                        │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### 5. PLDA IP 核的关键组件

#### 5.1 AXI Bridge（在 PLDA IP 中）

**PLDA IP 核集成了 AXI Bridge：**

```c
// PLDA IP 核中的 AXI Bridge 寄存器定义
// drivers/pci/controller/plda/pcie-plda.h

// PCIe Master 表（Outbound 窗口）
#define ATR0_PCIE_WIN0_SRCADDR_PARAM    0x600u
#define ATR0_PCIE_WIN0_SRC_ADDR         0x604u
#define ATR0_PCIE_WIN0_TRSL_ADDR_LSB    0x608u
#define ATR0_PCIE_WIN0_TRSL_ADDR_UDW    0x60cu
#define ATR0_PCIE_WIN0_TRSL_PARAM       0x610u

// PCIe AXI Slave 表（Inbound 窗口）
#define ATR0_AXI4_SLV0_SRCADDR_PARAM    0x800u
#define ATR0_AXI4_SLV0_SRC_ADDR         0x804u
#define ATR0_AXI4_SLV0_TRSL_ADDR_LSB    0x808u
#define ATR0_AXI4_SLV0_TRSL_ADDR_UDW    0x80cu
#define ATR0_AXI4_SLV0_TRSL_PARAM       0x810u
```

#### 5.2 配置空间访问

**PLDA IP 核通过 AXI Bridge 提供配置空间访问：**

```c
// PLDA IP 核中的配置空间访问
void __iomem *plda_pcie_map_bus(struct pci_bus *bus, 
                                 unsigned int devfn, int where)
{
    struct plda_pcie_rp *pcie = bus->sysdata;
    
    // 通过 AXI Bridge 访问 PCIe 配置空间
    return pcie->config_base + PCIE_ECAM_OFFSET(bus->number, devfn, where);
}
```

### 6. PLDA IP 核的使用场景

#### 6.1 SoC 设计

**SoC 厂商使用 PLDA IP 核：**

```
SoC 设计流程：
  1. 从 PLDA 购买 PCIe IP 核授权
  2. 将 PLDA IP 核集成到 SoC 设计
  3. 配置 IP 核参数（Root Port/Endpoint、Lane 数等）
  4. 生成 SoC RTL 代码
  5. 流片生产
```

#### 6.2 FPGA 设计

**FPGA 设计中使用 PLDA IP 核：**

```
FPGA 设计流程：
  1. 从 PLDA 购买 PCIe IP 核授权
  2. 在 FPGA 工具中实例化 PLDA IP 核
  3. 配置 IP 核参数
  4. 综合和布局布线
  5. 生成 FPGA 比特流
```

### 7. PLDA 与其他 PCIe IP 核供应商

| IP 核供应商 | 产品名称 | 特点 |
|-----------|---------|------|
| **PLDA** | XpressRich | 广泛使用的 PCIe IP 核 |
| **Synopsys** | DesignWare PCIe | 业界标准，广泛使用 |
| **Cadence** | PCIe Controller | 高性能 PCIe IP 核 |
| **Xilinx** | UltraScale+ PCIe | FPGA 专用 PCIe IP 核 |
| **Altera/Intel** | Hard IP PCIe | FPGA 专用 PCIe IP 核 |

### 8. 在代码中识别 PLDA IP 核

#### 8.1 设备树兼容性

```dts
// 使用 PLDA IP 核的设备通常在设备树中标识
pcie@f8000000 {
    compatible = "plda,xpressrich-pcie";
    // 或
    compatible = "microchip,plda-pcie";
    // 或
    compatible = "starfive,plda-pcie";
    reg = <0x0 0xf8000000 0x0 0x1000000>;
    // ...
};
```

#### 8.2 驱动识别

```c
// Linux 内核驱动会识别 PLDA IP 核
static const struct of_device_id plda_pcie_of_match[] = {
    { .compatible = "plda,xpressrich-pcie" },
    { .compatible = "microchip,plda-pcie" },
    { .compatible = "starfive,plda-pcie" },
    { /* sentinel */ }
};
```

### 9. PLDA IP 核与 AXI Bridge 的关系

**在 PLDA IP 核中：**

- **AXI Bridge 是 PLDA IP 核的组成部分**
- **PLDA IP 核集成了 AXI Bridge 功能**
- **AXI Bridge 提供 AXI 到 PCIe 的接口**

```
PLDA XpressRich IP 核
  ├─ PCIe Core
  └─ AXI Bridge（集成在 IP 核中）
      ├─ AXI Master/Slave 接口
      ├─ 地址转换（ATR）
      └─ 配置空间访问
```

### 10. 总结

**PLDA 在 PCIe 中的含义：**

| 项目 | 说明 |
|------|------|
| **PLDA** | PCIe IP 核供应商 |
| **产品** | XpressRich PCIe Controller IP 核 |
| **用途** | SoC 和 FPGA 设计中的 PCIe 控制器 |
| **特点** | 集成 AXI Bridge、支持 Root Port/Endpoint |
| **在 Linux 中** | 有专门的驱动支持（drivers/pci/controller/plda/） |

**关键点：**
- **PLDA 是 IP 核供应商**，不是硬件组件
- **PLDA XpressRich 是 PCIe 控制器 IP 核**
- **IP 核集成了 AXI Bridge 功能**
- **SoC/FPGA 厂商购买授权后集成到设计中**

**关系链：**
```
PLDA（IP 核供应商）
  ↓ 提供
PLDA XpressRich IP 核（PCIe 控制器 IP）
  ↓ 集成
SoC/FPGA 设计
  ↓ 实现
硬件 PCIe 控制器
  ↓ 驱动
Linux 内核 PLDA 驱动
```

## 二十二、Linux 中 PCIe 设备驱动如何写

这一节总结一下**在 host 控制器（比如 pcie-starfive + plda host）已经加载并完成总线枚举之后，普通 PCIe 设备驱动要怎么写、用哪些接口、传什么参数**。

### 1. 设备是怎么“找到”你这个驱动的？

1. **host 控制器驱动阶段（你前面看的 pcie-starfive/tegra/plda）**
   - 初始化 AXI Bridge / RC、自身寄存器、ECAM 窗口、地址转换窗口（ATR）、中断等
   - 调用 `pci_host_probe()`：
     - 创建 root bus（bus 0）
     - 扫描 bus/devfn，发现下面的 Endpoint / Bridge / Switch
     - 为每个设备创建一个 `struct pci_dev`，并填好：
       - `pdev->bus->number`（bus 号）
       - `pdev->devfn`（包含 device + function 号）

2. **设备驱动阶段**
   - 你写一个 `struct pci_driver`，里面有：
     - `id_table`：列出你要支持哪些设备（Vendor ID / Device ID / Class）
     - `probe`：找到匹配设备后调用
   - 内核会把所有已存在的 `struct pci_dev` 拿来和 `id_table` 做匹配，
     匹配成功就调用你的 `probe(struct pci_dev *pdev, ...)`。

**关键点：**
- **bus 号 / devfn 都是在 host 枚举时已经分配好的，存放在 `struct pci_dev` 里**；
- 你在 `id_table` 里只写 Vendor/Device ID，就能拿到已经带好 bus/devfn 的 `pdev`。

### 2. 一个最小的 PCIe 设备驱动骨架

```c
// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>

#define DRV_NAME     "my_pcie_demo"
#define MY_VENDOR_ID 0x1234      // TODO: 替换成你的 Vendor ID
#define MY_DEVICE_ID 0x5678      // TODO: 替换成你的 Device ID
#define MY_BAR       0           // 假设设备寄存器在 BAR0

struct my_pcie_dev {
    struct pci_dev *pdev;
    void __iomem   *bar0;
    int             irq;
};

static irqreturn_t my_isr(int irq, void *data)
{
    struct my_pcie_dev *mdev = data;
    /* TODO: 读状态寄存器、清中断、唤醒底半部 */
    return IRQ_HANDLED;
}

static int my_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct my_pcie_dev *mdev;
    int err;

    mdev = devm_kzalloc(&pdev->dev, sizeof(*mdev), GFP_KERNEL);
    if (!mdev)
        return -ENOMEM;

    mdev->pdev = pdev;
    pci_set_drvdata(pdev, mdev);

    /* 1. 使能 PCI 设备，并允许做 Bus Master（DMA） */
    err = pcim_enable_device(pdev);
    if (err)
        return dev_err_probe(&pdev->dev, err, "enable device failed\n");

    pci_set_master(pdev);

    /* 2. 申请并映射 BAR0 寄存器空间 */
    err = pcim_iomap_regions(pdev, BIT(MY_BAR), DRV_NAME);
    if (err)
        return dev_err_probe(&pdev->dev, err, "iomap regions failed\n");

    mdev->bar0 = pcim_iomap_table(pdev)[MY_BAR];
    if (!mdev->bar0)
        return dev_err_probe(&pdev->dev, -ENODEV, "iomap table failed\n");

    /* 3. 申请中断（优先 MSI，其次 INTx） */
    err = pci_alloc_irq_vectors(pdev, 1, 1,
                                PCI_IRQ_MSI | PCI_IRQ_LEGACY);
    if (err < 0)
        return dev_err_probe(&pdev->dev, err, "alloc irq vectors failed\n");

    mdev->irq = pci_irq_vector(pdev, 0);
    err = devm_request_irq(&pdev->dev, mdev->irq, my_isr,
                           0, DRV_NAME, mdev);
    if (err)
        return dev_err_probe(&pdev->dev, err, "request irq failed\n");

    /* 4. 在这里按设备手册配置寄存器，启动设备 */
    /* 例如：writel(START_BIT, mdev->bar0 + REG_CTRL); */

    dev_info(&pdev->dev, "my_pcie_demo probed: bus=%u dev=%u func=%u\n",
             pdev->bus->number,
             PCI_SLOT(pdev->devfn),
             PCI_FUNC(pdev->devfn));
    return 0;
}

static void my_remove(struct pci_dev *pdev)
{
    struct my_pcie_dev *mdev = pci_get_drvdata(pdev);

    /* 停止设备（按手册清控制位即可） */
    if (mdev && mdev->bar0) {
        /* 例如：writel(0, mdev->bar0 + REG_CTRL); */
    }

    pci_free_irq_vectors(pdev);
}

static const struct pci_device_id my_ids[] = {
    { PCI_DEVICE(MY_VENDOR_ID, MY_DEVICE_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, my_ids);

static struct pci_driver my_pcie_driver = {
    .name     = DRV_NAME,
    .id_table = my_ids,
    .probe    = my_probe,
    .remove   = my_remove,
};

module_pci_driver(my_pcie_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Simple PCIe demo driver");
```

### 3. 关键接口和参数对应关系

- **匹配某个具体设备：**
  - 在 `my_ids[]` 里写 `PCI_DEVICE(VID, DID)`：
    - `VID` / `DID` 来自 `lspci -nn` 里方括号中的 Vendor/Device ID。
  - 内核会在所有已枚举的 `struct pci_dev` 里找到 `vendor == VID && device == DID` 的那个，
    然后调用你的 `my_probe(pdev, id)`。

- **如何知道 bus / device / function？**
  - 在 `probe()` 里直接用：
    - `pdev->bus->number`      → bus 号
    - `PCI_SLOT(pdev->devfn)`  → device 号
    - `PCI_FUNC(pdev->devfn)`  → function 号  
  - 这些都是 **host 控制器驱动在枚举阶段已经分配好的**。

- **访问配置空间：**
  - `pci_read_config_*()` / `pci_write_config_*()`：
    ```c
    u16 cmd;
    pci_read_config_word(pdev, PCI_COMMAND, &cmd);
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    pci_write_config_word(pdev, PCI_COMMAND, cmd);
    ```

- **访问设备寄存器（通过 BAR）：**
  - 先 `pcim_iomap_regions()` + `pcim_iomap_table()` 得到 `void __iomem *bar`；
  - 再用 `readl()` / `writel()`：
    ```c
    u32 v = readl(mdev->bar0 + offset);
    writel(v | BIT(0), mdev->bar0 + offset);
    ```

### 4. 和 AXI Bridge / host 控制器的关系

- **host 控制器驱动（pcie-starfive / pcie-plda-host / pci-tegra 等）：**
  - 负责 RC / AXI‑Bridge 自己的寄存器、窗口、中断、Link、Slot 等配置；
  - 负责枚举、创建 `struct pci_dev`、实现 `map_bus/read/write` 这些底层操作。

- **普通 PCIe 设备驱动（上面的 demo）：**
  - 完全站在 `struct pci_dev` 的视角，只通过标准 PCI API 访问设备；
  - 不需要直接碰 AXI Bridge 寄存器，也不用自己去算 bus/devfn——这些都已经由 host 控制器设置好了。

可以记成一句话：**“host 驱动把 PCIe 总线搭好并分配好地址；设备驱动只管在 `probe(pdev)` 里通过 `pci_*` 接口访问那个具体设备。”**

