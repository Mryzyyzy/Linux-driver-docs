GIC（Generic Interrupt Controller，通用中断控制器）是 ARM 架构中负责**中断分发、优先级仲裁、目标 CPU 选择、虚拟化中断支持**的核心硬件模块。它相当于 ARM SoC 里的“中断总控”。

---

## 1. 为什么需要 GIC？

在一个 ARM 系统里，可能有很多中断源：

- 定时器中断
- UART 中断
- GPIO 中断
- PCIe/MSI 中断
- IOMMU fault 中断
- GPU/NPU/USB/网卡中断
- CPU 间中断 IPI
- 虚拟机中断

这些中断源不能直接全部连到 CPU 核心上，否则系统会非常复杂。

所以需要一个统一的中断控制器：

```text
设备/外设
   |
   v
+--------+
|  GIC   |
+--------+
   |
   v
CPU0 CPU1 CPU2 CPU3 ...
```

GIC 的职责包括：

1. [[接收外设中断信号]]
2. 判断中断是否使能
3. 判断中断优先级
4. 决定发给哪个 CPU
5. 通知 CPU 进入异常处理流程
6. 等待软件确认并完成中断处理

---

## 2. GIC 的几个重要版本

常见版本：

| 版本 | 典型用途 |
|---|---|
| GICv2 | 老一些的 ARMv7/ARMv8 系统 |
| GICv3 | ARMv8 服务器、嵌入式、多核系统常见 |
| GICv4 | 增强虚拟化中断，特别是直通设备场景 |
| GICv4.1 | 进一步优化虚拟化和 vPE 管理 |

Xen 中对应的文件是 `xen/arch/arm/gic-v3.c`，主要处理 ARM GICv3 的初始化、CPU 接口、中断路由、虚拟化等逻辑。

---

## 3. GIC 的基本组成

以 GICv3 为例，主要组件有：

```text
+-------------------------+
|        Distributor      |
|        GICD             |
+-------------------------+
       |       |       |
       v       v       v
+---------+ +---------+ +---------+
| Redistributor CPU0 | CPU1 | CPU2 |
| GICR               |      |      |
+---------+ +---------+ +---------+
       |       |       |
       v       v       v
+---------+ +---------+ +---------+
| CPU IF  | | CPU IF  | | CPU IF  |
| ICC_*   | | ICC_*   | | ICC_*   |
+---------+ +---------+ +---------+
```

### 3.1 Distributor：GICD

Distributor 是全局中断分发器。

它负责：

- 管理 SPI 中断
- 设置中断使能/禁用
- 设置中断优先级
- 设置中断触发方式
- 设置中断目标 CPU
- 设置中断 group/security 属性

常见寄存器：

| 寄存器 | 作用 |
|---|---|
| `GICD_CTLR` | Distributor 总控制 |
| `GICD_ISENABLER` | 使能中断 |
| `GICD_ICENABLER` | 禁用中断 |
| `GICD_ISPENDR` | 设置 pending |
| `GICD_ICPENDR` | 清除 pending |
| `GICD_IPRIORITYR` | 设置优先级 |
| `GICD_ICFGR` | 设置 edge/level 触发 |
| `GICD_IROUTER` | 设置中断路由目标 CPU |

---

### 3.2 Redistributor：GICR

GICv3 引入 Redistributor，每个 CPU 通常有一个。

它负责管理每个 CPU 私有的中断：

- SGI
- PPI
- LPI 相关配置
- CPU 上线/下线时的中断状态

GICv2 中 SGI/PPI 主要由 Distributor 统一管理，GICv3 则把这些 per-CPU 的事情拆到了 Redistributor。

---

### 3.3 CPU Interface：ICC_* 系统寄存器

CPU Interface 是 CPU 访问 GIC 的接口。

在 GICv3 中，CPU interface 主要通过系统寄存器访问，而不是像 GICv2 那样主要通过 MMIO。

常见寄存器：

| 寄存器 | 作用 |
|---|---|
| `ICC_IAR1_EL1` | CPU 读取当前中断号，表示 ack |
| `ICC_EOIR1_EL1` | 通知 GIC 中断处理结束 |
| `ICC_PMR_EL1` | 优先级屏蔽寄存器 |
| `ICC_BPR1_EL1` | 优先级分组 |
| `ICC_CTLR_EL1` | CPU interface 控制 |
| `ICC_SRE_EL1` | 使能系统寄存器访问 GIC |

---

## 4. 中断类型

GIC 中断大致分几类。

### 4.1 SGI：Software Generated Interrupt

软件产生的中断，通常用于 CPU 间通信。

编号范围：

```text
INTID 0 - 15
```

典型用途：

- IPI
- CPU 之间发送调度通知
- TLB shootdown
- 让其他 CPU 执行某个动作

