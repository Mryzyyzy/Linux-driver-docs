## PCIe 高速接口与相关名词总览（入门向）

这篇文档把你前面问到的所有和 PCIe/SerDes 有关的概念系统串起来，适合作为入门速查表。

- 覆盖内容：
  - PCIe 基本架构与分层
  - Lane / 链路宽度 / 金手指
  - 系统同步 vs 源同步
  - SerDes / CDR / PCS / PMA / PIPE / MAC/控制器
  - 链路训练（Link Training）与 LTSSM
  - 64G PHY、大致带宽含义

---

## 1. PCIe 总体架构与分层

### 1.1 拓扑与角色

- **拓扑**：点对点串行，总体是树形结构：
  - **Root Complex（RC）**：挂在 CPU/内存一侧，类似“主桥”。
  - **Switch/Bridge**：把链路“分叉”出去。
  - **Endpoint（EP）**：终端设备，比如网卡、SSD、GPU、自研卡等。

### 1.2 三层协议模型

PCIe 标准协议分为三层（从上到下）：

- **事务层（Transaction Layer）**
  - 定义“要干什么”：
    - Memory Read/Write（内存/寄存器访问）
    - Configuration Read/Write（配置空间访问）
    - Message（MSI/MSI-X、错误报告等）
  - 使用 **TLP（Transaction Layer Packet）** 作为基本单元。

- **数据链路层（Data Link Layer）**
  - 保证“传得对”：
    - 给每个 TLP 加上 **序列号 + LCRC**。
    - 收到后通过 **ACK/NAK（DLLP 包）** 确认或请求重传。
    - 实现 **Flow Control（credit 流控）**，防止对端 buffer 爆掉。
  - 使用 **DLLP（Data Link Layer Packet）**。

- **物理层（Physical Layer）**
  - 保证“信号能在板子上跑起来”：
    - 编码（8b/10b、128b/130b）
    - Scrambler/Descrambler
    - SerDes 并串/串并
    - CDR（从数据恢复时钟）
    - 均衡、预加重、驱动电流、电气规范

实现上，物理层内部常再细分为：

- **PCS（Physical Coding Sublayer）**：物理编码子层（偏数字逻辑）
- **PMA（Physical Medium Attachment）**：物理介质附属层（SerDes + 模拟前端）

---

## 2. Lane、链路宽度与金手指

### 2.1 Lane 是什么？

- **Lane**：一条 PCIe 物理通道，由两对差分线组成：
  - 一对 TX+ / TX-（发送）
  - 一对 RX+ / RX-（接收）
- 每条 lane 上跑的是 **全双工高速串行比特流**。

### 2.2 链路宽度（Link Width）

- 链路宽度 = 使用了多少条 lane：
  - `x1`：1 lane
  - `x2`：2 lane
  - `x4`：4 lane
  - `x8`：8 lane
  - `x16`：16 lane
- **有效带宽 ≈ 单 lane 带宽 × 链路宽度**（逻辑上并行，物理上多条串行线一起跑）。
- 链路训练完成后，可以通过：

```bash
lspci -vvv | grep -A 5 "LnkSta"
# 例如: LnkSta: Speed 8GT/s, Width x4
```

中看到当前协商出的速度和宽度。

### 2.3 为什么 x4 卡插在 x16 槽只用 4 条 lane？

- 主板 **x16 插槽** 在 PCB 上布了 16 条 lane 的触点。
- 一块 **x4 卡** 的金手指上只接了 Lane0~Lane3，对应的是插槽最前面的 4 条触点。
- 插上去后：
  - 槽的 Lane0~3 → 接到卡的 Lane0~3，真正工作。
  - 槽的 Lane4~15 → 卡上根本没有对应金手指/走线 ⇒ 闲置。
- 链路训练时协商结果一般为 `Width x4`。

### 2.4 金手指（Edge Connector / Gold Finger）

