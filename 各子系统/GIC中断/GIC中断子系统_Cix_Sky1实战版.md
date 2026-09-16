# GIC 中断子系统深度剖析（基于 Cix Sky1 真实硬件）

本文档基于 **Cix Sky1 EVB** 的真实设备树配置和 Linux 6.12 内核源码，从硬件到软件、从启动到运行，代码级剖析 GIC 中断子系统。

---

## 🔧 真实硬件配置（来自设备树）

### Cix Sky1 GIC-700 配置

```dts
// arch/arm64/boot/dts/cix/sky1.dtsi
gic: interrupt-controller@0e001000 {
    compatible = "arm,gic-700", "arm,gic-v3";
    #interrupt-cells = <3>;
    interrupt-controller;
    reg = <0x0 0x0e010000 0 0x10000>,       /* GICD Distributor */
          <0x0 0x0e090000 0 0x300000>;      /* GICR Redistributor */
    redistributor-stride = <0x0 0x040000>;   /* 每个核 256KB */
    interrupts = <GIC_PPI 9 IRQ_TYPE_LEVEL_LOW>;

    // ITS 中断翻译服务（PCIe MSI 用）
    its_pcie: its@0e050000 {
        compatible = "arm,gic-v3-its";
        msi-controller;
        reg = <0x0 0x0e050000 0x0 0x30000>;
    };
};
```

### 关键硬件参数

| 组件           | 物理地址       | 大小  | 说明                      |
| -------------- | -------------- | ----- | ------------------------- |
| **GICD** | `0x0e010000` | 64KB  | 分发器，全局中断路由控制  |
| **GICR** | `0x0e090000` | 3MB   | 重分发器，每个核 256KB    |
| **ITS**  | `0x0e050000` | 192KB | 中断翻译服务，PCIe MSI 用 |

**GIC-700 是 ARM 最新一代车规级中断控制器，支持 GICv3/v4 架构，专为 8 核以上异构 SoC 设计。**

---

## 📋 中断类型（Cix 板子上真实在用的）

| 类型          | 缩写           | Cix 例子                                  | 触发方式         |
| ------------- | -------------- | ----------------------------------------- | ---------------- |
| **SGI** | 软件生成中断   | `IPI`（核间中断）                       | 软件写寄存器触发 |
| **PPI** | 私有外设中断   | `GIC_PPI 7` (PMU), `GIC_PPI 9` (VGIC) | 每个核私有       |
| **SPI** | 共享外设中断   | UART / I2C / PCIe / DRM 等所有外设        | 所有核共享       |
| **LPI** | 本地化特殊中断 | ITS 管理的 MSI 中断                       | 消息信号中断     |

---

## 🔄 启动时序：GIC 初始化全过程

### Phase 1: BL31 (EL3) 阶段初始化

> **重要说明：** 以下代码全部属于 **ARM Trusted Firmware-A (TF-A / ATF)** 独立项目，**不在 Linux 内核源码树里**。ATF 跑在 EL3，Linux 跑在 EL1，是两套完全独立的代码。
>
> - ATF 官方仓库：`https://git.trustedfirmware.org/TF-A/trusted-firmware-a.git`
> - Linux GIC 驱动路径：`drivers/irqchip/irq-gic-v3.c`（完全是另一套代码）

#### 1.1 入口：`bl31_main()` 调用平台初始化

**真实路径：** `bl31/bl31_main.c`

```c
void bl31_main(void)
{
    // ...
    /* 平台级中断控制器初始化 */
    plat_interrupt_setup();  // 配置 GICD, GICR 基础寄存器
    // ...
}
```

#### 1.2 平台接口声明（弱符号，各平台自行实现）

**真实路径：** `include/plat/common/platform.h`

```c
/*
 * 平台相关的中断控制器初始化接口
 * 弱符号声明，默认实现为空，各平台（Arm/海思/高通/Xilinx）覆盖实现
 */
void plat_interrupt_setup(void);
```

#### 1.3 Arm 标准平台实现（Cix Sky1 类似的 Arm 架构 SoC 基本都用这套）

