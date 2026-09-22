# PCIe 与 NoC 术语解释大全

本文档汇总了 PCIe 和 NoC 相关的重要术语和概念，便于快速查阅。

## 目录

1. [PCIe 基础术语](#pcie-基础术语)
2. [PCIe 配置与枚举](#pcie-配置与枚举)
3. [PCIe 物理层与链路](#pcie-物理层与链路)
4. [NoC 基础术语](#noc-基础术语)
5. [NoC 一致性协议](#noc-一致性协议)
6. [NoC 地址映射](#noc-地址映射)
7. [系统架构相关](#系统架构相关)

---

## PCIe 基础术语

### RC / EP / DM 模式

- **RC (Root Complex) 模式**：
  - PCIe 控制器作为"主机一侧"，负责枚举、BAR 分配、路由表配置
  - 典型位置：SoC 里连在 NoC/AXI 上，朝外伸出 PCIe 插槽
  - 职责：把 CPU/NoC 的 load/store ↔ PCIe TLP 互相转换

- **EP (Endpoint) 模式**：
  - PCIe 控制器作为"设备本身"，被外部主机枚举
  - 典型位置：外插板卡上的 PCIe 控制器，或 SoC 里的集成 Endpoint
  - 职责：暴露配置空间/BAR，实现具体功能（DMA、队列、寄存器）

- **DM (Dual Mode) 模式**：
  - 同一个控制器既具备 RC 能力，又具备 EP 能力
  - 可以通过配置寄存器/引脚选择当前角色，或同时暴露两种角色
  - 用途：一个 IP 既能用在 SoC 里当 RC，又能综合到 FPGA/ASIC 里当 EP

### BDF (Bus/Device/Function)

- **Bus (总线号)**：
  - Root Bus 一般固定为 0
  - Secondary/Subordinate Bus 由 BIOS/固件/OS 在枚举时动态分配，写进桥的 Bus Number 寄存器

- **Device (设备号)**：
  - 0 ~ 31，每个 Root Port/Switch 下游端口/插槽在硬件里自带固定编号
  - 在芯片/主板设计时定死，不是运行时协商

- **Function (功能号)**：
  - 0 ~ 7，一个设备可以有多个功能（Multi-Function Device）
  - 也是硬件设计时定死的

- **枚举后软件获取 BDF**：
  - 固件/裸机：在枚举代码里自己把每个发现的设备的 BDF 记到数组/链表里
  - 操作系统：OS 帮你枚举并建立内部数据结构（如 Linux 的 `struct pci_dev`），驱动通过 OS 提供的回调参数/API 拿到 BDF

### BAR (Base Address Register)

- **BAR0~BAR5**：
  - 每个 PCIe Function 最多 6 个 BAR
  - 32bit 或 64bit（BAR0+BAR1 组合）
  - 格式由 PCIe 规范固定，但大小和映射空间内部寄存器布局完全由厂商按需求自定义

- **BAR 映射最大地址**：
  - 32bit Memory BAR：理论最大 4GB 连续空间
  - 64bit Memory BAR：理论可以映射到整个 64bit 物理地址空间（受平台物理地址宽度限制）
  - 真正能映射多大，取决于设备自己实现的 BAR 尺寸和 BIOS/OS 能分配多少连续物理空间

- **BAR 地址分配流程**：
  1. 探测大小：往 BAR 寄存器写 `0xFFFF_FFFF`，通过读回值的 0 变 1 位置算出需要的大小
  2. 分配一段对齐的物理地址区间（如 0xFEA0_0000 ~ 0xFEA0_FFFF）
  3. 把这段物理地址的起始地址写回 BAR 寄存器
  4. 之后 CPU 访问这段系统物理地址时，Root Complex 会根据 BAR 内容转换为 PCIe TLP

### 配置空间

- **标准配置空间**：256 字节（0x00 - 0xFF）
- **扩展配置空间**：4096 字节（0x00 - 0xFFF）
  - 0x000 ~ 0x0FF：传统 256B 头部
  - 0x100 ~ 0xFFF：Extended Configuration Space，通过 Extended Capabilities 链接

- **厂商扩展方式**：
  - **VSEC/DVSEC (Vendor-Specific Capability)**：在 Extended Cap 链表里挂自定义寄存器
  - **BAR 映射 MMIO**：大量自定义寄存器放到 BAR 映射的 MMIO 空间里（推荐）
  - **片内专用寄存器**：只给内部 CPU/调试用的寄存器，做在 SoC 内部 APB/AHB/AXI 寄存器空间里

---

## PCIe 配置与枚举

### 枚举时机

- **固件阶段（BIOS/UEFI/Bootloader）**：
  - 早期枚举：使能 RC、开链路训练、给基本设备配置 BAR
  - 让后续启动（加载内核）能正常进行

- **操作系统 PCI 子系统初始化**：
  - 内核启动后，OS 的 PCI/PCIe 核心代码做完整枚举
  - 从 Root Bus 开始，按 BDF 扫设备、识别桥、分配 Bus 号、建立拓扑结构

- **设备驱动阶段**：
  - OS 枚举完成后，内核根据 vendor/device/class 匹配驱动
  - 调用驱动的 `probe()` 函数，此时 BDF、BAR、Capability 等信息都已经有了

### 设备驱动的作用

- **初始化设备内部寄存器**：复位、配置模式、建立 DMA 队列/描述符/Ring Buffer、使能中断
- **把设备能力挂到内核子系统**：网卡→netdev、存储→block、普通外设→char/misc/input
- **处理中断和 DMA**：中断处理、DMA 管理、描述符链表、Ring Buffer、地址映射（IOMMU）
- **电源管理、错误处理、热插拔、重训练**：suspend/resume、ASPM、AER、Link retrain

### 训练 vs 配置

- **链路训练（硬件自动）**：
  - 确定物理层的 Gen（速率）、Width（Lane 数）
  - 结果在 `Link Status` 寄存器里
  - 一般不频繁改，因为意味着 retrain，会中断传输

- **驱动配置（软件设置）**：
  - 队列数、Buffer 大小、DMA 描述符个数、工作模式
  - 这些是设备内部逻辑的工作模式，和 PCIe 物理层训练是两回事
  - 通过 BAR 映射的 MMIO 寄存器配置，随时可以改

---

## PCIe 物理层与链路

### Gen1/2/3/4 (PCIe 速率等级)

| 代际 | 速率 (GT/s) | 编码 | 有效数据率 (Gbps/lane) | x1 有效带宽 (单向) |
|------|------------|------|----------------------|-------------------|
| Gen1 | 2.5 | 8b/10b | ~2.0 | ~250 MB/s |
| Gen2 | 5.0 | 8b/10b | ~4.0 | ~500 MB/s |
| Gen3 | 8.0 | 128b/130b | ~7.88 | ~985 MB/s |
| Gen4 | 16.0 | 128b/130b | ~15.75 | ~1.97 GB/s |

- **GT/s**：Giga Transfers per second（每秒传输次数），不是直接的 Gbps
- **编码效率**：Gen1/2 的 8/10，Gen3/4 的 128/130
- **多 Lane 带宽**：每个 Lane 带宽 × Lane 数（x4、x8、x16）

### SerDes 三部分

- **PLL (Phase-Locked Loop)**：
  - 把参考时钟倍频，生成高速发送/接收用的位时钟
  - 是整个 SerDes 的"心脏节拍器"

- **Serializer (串化器，发送方向)**：
  - 把控制器/PCS 给的并行数据（8bit/16bit/32bit 宽）串行化
  - 用 PLL 产生的高速时钟，把这些并行位按顺序推到一条线（差分对）上
  - 加上线编码、前驱加重、幅度/摆率控制等发射端模拟电路

- **Deserializer (解串器，接收方向，含 CDR)**：
  - 从线上收到高速串行比特流，先经过 CTLE/DFE 等模拟均衡
  - CDR 从比特边沿里恢复出采样时钟，在"眼图中心"对接收波形采样
  - 把采样到的一串比特重新按宽度（8/16/32bit 等）拼回并行数据，交给 PCS/控制器

### PIPE (PHY Interface for PCI Express)

- **定位**：PCIe 控制器侧（链路层/PCS 上半部分）和物理层 PHY（PCS 下半部分 + PMA/SerDes）之间的标准化数字接口
- **作用**：
  - 对 MAC/Controller/链路层：只看到一组"并行的、低速的数字信号接口"（数据总线、控制信号、状态信号）
  - 对 PHY：只要按 PIPE 规范收发这些数字信号，就能被各种不同家的 PCIe 控制器复用
- **本质**：把"链路/协议层"和"模拟物理层"解耦

### bif mode (Bifurcation Mode)

- **含义**：通道分裂模式，决定一组物理 Lane 怎么被拆分成多个逻辑链路
- **常见配置**：
  - `x16`：16 条 Lane 全部给一条链路
  - `x8 + x8`：16 条 Lane 拆成两条 x8 链路
  - `x8 + x4 + x4`：16 条 Lane 拆成一条 x8 和两条 x4
  - `x4 + x4 + x4 + x4`：16 条 Lane 拆成四条 x4
- **配置位置**：硬件/BIOS/RC 控制器寄存器里的 "bifurcation mode" 配置
- **注意**：一个物理"金手指卡槽"本身只能插一块卡，bif mode 是在"主板/板卡内部"怎么分 Lane

### Lane 数 / Gen 速率配置

- **Lane 数**：
  - 主要由硬件连线 + 训练结果决定
  - 软件一般只读 `Link Status` 看结果，很少手动改（意味着要关某些 Lane、重训练链路）

- **Gen 速率**：
  - 硬件协商为主，软件可以"设上限/降档/重训练"
  - 通过 RC/端口配置寄存器设"目标/上限速率"（如只允许到 Gen3）
  - 通过写 `Link Control`/`Link Control 2` 的某些位修改目标 Gen、触发 retrain

---

## NoC 基础术语

### NoC (Network-on-Chip)

- **含义**：片上网络，用"路由器 + 链路 + 网络接口"的方式连接 SoC 里的各个 IP
- **特点**：
  - 比传统共享总线更灵活、可扩展
  - 支持多主设备、并行传输、QoS、路由算法
- **传递内容**：数据、地址、控制信号、中断/消息（通过普通写事务承载）

### NoC 与总线协议的关系

- **NoC 不是 AXI/APB 本身**：
  - NoC 是**网络拓扑结构**（路由器 + 链路 + 网络接口）
  - AXI/APB/CHI 是**协议/接口标准**（定义信号、时序、握手方式）
  - NoC **承载**这些协议，就像"高速公路"承载"不同车型"一样

- **为什么不同设备用不同协议**：
  - **AXI**：高速外设（PCIe RC、DMA、GPU、高速存储控制器）
    - 高带宽、流水线、多未完成事务
    - 适合大数据量传输
  - **APB**：低速外设（GPIO、UART、I2C、SPI、定时器、配置寄存器）
    - 简单、低功耗、低面积
    - 适合小数据量、寄存器配置
  - **CHI**：需要缓存一致性的设备（CPU 集群、加速器）
    - 支持 snoop、目录、一致性协议
    - 适合多核共享内存场景

- **NoC 如何支持多种协议**：
  - **Network Interface (NI)**：协议转换器
    - AXI → NoC Packet：把 AXI 事务打包成 NoC 内部包格式
    - NoC Packet → AXI：把 NoC 包还原成 AXI 事务
    - APB → NoC Packet / NoC Packet → APB：同样原理
  - **NoC 内部**：统一使用 NoC 自己的包格式和路由协议
  - **到达目标**：再通过 NI 转换回对应的协议（AXI/APB/CHI）

- **典型架构**：
  ```
  CPU (AXI Master) → NI (AXI→NoC) → NoC Router → NI (NoC→AXI) → DDR Controller
  GPIO (APB Slave)  → NI (APB→NoC) → NoC Router → NI (NoC→APB) → APB Bridge
  ```

### PCIe 与 AXI 的关系

- **为什么 PCIe 设备还要用 AXI？**
  - **PCIe** 是**外部总线协议**（用于连接外部设备，如 GPU、网卡、SSD）
  - **AXI** 是**片内总线协议**（用于 SoC 内部连接，如 CPU ↔ DDR、CPU ↔ 外设）
  - **Root Complex (RC)** 在 SoC 内部，必须连接到 SoC 内部总线（AXI/NoC）

- **协议转换的必要性**：
  ```
  CPU (AXI Master) → NoC/AXI → Root Complex (协议转换) → PCIe 链路 → 外部设备
  ```
  - **SoC 内部**：CPU 和其他模块用 AXI/NoC 通信
  - **SoC 外部**：PCIe 设备用 PCIe TLP 通信
  - **Root Complex**：负责 AXI ↔ PCIe TLP 的双向转换

- **Root Complex 的双重身份**：
  1. **在 SoC 内部**：作为 AXI Slave，接收 CPU 的 AXI 事务
  2. **在 PCIe 侧**：作为 PCIe Host，生成/接收 PCIe TLP

- **典型数据流**：
  - **CPU 访问 PCIe 设备**：
    ```
    CPU 写操作 (AXI) → NoC/AXI → RC (AXI→TLP) → PCIe 链路 → 设备
    ```
  - **PCIe 设备访问内存**：
    ```
    设备 DMA (TLP) → PCIe 链路 → RC (TLP→AXI) → NoC/AXI → DDR
    ```

- **AXI 和 PCIe 都是高速协议，但应用场景不同**：
  | 特性 | AXI | PCIe |
  |------|-----|------|
  | **应用场景** | SoC 内部（片内） | SoC 外部（片外） |
  | **连接距离** | 毫米级（芯片内部） | 厘米级（板级连接） |
  | **速率** | 通常 1-4 GHz（时钟频率） | 2.5-64 GT/s（传输速率） |
  | **带宽** | 取决于位宽（32/64/128/256 bit） | 取决于 Lane 数和 Gen（x1 Gen3 ≈ 1 GB/s） |
  | **复杂度** | 相对简单（点对点、同步） | 更复杂（串行、链路训练、错误恢复） |
  | **用途** | CPU ↔ DDR、CPU ↔ 外设、DMA | 连接外部扩展卡（GPU、网卡、SSD） |
  | **协议层** | 单层（事务层） | 三层（事务层、数据链路层、物理层） |

- **它们不是"平等"的关系，而是互补的关系**：
  - **AXI**：负责 SoC 内部的快速通信（CPU、内存、外设之间）
  - **PCIe**：负责 SoC 与外部设备的标准化连接
  - **Root Complex**：作为桥梁，让两个协议世界能够通信
  - 两者都是高速协议，但服务于不同的应用场景

- **总结**：
  - PCIe 是**外部协议**，用于连接外部设备
  - AXI 是**内部协议**，用于 SoC 内部通信
  - Root Complex 是**协议转换桥**，连接两个不同的协议世界
  - 两者都是高速协议，但应用场景不同，是互补关系而非竞争关系

### Master / Slave 概念

- **Master（主设备）**：
  - 能够**主动发起**总线事务的设备
  - 例如：CPU、DMA 控制器、GPU（发起 DMA 时）
  - 可以主动发起读/写请求

- **Slave（从设备）**：
  - **被动响应**总线事务的设备
  - 例如：DDR 控制器、外设寄存器、配置寄存器
  - 只能响应 Master 的请求，不能主动发起

- **在 AXI 协议中**：
  - **AXI Master**：发起 AXI 事务的一端（如 CPU）
  - **AXI Slave**：响应 AXI 事务的一端（如 DDR 控制器、外设）

### Master Bridge / Slave Bridge

- **Master Bridge（主桥）**：
  - **功能**：把某个协议/总线的 Master 接口连接到 NoC/更高级总线
  - **方向**：从外部 Master → NoC
  - **典型场景**：
    - 外部 AXI Master 设备连接到 NoC
    - 外部 PCIe 设备（作为 Master）连接到 NoC
    - 外部 DMA 控制器连接到 NoC
  - **作用**：让外部 Master 能够通过 NoC 访问系统资源（内存、外设）

- **Slave Bridge（从桥）**：
  - **功能**：把某个协议/总线的 Slave 接口连接到 NoC/更高级总线
  - **方向**：从 NoC → 外部 Slave
  - **典型场景**：
    - NoC 连接到外部 AXI Slave 设备
    - NoC 连接到外部寄存器组（APB Slave）
    - NoC 连接到外部低速外设
  - **作用**：让 NoC 上的 Master（如 CPU）能够访问外部 Slave 设备

- **典型架构示例**：
  ```
  CPU (AXI Master) → NoC → Slave Bridge → 外部 AXI Slave 设备
  外部 AXI Master → Master Bridge → NoC → DDR Controller (AXI Slave)
  ```

- **DVM Slave Bridge**：
  - **特殊类型**：专门处理 DVM（Distributed Virtual Memory）消息的 Slave Bridge
  - **功能**：接收来自 NoC 的 DVM 消息（如 TLB invalidate），并转发给连接的设备
  - **用途**：在虚拟内存管理系统中，同步 TLB 状态

- **CoreTile Bridge**：
  - **含义**：连接 CPU Core Tile（CPU 核心模块）到 NoC 的桥接器
  - **功能**：把 CPU 核心的总线接口（如 AXI Master）连接到 NoC
  - **作用**：让 CPU 核心能够通过 NoC 访问系统资源

### Bridge 连接外设数量

- **PCIe Bridge**：
  - **标准限制**：一个 PCIe Bridge **只能连接一个下游设备**
  - **下游设备类型**：可以是另一个 Bridge、Switch 或 Endpoint
  - **扩展方式**：通过 Switch 可以扩展连接多个设备
  - **典型拓扑**：
    ```
    RC → Bridge → Switch → 多个 Endpoint
    RC → Bridge → Endpoint (单个设备)
    ```

- **AXI/APB Bridge（SoC 内部）**：
  - **连接方式**：取决于具体实现
  - **APB Bridge**：
    - 通常可以连接**多个 APB Slave 设备**（通过地址解码）
    - 例如：一个 APB Bridge 可以连接 GPIO、UART、I2C、SPI 等多个外设
    - 通过地址范围区分不同的外设
  - **AXI Bridge**：
    - 通常连接**单个设备**或**设备组**
    - 可以通过地址解码连接多个逻辑设备（但物理上可能是一个 IP 模块）

- **NoC Bridge**：
  - **连接方式**：取决于具体实现
  - **Master Bridge**：通常连接**单个外部 Master 设备**
  - **Slave Bridge**：可能连接**单个或多个外部 Slave 设备**（通过地址解码）

- **总结**：
  | Bridge 类型 | 典型连接数量 | 扩展方式 |
  |------------|------------|---------|
  | **PCIe Bridge** | 1 个下游设备 | 通过 Switch 扩展 |
  | **APB Bridge** | 多个外设（通过地址解码） | 地址空间划分 |
  | **AXI Bridge** | 1 个设备或设备组 | 地址解码 |
  | **NoC Master Bridge** | 1 个外部 Master | - |
  | **NoC Slave Bridge** | 1 个或多个外部 Slave | 地址解码 |

- **关键因素**：
  - **地址空间**：一个 Bridge 能连接多少外设，取决于地址空间是否足够
  - **硬件实现**：具体芯片/IP 的设计决定
  - **协议限制**：PCIe 规范限制一个 Bridge 只能连接一个下游设备

### AXI / CHI

- **AXI (Advanced eXtensible Interface)**：
  - 点对点、无缓存一致性、相对简单
  - 适合 CPU ↔ 外设、DMA ↔ 内存这类"非一致性访问"
  - 同步协议，有 VALID/READY 握手，支持流水线、多未完成事务

- **APB (Advanced Peripheral Bus)**：
  - ARM 的低速外设总线协议
  - 简单、低功耗、适合寄存器配置
  - 同步协议，简单的读写握手
  - 通常通过 APB Bridge 连接到 NoC/AXI

- **CHI (Coherent Hub Interface)**：
  - ARM 的多核一致性互联协议
  - 支持 cache coherent（缓存一致性），可以在多个 CPU cluster、加速器之间维护共享内存的一致性
  - 有请求/响应/数据多个通道、Snoop、Directory 等机制
  - 通常跑在一个专门的 Coherent NoC 上

### 同步 vs 异步

- **同步（Synchronous）**：
  - 有"共同的节拍"（时钟），大家按同一时钟的边沿说话、听话
  - 时序简单、吞吐高，对时钟偏斜/线长/抖动有要求
  - 典型：AXI、AHB、APB、CHI、片内寄存器读写

- **异步（Asynchronous）**：
  - 没有共享节拍，靠"你准备好了我再收"这类握手信号来协调
  - 不要求共享时钟，可以跨时钟域/跨芯片/跨很长距离
  - 协议更复杂，需要考虑亚稳态、握手机制
  - 典型：异步 FIFO、跨时钟域桥、很多片外接口的上层握手

- **注意**：这里的"同步/异步"和"需不需要等回复"是两套概念：
  - 同步/异步协议：指有没有共同时钟
  - 需不需要等回复：指软件/协议层面的同步 vs 异步调用方式

---

## NoC 一致性协议

### snoop (窥探)

- **含义**：为了保持多核缓存一致性，一个硬件去"打招呼问别的缓存：你那儿有没有这条数据？要不要失效/写回？"的那条消息/动作
- **流程**：
  1. CPU0 想修改地址 X，但 CPU1 的 cache 里也有 X
  2. 互联（如 CHI）发出 snoop request 给其它 CPU/LLC
  3. 对方的 cache 控制器收到后检查自己的 tag，决定：
     - 告诉对方"我有/我没有这条 line"
     - 必要时做写回（writeback）或失效（invalidate）
- **作用**：让多核/多 master 在硬件层面自动保持共享数据的一致，不用全靠软件手动 flush/invalidate

### CHA (Cache Home Agent)

- **含义**：Cache/Coherent Home Agent，某一段物理地址在"一致性系统里的管家/户籍办"
- **职责**：
  - 记录：这条 line 现在在什么状态（M/E/S/I…）
  - 哪些 CPU/设备的 cache 里有副本
  - 谁是 owner，谁需要被 snoop、谁要写回
  - 收到对某个地址的读/写/一致性请求时，查状态/目录，发 snoop，协调写回/失效/状态转换
- **CHA SAM**：配置"哪个 CHA 负责哪一段物理地址"

### SNF (Snoop Node with Filter)

- **含义**：Snoop Node with Filter，负责和外部/某类 agent 做 snoop 交互，并带有 snoop 过滤/目录的节点
- **内部定义**：SNF 是 DDR 控制器接口，作为 DDR 控制器在 NoC/CHI 协议里的节点表示
- **职责**：
  - 站在某个 coherent agent（如外部 cache、某个 cluster）前面
  - 接收来自 NoC 的 snoop 请求
  - 维护一个 snoop filter/directory，知道哪些 line 在后端 agent 里有副本
  - 只对真正有副本的目标发 snoop，减少不必要的 snoop 广播
- **作用**：一致性系统里的 snoop 网关 + 过滤器，同时也是 DDR 控制器在 CHI 协议中的接口节点

### DVM (Distributed Virtual Memory)

- **含义**：Distributed Virtual Memory 消息，在一大堆有 TLB/缓存的核和 IOMMU 之间，用硬件消息来"同步虚拟内存状态"的机制
- **用途**：
  - 当某个地方做了 TLB 刷新、页表更新、内存属性改变等操作时
  - 需要把这些信息广播/分发给系统里其它有 TLB/cache 的 agent，避免有人继续用旧的翻译或旧属性
- **实现**：
  - 在 CHI/NoC 里，作为一种专门的消息类型
  - 携带"哪一段虚拟地址/ASID/VMID 发生了什么变化（invalidate、sync 等）"
  - 由 NoC 硬件分发到其它 CPU、SMMU、system cache 等，触发它们做对应的 DVM 操作（TLBI、同步等）

---

## NoC 地址映射

### SAM (System Address Map)

- **含义**：系统地址映射表，硬件里那张"地址路由/分配表"，告诉 NoC/互联：某个发起端发来的某一段地址，要送到哪一个目标节点（哪个 DDR 控制器/外设/CHA 等）
- **作用**：
  - 把"系统物理地址空间"划成若干段，每段说明：去哪个目标、是否 hash、走哪条路径
  - CPU/PCIe/DMA 只需要发"访问地址 X"，NoC 根据 SAM 自动帮你送到正确的地方

### RN-SAM (Request Node System Address Map)

- **含义**：Request Node 的系统地址映射表
- **作用**：
  - 对每个 RN（如 CPU0 集群、PCIe RC、某个 DMA），NoC 里都有一张 RN-SAM 表
  - 里面按地址范围配好：这段地址是 non-hash range 直连某个 SN，还是 hash range 需要按某种 hash 规则打散到多个目标
  - 当这个 RN 发生一次读/写，NoC 硬件用 RN-SAM 来决定：这次访问落在哪个 range，是否需要 hash/interleave，应该送到哪一个 SN

### CHA-SAM (Cache Home Agent System Address Map)

- **含义**：CHA 的系统地址映射表
- **作用**：
  - 告诉这个 CHA：我负责哪些地址范围
  - 配置"这个 CHA 负责的物理地址区间/region"
  - NoC 根据这些表决定：一次访问落在某个地址时，应当由哪一个 CHA/Home 节点来处理一致性和目录

### ISB-SAM

- **含义**：ISB（Internal/System/IO Subsystem Bus 等，厂商自定义）的系统地址映射表
- **作用**：
  - 从这个 ISB 节点看出去的系统地址映射表
  - 决定从 ISB 发起的访问，某一段地址送到哪个 LDID/target 节点

### hash range / non-hash range

- **non-hash range**：
  - 按连续地址区间直连到某个固定目标
  - 例如：`0x8000_0000 ~ 0x8FFF_FFFF` → 只走 DDR0
  - 特点：简单可预测，但可能把某个通道打满，另一个通道闲着

- **hash range**：
  - 对地址做哈希/取几位打散，把请求分配到多个目标之间
  - 例如：`0xA000_0000 ~ 0xAFFF_FFFF` = hash range，NoC 对这段地址取某几位（如 bit[7:6]）做 hash，决定去 DDR0 还是 DDR1
  - 好处：
    - 负载均衡：多通道 DDR/多 CHA 时，避免所有流量都砸在一个通道
    - 提高并行度：连续 cache line 分布在不同 Bank/通道，可以并行服务
    - 对软件透明：看起来就像"一个连续的大物理地址空间"

- **hash 映射公式示例**：
  - `hash_index[0] = addr[6] ^ addr[9] ^ addr[12] ^ ... ^ addr[40]`（异或多个位）
  - `hash_index[2:0]`：3 位索引，可以表示 0~7，对应 8 个 CHA/DDR 通道
  - 对每个具体地址，把它的各个 bit 代入 XOR 公式，算出一个固定的 `hash_index`，这笔访问就永远走到同一个 CHA/DDR 上

### LDID (Logical Device ID)

- **含义**：Logical Device ID，NoC 里给某个"目标设备/节点"的一个逻辑编号
- **用途**：
  - 在 RN-SAM 这类表项里，按地址范围决定：这段地址落在哪个 LDID 上
  - 然后 NoC 再把 LDID 映射到具体的物理目标节点/端口
  - 可以把它想成：NoC 内部用的"目标设备编号"，先用地址查出 LDID，再根据 LDID 找到真正的 SN/HN/内存控制器

### final targetid

- **含义**：CHI 请求的最终 targetid，这条 CHI 请求最后要到达、并在那儿"终结"的那个节点 ID
- **流程**：
  - 先由 RN 按 RN-SAM（地址映射表）把地址解析成某个目标节点的 ID
  - NoC 里的路由器/桥可以中途转发、重定向
  - final targetid 指的就是这一跳跳转完之后，真正负责处理这条事务的那个 Node（如某个 HN/CHA、某个 SN/DDR 控制器）

### RN -> CHA

- **含义**：从 Request Node 发起的访问，路由到对应的 Cache Home Agent 处理
- **流程**：
  1. CPU（RN）发起请求
  2. NoC 根据地址查表，路由到对应的 CHA
  3. CHA 处理一致性（snoop、状态管理、写回等）
  4. 数据从 CHA 或它后面的 SLC/DDR 返回给 CPU
- **hash 映射**：从 RN 发起的请求，不是按"连续地址区间 → 固定某个 CHA"走，而是对地址做 hash 运算，把请求分散到多个 CHA 上，实现负载均衡

---

## 系统架构相关

### SRAM / SF RAM

- **SRAM (Static Random Access Memory)**：
  - **含义**：静态随机存取存储器，不需要刷新就能保持数据的 RAM
  - **特点**：
    - **速度快**：访问延迟低，通常比 DRAM 快得多
    - **不需要刷新**：只要供电就能保持数据（与 DRAM 不同）
    - **面积大、成本高**：每个 bit 需要 6 个晶体管（DRAM 只需 1 个）
    - **功耗**：静态功耗（待机时也有功耗，但比 DRAM 刷新功耗低）
  - **在 SoC 中的应用**：
    - **Cache**：L1/L2/L3 缓存通常用 SRAM 实现
    - **片上内存**：SoC 内部的快速内存（如 Boot ROM、Scratchpad RAM）
    - **FIFO/Buffer**：高速数据缓冲
    - **寄存器文件**：CPU 内部的寄存器组
  - **与 DRAM 的区别**：
    | 特性 | SRAM | DRAM |
    |------|------|------|
    | **速度** | 快（纳秒级） | 慢（微秒级） |
    | **容量** | 小（KB-MB） | 大（GB-TB） |
    | **成本** | 高 | 低 |
    | **刷新** | 不需要 | 需要定期刷新 |
    | **用途** | Cache、片上内存 | 主内存（DDR） |

- **SF RAM（可能是厂商特定术语）**：
  - 可能是 **Special Function RAM** 或厂商自定义的 SRAM 变种
  - 也可能是 **Scratchpad RAM**（临时存储区）
  - **注意**：具体含义需要查看芯片 TRM 文档

### SLC (System Level Cache)

- **含义**：System Level Cache 或 Shared Last Level Cache，系统里共享的最后一级缓存（如 L3/LLC）
- **与 CHA 的关系**：
  - CHA 负责的地址范围，对应到某个 SLC 的地址区间
  - CHA 作为 home，它管理的地址段，实际落在某个 SLC 的地址空间里
  - CHA SAM 里配的地址范围，就是告诉系统"这段地址的一致性由这个 CHA 管，而且这段地址实际落在某个 SLC 的地址区间里"

### SCG region

- **含义**：System Cache Group region 或类似，把系统地址空间按段划分成若干 region，每个 region 归到某个 SCG（某组 system cache/snoop/slave 目标集合）
- **作用**：
  - NoC/RN-SAM 按地址先判断落在哪个 range
  - 是否需要 hash/interleave
  - 应该送到哪一个 SN（Slave Node）

### NPS

- **含义**：Node Power State / Node Power Switch / NoC Power Subsystem 等，和节点电源/时钟管理相关的模块（厂商自定义）
- **注意**：这个缩写在 ARM/NoC 里没有单一标准含义，不同芯片/文档里用法不一样，需要看具体 TRM 的缩写表

### PLDA 寄存器

- **含义**：PLDA 是一家做 PCIe/AXI/NoC 之类 IP Core 的公司，他们提供的 PCIe 控制器 IP 自己定义的一组控制/状态寄存器
- **位置**：片内 AXI/APB 寄存器空间里（给 CPU/固件配置用）
- **用途**：
  - 配置 RC/EP 模式、链路参数限制（最大 Gen/宽度）、BAR/地址映射策略
  - 中断/DMA/流控等内部功能
  - 调试、统计、错误状态等
- **注意**：这些寄存器不是 PCIe 规范里的配置空间寄存器（不是标准的 0x00–0xFF/0xFFF 那套），而是"控制这块 PLDA IP 的 SoC 内部寄存器"

### HPB

- **含义**：不是标准 PCIe 规范里的术语，常见含义：
  - UFS 里的 Host Performance Booster
  - 某些厂商 IP/SoC 手册中的自定义模块/寄存器名（High Performance Bus / Host Port Bridge / Host Prefetch Buffer 等）
- **注意**：必须按芯片 TRM/用户手册给的解释来理解，不能按"PCIe 标准"去找

### d2da

- **含义**：不是标准术语，常见可能是：
  - D2D Adapter / Die-to-Die Adapter：芯片内或芯片间互联的适配器模块
  - Device-to-Device Adapter / DMA-to-DDR Adapter 等
- **注意**：具体含义需要看芯片/NoC 的 TRM 或 RTL 注释

### LSD

- **含义**：不是标准 PCIe/NoC/ARM 术语，可能是厂商自定义缩写，常见可能含义：
  - **Link State Descriptor**：链路状态描述符（某些 NoC/互联 IP 中用于描述链路状态）
  - **Local System Domain**：本地系统域（某些多域系统架构中的概念）
  - **Low Speed Data**：低速数据接口
  - **厂商特定模块**：某些 SoC/NoC IP 厂商自定义的接口或模块名称
- **注意**：必须查看具体芯片的 TRM（Technical Reference Manual）或 NoC IP 文档来确定准确含义，不同厂商/芯片可能有不同定义

### DMU

- **含义**：不是标准 PCIe/NoC/ARM 术语，可能是厂商自定义缩写，常见可能含义：
  - **DMA Management Unit**：DMA 管理单元（某些 SoC 中用于管理 DMA 传输的模块）
  - **Data Management Unit**：数据管理单元（某些 NoC/互联 IP 中用于数据路径管理的模块）
  - **Device Management Unit**：设备管理单元（某些系统架构中用于设备配置和管理的模块）
  - **Debug Management Unit**：调试管理单元（某些芯片中用于调试接口的模块）
  - **厂商特定模块**：某些 SoC/NoC IP 厂商自定义的接口或模块名称
- **注意**：必须查看具体芯片的 TRM（Technical Reference Manual）或 NoC IP 文档来确定准确含义，不同厂商/芯片可能有不同定义

### 密钥吊销 (Key Revocation)

- **含义**：在密钥的有效期内，提前终止其有效性，使其不再被信任或使用的过程
- **应用场景**：
  - **安全启动（Secure Boot）**：当某个签名密钥泄露或被破解时，需要吊销该密钥
  - **设备认证**：当某个设备的证书/密钥被泄露时，需要吊销该证书
  - **固件签名**：当某个固件签名密钥不再安全时，需要吊销该密钥
  - **PKI（公钥基础设施）**：证书颁发机构（CA）可以吊销已签发的证书

- **吊销原因**：
  - **密钥泄露**：私钥被泄露或被盗
  - **密钥被破解**：密钥强度不足，已被破解
  - **身份变更**：证书持有者身份发生变更
  - **错误签发**：证书被错误签发或包含错误信息
  - **安全策略变更**：系统安全策略要求吊销某些密钥

- **实现机制**：
  - **CRL (Certificate Revocation List)**：证书吊销列表，包含所有被吊销的证书
  - **OCSP (Online Certificate Status Protocol)**：在线证书状态协议，实时查询证书是否被吊销
  - **硬件支持**：某些 SoC 提供硬件密钥吊销列表（如 eFuse、OTP）

- **在嵌入式系统中的应用**：
  - **安全启动链**：Boot ROM → Bootloader → Kernel，每个阶段验证签名，检查密钥是否被吊销
  - **设备认证**：设备连接到系统时，验证设备证书是否被吊销
  - **固件更新**：更新固件时，验证新固件签名密钥是否被吊销

- **关键点**：
  - 密钥吊销是**不可逆**的，一旦吊销就无法恢复
  - 需要及时更新吊销列表，确保系统能够识别被吊销的密钥
  - 硬件支持可以提高吊销检查的效率和安全性

### NUMA (Non-Uniform Memory Access)

- **含义**：非统一内存访问，多 CPU 系统里，不同 CPU 访问不同内存区域的延迟/带宽不一样
- **Linux 支持**：
  - 支持多 NUMA node（如 2 个 NUMA，每个 8 核）
  - 前提：硬件/固件正确暴露 NUMA 拓扑（ACPI/DT 里有 SRAT/SLIT 或等价描述）
  - 内核编译启用 `CONFIG_NUMA`（主流发行版内核默认打开）

- **设备树配置**：
  - 在 `cpus` 下面的每个 `cpu@...` 节点里加 `numa-node-id = <0>` 或 `<1>`
  - 在 `memory@...` 节点里加 `numa-node-id = <0/1>`
  - 距离（哪两个 node 近/远）在 DT 里其实没地方标准化描述，这点是 ACPI SRAT/SLIT 强项

---

## 总结

### PCIe 相关

- **RC/EP/DM**：控制器的三种工作模式
- **BDF**：Bus/Device/Function，设备的唯一标识
- **BAR**：基址寄存器，映射设备 MMIO 到系统物理地址
- **配置空间**：标准 256B + 扩展 4KB，包含设备信息、Capability 链表
- **枚举**：固件/OS 发现设备、分配资源、建立拓扑
- **链路训练**：硬件自动协商 Gen/Width，结果在 Link Status
- **Gen1/2/3/4**：PCIe 速率等级，从 2.5 GT/s 到 16.0 GT/s

### NoC 相关

- **SAM**：系统地址映射表，决定地址路由到哪个目标
- **RN-SAM/CHA-SAM/ISB-SAM**：不同节点的地址映射表
- **hash range**：用地址 hash 把请求分散到多个目标，实现负载均衡
- **CHA**：Cache Home Agent，负责某段地址的一致性管理
- **SNF**：Snoop Node with Filter，snoop 网关 + 过滤器
- **snoop**：窥探机制，保持多核缓存一致性
- **DVM**：分布式虚拟内存消息，同步虚拟内存状态

### 关键区别

- **同步/异步协议**：有没有共同时钟（AXI/CHI 都是同步）
- **需不需要等回复**：软件/协议层面的同步 vs 异步调用方式
- **hash vs non-hash**：是否用地址 hash 分散到多个目标
- **训练 vs 配置**：硬件自动协商物理参数 vs 软件配置功能层参数

---

## 参考文档

- [PCIe_CONFIG_SPACE_LAYOUT.md](PCIe_CONFIG_SPACE_LAYOUT.md) - PCIe 配置空间完整布局
- [PCIe_LINK_TRAINING.md](PCIe_LINK_TRAINING.md) - PCIe 链路训练详解
- [PCIe_BAR_DETAILED.md](PCIe_BAR_DETAILED.md) - PCIe BAR 详解
- [NOC_ARCHITECTURE_DIAGRAM.md](NOC_ARCHITECTURE_DIAGRAM.md) - 基于 NoC 的嵌入式系统架构图
- [PCIe_OVERVIEW.md](PCIe_OVERVIEW.md) - PCIe/SerDes 总体入门概览

