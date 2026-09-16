# 从核启动与 GICR 初始化（PSCI CPU_ON 全流程）

从核启动的完整流程 + GICR 初始化串起来讲。

---

## 一、从核怎么"活过来"的：PSCI CPU_ON 全流程

从核上电后不是自己跑起来的，是**主核通过 PSCI 接口把它叫醒的**。

### 1. 总览图

```
主核 (CPU0, EL1)                     从核 (CPU1, 复位/关闭状态)
    │                                          │
    │  1. 触发 PSCI CPU_ON SMC 调用            │
    ├───────────────► EL3 ◄───────────────────┤
    │                │                         │
    │                │ 2. 执行 psci_cpu_on()   │
    │                │    - 配置从核启动地址   │
    │                │    - 给从核上电/解复位  │
    │                │                         │
    │                │ 3. 从核开始执行         │
    │                │     ↓                   │
    │                │   BL31 entrypoint       │
    │                │     ↓                   │
    │                │   cm_prepare_el3_exit() │
    │                │   gicv3_rdistif_init(1) │
    │                │   gicv3_cpuif_enable(1) │
    │                │     ↓                   │
    │                │   eret → EL1            │
    │                │                         │
    │                └───────────► Linux secondary entry
    │                                          │
    │                                          │ 4. Linux 从核初始化
    │                                          │    gic_cpu_init() / set_cpu_online
```

---

### 2. 主核触发：Linux 怎么发起 CPU_ON

**路径：** `arch/arm64/kernel/smp.c`

```c
int __cpu_up(unsigned int cpu, struct task_struct *idle)
{
    // 把从核的启动地址设为 secondary_entry
    // 然后调用 PSCI 的 CPU_ON
    return cpu_ops[cpu]->cpu_boot(cpu);
}
```

底层调 PSCI：

```c
// drivers/firmware/psci/psci.c
static int psci_cpu_on(unsigned long cpuid, unsigned long entry_point)
{
    // 发 SMC #0 陷入 EL3，function ID = PSCI_CPU_ON
    return psci_invoke(PSCI_0_2_FN_CPU_ON, cpuid, entry_point, 0, NULL);
}
```

`SMC` 指令一执行，**CPU 就从 EL1 陷到 EL3**，ATF 接棒。

---

### 3. EL3 里发生了什么：ATF 的 PSCI 处理

**路径（ATF）：** `lib/psci/psci_main.c`

```c
u_register_t psci_smc_handler(uint32_t smc_fid,
    u_register_t x1, u_register_t x2, u_register_t x3,
    u_register_t x4, void *cookie, void *handle,
    u_register_t flags)
{
    switch (smc_fid) {
    case PSCI_CPU_ON_AARCH64:
        return psci_cpu_on(x1, x2, x3);  // x1=cpuid, x2=entry点
    // ...
    }
}
```

`psci_cpu_on()` 里干三件事：

```c
int psci_cpu_on(u_register_t target_cpu,
                u_register_t entrypoint,
                u_register_t context_id)
{
    // 1. 校验目标核状态（是不是真的关着的）
    rc = psci_validate_mpidr(target_cpu, &state_info);

    // 2. 给从核设"醒来后跑哪儿"——把 entrypoint 存到从核的上下文里
    psci_set_pwr_domain_state(...)
    // 平台相关：真正给从核上电/解复位
    // （不同 SoC 实现不一样，有的写电源控制器，有的写复位控制器）
    psci_pwr_ops->pwr_domain_on(target_cpu);

    // 3. 主核这边的 SMC 调用返回，从核那边开始跑了
    return PSCI_E_SUCCESS;
}
```

---

### 4. 从核醒过来：从哪里开始跑？

从核上电/解复位后，**不是直接进 Linux**，而是从 **BL31 的复位入口**开始跑（因为从核复位默认在 EL3）。

**路径（ATF）：** `bl31/aarch64/bl31_entrypoint.S`

```asm
func bl31_entrypoint
    // 1. 最早期的 CPU 初始化（关 cache、关 MMU、设异常向量）
    // 2. 识别自己是哪个核
    mrs     x0, mpidr_el1
    bl      plat_my_core_pos

    // 3. 判断：我是主核还是从核？
    //    主核 → 正常走 bl31_main 初始化全流程
    //    从核 → 跳过大部分初始化，直接走 warm reset 路径
    // ...
endfunc bl31_entrypoint
```

**从核走的是"热启动（warm reset）"路径**，比主核短得多，只做 per-CPU 必要的初始化。

---

### 5. 从核 EL3 的 GICR 初始化

**路径（ATF）：** `bl31/bl31_main.c` → `bl31_warm_entrypoint()`

从核暖启动时，会调：

```c
void bl31_warm_entrypoint(void)
{
    // ...
    /* 初始化本核的 GIC Redistributor */
    gicv3_rdistif_init(plat_my_core_pos());

    /* 使能本核的 CPU Interface */
    gicv3_cpuif_enable(plat_my_core_pos());

    /* 准备跳 EL1 */
    cm_prepare_el3_exit(NON_SECURE);

    /* 异常返回，跳到 Linux 的 secondary_entry */
    el3_exit();
}
```