**真实路径：** `plat/arm/common/arm_gicv3.c`

```c
void plat_interrupt_setup(void)
{
    arm_gic_init();   // ← 真正的 GIC 初始化入口
}
```

`arm_gic_init()` 同文件中的实现：

```c
void arm_gic_init(void)
{
    /* 1. 初始化 GIC Distributor（全局，只配一次） */
    gicv3_distif_init();

    /* 2. 初始化本核的 GIC Redistributor（per-CPU，每个核都要调） */
    gicv3_rdistif_init(plat_my_core_pos());

    /* 3. 使能本核的 CPU Interface */
    gicv3_cpuif_enable(plat_my_core_pos());
}
```

#### 1.4 GICv3 驱动核心（通用层）

| 函数                     | 真实路径                               | 做什么                                                                              |
| ------------------------ | -------------------------------------- | ----------------------------------------------------------------------------------- |
| `gicv3_distif_init()`  | `drivers/arm/gic/v3/gicv3_main.c`    | 复位 GICD、配置`GICD_CTLR`（ARE/DS 位）、所有 SPI 设为 Group 1 NS、设默认优先级   |
| `gicv3_rdistif_init()` | `drivers/arm/gic/v3/gicv3_main.c`    | 唤醒 GICR、配置`GICR_CTLR`、SGI/PPI 分组和优先级、使能 SGI (0-15)                 |
| `gicv3_cpuif_enable()` | `drivers/arm/gic/v3/gicv3_helpers.c` | 设`ICC_SRE_EL3`（开系统寄存器接口）、设 `ICC_PMR` 优先级掩码、使能 Group 1 中断 |

**GICv3 驱动头文件：** `include/drivers/arm/gicv3.h`

#### 1.5 EL3 阶段到底做了什么（和 Kernel 初始化的边界）

| 初始化内容                            | 谁做                  | 为什么                                     |
| ------------------------------------- | --------------------- | ------------------------------------------ |
| GICD 全局复位 + 安全分组（Group 0/1） | **EL3 (ATF)**   | 安全相关的寄存器只有 EL3 能碰              |
| 主核 GICR 唤醒 + CPU Interface 使能   | **EL3 (ATF)**   | 主核起来就要能用中断（EL3 自己也会用到）   |
| 从核 GICR 初始化                      | **EL3 (ATF)**   | 从核通过 PSCI CPU_ON 起来时，在 EL3 阶段配 |
| 具体外设中断的使能/禁能/亲和性        | **EL1 (Linux)** | 由各驱动按需操作                           |
| ITS + MSI 初始化                      | **EL1 (Linux)** | 正常世界管，ATF 不碰                       |
| 中断处理函数注册（`request_irq`）   | **EL1 (Linux)** | 驱动逻辑                                   |

> **一句话：** ATF 负责"把 GIC 从复位状态拉起来、配好安全墙"，Linux 负责"日常使用每个具体中断"。

---

### Phase 2: Kernel GIC 驱动初始化

**代码位置：** `drivers/irqchip/irq-gic-v3.c`

#### 第 1 步：设备树匹配

```c
static const struct of_device_id gic_device_match[] = {
    { .compatible = "arm,gic-700", .data = gic700_of_match },
    { .compatible = "arm,gic-v3", ... },
    { }
};

IRQCHIP_DECLARE(gic_v3, "arm,gic-v3", gic_of_init);
```

内核启动时看到设备树里的 `"arm,gic-700"`，就会调用 `gic_of_init()`。

#### 第 2 步：寄存器映射初始化

```c
static int __init gic_of_init(struct device_node *node, struct device_node *parent)
{
    // 1. 映射 GICD 寄存器
    dist_base = of_iomap(node, 0);  // 0x0e010000

    // 2. 映射 GICR 寄存器
    rdist_base = of_iomap(node, 1);  // 0x0e090000

    // 3. 初始化 GIC 分发器
    gic_dist_init(gic_data);

    // 4. 初始化每个核的 Redistributor
    gic_cpu_init(gic_data);

    // 5. 设置中断域根句柄
    gic_irq_domain = irq_domain_add_linear(node, ...);

    // 6. 注册中断回调
    set_handle_irq(gic_handle_irq);

    return 0;
}
```

