# SCMI 子系统学习指南

> **一句话总结**：SCMI（System Control and Management Interface）是 ARM 定义的一套标准化 RPC 协议，让 Linux（AP）通过 mailbox + 共享内存向独立的 SCP/PM 微控制器请求时钟、电源、DVFS、传感器等系统服务，是现代 SoC 电源管理的核心基础设施。

---

## 📋 目录

- [SCMI 基础概念](#scmi-基础概念)
- [四层架构模型](#四层架构模型)
- [核心数据结构 `scmi_xfer`](#核心数据结构-scmi_xfer-原子货币)
- [完整调用数据流](#完整调用数据流)
- [关键文件解读](#关键文件解读)
- [调试与问题定位](#调试与问题定位)
- [CIX 平台定制点](#cix-平台定制点)
- [面试要点总结](#面试要点总结)

---

## SCMI 基础概念

### 为什么需要 SCMI？

现代 SoC 上，时钟、电压、功耗、传感器、复位这些"系统级资源"**不归 Linux 直接管**，而是交给一颗专门的小核（**SCP / PM**）统一管理：

```
┌───────────────────────── AP (Linux) ─────────────────────────┐
│  cpufreq  clk  regulator  thermal  reset ... 各内核子系统      │
│      │       │        │         │       │                      │
│      └───────┴────────┴─────────┴───────┘                      │
│                    SCMI 协议层                                    │
│                          │                                       │
│                  Transport（mailbox / SMC / virtio）            │
└──────────────────────────┼───────────────────────────────────────┘
                           │ 共享内存 + doorbell
┌──────────────────────────┼──────────────────────────────────────────┐
│                    SCP / PM 固件（另一颗核）                           │
│   真正去操作 PLL、电源开关、ADC、复位线                                  │
└───────────────────────────────────────────────────────────────────────┘
```

Linux 想调频、开时钟、读温度，就向 SCP 发 SCMI 请求。

### 核心设计思想

**分层 + 解耦**：每一层只和相邻层打交道，通过函数指针抽象。

```
消费者（cpufreq/clk）→ 协议层 → 核心层 → 传输层 → SCP
```

好处：
- 换传输层（如从 mailbox 换成 SMC），上面三层完全不用改
- 加新协议（如 Powercap），下面三层完全不用改
- 不同 SoC 厂商都可以复用这套标准框架

---

## 四层架构模型

```
① 消费者层   cpufreq / clk / regulator / thermal / reset
             （这些是"用户"，不在 arm_scmi/ 目录）
─────────────────────────────────────────────────────
② 协议层      clock.c power.c perf.c sensors.c reset.c
              voltage.c powercap.c system.c base.c
              （每个 SCMI 协议一个文件）
─────────────────────────────────────────────────────
③ 核心 / 总线层  driver.c（核心引擎） bus.c（scmi_bus）
               notify.c（通知） common.h protocols.h
─────────────────────────────────────────────────────
④ 传输层      mailbox.c / smc.c / optee.c / virtio.c
              shmem.c（共享内存读写） msg.c
```

### 各层职责详解

| 层 | 核心能力 | 关键接口 |
|----|----------|----------|
| **消费者层** | 使用标准内核 API，完全不知道 SCMI 存在 | `clk_set_rate()` / `cpufreq_driver_target()` |
| **协议层** | 每个协议实现自己的命令/响应编码解码 | `scmi_clock_rate_get()` / `scmi_perf_level_set()` |
| **核心层** | xfer 生命周期管理、token 分配、等待完成、中断回调 | `do_xfer()` / `scmi_rx_callback()` |
| **传输层** | 把 xfer 写到共享内存、敲门铃、从硬件读响应 | `send_message()` / `fetch_response()` |

### 协议 ID 速查

| 协议 ID | 名称 | 文件 | 用途 |
|---------|------|------|------|
| 0x10 | Base | base.c | 发现 SCP 支持哪些协议、版本 |
| 0x11 | Power Domain | power.c | 电源域开关 |
| 0x13 | Powercap | powercap.c | 功耗封顶 |
| 0x14 | Clock | clock.c | 时钟开关/调频（DP 调试时卡住的就是它） |
| 0x15 | Sensor | sensors.c | 温度/电压传感器 |
| 0x16 | Reset | reset.c | 复位控制 |
| 0x17 | Voltage | voltage.c | 电压域 |
| 0x18 | System Power | system.c | 关机/重启 |

> **注意**：Perf (DVFS) 协议通常也用 0x13，和 Powercap 区分看具体命令 ID。

---

## 核心数据结构：`scmi_xfer`（原子货币）

`struct scmi_xfer` 是整个 SCMI 的"一次事务的完整生命体"。它是协议层、核心层、传输层之间传递的**唯一载体**。

### 结构拆解（protocols.h）

```c
struct scmi_xfer {
    int transfer_id;            // 全局唯一递增 ID，仅用于 debug/trace

    struct scmi_msg_hdr hdr;    // 【消息头】协议号/消息号/类型/seq(token)/status
    struct scmi_msg tx;         // 【发送缓冲】 { void *buf; size_t len; }
    struct scmi_msg rx;         // 【接收缓冲】 可与 tx 复用同一块 buf

    struct completion done;     // 【同步原语】等待者睡这里，中断 complete 它
    struct completion *async_done;  // 异步延迟响应专用

    bool pending;               // 是否已挂进 pending_xfers 哈希表
    struct hlist_node node;     // 复用：要么挂 free_xfers，要么挂 pending_xfers

    refcount_t users;           // 引用计数：防 TX 超时与 RX 处理并发时被提前释放
    atomic_t busy;              // RESP/DRESP 并发到达时的独占写标志
    int state;                  // 状态机：SENT_OK → RESP_OK → DRESP_OK
    int flags;                  // IS_RAW / CHAN_SET
    spinlock_t lock;            // 保护 state 和 busy

    void *priv;                 // 传输层私有数据挂载点
};
```

### `scmi_xfer` 同时扮演四个角色

1. **数据容器**：`tx.buf` 装请求、`rx.buf` 装响应。协议层往 `tx.buf` 填参数，从 `rx.buf` 解析结果。

2. **身份标识**：`hdr.seq` (token) 是 SCP 乱序回复时反查的唯一钥匙。SCP 响应里带回这个 seq，`scmi_handle_response` 用它在 `pending_xfers` 哈希表里反查是哪条 xfer。

3. **同步原语**：`done` 这个 completion 是发送方睡眠、中断方唤醒的**会合点**。300ms 超时本质就是 `wait_for_completion_timeout(&xfer->done)` 没等到。

4. **并发保护**：`state` / `busy` / `users` / `lock` 四件套，保护它在 TX 超时与 RX 到达的竞态中不被错误释放或重复处理。

### 对象池设计

`scmi_xfer` 在 `__scmi_xfer_info_init` 里**预分配**好（max_msg 个），挂在 `free_xfers` 链表上。

热路径上发命令只是从池里摘一个、用完还回去——**零分配开销**，这对频繁的 DVFS/clock 调用至关重要。

---

## 完整调用数据流

### 发送路径（以 `scmi_clock_rate_get` 为例）

```
clk_get_rate()                         ← 消费者层（DP 驱动）
  scmi_clk_recalc_rate()               ← clk-scmi provider 回调
    clk_ops->rate_get
      scmi_clock_rate_get()            ← 协议层 clock.c
        ├─ xfer_get_init(CLOCK_RATE_GET)  // 从池里拿 xfer，填 header
        ├─ put_unaligned_le32(clk_id, xfer->tx.buf)  // 填参数
        └─ do_xfer(ph, xfer)           ← 核心层 driver.c
             ├─ reinit_completion(&xfer->done)
             ├─ xfer->state = SCMI_XFER_SENT_OK
             ├─ smp_mb()                // 屏障：防 RX 早到看到旧 state
             ├─ info->desc->ops->send_message()
             │   └─ mailbox_send_message() → mailbox 层
             │       ├─ shmem_tx_prepare()  // 写命令进 shmem，置 BUSY
             │       └─ mbox_send_message() → cix_mbox 敲门铃
             │           → cix_mbox_send_data_db(): 写 0x80=0x1
             └─ scmi_wait_for_message_response()
                 └─ wait_for_completion_timeout(&done, 300ms)  // 睡等
```

### 接收路径（中断回调）

```
SCP 写响应进 shmem → 置 channel FREE → 敲门铃
  ↓
GIC 中断路由给 CPU
  ↓
gic_handle_irq() → cix_mbox_isr() → cix_mbox_isr_db() （读 0xc8）
  ↓
mbox_chan_received_data()
  ↓
rx_callback()  (mailbox.c)  // 这里有 spurious A2P IRQ 守卫
  ↓
scmi_rx_callback()  (driver.c)
  ↓
scmi_handle_response()
  ├─ scmi_xfer_command_acquire()  // 按 seq 反查 xfer + 状态校验
  ├─ fetch_response() → shmem_fetch_response()  // 从 SHMEM 读响应
  └─ complete(&xfer->done)   // ← 唤醒发送方！
```

> **超时根因**：`complete()` 没被调用。要么 SCP 没回（硬件问题），要么中断没进来（GIC 路由问题），要么进来了但中间某层丢了。

### 状态机变化

```
scmi_xfer_get_init() → xfer 从 free_xfers 摘下
      ↓
do_xfer() → SCMI_XFER_SENT_OK
      ↓
wait_for_completion_timeout()  睡
      ↓
[ 中断来了 ]
      ↓
scmi_handle_response() → SCMI_XFER_RESP_OK
      ↓
complete(&xfer->done) → 唤醒
      ↓
xfer_put() → 归还 free_xfers
```

---

## 关键文件解读

### 1. common.h —— 先读，建立词汇表

所有核心结构体都在这里：

- `struct scmi_xfer` —— 一次事务
- `struct scmi_desc` —— 一个 transport 的描述（max_msg、max_msg_size、ops）
- `struct scmi_transport_ops` —— transport 必须实现的回调
  ```c
  struct scmi_transport_ops {
      int (*send_message)(struct scmi_chan_info *cinfo, struct scmi_xfer *xfer);
      int (*fetch_response)(struct scmi_chan_info *cinfo, struct scmi_xfer *xfer);
      int (*poll_done)(struct scmi_chan_info *cinfo, struct scmi_xfer *xfer);
      ...
  };
  ```
- `MSG_*` 宏：msg_header 的位段打包/解包

### 2. driver.c —— 核心引擎（最重要）

整个子系统的心脏，几个关键函数：

| 函数 | 作用 |
|------|------|
| `scmi_probe()` | 探测 SCP，初始化 transport，建 base 协议 |
| `scmi_xfer_get_init()` | 从 xfer 池分配一个事务并初始化 header |
| `do_xfer()` | **发送 + 等待应答**的主流程（超时的地方） |
| `scmi_wait_for_reply()` | 等待逻辑（poll 或中断两条路径） |
| `scmi_rx_callback()` | transport 收到应答时回调它 → complete(done) |
| `xfer_put()` | 归还事务到池 |

### 3. shmem.c —— SMT 共享内存读写

实现 SCMI 规范的 Shared Memory Transport 布局：

- `shmem_tx_prepare()`：把 xfer 写进 SHMEM（设 header/length/payload，清 CHANNEL_FREE）
- `shmem_read_header()` / `shmem_fetch_response()`：从 SHMEM 读回应答
- `shmem_poll_done()`：轮询模式判断 SCP 是否回完

SHMEM 布局（CIX 定制，带 pm_header）：
```
| 32-bit header | payload | 32-bit status | 32-bit trace | token |
```

### 4. mailbox.c —— 把 SMT 接到 CIX mailbox

实现 `scmi_transport_ops`，把 shmem 操作和 cix-mailbox.c 的 mbox client 接口缝起来：

- `mailbox_send_message()` → `shmem_tx_prepare()` + `mbox_send_message()`（触发 doorbell）
- `rx_callback`（mbox 回调）→ `scmi_rx_callback()`（通知 driver.c 应答到了）
- `mailbox_fetch_response()` → `shmem_fetch_response()`

> 这就是 SCMI 核心和 CIX mailbox 的**接缝点**。

### 5. clock.c —— 一个协议的范本

读懂一个协议，其它都同理：

- `scmi_clock_protocol_init()`：向 SCP 查询有几个 clock（`CLOCK_ATTRIBUTES`）
- `scmi_clock_rate_get()` / `scmi_clock_rate_set()` / `scmi_clock_enable()`：每个对应一条 message_id，内部都走 `xfer_get_init → do_xfer`
- 末尾 `DEFINE_SCMI_PROTOCOL_REGISTER_UNREGISTER` 把自己注册进核心

---

## 调试与问题定位

### 最常见问题：超时

```
arm-scmi base: request timed out
```

**按这个顺序排查**：

1. **门铃有没有发出去？**
   - 在 `cix_mbox_send_data_db()` 加 print，确认写了寄存器
   - 读回寄存器值确认写进去了

2. **SCP 有没有收到？**
   - 读 SHMEM 的 channel status 位，看 SCP 是否清 BUSY
   - 可用 `dump_status`（mailbox.c 里加的）回读确认

3. **中断有没有进来？**
   - `cat /proc/interrupts | grep mailbox` 看计数涨不涨
   - 在 `cix_mbox_isr_db()` 入口加计数

4. **中断到了有没有走到 complete()？**
   - 在 `scmi_rx_callback()` 加 print
   - 在 `complete(&xfer->done)` 前加计数

### 零开销观测手段（不扰动时序）

> **为什么不能用 printk**：`pr_err`/`printk` 到串口是**同步**的，boot 阶段 115200 波特率下，一行约 80 字符 ≈ **7ms**，dump 一次 3~4 行 ≈ 20~30ms。更关键：如果在 ISR 或热路径里加了 print，串口耗时会**直接挤占** SCMI 完成中断的处理时机，出现"加了 print 反而 PM 看起来正常响应了"的 **Heisenbug（观察行为改变了被观察对象）**--print 拖慢了整体节奏，把原本紧张的时序"撑松"了。所以"PM 给了门铃响应"可能是 print 副作用，不能下结论；**写内存 ring buffer（零格式化、纳秒级）再事后/panic 时回读**才是不扰动时序的观测法。

#### 方法 1：ftrace tracepoint（推荐）

内核内置 tracepoint，零串口开销：

```bash
echo 1 > /sys/kernel/debug/tracing/events/scmi/enable
cat /sys/kernel/debug/tracing/trace_pipe
```

会看到：
- `scmi_xfer_begin` —— 发送开始
- `scmi_xfer_end` —— 收到响应完成
- `scmi_rx_done` —— RX 回调被调用

> 超时的那个 seq 如果有 `scmi_rx_done` 但没 `scmi_xfer_end` = 核心层处理丢了。如果连 `scmi_rx_done` 都没有 = 中断没进来。

#### 方法 2：内存 ring buffer

如果 print 扰动时序，自己实现一个静态环形缓冲区：

```c
// 只写内存，不打串口
static void trace_record(int seq, u32 status, ktime_t time)
{
    struct trace_entry *e = &ring_buf[next++];
    e->seq = seq;
    e->status = status;
    e->time = time;
}
```

panic 或事后通过 debugfs 回读。

### 三种等待策略对比

| 模式 | 条件 | 机制 | 适用场景 |
|------|------|------|---------|
| **睡眠等待** | 普通同步命令 | `wait_for_completion_timeout`，靠中断唤醒 | 99% 情况 |
| **轮询** | `poll_completion` 且传输支持 | `spin_until_cond(poll_done)` 忙等 shmem 状态 | 中断有问题时的降级方案 |
| **异步** | `do_xfer_with_response` | 先同步 ACK，再等一条 Delayed Response | 长耗时命令 |

---

## CIX 平台定制点

### 1. SHMEM 布局扩展

CIX 在标准 SCMI SHMEM 布局基础上加了 PM 专用字段：

```c
#ifdef CONFIG_PM_EXCEPTION_PROTOCOL
struct scmi_shared_mem {
    __le32 reserved;          // 标准 header
    __le32 length;
    u8 msg_header[];
    __le32 statusCode;        // CIX 加的
    __le32 traceCode;         // CIX 加的
    __le32 pm_header;         // CIX 加的，带 token
    /* ... */
};
#endif
```

### 2. 定制协议 0x81

PM exception 协议，物理上放在 `drivers/soc/cix/`，但本质还是 SCMI 协议：

```c
ret = ph->xops->xfer_get_init(ph, EXCEPTION_GET, 0, sizeof(*exception_info), &t);
exception_info = t->rx.buf;
ret = ph->xops->do_xfer(ph, t);
// ... 解析 exception_info ...
ph->xops->xfer_put(ph, t);
```

### 3. CIX mailbox 门铃硬件

`drivers/mailbox/cix-mailbox.c`:

| 寄存器 | 地址 | 作用 |
|--------|------|------|
| INT_STATUS | 0xc8 | 读 pending 中断位 |
| INT_CLEAR | 0xc4 | 写 1 清中断（W1C） |
| REG_DB_ACK | 0x80 | 写 1 敲门铃 / 回 ACK |

11 个通道共用一组寄存器，靠 bit 位区分。

---

## 面试要点总结

### 1. 架构必背

| 问题 | 标准回答 |
|------|----------|
| **SCMI 是什么？** | ARM 定义的系统控制管理接口，AP 通过 mailbox + 共享内存向 SCP 请求时钟/电源/DVFS 等服务的标准化 RPC 协议 |
| **四层模型？** | 消费者层（cpufreq/clk）→ 协议层（clock/perf）→ 核心层（driver.c, xfer 管理）→ 传输层（mailbox/shmem） |
| **为什么分层？** | 解耦。换传输层上面不用改，加新协议下面不用改。不同 SoC 厂商都可以复用标准框架。 |

### 2. `scmi_xfer` 深度理解

| 问题 | 要点 |
|------|------|
| **scmi_xfer 是什么？** | 一次 SCMI 事务的完整生命体，同时是数据容器、身份标识、同步原语、并发保护四合一 |
| **为什么用对象池？** | 热路径零分配开销，DVFS/clock 调用频繁，kmalloc 太慢 |
| **token/seq 机制？** | 单调递增分配，SCP 响应带回，用于乱序时反查 pending xfer。单调是为了避免刚超时的旧 token 被立刻复用导致匹配错误。 |
| **completion 的作用？** | 发送方和中断方的会合点。发送方睡在上面，中断方唤醒它。超时就是没被唤醒。 |

### 3. 超时排查经验

> **面试官最爱问**："SCMI 命令超时了，你怎么查？"

标准回答框架：
1. **先看发送**：门铃有没有发出去？读 mailbox 寄存器确认
2. **再看 SHMEM**：SCP 有没有清 BUSY 位？写回响应了吗？
3. **再看中断**：`/proc/interrupts` 计数涨不涨？ISR 进不进？
4. **最后看核心**：`scmi_rx_callback()` 有没有被调用？`complete()` 有没有执行？

> **加分项**：强调 printk 会扰动时序，用 ftrace tracepoint 或内存 ring buffer 观测。

### 4. Xen 下的特殊点

Xen bringup 时 SCMI 特别容易出问题：

- **中断路由**：mailbox 中断必须被 Xen 正确路由给 dom0
- **P2M 映射**：SHMEM 物理地址必须在 dom0 可访问，不能被 reserved-memory 覆盖
- **SMMU**：如果 mailbox controller 在 SMMU 后面，bypass 要设对
- **SMC 转发**：有些平台用 SMC 做 transport（不是 mailbox），需要 Xen 转发 SiP SMC

---

**文档版本**: v1.0
**最后更新**: 2026-07-22
