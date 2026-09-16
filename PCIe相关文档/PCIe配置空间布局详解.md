# PCIe 配置空间完整布局

本文档详细展示 PCIe 配置空间的完整布局，包括标准配置空间和 PCIe Capability 空间的完整结构。

## 目录

1. [配置空间概述](#配置空间概述)
2. [标准配置空间布局（256B）](#标准配置空间布局256b)
3. [扩展配置空间布局（4KB）](#扩展配置空间布局4kb)
4. [PCIe Capability 空间完整布局](#pcie-capability-空间完整布局)
5. [其他 Capability 空间](#其他-capability-空间)
6. [完整空间访问方法](#完整空间访问方法)

---

## 配置空间概述

### 配置空间大小

- **标准 PCI 配置空间**：256 字节（0x00 - 0xFF）
- **扩展 PCIe 配置空间**：4096 字节（0x00 - 0xFFF）

### BDF 与配置空间的关系（枚举后软件怎么看到设备）

- **BDF = Bus / Device / Function**：
  - Bus：0~255，根总线一般是 Bus 0，下游 Bus 号由软件在“枚举”过程中分配，写入各个 Bridge 的 `Primary/Secondary/Subordinate Bus Number` 寄存器。
  - Device (dev)：0~31，每个 Root Port / Switch 下游端口在硬件里固定连在某个 dev 槽位上，由芯片/主板设计时决定。
  - Function (func)：0~7，同一个 Device 下的多功能设备使用不同的 func。
- **硬件视角**：
  - 哪些 `(bus, dev, func)` 可能存在设备，是由硬件连线和枚举时写入的 Bus Number 寄存器共同决定的。
  - 对于片内固定的 RC/EP，SoC 手册通常会直接给出固定的 BDF（例如：`Bus 0, Dev 0, Func 0`）。
- **软件视角（“枚举出来”的含义）**：
  - 固件 / OS 通过循环扫描 `(bus, dev, func)` 读取 `Vendor ID`：
    - `Vendor ID != 0xFFFF` → 这里有设备。
  - 再读取 `Class Code` / `Header Type` 判断是 Endpoint 还是 Bridge：
    - 对 Bridge 分配新的 Bus 号，写回 Bridge 的 Bus Number 寄存器，并递归扫描新的 Bus。
  - **枚举结束后**：
    - 固件/裸机：通常把所有发现的设备的 BDF 等信息记录在自己的设备表里（数组/链表），后续访问配置空间或 BAR 时都从这个表里取 BDF。
    - 操作系统（例如 Linux）：内核维护 `struct pci_dev` 等数据结构，驱动在 `probe()` 回调中通过 `pdev` 参数间接拿到 BDF，而不需要自己枚举。

可以理解为：**硬件决定“BDF 空间里哪些位置可能有设备”，软件通过“枚举”确认“哪些 BDF 实际上挂了设备”，并把这些信息存入自己的数据结构中提供给驱动使用。**

### 扩展配置空间与厂商自定义内容

- **总容量上限**：
  - 每个 PCIe Function 的配置空间 **总大小上限是 4KB**，规范不允许再大。
  - 0x00–0xFF：标准配置空间（老 PCI 头 + 能力指针）。
  - 0x100–0xFFF：扩展配置空间，主要通过 Extended Capabilities 链表组织。
- **标准 Capability 与扩展 Capability**：
  - 标准 Capability：通过 0x34 的 `Capabilities Pointer` 链表挂接（PM、MSI、PCIe Cap 等）。
  - Extended Capability：从 0x100 开始，每个扩展能力都有自己的 ID/Next 字段（AER、SR-IOV 等）。
- **厂商自定义扩展的几种方式**：
  - **Vendor-Specific Capability (VSEC / DVSEC)**：
    - 在扩展能力空间中，预留了专门给厂商自定义的 Capability 类型。
    - 可以在 4KB 配置空间内部，放少量自定义寄存器（例如调试信息、版本号、少数控制位）。
    - 适合“体积小但希望通过 Config TLP 访问”的内容。
  - **通过 BAR 暴露大量寄存器（推荐）**：
    - 大部分自定义寄存器（配置寄存器、状态寄存器、设备内部 RAM 映射等）应该放在 **BAR 对应的 MMIO 空间** 中。
    - 配置空间里的 BAR 寄存器只保存“这片 MMIO 空间在系统物理地址里的起始地址 + 类型信息”，真正的寄存器布局由厂商在 BAR 后面的地址空间中自由定义。
  - **SoC 内部专用寄存器**：
    - 对于片内 PCIe 控制器的调试/测试寄存器，可以完全只做在片内 APB/AHB/AXI 空间，不通过 PCIe 配置空间和 BAR 暴露给外部 Host。

整体建议：**配置空间用来放标准结构 + 少量关键信息，大量自定义功能尽量放到 BAR 映射的 MMIO 空间里。**

### 配置空间访问方式

```
软件访问配置空间:
    │
    ├─> 通过 /sys/bus/pci/devices/.../config (Linux)
    ├─> 通过 pci_read_config_*() 函数 (内核)
    └─> 通过 lspci 命令 (用户空间)
```

---

## 标准配置空间布局（256B）

### 完整布局图

```
PCIe 配置空间 (256 字节 = 0x00 - 0xFF)
═══════════════════════════════════════════════════════════════════

偏移    大小    寄存器名称                    说明
─────────────────────────────────────────────────────────────────
0x00    2B      Vendor ID                    厂商 ID
0x02    2B      Device ID                    设备 ID
0x04    2B      Command                     命令寄存器
0x06    2B      Status                      状态寄存器
0x08    1B      Revision ID                  版本号
0x09    3B      Class Code                   类别代码
        │       ├─> Base Class (0x0B)       基础类别
        │       ├─> Sub Class (0x0A)        子类别
        │       └─> Prog IF (0x09)          编程接口
0x0C    1B      Cache Line Size              缓存行大小
0x0D    1B      Latency Timer                延迟定时器
0x0E    1B      Header Type                  头类型
0x0F    1B      BIST                         内建自检
─────────────────────────────────────────────────────────────────
0x10    4B      BAR0                         基址寄存器 0
0x14    4B      BAR1                         基址寄存器 1
0x18    4B      BAR2                         基址寄存器 2
0x1C    4B      BAR3                         基址寄存器 3
0x20    4B      BAR4                         基址寄存器 4
0x24    4B      BAR5                         基址寄存器 5
─────────────────────────────────────────────────────────────────
0x28    4B      Cardbus CIS Pointer           Cardbus CIS 指针
0x2C    2B      Subsystem Vendor ID          子系统厂商 ID
0x2E    2B      Subsystem ID                 子系统 ID
0x30    4B      Expansion ROM Base Address  扩展 ROM 基址
─────────────────────────────────────────────────────────────────
0x34    1B      Capabilities Pointer          能力指针 ← 指向 Capability 链表
0x35    3B      Reserved                     保留
─────────────────────────────────────────────────────────────────
0x38    4B      Reserved                     保留
0x3C    1B      Interrupt Line               中断线
0x3D    1B      Interrupt Pin                中断引脚
0x3E    2B      Min_Gnt / Max_Lat           最小授权/最大延迟
─────────────────────────────────────────────────────────────────
0x40-0xFF       Reserved / Vendor Specific   保留/厂商特定
─────────────────────────────────────────────────────────────────
```

### 标准配置空间详细说明

#### 0x00 - 0x0F: 设备识别和状态

```
0x00: Vendor ID (16-bit)
      ┌─────────────────┐
      │  0x1234         │  ← 厂商 ID（例如：Intel = 0x8086）
      └─────────────────┘

0x02: Device ID (16-bit)
      ┌─────────────────┐
      │  0x5678         │  ← 设备 ID（具体型号）
      └─────────────────┘

0x04: Command (16-bit)
      ┌─────────────────┐
      │  Bit 0: I/O     │  ← I/O 空间使能
      │  Bit 1: Memory  │  ← 内存空间使能
      │  Bit 2: Master  │  ← 总线主控使能
      │  Bit 3: Special │  ← 特殊周期
      │  Bit 4: VGA     │  ← VGA 调色板侦听
      │  Bit 5: Parity  │  ← 奇偶校验错误响应
      │  Bit 6: SERR    │  ← SERR# 使能
      │  Bit 7: Fast    │  ← 快速背对背使能
      │  Bit 8: INTx    │  ← INTx 中断禁用
      │  Bit 9: INTx    │  ← INTx 中断禁用
      └─────────────────┘

0x06: Status (16-bit)
      ┌─────────────────┐
      │  Bit 4: Cap     │  ← Capabilities List 存在
      │  Bit 5: 66MHz   │  ← 66MHz 能力
      │  Bit 6: UDF     │  ← 用户定义功能
      │  Bit 7: Fast    │  ← 快速背对背能力
      │  Bit 8: Master  │  ← 主控数据奇偶校验错误
      │  Bit 10: DEVSEL │  ← DEVSEL# 时序
      │  Bit 11: TAbort │  ← 目标中止
      │  Bit 12: TAbort │  ← 主控中止
      │  Bit 13: SERR   │  ← SERR# 信号
      │  Bit 14: Parity │  ← 检测到奇偶校验错误
      └─────────────────┘

0x08: Revision ID (8-bit)
      ┌─────────────────┐
      │  0x01           │  ← 设备版本号
      └─────────────────┘

0x09-0x0B: Class Code (24-bit)
      ┌─────────────────┐
      │  0x0B: Base     │  ← 基础类别（例如：0x02 = 网络控制器）
      │  0x0A: Sub      │  ← 子类别
      │  0x09: Prog IF  │  ← 编程接口
      └─────────────────┘
```

#### 0x10 - 0x27: Base Address Registers

```
0x10: BAR0 (32-bit 或 64-bit)
      ┌─────────────────┐
      │  Bit 0: Type     │  ← 0=Memory, 1=I/O
      │  Bit 1-2: Loc    │  ← 位置（Memory 类型）
      │  Bit 3: Prefetch  │  ← 可预取（Memory 类型）
      │  Bit 4-31: Addr  │  ← 基址（分配后）
      └─────────────────┘

0x14: BAR1 (32-bit 或 64-bit)
      └─> 如果 BAR0 是 64-bit，BAR1 是低 32-bit 的高 32-bit

0x18: BAR2
0x1C: BAR3
0x20: BAR4
0x24: BAR5
```

##### BAR 大小与“最大地址空间”的理解

- **BAR 寄存器本身只有 32bit（或两个组成 64bit）**：
  - 低几位用于类型信息（I/O / Memory、32/64bit、Prefetchable 等）。
  - 高位用于保存“起始基址”，同时通过哪些位可以被写成 1/0 的掩码来反推 BAR 需要的大小。
- **BAR 空间的理论上限**：
  - 对于 **32bit Memory BAR**：最大可以表示一个 **4GB** 对齐的空间。
  - 对于 **64bit Memory BAR（BAR0+BAR1 组合）**：理论上可以覆盖整个 64bit 物理地址空间，但实际上受平台物理地址位宽限制（例如 48bit/52bit）。
- **设备实际支持的 BAR 大小**：
  - 由设备内部实现决定。软件通过“写全 1 再读回”的方式获得大小掩码来推算真实大小：
    - 写 `0xFFFF_FFFF` 到 BAR。
    - 读回值中，高位为 0 的那一段表示“地址对齐 + 大小”，据此计算出 BAR 所需的空间大小（例如 4KB、64KB、1MB、1GB 等）。
  - 规范只提供“最大可能值”的表达方式，**并不强制要求 BAR0 一定要很大，多大完全取决于 IP 设计者的需求。**

##### BAR0 的含义：规范约束 + 厂商自由度

- **规范约束的部分**：
  - BAR0 这个字段的 **位置（0x10）和格式** 是标准化的：
    - Bit0：0 = Memory Space，1 = I/O Space。
    - Bit1–2：Memory 类型（32bit、64bit 等）。
    - Bit3：Prefetchable 标志。
    - 其余高位：基址（按规范对齐）。
  - 一个 Function 最多 6 个 BAR（BAR0–BAR5），还要遵守 64bit BAR 需要占用两个连续 BAR 槽位的规则。
- **厂商可以自由定义的部分**：
  - **BAR0 表示的“这片地址空间多大”**：由厂商在 RTL 里通过掩码实现，可以是 4KB、64KB、1MB、1GB 等。
  - **BAR0 映射空间内部的寄存器布局**：
    - 从 `BAR0_BASE + 0x0` 到 `BAR0_BASE + (size-1)` 之间的每一个 offset 上具体是什么寄存器 / FIFO / 内部 RAM，完全由厂商自定义。
    - 驱动通过文档（寄存器 map）知道这些 offset，然后通过 MMIO 访问。

可以简单理解为：

- **PCIe 规范：定义了“BAR 是一个门牌号 + 类型标志”，门牌号字段在配置空间 0x10–0x27；**
- **厂商：决定“这块门牌后面是多大的地（BAR 大小），地里有哪几栋楼（寄存器/缓冲区布局）”。**

#### 0x34: Capabilities Pointer

```
0x34: Capabilities Pointer (8-bit)
      ┌─────────────────┐
      │  0x50           │  ← 指向第一个 Capability 的偏移
      │                 │     (0x00 或 0xFF = 无 Capability)
      └─────────────────┘
           │
           ▼
      Capability 链表开始
```

---

## 扩展配置空间布局（4KB）

### PCIe 扩展配置空间

```
标准配置空间 (0x00 - 0xFF): 256 字节
    │
    ├─> 标准 PCI 寄存器（如上）
    │
    └─> Capability 链表（从 0x34 指针开始）
            │
            ▼
扩展配置空间 (0x100 - 0xFFF): 3840 字节
    │
    ├─> 更多 Capability
    ├─> 扩展配置寄存器
    └─> 厂商特定空间
```

---

## PCIe Capability 空间完整布局

### Capability 链表结构

```
配置空间 0x34: Capability Pointer → 0x50
    │
    ▼
0x50: 第一个 Capability
    ├─> Capability ID (8-bit)
    ├─> Next Pointer (8-bit) → 指向下一个 Capability
    └─> Capability 特定数据
            │
            ▼
    下一个 Capability (根据 Next Pointer)
            │
            ▼
    ... (链表继续)
            │
            ▼
    最后一个 Capability
    └─> Next Pointer = 0x00 (链表结束)
```

### PCIe Capability 完整布局

**PCIe Capability 从偏移 0x50 开始（假设）：**

```
PCIe Capability 空间完整布局
═══════════════════════════════════════════════════════════════════

偏移    大小    寄存器名称                    位定义
─────────────────────────────────────────────────────────────────
0x50    1B      Capability ID                 0x10 (PCIe Capability)
0x51    1B      Next Capability Pointer       下一个 Capability 偏移
0x52    2B      PCIe Capabilities Register   PCIe 能力寄存器
─────────────────────────────────────────────────────────────────
0x54    4B      Device Capabilities (DevCap)  设备能力
        │       ├─> Bit 0-2: Max Payload Size
        │       ├─> Bit 3-5: Phantom Functions
        │       ├─> Bit 6: Extended Tag
        │       ├─> Bit 7-9: Endpoint L0s Acceptable Latency
        │       ├─> Bit 10-12: Endpoint L1 Acceptable Latency
        │       ├─> Bit 13: Role Based Error Reporting
        │       ├─> Bit 14-15: Captured Slot Power Limit
        │       └─> Bit 16-31: 其他能力位
─────────────────────────────────────────────────────────────────
0x58    2B      Device Control (DevCtl)      设备控制（可读写）
        │       ├─> Bit 0: Correctable Error Reporting
        │       ├─> Bit 1: Non-Fatal Error Reporting
        │       ├─> Bit 2: Fatal Error Reporting
        │       ├─> Bit 3: Unsupported Request Reporting
        │       ├─> Bit 4: Enable Relaxed Ordering
        │       ├─> Bit 5: Max Payload Size
        │       ├─> Bit 6-7: Extended Tag Field
        │       ├─> Bit 8: Phantom Functions Enable
        │       ├─> Bit 9: Aux Power PM Enable
        │       ├─> Bit 10: No Snoop Enable
        │       └─> Bit 11-15: 其他控制位
─────────────────────────────────────────────────────────────────
0x5A    2B      Device Status (DevSta)       设备状态（只读）
        │       ├─> Bit 0: Correctable Error Detected
        │       ├─> Bit 1: Non-Fatal Error Detected
        │       ├─> Bit 2: Fatal Error Detected
        │       ├─> Bit 3: Unsupported Request Detected
        │       ├─> Bit 4: Aux Power Detected
        │       └─> Bit 5: Transactions Pending
─────────────────────────────────────────────────────────────────
0x5C    4B      Link Capabilities (LnkCap)   链路能力（只读）
        │       ├─> Bit 0-3: Max Link Speed
        │       ├─> Bit 4-9: Max Link Width
        │       ├─> Bit 10-11: ASPM Support
        │       ├─> Bit 12-14: L0s Exit Latency
        │       ├─> Bit 15-17: L1 Exit Latency
        │       ├─> Bit 18: Clock Power Management
        │       ├─> Bit 19: Surprise Down Error Reporting
        │       ├─> Bit 20: Data Link Layer Link Active Reporting
        │       ├─> Bit 21: Link Bandwidth Notification Capability
        │       ├─> Bit 22: Link Autonomous Bandwidth Status
        │       ├─> Bit 23: Clock PM Synchronization
        │       ├─> Bit 24-26: Data Link Layer Active Reporting
        │       ├─> Bit 27: Port Number
        │       └─> Bit 28-31: PHY Version
─────────────────────────────────────────────────────────────────
0x60    2B      Link Control (LnkCtl)        链路控制（可读写）
        │       ├─> Bit 0-1: ASPM Control
        │       ├─> Bit 2: Read Completion Boundary (RCB)
        │       ├─> Bit 3: Link Disable
        │       ├─> Bit 4: Retrain Link
        │       ├─> Bit 5: Common Clock Configuration
        │       ├─> Bit 6: Extended Synch
        │       ├─> Bit 7: Clock Power Management Enable
        │       ├─> Bit 8: Hardware Autonomous Width Disable
        │       ├─> Bit 9: Link Bandwidth Management Interrupt Enable
        │       └─> Bit 10: Link Autonomous Bandwidth Interrupt Enable
─────────────────────────────────────────────────────────────────
0x62    2B      Link Status (LnkSta)        链路状态（只读）← 训练结果
        │       ├─> Bit 0-3: Current Link Speed
        │       ├─> Bit 4-9: Negotiated Link Width
        │       ├─> Bit 10: Link Training
        │       ├─> Bit 11: Slot Clock Configuration
        │       ├─> Bit 12: Data Link Layer Link Active
        │       ├─> Bit 13: Link Bandwidth Management Status
        │       ├─> Bit 14: Link Autonomous Bandwidth Status
        │       └─> Bit 15: Clock Configuration Status
─────────────────────────────────────────────────────────────────
0x64    2B      Slot Capabilities (SltCap)   插槽能力（仅 Root Port/Switch）
0x66    2B      Slot Control (SltCtl)       插槽控制
0x68    2B      Slot Status (SltSta)        插槽状态
─────────────────────────────────────────────────────────────────
0x6A    2B      Root Capabilities (RtCap)    根能力（仅 Root Port）
0x6C    2B      Root Control (RtCtl)         根控制
0x6E    2B      Root Status (RtSta)          根状态
─────────────────────────────────────────────────────────────────
0x70    4B      Device Capabilities 2 (DevCap2) 设备能力 2
0x74    2B      Device Control 2 (DevCtl2)   设备控制 2
0x76    2B      Device Status 2 (DevSta2)    设备状态 2
─────────────────────────────────────────────────────────────────
0x78    4B      Link Capabilities 2 (LnkCap2) 链路能力 2
0x7C    2B      Link Control 2 (LnkCtl2)    链路控制 2
0x7E    2B      Link Status 2 (LnkSta2)      链路状态 2
─────────────────────────────────────────────────────────────────
0x80+   可变    其他扩展寄存器（根据 PCIe 版本）
─────────────────────────────────────────────────────────────────
```

### PCIe Capability 空间可视化

```
PCIe Capability 空间 (从 0x50 开始，假设)
═══════════════════════════════════════════════════════════════════

0x50 ┌─────────────────────────────────────┐
     │ Capability ID: 0x10                 │ ← PCIe Capability
     │ Next Pointer: 0x80                 │ → 指向下一个 Capability
     └─────────────────────────────────────┘
     
0x52 ┌─────────────────────────────────────┐
     │ PCIe Capabilities Register          │
     └─────────────────────────────────────┘
     
0x54 ┌─────────────────────────────────────┐
     │ Device Capabilities (DevCap)       │ ← 设备能力
     │ - Max Payload Size                 │
     │ - Phantom Functions                │
     │ - Extended Tag                     │
     │ - Endpoint Latency                 │
     └─────────────────────────────────────┘
     
0x58 ┌─────────────────────────────────────┐
     │ Device Control (DevCtl)            │ ← 设备控制（可写）
     │ - Error Reporting Enable          │
     │ - Max Payload Size                │
     │ - Phantom Functions Enable         │
     └─────────────────────────────────────┘
     
0x5A ┌─────────────────────────────────────┐
     │ Device Status (DevSta)            │ ← 设备状态
     │ - Error Status                    │
     │ - Aux Power                        │
     │ - Transaction Pending              │
     └─────────────────────────────────────┘
     
0x5C ┌─────────────────────────────────────┐
     │ Link Capabilities (LnkCap)         │ ← 链路能力
     │ - Max Speed: Gen1/2/3/4/5          │
     │ - Max Width: x1/x2/x4/x8/x16      │
     │ - ASPM Support                     │
     │ - Exit Latency                     │
     └─────────────────────────────────────┘
     
0x60 ┌─────────────────────────────────────┐
     │ Link Control (LnkCtl)              │ ← 链路控制（可写）
     │ - ASPM Control                     │
     │ - Retrain Link                     │
     │ - Common Clock                     │
     └─────────────────────────────────────┘
     
0x62 ┌─────────────────────────────────────┐
     │ Link Status (LnkSta)               │ ← 链路状态（训练结果）
     │ - Current Speed: Gen?              │ ← 训练后的速度
     │ - Current Width: x?                │ ← 训练后的宽度
     │ - Link Training: Complete/In Prog │
     │ - DLLActive: Yes/No                │ ← 链路是否激活
     └─────────────────────────────────────┘
     
0x64 ┌─────────────────────────────────────┐
     │ Slot Capabilities (SltCap)         │ ← 插槽能力（Root Port）
     └─────────────────────────────────────┘
     
0x66 ┌─────────────────────────────────────┐
     │ Slot Control (SltCtl)              │ ← 插槽控制
     └─────────────────────────────────────┘
     
0x68 ┌─────────────────────────────────────┐
     │ Slot Status (SltSta)               │ ← 插槽状态
     └─────────────────────────────────────┘
     
0x6A ┌─────────────────────────────────────┐
     │ Root Capabilities (RtCap)          │ ← 根能力（Root Port）
     └─────────────────────────────────────┘
     
0x6C ┌─────────────────────────────────────┐
     │ Root Control (RtCtl)               │ ← 根控制
     └─────────────────────────────────────┘
     
0x6E ┌─────────────────────────────────────┐
     │ Root Status (RtSta)                │ ← 根状态
     └─────────────────────────────────────┘
     
0x70+ ┌─────────────────────────────────────┐
      │ Extended Capabilities             │ ← 扩展能力（PCIe 2.0+）
      └─────────────────────────────────────┘
```

---

## 其他 Capability 空间

### 常见的其他 Capability

```
Capability 链表可能包含：
│
├─> PCIe Capability (0x10) - 0x50
│   └─> Next Pointer → 0x70
│
├─> Power Management Capability (0x01) - 0x70
│   └─> Next Pointer → 0x90
│
├─> MSI Capability (0x05) - 0x90
│   └─> Next Pointer → 0xB0
│
├─> MSI-X Capability (0x11) - 0xB0
│   └─> Next Pointer → 0xD0
│
└─> Advanced Error Reporting (0x01) - 0xD0
    └─> Next Pointer → 0x00 (链表结束)
```

### Capability ID 列表

| Capability ID | 名称 | 说明 |
|--------------|------|------|
| 0x01 | Power Management | 电源管理 |
| 0x05 | MSI | Message Signaled Interrupts |
| 0x10 | PCIe | PCIe Capability |
| 0x11 | MSI-X | Extended MSI |
| 0x13 | Advanced Error Reporting | 高级错误报告 |

---

## 完整空间访问方法

### 1. 读取整个配置空间

```c
// 读取整个配置空间（256B 或 4KB）
void read_full_config_space(struct pci_dev *pdev)
{
    u8 *config;
    int size = pdev->cfg_size;  // 256 或 4096
    
    config = kmalloc(size, GFP_KERNEL);
    if (!config)
        return;
    
    // 读取整个配置空间
    for (int i = 0; i < size; i++) {
        pci_read_config_byte(pdev, i, &config[i]);
    }
    
    // 打印配置空间内容
    print_hex_dump(KERN_INFO, "Config Space: ", DUMP_PREFIX_OFFSET,
                   16, 1, config, size, true);
    
    kfree(config);
}
```

### 2. 遍历 Capability 链表

```c
// 遍历所有 Capability
void traverse_capabilities(struct pci_dev *pdev)
{
    u8 cap_ptr;
    u8 cap_id;
    int count = 0;
    
    // 读取 Capability Pointer
    pci_read_config_byte(pdev, PCI_CAPABILITY_LIST, &cap_ptr);
    
    if (cap_ptr == 0 || cap_ptr == 0xFF) {
        pr_info("No capabilities\n");
        return;
    }
    
    pr_info("=== Capability List ===\n");
    
    // 遍历链表
    while (cap_ptr && count < 48) {  // 防止无限循环
        // 读取 Capability ID
        pci_read_config_byte(pdev, cap_ptr, &cap_id);
        
        pr_info("Capability at 0x%02x: ID=0x%02x", cap_ptr, cap_id);
        
        // 根据 ID 打印名称
        switch (cap_id) {
        case 0x01:
            pr_cont(" (Power Management)\n");
            break;
        case 0x05:
            pr_cont(" (MSI)\n");
            break;
        case 0x10:
            pr_cont(" (PCIe)\n");
            // 可以在这里读取 PCIe Capability 的详细信息
            read_pcie_capability(pdev, cap_ptr);
            break;
        case 0x11:
            pr_cont(" (MSI-X)\n");
            break;
        default:
            pr_cont(" (Unknown)\n");
            break;
        }
        
        // 读取 Next Pointer
        pci_read_config_byte(pdev, cap_ptr + 1, &cap_ptr);
        
        if (cap_ptr == 0 || cap_ptr == 0xFF)
            break;
        
        count++;
    }
}
```

### 3. 读取 PCIe Capability 完整空间

```c
// 读取 PCIe Capability 的完整空间
void read_pcie_capability_space(struct pci_dev *pdev, int cap_ptr)
{
    u16 val16;
    u32 val32;
    
    pr_info("=== PCIe Capability Space (starting at 0x%02x) ===\n", cap_ptr);
    
    // Capability ID 和 Next Pointer
    u8 cap_id, next_ptr;
    pci_read_config_byte(pdev, cap_ptr + 0x00, &cap_id);
    pci_read_config_byte(pdev, cap_ptr + 0x01, &next_ptr);
    pr_info("0x%02x: Capability ID = 0x%02x\n", cap_ptr + 0x00, cap_id);
    pr_info("0x%02x: Next Pointer = 0x%02x\n", cap_ptr + 0x01, next_ptr);
    
    // PCIe Capabilities Register
    pci_read_config_word(pdev, cap_ptr + 0x02, &val16);
    pr_info("0x%02x: PCIe Capabilities = 0x%04x\n", cap_ptr + 0x02, val16);
    
    // Device Capabilities
    pci_read_config_dword(pdev, cap_ptr + 0x04, &val32);
    pr_info("0x%02x: Device Capabilities = 0x%08x\n", cap_ptr + 0x04, val32);
    
    // Device Control
    pci_read_config_word(pdev, cap_ptr + 0x08, &val16);
    pr_info("0x%02x: Device Control = 0x%04x\n", cap_ptr + 0x08, val16);
    
    // Device Status
    pci_read_config_word(pdev, cap_ptr + 0x0A, &val16);
    pr_info("0x%02x: Device Status = 0x%04x\n", cap_ptr + 0x0A, val16);
    
    // Link Capabilities
    pci_read_config_dword(pdev, cap_ptr + 0x0C, &val32);
    pr_info("0x%02x: Link Capabilities = 0x%08x\n", cap_ptr + 0x0C, val32);
    
    // Link Control
    pci_read_config_word(pdev, cap_ptr + 0x10, &val16);
    pr_info("0x%02x: Link Control = 0x%04x\n", cap_ptr + 0x10, val16);
    
    // Link Status (训练结果)
    pci_read_config_word(pdev, cap_ptr + 0x12, &val16);
    pr_info("0x%02x: Link Status = 0x%04x (Training Results)\n", 
            cap_ptr + 0x12, val16);
    
    // 解析 Link Status
    u8 speed = val16 & 0x0F;
    u8 width = (val16 >> 4) & 0x3F;
    bool training = !!(val16 & (1 << 10));
    bool dll_active = !!(val16 & (1 << 12));
    
    pr_info("  - Speed: Gen%d\n", speed);
    pr_info("  - Width: x%d\n", width);
    pr_info("  - Training: %s\n", training ? "In Progress" : "Complete");
    pr_info("  - DLLActive: %s\n", dll_active ? "Yes" : "No");
    
    // Slot Capabilities (如果是 Root Port)
    pci_read_config_word(pdev, cap_ptr + 0x14, &val16);
    pr_info("0x%02x: Slot Capabilities = 0x%04x\n", cap_ptr + 0x14, val16);
    
    // Slot Control
    pci_read_config_word(pdev, cap_ptr + 0x18, &val16);
    pr_info("0x%02x: Slot Control = 0x%04x\n", cap_ptr + 0x18, val16);
    
    // Slot Status
    pci_read_config_word(pdev, cap_ptr + 0x1A, &val16);
    pr_info("0x%02x: Slot Status = 0x%04x\n", cap_ptr + 0x1A, val16);
    
    // Root Capabilities (如果是 Root Port)
    pci_read_config_word(pdev, cap_ptr + 0x1C, &val16);
    pr_info("0x%02x: Root Capabilities = 0x%04x\n", cap_ptr + 0x1C, val16);
    
    // Root Control
    pci_read_config_word(pdev, cap_ptr + 0x1E, &val16);
    pr_info("0x%02x: Root Control = 0x%04x\n", cap_ptr + 0x1E, val16);
    
    // Root Status
    pci_read_config_word(pdev, cap_ptr + 0x20, &val16);
    pr_info("0x%02x: Root Status = 0x%04x\n", cap_ptr + 0x20, val16);
}
```

### 4. 完整的配置空间转储工具

```c
// 完整的配置空间转储函数
void dump_full_config_space(struct pci_dev *pdev)
{
    u8 *config;
    int size = pdev->cfg_size;
    int i;
    
    pr_info("=== Full Configuration Space Dump ===\n");
    pr_info("Device: %04x:%04x\n", pdev->vendor, pdev->device);
    pr_info("Config Space Size: %d bytes\n", size);
    
    config = kmalloc(size, GFP_KERNEL);
    if (!config)
        return;
    
    // 读取整个配置空间
    for (i = 0; i < size; i++) {
        pci_read_config_byte(pdev, i, &config[i]);
    }
    
    // 打印标准配置空间 (0x00 - 0xFF)
    pr_info("\n--- Standard Configuration Space (0x00 - 0xFF) ---\n");
    for (i = 0; i < 256; i += 16) {
        pr_info("%04x: %02x %02x %02x %02x %02x %02x %02x %02x "
                "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                i,
                config[i+0], config[i+1], config[i+2], config[i+3],
                config[i+4], config[i+5], config[i+6], config[i+7],
                config[i+8], config[i+9], config[i+10], config[i+11],
                config[i+12], config[i+13], config[i+14], config[i+15]);
    }
    
    // 如果是扩展配置空间，打印扩展部分
    if (size > 256) {
        pr_info("\n--- Extended Configuration Space (0x100 - 0xFFF) ---\n");
        for (i = 256; i < size; i += 16) {
            pr_info("%04x: %02x %02x %02x %02x %02x %02x %02x %02x "
                    "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    i,
                    config[i+0], config[i+1], config[i+2], config[i+3],
                    config[i+4], config[i+5], config[i+6], config[i+7],
                    config[i+8], config[i+9], config[i+10], config[i+11],
                    config[i+12], config[i+13], config[i+14], config[i+15]);
        }
    }
    
    // 解析关键寄存器
    pr_info("\n--- Key Registers ---\n");
    pr_info("Vendor ID: 0x%04x\n", config[0] | (config[1] << 8));
    pr_info("Device ID: 0x%04x\n", config[2] | (config[3] << 8));
    pr_info("Command: 0x%04x\n", config[4] | (config[5] << 8));
    pr_info("Status: 0x%04x\n", config[6] | (config[7] << 8));
    pr_info("Class Code: 0x%06x\n", 
            config[9] | (config[10] << 8) | (config[11] << 16));
    pr_info("Capability Pointer: 0x%02x\n", config[0x34]);
    
    // 遍历 Capability
    if (config[0x34] != 0 && config[0x34] != 0xFF) {
        pr_info("\n--- Capability List ---\n");
        traverse_capabilities(pdev);
    }
    
    kfree(config);
}
```

### 5. 使用示例

```c
static int my_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    // ... 其他初始化 ...
    
    // 转储整个配置空间（调试用）
    dump_full_config_space(pdev);
    
    // 或者只读取 PCIe Capability
    int pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (pos) {
        read_pcie_capability_space(pdev, pos);
    }
    
    // ... 继续初始化 ...
}
```

---

## 配置空间映射关系

### 完整的内存映射视图

```
系统内存视图:
┌─────────────────────────────────────────┐
│ 配置空间 (通过 /sys/bus/pci/devices/   │
│            .../config 访问)            │
│                                         │
│  0x00 - 0xFF: 标准配置空间            │
│  0x100 - 0xFFF: 扩展配置空间          │
└─────────────────────────────────────────┘

PCIe Capability 在配置空间中的位置:
┌─────────────────────────────────────────┐
│ 配置空间 0x34: Capability Pointer      │
│   → 指向 0x50 (例如)                   │
│                                         │
│  0x50: PCIe Capability 开始            │
│    ├─> 0x50: Capability ID            │
│    ├─> 0x51: Next Pointer              │
│    ├─> 0x52: PCIe Capabilities        │
│    ├─> 0x54: Device Capabilities      │
│    ├─> 0x58: Device Control           │
│    ├─> 0x5A: Device Status            │
│    ├─> 0x5C: Link Capabilities        │
│    ├─> 0x60: Link Control             │
│    ├─> 0x62: Link Status ← 训练结果   │
│    └─> 0x64+: 其他寄存器              │
└─────────────────────────────────────────┘
```

---

## 总结

### 配置空间完整结构

1. **标准配置空间 (0x00 - 0xFF)**：
   - 设备识别（Vendor ID, Device ID）
   - 命令和状态
   - BAR0~BAR5
   - Capability Pointer

2. **PCIe Capability 空间**：
   - 从 Capability Pointer 指向的位置开始
   - 包含所有 PCIe 相关寄存器
   - **Link Status (0x12) 包含训练结果**

3. **扩展配置空间 (0x100 - 0xFFF)**：
   - 更多 Capability
   - 扩展寄存器

### 软件访问方式

- **读取整个配置空间**：`pci_read_config_byte/word/dword()`
- **遍历 Capability**：从 Capability Pointer 开始遍历链表
- **读取 PCIe Capability**：找到 PCIe Capability 后，读取各个寄存器

### 训练结果位置

**所有训练结果都在 PCIe Capability 的 Link Status 寄存器 (偏移 0x12)**，包括：
- 当前速度、宽度
- 训练状态
- 数据链路层状态
- 其他链路状态信息

