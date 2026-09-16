# ARM 系统寄存器参考（TF-A / Secure World 相关）

本文档汇总与异常等级切换、安全世界、MMU、中断路由等相关的关键系统寄存器，便于查阅。

---

## 1. SPSR_ELx（Saved Program Status Register）

- **作用**：保存“发生异常/进入当前 EL 之前”的 PSTATE；**ERET 时用 SPSR 恢复 PSTATE**，从而决定返回后的 EL、栈指针、中断屏蔽等。
- **可访问**：每个 EL 有 SPSR_EL1/EL2/EL3，仅在对应 EL 或更高 EL 可写。

### AArch64 主要字段（ERET 时恢复）

| 位域 | 名称 | 含义 |
|------|------|------|
| **M[4:0]** | Mode | **返回后的异常等级 + 栈指针选择**（EL0/1/2/3，以及用 SP_EL0 还是 SP_ELx） |
| **D[9]** | Debug mask | 1 = 屏蔽 Debug 异常 |
| **A[8]** | SError mask | 1 = 屏蔽 SError/外部 abort |
| **I[7]** | IRQ mask | 1 = 屏蔽 IRQ |
| **F[6]** | FIQ mask | 1 = 屏蔽 FIQ |
| **nRW** | 执行状态 | 0 = AArch64，1 = AArch32 |
| **NZCV[31:28]** | 条件标志 | N/Z/C/V |
| **IL[20]** | Illegal Execution | 通常 0 |
| **SSBS[12]** | Speculative Store Bypass Safe | 与推测执行安全相关 |

**说明**：M[4:0] 同时编码“返回后的 EL”和“该 EL 下使用哪根栈指针”，并非仅选栈指针。

---

## 2. ELR_ELx（Exception Link Register）

- **作用**：保存发生异常/进入当前 EL 时的**返回地址**。**ERET 时 PC = ELR_ELx**。
- **与 SPSR 配合**：ELR 提供返回后的 PC，SPSR 提供返回后的 PSTATE（含 EL、SP、DAIF）；二者一起完成等级切换。

---

## 3. SCR_EL3（Secure Configuration Register, EL3）

- **作用**：配置 **EL3 及“下级”（EL2/1/0）的安全与路由**。仅 EL3 可读写。
- **“下级”含义**：指从 EL3 ERET 后将要运行的 EL2/EL1/EL0，即下一级要执行的是 Secure 还是 Non-secure 世界。

### 主要位

| 位 | 名称 | 含义 |
|----|------|------|
| **NS** | Non-secure | 1 = 下级为 Non-secure 世界；0 = 下级为 Secure 世界 |
| **IRQ** | 路由 | 1 = IRQ 路由到 EL3 |
| **FIQ** | 路由 | 1 = FIQ 路由到 EL3 |
| **EA** | External Abort | 1 = 外部 abort 路由到 EL3 |
| **SMD** | SMC disable | 1 = 禁止 SMC 进入 EL3（通常 0） |
| **HCE** | HVC enable | 1 = 允许 HVC 进入 EL3 |
| **RW** | Execution state | 1 = 下级 AArch64；0 = AArch32 |
| **ST** | Secure timer | 安全物理计时器是否 trap 到 EL3 |
| **TWI/TWE** | WFE/WFI trap | 是否将 WFE/WFI trap 到 EL3 |
| **SIF** | Secure instruction fetch | 1 = Non-secure 不能取 Secure 执行的指令 |

---

## 4. HCR_EL2（Hypervisor Configuration Register）

- **作用**：配置 **EL2 对 EL1/EL0 的虚拟化与 trap 行为**。EL2 或 EL3 可写。

### 主要位

| 位 | 名称 | 含义 |
|----|------|------|
| **VM** | VMs enable | 1 = 使能 Stage 2 转换（虚拟化） |
| **RW** | Execution state | EL1 为 AArch64(1) 或 AArch32(0) |
| **AMO/IMO/FMO** | 路由 | Abort、IRQ、FIQ 路由到 EL2 或 EL1 |
| **TSC** | Trap SMC | 1 = SMC 从 EL1 trap 到 EL2 |
| **TWE/TWI** | WFE/WFI trap | 1 = trap 到 EL2 |
| **TGE** | Trap General Exceptions | 1 = 多种异常 trap 到 EL2 |
| **E2H** | EL2 Host | 1 = VHE（EL2 作为主机） |
| **HCD** | HVC disable | 1 = 禁止 HVC |
| **SWIO** | Set/Way override | 与 cache 维护指令行为相关 |

---

## 5. SCTLR_ELx（System Control Register）