#### 第 3 步：`set_handle_irq` 注册中断入口

**这是最关键的解耦设计！** （详细分析见 `set_handle_irq注册与解耦逻辑.md`）

```c
// arch/arm64/kernel/entry.S
/*
 * 中断发生时，CPU 硬件跳到这里
 * 然后调用 gic_handle_irq()
 */
el1_irq:
    kernel_entry 1
    enable_daif
    irq_handler  // ← 这里调用的就是 set_handle_irq 注册的函数
```

---

## 🚀 中断处理全流程（代码级）

### 第 1 步：中断发生，CPU 跳异常向量

```
外设产生中断信号
    ↓
GIC 把中断送到某个 CPU 核
    ↓
CPU 自动保存寄存器到栈
    ↓
跳转到 el1_irq 异常向量（entry.S）
```

### 第 2 步：GIC 驱动读取中断号

**代码位置：** `drivers/irqchip/irq-gic-v3.c`

```c
static asmlinkage void __exception_irq_entry gic_handle_irq(struct pt_regs *regs)
{
    u32 irqstat, irqnr;

    do {
        // 读 ICC_IAR1_EL1 寄存器，拿到中断号
        irqstat = gic_read_iar();
        irqnr = irqstat & GICC_IAR_INT_ID_MASK;

        // 1020-1023 是特殊中断号，跳过
        if (irqnr < 1020) {
            // 调用对应中断的处理函数
            generic_handle_domain_irq(gic_irq_domain, irqnr);
        }

        // 写 EOI 寄存器，结束中断
        gic_write_eoir(irqstat);

    } while (irqnr < 1020);
}
```

### 第 3 步：执行外设驱动注册的中断处理函数

```c
// 比如 UART 驱动的中断注册
request_irq(uart_irq, uart_interrupt_handler, IRQF_SHARED, "uart", dev);

// 中断触发时就会走到这里
irqreturn_t uart_interrupt_handler(int irq, void *dev_id)
{
    // 读 UART 状态寄存器
    // 处理接收/发送 FIFO
    // 清中断标志
    return IRQ_HANDLED;
}
```

---

## 📬 Cix 板子上真实的中断例子

### 例子 1: PMU 性能计数器中断（PPI）

```dts
pmu_a720: pmu_a720 {
    compatible = "arm,cortex-a720-pmu";
    interrupts = <GIC_PPI 7 IRQ_TYPE_LEVEL_LOW>;
    interrupt-parent = <&gic>;
    cpus = <&CPU4 ... &CPU11>;  // 8 个大核
};
```

**PPI = Private Peripheral Interrupt**

- 每个核都有自己的 PMU 中断 7
- 中断只送到对应的核，其他核看不到
- `#interrupt-cells = <3>` 的含义：
  - 第 1 个 cell：类型（0=SPI, 1=PPI）
  - 第 2 个 cell：中断号
  - 第 3 个 cell：触发类型

### 例子 2: ITS + PCIe MSI 中断（LPI）

```dts
its_pcie: its@0e050000 {
    compatible = "arm,gic-v3-its";
    msi-controller;
    reg = <0x0 0x0e050000 0x0 0x30000>;
};
```

ITS = Interrupt Translation Service，用来支持：

- PCIe MSI/MSI-X 消息中断
- 可以支持几千个中断号，远超过 SPI 的 1020 限制
- 你的 PCIe 显卡、NVMe SSD 用的就是这种中断

---

## 🔍 关键代码文件索引

### GICv3 核心驱动（你的工作区里都有）

| 文件                       | 路径                                                   | 核心功能                       |
| -------------------------- | ------------------------------------------------------ | ------------------------------ |
| **irq-gic-v3.c**     | `drivers/irqchip/irq-gic-v3.c`                       | GICv3 主驱动，初始化、中断处理 |
| **irq-gic-v3-its.c** | `drivers/irqchip/irq-gic-v3-its.c`                   | ITS 中断翻译服务               |
| **irq-gic-common.c** | `drivers/irqchip/irq-gic-common.c`                   | GICv2/v3 公共函数              |
| **arm-gic.h**        | `include/dt-bindings/interrupt-controller/arm-gic.h` | 设备树宏定义                   |

