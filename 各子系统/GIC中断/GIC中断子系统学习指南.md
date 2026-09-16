# GIC 中断子系统学习指南

> **一句话总结**：GIC（Generic Interrupt Controller）是 ARM 系统的"中断总控"，负责接收所有外设中断、裁决优先级、路由到目标 CPU、发送 IRQ/FIQ 异常，是整个系统并发和实时性的硬件基础。

---

## 📋 目录

- [GIC 基础概念](#gic-基础概念)
- [中断接收与处理流程](#中断接收与处理流程)
- [set_handle_irq 注册与解耦逻辑](#set_handle_irq-注册与解耦逻辑)
- [关于 `__ro_after_init` 的解释](#关于__ro_after_init-的解释)
- [关键数据结构与源码位置](#关键数据结构与源码位置)
- [调试与实验指南](#调试与实验指南)
- [面试要点总结](#面试要点总结)

---

## GIC 基础概念

### 1. 为什么需要 GIC？

在一个 ARM 系统里，可能有几十到几百个中断源：

```
定时器、UART、GPIO、PCIe/MSI、IOMMU fault、
GPU/NPU/USB/网卡、CPU 间中断 IPI、虚拟机中断...
```

这些中断源不能直接全部连到 CPU 核心上，否则系统会非常复杂。所以需要一个统一的中断控制器。

### 2. GIC 的职责

1. 接收外设中断信号
2. 判断中断是否使能
3. 判断中断优先级
4. 决定发给哪个 CPU
5. 通知 CPU 进入异常处理流程
6. 等待软件确认并完成中断处理

### 3. GICv3 组成

```
┌─────────────────────────┐
│      Distributor        │  ← 全局中断分发，管理 SPI
│      GICD               │
└────────┬──────┬────────┘
         │      │
         ▼      ▼
┌──────────┐ ┌──────────┐
│ Redistributor │ Redistributor │  ← 每个 CPU 一个，管理 PPI/SGI
│ GICR (CPU0)   │ GICR (CPU1)   │
└────────┬───┘ └────────┬───┘
         │               │
         ▼               ▼
┌─────────────────────────────────┐
│  CPU Interface (ICC_* 系统寄存器) │  ← CPU 访问 GIC 的接口
└─────────────────────────────────┘
```

### 4. 中断类型

| 类型 | 范围 | 说明 |
|------|------|------|
| **SGI** | INTID 0-15 | Software Generated Interrupt，CPU 间通信（IPI） |
| **PPI** | INTID 16-31 | Private Peripheral Interrupt，每个 CPU 私有（本地 timer） |
| **SPI** | INTID 32-1019 | Shared Peripheral Interrupt，共享外设中断，可路由到任意 CPU |
| **LPI** | INTID ≥ 8192 | Locality-specific Peripheral Interrupt，MSI 用，配合 ITS |

### 5. 中断状态机

GIC 中断不是简单的"来了/没来"，它有状态：

```
Inactive
    ↓ 外设触发中断
Pending
    ↓ CPU 读取 ICC_IAR1_EL1
Active
    ↓ CPU 写 ICC_EOIR1_EL1
Inactive
```

对于 level-triggered 中断，如果设备中断源没有清除，即使软件写了 EOIR，中断会很快又 pending。

---

## 中断接收与处理流程

### 核心认知

> GIC "接收外设中断"不是软件主动 read 出来的，而是**硬件中断线被外设拉起后，GIC 硬件自动把对应 INTID 标记为 pending**。Linux 代码主要负责：提前配置 GIC，等 CPU 进中断后再读取 GIC 的 IAR 寄存器确认是哪一路中断。

### 完整链路（以 UART 为例）

```
UART RX FIFO 有数据
    ↓
UART 控制器置位 RX 中断状态
    ↓
UART 中断输出线有效
    ↓
GIC 硬件接收到这根线
    ↓
GIC 把对应 SPI 标记为 pending
    ↓
GIC 根据优先级/路由选择某个 CPU
    ↓
CPU 进入 IRQ exception
    ↓
Linux 调用 gic_handle_irq()
    ↓
读取 ICC_IAR1_EL1，拿到中断号
    ↓
generic_handle_domain_irq()
    ↓
调用 UART 驱动 handler
    ↓
驱动清 UART 中断源
    ↓
gic_eoi_irq() 写 ICC_EOIR1_EL1
```

### GIC 初始化关键步骤

```c
// 1. 注册根 IRQ 处理函数
set_handle_irq(gic_handle_irq);

// 2. 初始化 CPU interface
gic_cpu_sys_reg_enable();
gic_prio_init();

// 3. 初始化 Distributor
gic_v3_dist_init();
    → writel_relaxed(0, base + GICD_CTLR);  // 先关
    → 配置所有 SPI 默认属性
    → 设置路由 affinity
    → writel_relaxed(val, base + GICD_CTLR);  // 再开

// 4. CPU 私有初始化
gic_v3_cpu_init();
```

### 驱动注册与 GIC 打开中断

```c
// 驱动调用 request_irq
request_irq(port->irq, stm32_usart_interrupt, IRQF_NO_SUSPEND, name, port);
    ↓
IRQ core 配置这一路 IRQ
    ↓
调用 gic_unmask_irq()
    ↓
写 GICD_ISENABLER 寄存器
    ↓
GIC 允许接收这一路外设中断
```

---

## set_handle_irq 注册与解耦逻辑

### 为什么需要注册？

ARM64 收到 IRQ 后，最早进入的是架构异常入口：

```c
el1h_64_irq_handler()  // arch/arm64/kernel/entry-common.c
    ↓
el1_interrupt(regs, handle_arch_irq);
```

这里的 `handle_arch_irq` 是一个**函数指针**，默认值是会 panic 的空实现。架构层不知道具体中断控制器是 GICv2/GICv3/Apple AIC，所以需要 irqchip 驱动初始化时**注册**自己的顶层处理函数。

### 注册机制

```c
// 定义：初始值是默认 panic 函数
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init = default_handle_irq;

// 注册函数
int __init set_handle_irq(void (*handle_irq)(struct pt_regs *))
{
    if (handle_arch_irq != default_handle_irq)
        return -EBUSY;  // 只允许注册一次

    handle_arch_irq = handle_irq;
    pr_info("Root IRQ handler: %ps\n", handle_irq);
    return 0;
}
```

### 注册后的完整调用链

```
硬件 IRQ
    → ARM64 exception vector（entry.S）
    → el1h_64_irq_handler()
    → handle_arch_irq()  ← 这个指针被 set_handle_irq 赋值
    → gic_handle_irq()   ← GIC 驱动注册的 handler
    → 读 ICC_IAR1_EL1 得到 INTID
    → generic_handle_domain_irq()
    → 设备驱动中断 handler
```

### 为什么这是好的设计？

**解耦**：架构层（entry-common.c）和 irqchip 驱动（irq-gic-v3.c）完全独立。
- 更换中断控制器（如从 GICv2 换到 GICv3），架构层代码一行都不用改
- 不同厂商的中断控制器（Apple AIC、RISC-V PLIC）都可以用同一套机制

### 级联中断控制器

CIX Sky1 上有 PDC（Power Domain Controller）作为二级中断控制器：

```
USB 中断 → PDC → GIC
```

GIC 原生代码只认直接连接到 GIC 的中断，PDC 下游的中断需要额外处理：

```c
if (rirq.controller != dt_interrupt_controller) {
    // 检查：父节点是 GIC + cell 格式相同
    if (irq_parent == dt_interrupt_controller &&
        be32_to_cpu(*tmp) == rirq.size &&
        dt_irq_xlate(rirq.specifier, rirq.size, &hwirq, &irq_type) == 0)
    {
        irq = hwirq;  // 直接用翻译后的 GIC hwirq
        irq_set_type(irq, irq_type);
    }
}
```

> 这就是你 Xen patch 0003 里级联中断处理的依据。

---

## 关于 `__ro_after_init` 的解释

### 是什么？

`__ro_after_init` 是一个 section 属性，标记"初始化后设为只读"。

```c
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init = default_handle_irq;
```

### 为什么要保护？

- **安全**：防止运行时被篡改。如果攻击者能改掉 `handle_arch_irq`，就可以 hook 所有中断。
- **稳健**：根 IRQ handler 这个概念上就应该是"设置一次，永远不变"。

### 它保护什么？

只读保护的是**函数指针变量本身**，不是中断处理函数，也不是 GIC 硬件接收能力。

| 操作 | 是否允许 |
|------|----------|
| 读取 `handle_arch_irq` 的值 | ✅ 允许 |
| 调用 `gic_handle_irq()` | ✅ 允许 |
| 修改 `handle_arch_irq` 指向别的函数 | ❌ 不允许 |

### 时序

```
init 阶段：
    set_handle_irq(gic_handle_irq);  ← 写操作，此时内存还是可写的
    ↓
init 完成：
    内核把 .data..ro_after_init 段对应的页表改成只读
    ↓
runtime 阶段：
    el1h_64_irq_handler() 读取函数指针并调用  ← 只读，正常工作
```

### 类比理解

```c
const int x = 100;  // 初始化后只读

printf("%d\n", x);  // ✅ 可以读
x = 200;             // ❌ 不可以写
```

函数指针也是一样：

```c
void (*handler)(void) = my_func;  // __ro_after_init 类似 const

handler();          // ✅ 可以调用（本质是读）
handler = other;    // ❌ 不可以改（写操作）
```

---

## 关键数据结构与源码位置

### 核心文件

| 文件 | 路径 | 作用 |
|------|------|------|
| **irq-gic-v3.c** | `drivers/irqchip/irq-gic-v3.c` | GICv3 驱动主文件 |
| **entry-common.c** | `arch/arm64/kernel/entry-common.c` | IRQ 异常入口 |
| **irq.c** | `arch/arm64/kernel/irq.c` | `set_handle_irq()` 实现 |
| **irqdesc.c** | `kernel/irq/irqdesc.c` | 中断描述符管理 |
| **handle.c** | `kernel/irq/handle.c` | 中断通用处理 |

### 关键函数定位

```c
// 入口：GIC 顶层 IRQ handler
static void __exception_irq_entry gic_handle_irq(struct pt_regs *regs)
    → __gic_handle_irq_from_irqson(regs)
        → irqnr = gic_read_iar();          // 读中断号
        → gic_complete_ack(irqnr);
        → generic_handle_domain_irq(gic_data.domain, irqnr);

// EOI 回调：告诉 GIC 处理完了
static void gic_eoi_irq(struct irq_data *d)

// 打开/关闭某路中断
static void gic_unmask_irq(struct irq_data *d)  // 写 GICD_ISENABLER
static void gic_mask_irq(struct irq_data *d)    // 写 GICD_ICENABLER
```

### 关键寄存器

| 寄存器 | 作用 |
|--------|------|
| **ICC_IAR1_EL1** | Interrupt Acknowledge Register，CPU 读它拿中断号 |
| **ICC_EOIR1_EL1** | End Of Interrupt Register，写它告诉 GIC 处理完了 |
| **ICC_PMR_EL1** | Priority Mask Register，优先级屏蔽 |
| **GICD_ISENABLER** | Interrupt Set-Enable Register，打开某路中断 |
| **GICD_ICENABLER** | Interrupt Clear-Enable Register，关闭某路中断 |
| **GICD_IROUTER** | Interrupt Routing Register，设置路由到哪个 CPU |

---

## 调试与实验指南

### 基础命令

```bash
# 查看所有中断及计数
cat /proc/interrupts

# 只看 GIC SPI 中断
cat /proc/interrupts | grep SPI

# 实时看中断计数变化
watch -n 1 "cat /proc/interrupts | head -30"

# 看中断亲和性（路由到哪个 CPU）
cat /proc/irq/<irq_number>/smp_affinity
```

### 调试手段

1. **数中断计数**：`/proc/interrupts` 里某中断计数不涨 = GIC 没收到 / 没路由对 / 被 mask 了
2. **打桩计数**：在 `gic_handle_irq` 入口加计数，确认中断确实走到 GIC handler
3. **读 GIC 寄存器**：
   ```c
   // 用 ioremap 后读 GICD 寄存器，确认 ISENABLER 是否真的置位了
   val = readl_relaxed(base + GICD_ISENABLER + (irq / 32) * 4);
   ```
4. **tracepoint**：
   ```bash
   echo 1 > /sys/kernel/debug/tracing/events/irq/enable
   cat /sys/kernel/debug/tracing/trace_pipe
   ```

### 常见问题排查

| 现象 | 可能原因 | 排查点 |
|------|----------|--------|
| 中断计数永远为 0 | 1. GIC 中该中断被 mask 了<br>2. 中断路由到了其他 CPU<br>3. 外设根本没产生中断 | 读 GICD_ISENABLER / IROUTER / ISPENDR |
| 中断风暴（计数疯涨） | 电平触发中断，驱动没清设备侧中断源 | 检查 handler 里是否清除了设备的中断 status 位 |
| 中断偶尔丢失 | 1. 优先级太低被屏蔽<br>2. 中断被其他 CPU 抢走（负载均衡） | 看 ICC_PMR_EL1 配置、smp_affinity |

---

## 面试要点总结

### 1. 概念必背

| 问题 | 标准回答 |
|------|----------|
| **GIC 的作用是什么？** | ARM 系统的中断控制器，负责中断接收、优先级裁决、CPU 路由、虚拟化中断注入 |
| **SGI/PPI/SPI 区别？** | SGI：软件产生，CPU 间通信（0-15）；PPI：每个 CPU 私有（16-31）；SPI：共享外设中断（32+） |
| **Edge vs Level 触发？** | Edge：跳变触发一次；Level：保持高电平一直有效，必须先清设备侧再 EOI |
| **中断状态机？** | Inactive → Pending → Active → Inactive |

### 2. 代码理解

| 问题 | 要点 |
|------|------|
| **set_handle_irq 机制？** | 架构层提供函数指针钩子，irqchip 驱动注册自己的顶层 handler，实现解耦。只允许注册一次。 |
| **gic_handle_irq 流程？** | 读 IAR 拿中断号 → ack → generic_handle_domain_irq 分发 → EOI |
| **级联中断怎么处理？** | 识别 cascaded controller，把下游中断号翻译成 GIC hwirq，再走正常路径。三重检查：父是 GIC、cell 格式匹配、xlate 成功。 |
| **__ro_after_init 作用？** | 初始化后设为只读，防止运行时被篡改，保护根中断入口安全。只保护指针变量本身，不影响读取和调用。 |

### 3. 调试经验

> **面试官最爱问**："USB 中断不工作，你怎么排查？"

标准回答框架：
1. **先看外设侧**：设备 status 寄存器中断位有没有置？驱动有没有 request_irq？
2. **再看 GIC 侧**：`/proc/interrupts` 计数涨不涨？GICD_ISENABLER 置位了吗？GICD_IROUTER 路由对吗？
3. **最后看 CPU 侧**：IRQ exception 进不进？gic_handle_irq 进不进？handler 里是不是提前 return 了？

---

**文档版本**: v1.0
**最后更新**: 2026-07-22