- **作用**：控制**该 EL** 的 MMU、Cache、对齐检查等。每个 EL 有 SCTLR_EL0/EL1/EL2/EL3。

### 主要位

| 位 | 名称 | 含义 |
|----|------|------|
| **M** | MMU | 1 = 使能 MMU（该 EL 的地址转换） |
| **C** | Cache | 1 = 使能 D-cache |
| **I** | I-cache | 1 = 使能 I-cache |
| **A** | Alignment check | 1 = 未对齐访问产生 fault |
| **SA** | Stack alignment | 1 = 强制 SP 对齐检查 |
| **EE** | Exception Endianness | 异常取指/访问的大小端 |
| **WXN** | Write XN | 写后不可执行（安全加固） |
| **nTWE/nTWI** | 不 trap WFE/WFI | 与 trap 行为相关 |

---

## 6. TCR_ELx（Translation Control Register）

- **作用**：控制**该 EL** 的**页表转换**（TTBR 对应的 Stage 1 转换）：地址空间大小、粒度、SH/ORGN/IRGN 等。
- **常见**：TCR_EL1（EL1 的 Stage 1）、TCR_EL2、TCR_EL3；虚拟化时还有 VTCR_EL2（Stage 2）。

### 常用字段（以 TCR_EL1 为例）

| 字段 | 含义 |
|------|------|
| **T0SZ/T1SZ** | TTBR0_EL1 / TTBR1_EL1 对应地址空间大小（2^(64-TxSZ) 字节） |
| **TG0/TG1** | 页表粒度（4K/16K/64K） |
| **SH0/SH1** | Shareability |
| **ORGN0/ORGN1, IRGN0/IRGN1** | Outer/Inner cacheability |
| **IPS** | 中间物理地址位宽（TCR_EL2/EL3） |

---

## 7. TTBR0_ELx / TTBR1_ELx（Translation Table Base Register）

- **作用**：指向**页表基地址**（L0 表）。TTBR0 通常用于用户空间，TTBR1 用于内核/高地址。
- **EL1**：TTBR0_EL1、TTBR1_EL1（Stage 1）。  
- **EL2**：TTBR0_EL2；若用 Stage 2，还有 VTTBR_EL2。  
- **EL3**：TTBR0_EL3（仅 Stage 1）。

---

## 8. VBAR_ELx（Vector Base Address Register）

- **作用**：**异常向量表基地址**。发生异常时，CPU 根据 VBAR_ELx + 偏移取向量入口。
- **各 EL**：VBAR_EL1、VBAR_EL2、VBAR_EL3；每个 EL 有各自向量表。

---

## 9. DAIF（PSTATE 中的中断/异常屏蔽）

- **D**：Debug 异常屏蔽  
- **A**：SError 屏蔽  
- **I**：IRQ 屏蔽  
- **F**：FIQ 屏蔽  

在 SPSR 中保存/恢复；也可用 MSR/MRS 操作 DAIF 的单独位（如 DAIFSet、DAIFClr）。

---

## 10. ESR_ELx（Exception Syndrome Register）

- **作用**：记录**发生异常的原因与部分信息**（如 EC、IL、ISS），便于异常处理程序判断类型并处理。
- **常见**：ESR_EL1、ESR_EL2、ESR_EL3。

---

## 11. FAR_ELx（Fault Address Register）

- **作用**：记录**导致 fault 的**（数据/指令）**访问地址**。与 ESR 配合用于调试与处理 page fault、alignment fault 等。

---

## 12. MAIR_ELx（Memory Attribute Indirection Register）

- **作用**：定义**内存属性索引**（如 Device、Normal WB/NC/WT）与页表描述符中 AttrIndx 的对应关系，用于 Stage 1 转换的 cacheability/shareability 等。

---

## 13. 与 FF-A / 世界切换相关的使用小结

| 场景 | 常用寄存器 |
|------|------------|
| EL3 → 下级（NS 或 S） | SCR_EL3（NS、RW）、ELR_EL3、SPSR_EL3 |
| EL2 → EL1（如 SPMC→SP） | HCR_EL2、ELR_EL2、SPSR_EL2、SCTLR_EL1、TTBR0_EL1 等 |
| 各 EL 使能 MMU/页表 | SCTLR_ELx（M）、TCR_ELx、TTBR0_ELx（及 TTBR1） |
| 异常入口 | VBAR_ELx |
| 异常原因/地址 | ESR_ELx、FAR_ELx |

---

## 14. 异常等级切换的典型寄存器操作

本节汇总常见“从 EL3/EL2 切换到下一级 EL”的寄存器设置，便于查阅（以 AArch64 为例）。