- **金手指**：板卡边缘那一排镀金的触点总称，用来和插槽里的金属弹片接触。
- 金手指上分布的信号类型（抽象）：
  - **P（Power）**：3.3V、12V 等电源触点。
  - **G（Ground）**：大量 GND，夹在高速差分对之间，做参考平面、减小串扰。
  - **H（High-speed）**：每个 lane 的 TX+/TX-、RX+/RX- 差分信号。
  - **C（Control/Sideband）**：PERST#、WAKE#、CLKREQ#、REFCLK 差分对、SMBus、JTAG 等。
- 所以：**金手指覆盖的是“电源+地+高速信号+控制信号”的整个接触区域**，远不止 P 和 G。

---

## 3. 系统同步 vs 源同步（System vs Source Synchronous）

这部分对应 `SYNC_INTERFACE_COMPARISON.md` / `SYNC_QUICK_REFERENCE.md`，在 PCIe 物理层理解很关键。

### 3.1 系统同步（System Synchronous）

- 特点：
  - 发送端和接收端使用 **同一个系统时钟**（通过时钟树分配）。
  - 数据线和时钟线分开走，路径长短可能不一样 → **时钟偏斜（skew） 较大**。
- 优点：
  - 时钟架构简单，适合低速、多设备共享时钟。
- 缺点：
  - 难以控制时钟与数据的相对相位，高速时时序裕量变小。
  - 频率一般 <100MHz 级别比较现实。

### 3.2 源同步（Source Synchronous）

- 特点：
  - **发送端同时发出时钟和数据**（时钟随数据一起走，或时钟嵌入数据）。
  - 时钟与数据的路径几乎相同 → **时钟-数据偏斜小，可控**。
- 优点：
  - 支持更高频率，适合高速接口（DDR、MIPI、SerDes 等）。
  - 时序窗口大，抗工艺/温度变化能力更好。
- 缺点：
  - PCB 走线需严格匹配、布线要求高。

### 3.3 PCIe 属于哪种？

- PCIe 实际上是 **改进的源同步**：
  - 高速数据线上 **不单独走时钟线**，使用 8b/10b 或 128b/130b + Scrambler 编码，让位流里有足够的跳变。
  - 接收端用 **CDR（Clock Data Recovery）** 从数据流中恢复时钟。
  - 某些平台上会有一对 **参考时钟 REFCLK**，作为 PHY 内部 PLL/CDR 的频率基准（不是逐比特的采样时钟）。

---

## 4. SerDes、CDR、PCS、PMA

### 4.1 SerDes（Serializer / Deserializer）

- SerDes 的职责：在 **芯片内部并行总线** 和 **板外串行差分线** 之间做转换：

```text
TX：并行 → 串行
  上层提供 N bit 并行字 (例如 16/32/130 bit)
    ↓
  编码/扰码
    ↓
  Serializer 一 bit 一 bit 推到 TX 差分线上

RX：串行 → 并行
  线上的 1 bit 串行比特流
    ↓ CDR 恢复时钟并判决 0/1
  Deserializer 每 N bit 重新拼成并行字
    ↓
  解扰/解码，交给上层逻辑
```

- 外部你看到的 **lane** 上永远是串行；  
  “并行/串行转换”说的是 **芯片内部总线 ↔ 外部线** 的关系。

### 4.2 CDR（Clock Data Recovery）

详见 `CDR_MECHANISM.md` / `CDR_VISUAL_GUIDE.md`，核心作用：

- 从 **没有显式时钟线的数据边沿** 中恢复时钟：
  - 检测数据流中的上升沿/下降沿。
  - 用相位检测 + PLL/DLL 让内部振荡器频率与数据速率一致。
  - 调整相位，使采样点落在眼图中心（最稳的位置）。
- CDR 关键环节：
  - 边沿检测 → 相位误差估计 → 环路滤波 → 控制 VCO/DLL → 相位插值 → 优化采样点。

### 4.3 PCS（Physical Coding Sublayer）

PCS 是物理层的“编码子层”（主要是数字逻辑）：

- 编码 / 解码：
  - PCIe Gen1/2：8b/10b
  - PCIe Gen3+：128b/130b（配合 Scrambler）
- Scrambler / Descrambler（扰码/解扰）
- Block / Word 对齐（找到 10b/66b/130b 边界）
- 多 lane 场景下的：
  - Lane striping（按顺序把数据分配到 Lane0..N）
  - Lane deskew / 重组（接收端按顺序拼回）