例如 CPU0 想通知 CPU1：

```text
CPU0 -> 写 GIC SGI 寄存器 -> GIC -> CPU1 收到 IPI
```

---

### 4.2 PPI：Private Peripheral Interrupt

每个 CPU 私有的外设中断。

编号范围：

```text
INTID 16 - 31
```

典型例子：

- 每个 CPU 的本地 timer 中断
- performance monitor 中断
- local watchdog

PPI 是 per-CPU 的，也就是说同一个 PPI 号在不同 CPU 上代表各自 CPU 的本地中断。

---

### 4.3 SPI：Shared Peripheral Interrupt

共享外设中断。

编号范围通常是：

```text
INTID 32 - 1019
```

典型例子：

- UART
- USB
- 网卡
- MMC
- GPU
- IOMMU
- PCIe controller

SPI 可以路由到一个或多个 CPU。

---

### 4.4 LPI：Locality-specific Peripheral Interrupt

GICv3 引入，主要用于 MSI/MSI-X 场景。

编号范围通常从：

```text
INTID >= 8192
```

LPI 通常配合 ITS 使用。

---

## 5. ITS 是什么？

ITS 是 Interrupt Translation Service。

它主要用于把设备发出的 MSI/MSI-X 转换成 GIC 能识别的 LPI。

典型场景：

```text
PCIe 设备发 MSI
     |
     v
ITS 翻译 DeviceID/EventID
     |
     v
生成 LPI
     |
     v
GIC Redistributor
     |
     v
目标 CPU
```

ITS 维护一些表：

- Device Table
- Collection Table
- Interrupt Translation Table
- vPE Table，虚拟化相关

在服务器、PCIe、虚拟化场景下，ITS 很重要。

---

## 6. 中断状态机

GIC 中断不是简单的“来了/没来”，它有状态。

常见状态：

```text
Inactive
Pending
Active
Active + Pending
```

含义：

| 状态 | 含义 |
|---|---|
| Inactive | 没有中断 |
| Pending | 中断来了，但 CPU 还没处理 |
| Active | CPU 已经 acknowledge，正在处理 |
| Active + Pending | 正在处理时又来了一个同样中断 |

典型流程：

```text
Inactive
   |
   | 外设触发中断
   v
Pending
   |
   | CPU 读取 IAR
   v
Active
   |
   | CPU 写 EOIR
   v
Inactive
```

对于 level-triggered 中断，如果设备中断源没有清除，即使软件写了 EOIR，中断可能很快又 pending。

---

## 7. 中断触发方式

GIC 支持两类常见触发方式：

### 7.1 Edge-triggered

边沿触发。

```text
低 -> 高 的跳变触发一次中断
```

特点：

- 触发一次就是一次事件
- 不要求中断线一直保持有效
- 如果软件处理太慢，可能需要硬件/控制器保存事件

常用于 MSI、某些 GPIO。

---

### 7.2 Level-triggered

电平触发。

```text
只要中断线保持高电平，中断就一直有效
```

特点：

- 必须先清除设备侧中断源
- 再写 GIC EOIR
- 否则中断会再次触发

大多数传统外设中断是 level-triggered。

典型处理顺序：

```text
中断进入
  |
  v
驱动读取设备状态
  |
  v
清除设备中断源
  |
  v
写 GIC EOIR
```

---

## 8. CPU 处理中断的基本流程

以 GICv3 为例：

```text
外设触发中断
    |
    v
GICD/GICR 标记 pending
    |
    v
GIC 根据优先级和路由选择 CPU
    |
    v
CPU 收到 IRQ 异常
    |
    v
异常向量进入内核
    |
    v
内核读取 ICC_IAR1_EL1 获取 INTID
    |
    v
调用对应 IRQ handler
    |
    v
handler 清除设备中断源
    |
    v
内核写 ICC_EOIR1_EL1 完成中断
```

简化伪代码：

```c
void handle_irq(void)
{
    irq = read_sysreg(ICC_IAR1_EL1);

    if (irq_is_valid(irq))
        generic_handle_irq(irq);

    write_sysreg(irq, ICC_EOIR1_EL1);
}
```

---

## 9. 优先级机制

GIC 支持中断优先级。

优先级数值通常是：

```text
数值越小，优先级越高
```

例如：

```text
0x10 高优先级
0x80 普通优先级
0xf0 低优先级
```

CPU Interface 有一个重要寄存器：

```text
ICC_PMR_EL1
```

它是 Priority Mask Register。

只有优先级高于屏蔽阈值的中断才能送到 CPU。

例如：

```text
ICC_PMR_EL1 = 0x80
```

那么优先级数值小于等于某个实现定义范围的中断可以进入，较低优先级会被屏蔽。

---

## 10. 中断路由