### ARM64 中断入口

| 文件              | 路径                          | 功能                     |
| ----------------- | ----------------------------- | ------------------------ |
| **entry.S** | `arch/arm64/kernel/entry.S` | 异常向量表，中断入口汇编 |
| **irq.c**   | `arch/arm64/kernel/irq.c`   | 架构层中断初始化         |

---

## 💡 BSP 工程师必懂的 GIC 调试技巧

### 1. 查看系统中断统计

```bash
cat /proc/interrupts
```

输出示例（Cix 真实输出大概长这样）：

```
           CPU0       CPU1       CPU2  ...
  2:     12345          0          0   GICv3  2 Level      dsu_pmu
  9:         0        123          0   GICv3  9 Level      arch_timer
 34:      5678          0          0   GICv3 34 Level      serial
```

### 2. 看 GIC 寄存器状态

```bash
# 挂载 debugfs
mount -t debugfs debugfs /sys/kernel/debug

# 看 GIC 中断配置
cat /sys/kernel/debug/irq/irqs/34
```

### 3. 中断亲和性设置

```bash
# 把 UART 中断固定到 CPU0
echo 1 > /proc/irq/34/smp_affinity
```

---

## 🚫 常见 GIC 问题（你做 Xen 会遇到的）

### 问题 1: Dom0 里中断不触发

**现象：** Native Linux 下正常，Xen Dom0 下中断不来
**根因：** Xen 接管了 GIC，需要在 Xen 里正确配置虚拟中断控制器
**检查点：**

- Xen 设备树里的 `interrupt-parent` 是不是指向 vGIC
- GICR 的地址范围有没有预留出来给 Xen

### 问题 2: SMMU 中断风暴（你启动日志里的问题）

**现象：** DP 热插拔后，SMMU 中断一秒几千次
**根因：** GIC 里 SMMU 中断的触发类型配错，或者中断没有正确 ACK
**修复：** 在设备树里把中断类型改成 `IRQ_TYPE_EDGE_RISING` 或者 `LEVEL_HIGH`

### 问题 3: 核间中断 (IPI) 延迟

**现象：** 发 IPI 到另一个核要等几百微秒
**根因：** GICR 的唤醒配置不对，CPU idle 时没有及时响应中断
**修复：** 检查 GICR_WAKER 寄存器配置

---

## 🎯 总结：GIC 在整个系统中的位置

```
┌─────────────────────────────────────────────────────────┐
│                    外设硬件（UART/PCIe/DRM）             │
└──────────────────────────┬──────────────────────────────┘
                           │ 中断信号
┌──────────────────────────▼──────────────────────────────┐
│              GIC-700 中断控制器（0x0e010000）          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐            │
│  │  GICD    │  │   ITS    │  │  GICR x12│            │
│  └──────────┘  └──────────┘  └──────────┘            │
└──────────────────────────┬──────────────────────────────┘
                           │ 中断分发到某个核
┌──────────────────────────▼──────────────────────────────┐
│              Cortex-A55/A720 x12  CPU 核                │
│  ┌───────────────────────────────────────────────────┐│
│  │  el1_irq → gic_handle_irq → 驱动中断处理函数    ││
│  └───────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────┘
```

---

## 📚 参考文档索引

| 文档                                                                    | 内容                       |
| ----------------------------------------------------------------------- | -------------------------- |
| [GIC中断总览.md](./GIC中断总览.md)                                       | GIC 架构总览               |
| [接收外设中断信号.md](./接收外设中断信号.md)                             | 外设中断信号流详细分析     |
| [set_handle_irq注册与解耦逻辑.md](./set_handle_irq注册与解耦逻辑.md)     | 中断子系统解耦设计深度分析 |
| [关于中断中_ro_after_init的解释.md](./关于中断中_ro_after_init的解释.md) | 内核内存安全机制           |