- 插入 / 识别特殊控制字符、训练序列（TS1/TS2）、Ordered Set 等。

可以理解为：**PCS 把协议相关的“位级编码逻辑”做好，交给 PMA/SerDes 去发“纯比特流”。**

### 4.4 PMA（Physical Medium Attachment）

PMA 是 PHY 的“模拟/混合信号前端”：

- SerDes 并串/串并（真正做 1 bit 串行的那层逻辑）
- CDR（相位/频率恢复）
- 均衡（EQ）、预加重、幅度控制
- 驱动电路、接收放大、判决器
- 直接连接到物理介质（PCB 差分线 / 连接器 / 光模块）

---

## 5. MAC / 控制器 / PIPE 接口

### 5.1 MAC / 控制器层

在 PCIe 里，**控制器/MAC**（有时叫 Root Port/Endpoint Controller）通常包含：

- 事务层：
  - 生成/解析 TLP
  - 管理内存读写、配置读写、消息等。
- 数据链路层：
  - 生成/解析 DLLP（ACK/NAK、Flow Control）
  - 维护序列号、LCRC、重传机制。
- 物理层上半部分：
  - LTSSM（Link Training and Status State Machine）
  - Ordered Set、训练控制、速率/宽度协商等。

它向上接 **CPU/内存/用户逻辑（寄存器接口、DMA 引擎等）**，向下通过 PIPE 接 PHY。

### 5.2 PIPE（PHY Interface for PCI Express）

- **PIPE** 是 PCI-SIG 定义的 **控制器 ↔ PHY 之间的标准接口规范**。
- 解决的问题：让不同厂商的控制器 IP 和 PHY IP 能像“插 USB 线”一样对接，而不是每家自定义一套。
- 典型 PIPE 信号（抽象）：
  - `TXDATA[n:0] / RXDATA[n:0]`：并行数据总线。
  - `TXDATAK`：标记控制符号。
  - `RXVALID / PHYSTATUS / RXSTATUS`：状态与错误指示。
  - `POWERDOWN / RATE / ELECIDLE`：速率/功耗控制。
  - `PCLK`：PIPE 并行域时钟。

整体分层示意（你之前要的 MAC / PCS / PMA / PIPE）：

```text
 上层：CPU / 内存 / 驱动
        │
        ▼
 PCIe 控制器 / MAC
   - 事务层 + 数据链路层 + 物理层上半部分
        │ 并行数据 + 控制
        ▼
       PIPE 接口  ← 规范定义的统一接口
        │
        ▼
      PCS (编码子层)
        │ 宽并行位流
        ▼
      PMA / SerDes (模拟前端)
        │ 串行比特流
        ▼
    PCIe Lane 差分线
```

---

## 6. 链路训练（Link Training）与 LTSSM

详见 `PCIe_LINK_TRAINING.md` / `LINK_TRAINING_QUICK_REF.md`，这里给简要总览。

### 6.1 什么是链路训练？

- 设备上电后，为了让两端的 SerDes/CDR/协议参数“对上号”，需要一个自动协商过程：

```text
上电/复位
  ↓
Detect（检测链路存在）
  ↓
Polling（发送 TS1，CDR 锁定）
  ↓
Configuration（协商宽度/速度，对齐 lane）
  ↓
L0（正常工作）
```

- 这个状态机就叫 **LTSSM（Link Training and Status State Machine）**。

### 6.2 训练序列 TS1 / TS2

- 特殊格式的 Ordered Set，由 PCS 生成，通过 PHY 发送：
  - **TS1**：初始训练，帮助 CDR 锁定、传递链路参数。
  - **TS2**：参数确认，准备进入 L0。
- 作用：
  - 提供足够的跳变 → CDR 可以锁定频率/相位。
  - 携带期望的 **链路宽度/速度、Lane 编号、N_FTS 等参数**。
  - 多 lane 场景下做 **deskew/对齐**。

### 6.3 链路宽度/速度协商与降级

- 宽度例子：

```text
插槽/控制器支持: x16
板卡支持:       x4
→ 最终协商:     x4
```

- 速度例子：