### 14.1 EL3 → S-EL1（例如 TF-A 进入 OP-TEE / Secure EL1 内核）

**目标**：从 EL3 通过 ERET 进入 S-EL1。

- **SCR_EL3：选择安全状态与执行状态**
  - `SCR_EL3.NS = 0`：下级为 Secure 世界（S-ELx）。
  - `SCR_EL3.RW = 1`：下级使用 AArch64（若要 AArch32 则为 0）。
  - 其他如 `IRQ/FIQ/EA/ST/TWI/TWE` 按平台/需求决定是否路由/Trap 到 EL3。

- **SPSR_EL3：选择返回后的 EL/栈指针及中断屏蔽**
  - `SPSR_EL3.M[4:0] = 0b101`（EL1h）：返回到 EL1，使用 SP_EL1。
    - 或 `0b100`（EL1t）：返回到 EL1，使用 SP_EL0。
  - `SPSR_EL3.nRW = 0`：返回后为 AArch64（=1 为 AArch32）。
  - `SPSR_EL3.D/A/I/F`：根据需要屏蔽/允许 Debug、SError、IRQ、FIQ（如进入 Secure OS 早期初始化可先屏蔽 IRQ/FIQ）。

- **ELR_EL3：设置返回 PC**
  - `ELR_EL3 =` S-EL1 入口地址（例如 Secure EL1 启动入口）。

- **S-EL1 上下文恢复（在 ERET 前）**
  - 从 Secure EL1 的 `cpu_context` 恢复：  
    - `SCTLR_EL1`（开启/关闭 MMU、cache、对齐检查等）；  
    - `TCR_EL1`、`TTBR0_EL1`（及可选 `TTBR1_EL1`）用于配置页表；  
    - `VBAR_EL1`（异常向量表）；  
    - `SP_EL1` 或 `SP_EL0`（要和 M[4:0] 的 EL1h/EL1t 一致）；  
    - 通用寄存器 x0–x30（按约定传递入口参数）。

完成上述设置后执行 `ERET`，CPU 将以 S-EL1 的状态（由 SCR_EL3.NS=0 决定）和 SPSR_EL3 描述的 PSTATE 运行。

### 14.2 EL3 → NS-EL1（例如 TF-A 返回 Normal World 内核）

与 14.1 类似，但安全状态不同：

- `SCR_EL3.NS = 1`：下级为 Non-secure 世界（NS-ELx）。
- `SCR_EL3.RW`：按下级是否为 AArch64/AArch32 设置。
- `SPSR_EL3.M[4:0]`：同样选择 EL1h / EL1t（返回到 NS-EL1）。
- `ELR_EL3`：设置为 Linux/BL33 的入口或恢复点。
- 在 ERET 前恢复 NS-EL1 的 `SCTLR_EL1/TCR_EL1/TTBR0_EL1/VBAR_EL1/SP_ELx` 等上下文。

### 14.3 EL2 → EL1（例如 S-EL2 SPMC 进入 S-EL1 SP）

在 S-EL2 作为 SPMC 时，从 EL2 切换到 EL1 的关键寄存器：

- **HCR_EL2：控制 trap 与执行状态**
  - `HCR_EL2.RW`：EL1 的执行状态（0 = AArch32，1 = AArch64）。
  - `HCR_EL2.TSC/TWI/TWE/AMO/IMO/FMO`：控制 SMC、WFI/WFE、Abort/IRQ/FIQ 是否 trap 到 EL2 或留在 EL1。

- **SPSR_EL2：选择返回后的 EL 与 PSTATE**
  - `SPSR_EL2.M[4:0] = 0b101` 或 `0b100`：EL1h / EL1t。
  - `SPSR_EL2.nRW`：AArch64/AArch32。
  - `SPSR_EL2.D/A/I/F`：屏蔽/允许 Debug、SError、IRQ、FIQ。

- **ELR_EL2：返回到 EL1 的 PC**
  - `ELR_EL2 =` 目标 EL1（例如 S-EL1 Secure Partition）的入口地址。

- **EL1 上下文**
  - 恢复相应 EL1 的：`SCTLR_EL1`、`TCR_EL1`、`TTBR0_EL1/TTBR1_EL1`、`VBAR_EL1`、`SP_EL1/SP_EL0`、GPR 等。

在 S-EL2 完成上述设置并执行 `ERET` 后，CPU 将降到 S-EL1，并按 SPSR_EL2 的描述执行。

---

*文档基于 AArch64、与 TF-A / Secure Partition 使用场景整理，具体位域以 ARM 架构手册为准。*