**这就是"从核通过 PSCI CPU_ON 起来时，在 EL3 阶段配 GICR"**。

`gicv3_rdistif_init(cpu)` 具体做了什么？

- 唤醒本核的 GICR（清 `GICR_WAKER.ProcessorSleep`）
- 配置 `GICR_CTLR`
- 把 SGI/PPI 的 Group 都设成 Group 1 NS（非安全）
- 设置 SGI/PPI 默认优先级
- 使能 SGI 0-15（核间通信用，必须开）

`gicv3_cpuif_enable(cpu)` 具体做了什么？

- 设 `ICC_SRE_EL3.SRE = 1`（开系统寄存器接口，EL1 才能用 ICC_xxx_EL1）
- 设 `ICC_PMR_EL1` 为默认优先级掩码
- 设 `ICC_IGRPEN1_EL1.Enable = 1`（使能 Group 1 中断）

---

### 6. 从核进 Linux：EL1 侧的初始化

从核从 EL3 `eret` 出来，跳进 Linux 的从核入口：

**路径：** `arch/arm64/kernel/smp_spin_table.c` 或 `arch/arm64/kernel/head.S`

```c
// 从核的 C 入口
asmlinkage void secondary_start_kernel(void)
{
    // 1. 通知主核：我起来了
    // 2. 本核的 MMU、cache、定时器初始化
    // 3. GIC 侧的 CPU 初始化
    gic_cpu_init(gic_data);  // Linux 自己的 GICR 配置

    // 4. 通知调度器本核就绪
    set_cpu_online(smp_processor_id(), true);

    // 5. 进入 idle 循环，等调度
    cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
}
```

Linux 的 `gic_cpu_init()` 又做了一遍 GICR/CPU Interface 配置？ ——对，Linux 不信任固件的配置，它会按自己的策略**重新配置**：

- 重新设优先级掩码（`ICC_PMR_EL1`）
- 重新配置 PPI 的触发方式和优先级
- 开/关某些 PPI（比如 arch timer、PMU）
- 注册本核的中断域映射

---

## 二、具体外设中断的使能/禁能/亲和性

这个阶段 GIC 已经完全初始化好了，Linux 驱动**按需操作**。

### 1. 使能中断

驱动 `request_irq()` → 底层调 `irq_enable`：

```c
// drivers/irqchip/irq-gic-v3.c
static void gic_enable_irq(struct irq_data *d)
{
    u32 hwirq = d->hwirq;

    if (hwirq < 32) {
        /* SGI/PPI：写 GICR_ISENABLER0 (每核一份) */
        gic_write_isenabler0(hwirq);
    } else {
        /* SPI：写 GICD_ISENABLER<n> (全局共享) */
        gicd_write_isenabler(hwirq);
    }
}
```

### 2. 禁能中断

```c
static void gic_disable_irq(struct irq_data *d)
{
    if (hwirq < 32)
        gic_write_icenabler0(hwirq);   // GICR
    else
        gicd_write_icenabler(hwirq);    // GICD
}
```

### 3. 设置亲和性（绑核）

**只有 SPI 能设置亲和性**（SGI/PPI 是 per-CPU 的，不用设）：

```c
static int gic_set_affinity(struct irq_data *d,
                            const struct cpumask *mask_val,
                            bool force)
{
    u32 hwirq = d->hwirq;
    u64 mpidr = cpu_logical_map(cpu);  // 拿到目标核的 MPIDR

    /* 写 GICD_IROUTER< n > 寄存器，指定中断路由到哪个核 */
    gicd_write_irouter(hwirq, mpidr);

    return IRQ_SET_MASK_OK;
}
```

用户态也能改：

```bash
echo 2 > /proc/irq/34/smp_affinity   # 把34号中断绑到CPU1
```

---

## 三、一张表总结从核启动各阶段 GIC 做了什么

|阶段|在哪跑|谁的代码|GIC 操作|
|---|---|---|---|
|主核冷启动|EL3|ATF|`gicv3_distif_init` + 主核 `rdistif_init` + `cpuif_enable`|
|主核跳 EL1|EL1|Linux|`gic_dist_init` + `gic_cpu_init`（主核）|
|主核发 PSCI CPU_ON|EL1→EL3|Linux → ATF|发 SMC 陷 EL3|
|从核上电复位|EL3|ATF|走 warm reset 路径|
|**从核 GICR 初始化**|**EL3**|**ATF**|**`gicv3_rdistif_init(cpu)` + `gicv3_cpuif_enable(cpu)`**|
|从核跳 EL1|EL1|Linux|从核入口 `secondary_start_kernel`|
|从核 Linux GIC 初始化|EL1|Linux|`gic_cpu_init(cpu)`（重配优先级、PPI等）|
|驱动运行时|EL1|Linux 驱动|`request_irq` → 使能 / 设置亲和性 / 处理|

## 延伸方向

- PSCI 里从核的 entrypoint 是怎么设的
- Linux `gic_cpu_init` 里具体读改写了哪些寄存器
- CPU hotplug（热插拔）时 GIC 怎么处理