GICv3 使用 affinity 路由。

ARM CPU 通常有 MPIDR，表示 CPU 的层级编号：

```text
Aff3.Aff2.Aff1.Aff0
```

GICD_IROUTER 可以指定 SPI 路由到哪个 CPU affinity。

例如：

```text
SPI 45 -> CPU2
SPI 46 -> CPU0
SPI 47 -> 任意一个在线 CPU
```

这对 SMP 系统非常重要。

---

## 11. Group 和安全状态

GIC 支持安全状态相关的中断分组。

常见分组：

| Group | 用途 |
|---|---|
| Group 0 | Secure interrupt |
| Group 1 Secure | Secure EL1/EL3 使用 |
| Group 1 Non-secure | 普通 OS 使用 |

在 Linux/Xen 这类 non-secure OS 中，通常主要处理 Group 1 Non-secure 中断。

---

## 12. GIC 和异常级别

ARMv8 有多个 Exception Level：

```text
EL0: 用户态
EL1: 内核
EL2: Hypervisor，例如 Xen/KVM
EL3: Secure Monitor
```

GIC 的访问也和 EL 有关：

- EL1 OS 处理中断
- EL2 Hypervisor 可以截获/虚拟化中断
- EL3 Secure Monitor 管理安全世界中断

在 Xen 中，GIC 非常重要，因为 Xen 运行在 EL2，需要：

1. 接管物理 GIC
2. 管理物理中断
3. 给 Dom0/DomU 注入虚拟中断
4. 支持设备直通和虚拟 ITS

---

## 13. GIC 虚拟化

虚拟化场景下有两类中断：

```text
物理中断 pIRQ
虚拟中断 vIRQ
```

例如一个物理网卡中断进来：

```text
物理网卡
  |
  v
GIC 产生 pIRQ
  |
  v
Xen EL2 捕获
  |
  v
Xen 判断属于哪个 Guest
  |
  v
向 Guest 注入 vIRQ
  |
  v
Guest OS 以为自己收到了普通中断
```

GICv3 虚拟化相关组件包括：

- List Register，保存待注入给 guest 的虚拟中断
- Virtual CPU Interface
- Maintenance interrupt
- ITS 虚拟化，处理虚拟 MSI/LPI

GICv4/GICv4.1 进一步优化了虚拟中断注入，可以减少 Hypervisor 参与，提高性能。

---

## 14. Linux/Xen 中的抽象

在操作系统里，一般不会让驱动直接操作 GIC 寄存器。

Linux 中通常是：

```text
设备驱动
  |
  v
request_irq()
  |
  v
generic IRQ subsystem
  |
  v
irqchip driver
  |
  v
GIC driver
```

Xen 中也有类似抽象：

```text
物理 IRQ
  |
  v
Xen IRQ 管理
  |
  v
GICv3 driver
  |
  v
Guest vIRQ 注入
```

`gic-v3.c` 通常会涉及：

- GIC 初始化
- CPU interface 初始化
- Distributor 初始化
- Redistributor 初始化
- SGI/PPI/SPI 管理
- 中断 acknowledge/eoi
- 中断路由
- suspend/resume
- 虚拟化接口

---

## 15. 一个完整例子：UART 中断

假设 UART 收到一个字符：

```text
UART RX FIFO 有数据
    |
    v
UART 拉高中断线
    |
    v
GICD 标记 UART SPI pending
    |
    v
GIC 根据路由选择 CPU0
    |
    v
CPU0 进入 IRQ exception
    |
    v
内核读取 IAR，得到 UART IRQ number
    |
    v
调用 UART driver IRQ handler
    |
    v
driver 读取 UART 数据
    |
    v
driver 清除 UART 中断状态
    |
    v
内核写 EOIR
    |
    v
中断结束
```

如果 UART 中断源没有清掉：

```text
写 EOIR 后
    |
    v
GIC 发现 UART 中断线仍然有效
    |
    v
再次 pending
    |
    v
CPU 又进入 IRQ
```

这就是常见的“中断风暴”。

---

## 16. 学习 GIC 建议关注的核心概念

建议按这个顺序学习：

1. IRQ/FIQ 异常入口
2. SGI/PPI/SPI/LPI 区别
3. Distributor / Redistributor / CPU Interface
4. Pending / Active / EOI 状态机
5. Edge / Level 触发区别
6. 中断优先级和屏蔽
7. SMP 中断路由
8. ITS 和 MSI
9. 虚拟化中断
10. Xen/KVM 如何注入 vIRQ

---

一句话总结：

> GIC 是 ARM 系统里的中断总控，它把来自外设、CPU、PCIe/MSI 和虚拟机的各种中断统一管理，并根据优先级、目标 CPU、安全状态和虚拟化规则，把中断准确送到对应的处理者。