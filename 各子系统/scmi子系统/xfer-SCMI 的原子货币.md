# xfer：SCMI 的原子货币

`xfer` 是整个 SCMI 的"原子货币"，必须吃透。
## 一、`xfer` 是如何定义的

`struct scmi_xfer` 定义在 protocols.h,它由**三个子结构**拼成,逐字段拆解:

```c
struct scmi_xfer {
	int transfer_id;            // 全局唯一递增 ID,仅用于 debug/trace/profiling
	struct scmi_msg_hdr hdr;    // 【消息头】协议号/消息号/类型/seq(token)/status
	struct scmi_msg tx;         // 【发送缓冲】{void *buf; size_t len;}
	struct scmi_msg rx;         // 【接收缓冲】可与 tx 复用同一块 buf
	struct completion done;     // 同步响应的"门铃":等待者睡这里,中断 complete 它
	struct completion *async_done; // 异步延迟响应(do_xfer_with_response)专用
	bool pending;               // 是否已挂进 pending_xfers 哈希表
	struct hlist_node node;     // 复用:要么挂 free_xfers,要么挂 pending_xfers
	refcount_t users;           // 引用计数:防 TX 超时与 RX 处理并发时被提前释放
	atomic_t busy;              // RESP/DRESP 并发到达时的独占写标志
	int state;                  // 状态机:SENT_OK → RESP_OK → DRESP_OK
	int flags;                  // IS_RAW / CHAN_SET
	spinlock_t lock;            // 保护 state 和 busy
	void *priv;                 // 传输层私有数据挂载点
};
```

三个子结构各管一摊(都在 protocols.h):

| 子结构            | 职责                         | 关键字段                                     |
| -------------- | -------------------------- | ---------------------------------------- |
| `scmi_msg_hdr` | **协议语义**(会被打包成 32 位发给 SCP) | `id`/`protocol_id`/`type`/`seq`/`status` |
| `scmi_msg tx`  | **要发的字节**                  | `buf` + `len`                            |
| `scmi_msg rx`  | **收到的字节**                  | `buf` + `len`                            |

`hdr` 里的前几个字段最终会被 `pack_scmi_header()`(common.h)压成一个 `u32` 写进 shmem 头部——这就是 AP 和 SCP 之间真正"上线"的那 4 个字节。

## 二、`xfer` 在 SCMI 中的意义

一句话:**`xfer` = 一次 SCMI 事务(transaction)的完整生命体**。它是协议层、核心层、传输层之间传递的**唯一载体**,贯穿"发-等-收"全过程。

它同时扮演四个角色:

1. **数据容器**:`tx.buf` 装请求、`rx.buf` 装响应。协议层往 `tx.buf` 填参数,从 `rx.buf` 解析结果。
2. **身份标识**:`hdr.seq`(token)是 SCP 乱序回复时反查的唯一钥匙(`pending_xfers` 哈希表的 key)。
3. **同步原语**:`done` 这个 completion 是 (B) 发送方睡眠、(C) 中断方唤醒的**会合点**。300ms 超时,本质就是 `wait_for_completion_timeout(&xfer->done)` 没等到。
4. **并发状态机**:`state`/`busy`/`users`/`lock` 四件套,保护它在 TX 超时与 RX 到达的竞态中不被错误释放或重复处理。

为什么用"对象池"而非每次 kmalloc:`xfer` 在 `__scmi_xfer_info_init` 里**预分配**好(`max_msg` 个),挂在 `free_xfers`。热路径上发命令只是从池里摘一个、用完还回去——**零分配开销**,这对频繁的 DVFS/clock 调用至关重要。

## 三、其他子系统是否用到——关键结论

全树扫描 `struct scmi_xfer` 的引用(60+ 处),结论分两层:

### 1. 通用内核子系统:完全不接触 `xfer`(刻意的封装)

cpufreq / clk / regulator / thermal / hwmon 这些**消费者子系统,从不知道 `scmi_xfer` 的存在**。它们看到的是被解析好的高层数据。封装边界是 `scmi_xfer_ops`(protocols.h):

```
clk 子系统  →  clk_scmi 驱动  →  ph->xops->do_xfer(ph, t)  →  [xfer 在这条线以下才出现]
                                  ↑ scmi_xfer 在这层第一次被创建,也在这层被销毁
```

也就是说 `xfer` 的可见范围**严格锁死在 SCMI 协议栈内部**:核心层(driver.c)、各协议(clock.c/perf.c/voltage.c/reset.c/...)、各传输(mailbox.c/smc.c/virtio.c/shmem.c/msg.c)。一旦数据要交给 clk/cpufreq,早已被协议层从 `rx.buf` 解析成 `u64 rate`、`int level` 之类的普通值了。

**这是好的分层设计**:消费者只关心"给我设个频率",不关心 token、shmem、completion 这些传输细节。

### 2. 唯一的"外部"使用者:CIX 定制协议

全树扫描里,`drivers/firmware/arm_scmi/` 之外**只有一个**文件碰了 `scmi_xfer`:

pm_exception_protocol.c —— `CONFIG_PM_EXCEPTION_PROTOCOL`(协议 0x81)。它虽然物理上放在 `drivers/soc/cix/`,但**本质仍是一个 SCMI 协议实现**,用的还是那套标准范式:

```c
ret = ph->xops->xfer_get_init(ph, EXCEPTION_GET, 0, sizeof(*exception_info), &t);
exception_info = t->rx.buf;          // 拿 rx 缓冲
ret = ph->xops->do_xfer(ph, t);      // 发+等
// ... 解析 exception_info ...
ph->xops->xfer_put(ph, t);           // 还回池子
```

这恰好**印证了规则**:能直接操作 `xfer` 的,必须是 SCMI 协议层的一员(持有 `scmi_protocol_handle`),哪怕它被 CIX 挪到了 vendor 目录。它不是"另一个子系统",而是"SCMI 协议栈伸到 vendor 目录的一只手"。

## 横向对比(面试可能追问)

`xfer` 这种"一次事务对象"是内核**消息式子系统的通用设计母题**,你可以类比记忆:

| 子系统 | 事务对象 | 与 `scmi_xfer` 的相似点 |
|---|---|---|
| SPI | `struct spi_transfer` / `spi_message` | tx/rx buf + 完成回调 |
| I2C | `struct i2c_msg` | buf + len + 方向标志 |
| USB | `struct urb` | buf + completion + 状态机 + 引用计数 |
| mailbox | `struct mbox_client` 的发送上下文 | 异步完成通知 |
| virtio | `struct virtqueue` 的 buffer 描述符 | 环形缓冲 + token 反查 |

`scmi_xfer` 的独特之处是把 **token 乱序反查 + 双完成量(同步 done / 异步 async_done)+ 引用计数防竞态** 三件事捏在一个对象里,这是 SCMI"允许乱序、允许延迟响应"的协议特性逼出来的复杂度。


## 延伸：协议层如何驾驭 xfer 的最佳样本

看 clock.c 里 `rate_get` 的完整 `xfer_get_init -> 填 tx.buf -> do_xfer -> 解析 rx.buf` 范例，顺着 `scmi_xfer_ops` 这条封装边界走一遍。