```text
一端支持: Gen1/2/3/4
另一端:   Gen1/2/3
→ 最终协商: Gen3
```

- 如果高宽度/高速度训练失败，则自动尝试降级（例如从 x8 Gen4 降到 x4 Gen3）。

---

## 7. “64G PHY”等速率名词

### 7.1 “64G PHY” 一般指什么？

- 在 SerDes/PHY IP 手册里：
  - **“64G PHY”** 通常指 **单 lane 最高支持约 64Gbps 级别的物理速率**。
  - 常用于：
    - PCIe Gen6（64 GT/s，PAM4）
    - 400G/800G Ethernet 的单 lane 速率。

### 7.2 GT/s vs Gbps

- **GT/s（GigaTransfers/s）**：每秒传输次数（UI 速率）。
- **Gbps**：每秒比特数。
- NRZ（2 电平）时：
  - 1 transfer = 1 bit ⇒ GT/s ≈ Gbps。
- PAM4（4 电平）时：
  - 1 transfer = 2 bit ⇒ 64 GT/s ≈ 128 Gbps 物理比特率。

### 7.3 PHY vs 协议

- **PHY** 描述的是“这套 SerDes 能跑到什么物理速率”，不限定具体协议。
- 在这之上可以跑：
  - PCIe / CXL
  - 以太网（如 100G/200G/400G）
  - 其他自定义高速协议

---

## 8. 你之前问过的关键问题小结

1. **PCIe 链路宽度是什么意思？**
   - 表示链路用到了多少条 lane：x1/x2/x4/x8/x16。
   - 带宽 ≈ 单 lane 带宽 × 宽度。

2. **x4 设备插在 x16 槽能随便选 4 条 lane 吗？**
   - 不行。哪几条 lane 被用是由主板 PCB + 卡的金手指焊死决定的。
   - 一般是槽的 Lane0~3 ↔ 卡的 Lane0~3。

3. **金手指到底是什么？**
   - PCB 边缘那条镀金接触区域，包含电源/地/高速信号/控制信号的所有 pad。

4. **为什么看不到高速数据的“时钟线”？**
   - 高速数据线不单独走时钟，时钟信息嵌在比特流里。
   - 接收端用 CDR 从数据边沿恢复时钟。
   - 参考时钟（REFCLK）是一对低速差分，给 PHY 里 PLL/CDR 当基准。

5. **SerDes 说“并行转串行”，但 PCIe 不是一直串行吗？**
   - 线上的 lane 是串行；芯片内部处理用的是 N bit 宽的并行总线。
   - SerDes 做的就是 **芯片内部并行总线 ⇄ 外部串行线** 的转换。

6. **MAC/控制器 是哪一层？**
   - 实现上通常包含：
     - 事务层 + 数据链路层
     - 物理层的上半部分（LTSSM、训练控制等）。

7. **PCS 是什么？**
   - 物理编码子层：负责编码/扰码/对齐/lane 重组等位级逻辑。

8. **PMA 是什么？**
   - SerDes + 模拟前端：CDR、并串/串并、EQ、电气驱动。

9. **PIPE 是什么？**
   - 控制器/MAC 和 PHY 之间的标准接口（并行数据 + 控制信号）。

10. **PCIe 链路训练做了什么？**
    - Detect → Polling（TS1/CDR 锁定）→ Configuration（协商宽度/速度、对齐 lane）→ L0。
    - 失败时会尝试降级或重试。

---

这篇总览文档可以和项目里的其他专门文档配合阅读：

- 物理同步机制：`SYNC_INTERFACE_COMPARISON.md` / `SYNC_QUICK_REFERENCE.md`
- CDR 工作细节：`CDR_MECHANISM.md` / `CDR_VISUAL_GUIDE.md`
- 链路训练过程：`PCIe_LINK_TRAINING.md` / `LINK_TRAINING_QUICK_REF.md`
- 命令/工具实现：`COMMAND_IMPLEMENTATION.md`

如果你后面想继续深入到**TLP/DLLP 的具体格式**或者**PCIe 配置空间/BAR/MSI 的具体用法**，可以在此基础上再扩一篇“协议报文级”文档。


