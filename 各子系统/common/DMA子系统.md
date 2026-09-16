# DMA 子系统（dmaengine + DMA 映射 API）

> 两条线分清：① **dmaengine 框架**（驱动外挂 DMA 控制器搬运数据）② **DMA mapping API**（任何设备做 DMA 都要的地址翻译/一致性）。代码摘自本地内核树 6.12.58。

## 1. 全景分层

```
协议驱动          uart/spi/audio/net：dmaengine_prep_* + tx_submit + callback
                        │
dmaengine 核心    drivers/dma/dmaengine.c   通道分配/cookie 生命周期
                  drivers/dma/virt-dma.c    虚拟描述符队列（大多数控制器驱动基于它）
                        │ struct dma_chan / dma_device / dma_async_tx_descriptor
控制器驱动        pl330 / cadence / 厂商私有    真正写描述符寄存器、处理中断

────── 另一条线（所有 DMA 设备通用）──────
DMA mapping API   dma_map_single/page/sg / dma_sync_* / dma_alloc_coherent
                  + IOMMU（SMMU）: dma_addr(IOVA) --翻译--> phys
```

## 2. dmaengine 核心结构（源码摘录）

### `struct dma_chan` - 通道

```c
/* include/linux/dmaengine.h */
struct dma_chan {
	struct dma_device *device;
	struct device *slave;
	dma_cookie_t cookie;
	dma_cookie_t completed_cookie;
	int chan_id;
	struct dma_chan_dev *dev;
	const char *name;
	struct list_head device_node;
	int client_count;
	int table_count;
	/* DMA router */
	struct dma_router *router;
	void *route_data;
};
```

- 协议驱动不直接 new 通道，用 `dma_request_chan(dev, name)`（按 DT 的 `dmas = <...>` 名字拿）
- **cookie**：每个提交的描述符一个 cookie，`dma_async_is_complete/completed_cookie` 用于判断做到哪了

### `struct dma_slave_config` - slave 方向的硬件参数

```c
/* include/linux/dmaengine.h */
struct dma_slave_config {
	enum dma_transfer_direction direction;
	phys_addr_t src_addr;          /* 外设 FIFO 的物理地址 */
	phys_addr_t dst_addr;
	enum dma_slave_buswidth src_addr_width;   /* 1/2/4/8 字节 */
	enum dma_slave_buswidth dst_addr_width;
	u32 src_maxburst;              /* 一次 burst 的 beat 数 */
	u32 dst_maxburst;
	...
};
```

- 这就是"DMA 怎么访问外设"的合同：FIFO 地址、位宽、突发长度。**位宽×burst 配错 = 数据错位或 overrun**，bringup 高频坑

### `struct dma_async_tx_descriptor` - 一次传输

```c
/* include/linux/dmaengine.h */
struct dma_async_tx_descriptor {
	dma_cookie_t cookie;
	enum dma_ctrl_flags flags;
	dma_addr_t phys;
	struct dma_chan *chan;
	dma_cookie_t (*tx_submit)(struct dma_async_tx_descriptor *tx);
	int (*desc_free)(struct dma_async_tx_descriptor *tx);
	dma_async_tx_callback callback;          /* 完成回调 */
	dma_async_tx_callback_result callback_result;
	void *callback_param;
	...
};
```

## 3. 协议驱动标准用法（五步）

```c
struct dma_chan *chan = dma_request_chan(dev, "rx");
struct dma_slave_config cfg = {
    .direction = DMA_DEV_TO_MEM,
    .src_addr  = fifo_phys,
    .src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE,
    .src_maxburst   = 16,
};
dmaengine_slave_config(chan, &cfg);

/* 1. 准备描述符（方向决定用哪个 prep） */
struct dma_async_tx_descriptor *desc;
desc = dmaengine_prep_slave_single(chan, buf_dma, len,
        DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
/* 2. 挂完成回调 */
desc->callback = rx_done;
desc->callback_param = dev;
/* 3. 提交（进队列，不一定马上跑） */
dmaengine_submit(desc);
/* 4. 触发 */
dma_async_issue_pending(chan);
/* 5. 完成后中断 -> 驱动回调 -> upper layer */
```

prep 家族：`prep_slave_single`（单块）、`prep_slave_sg`（scatter-gather）、`prep_dma_cyclic`（环形/音频、周期回调）、`prep_interleaved`（2D/交错）、`prep_memcpy`（memory-to-memory）。

## 4. DMA mapping API（第二条线，面试更高频）

```c
/* 流式映射：驱动自管 buffer */
dma_addr_t daddr = dma_map_single(dev, buf, len, DMA_FROM_DEVICE);
if (dma_mapping_error(dev, daddr)) ...          /* 必查！ */
... 硬件用 daddr 做 DMA ...
dma_unmap_single(dev, daddr, len, DMA_FROM_DEVICE);

/* 一致性内存：CPU 与设备共享、硬件保证一致 */
void *vaddr = dma_alloc_coherent(dev, size, &daddr, GFP_KERNEL);

/* 非 coherent 平台上多次流式复用： */
dma_sync_single_for_device(dev, daddr, len, DMA_TO_DEVICE);  /* clean */
dma_sync_single_for_cpu(dev, daddr, len, DMA_FROM_DEVICE);   /* invalidate */
```

- 有 IOMMU/SMMU 时 `dma_addr` 是 **IOVA** 不是物理地址，翻译由 iommu-dma 层做
- `DMA_BIDIRECTIONAL` 是性能妥协（两次维护），能定向就定向
- 大 buffers 用 `dma_map_sg`；现代驱动首选 `dma_buf` attachment 接口（见简历 DMA-BUF IPC 项目）

## 5. 调试

| 手段 | 用法 |
|---|---|
| `/sys/class/dma/`、debugfs `dmaengine` | 通道状态、cookie、in-flight 描述符 |
| `dma-debug`（`dma_debug=on`） | 检查 unmap 泄漏、方向错误、映射越界 |
| swiotlb 统计 | `/sys/kernel/debug/swiotlb/`（bounce 用的多说明 IOMMU 映射有问题） |
| ftrace | `dma_map_single`/`dma_unmap` trace 点 |

常见错误模式：
- 忘 `dma_mapping_error` 检查 -> 拿着错误码当地址用
- CPU 访问 mapped buffer 没 sync -> 数据陈旧/脏（非一致平台）
- `dma_unmap` 后继续用 daddr -> use-after-unmap，dma-debug 能抓

## 6. 面试问答

**Q: dmaengine 和 DMA mapping API 什么关系？**
dmaengine 是"借用通用 DMA 控制器搬运"的框架（谁搬运）；mapping API 是"设备自己做 DMA 时地址怎么翻译、缓存怎么维护"（怎么寻址）。协议驱动两个都碰。

**Q: dma_alloc_coherent 和 dma_map_single 区别？**
coherent：分配+映射+硬件一致，长期共享控制流；map_single：流式，短生命周期，方向明确，非一致平台需要 sync。coherent 贵且占专属区域，按帧流式数据用 map/sg。

**Q: cookie 机制解决什么？**
异步完成的进度追踪：提交递增，硬件每完成一个描述符推进 completed_cookie，驱动据此判断"我提交的第 N 笔做完没"。

**Q: cyclic 和 sg 什么时候用？**
cyclic：环形音频/采集 buffer，周期中断回填（无界连续流）；sg：一帧多个物理不连续块（如 vb2 buffer）。

**Q: 有 SMMU 时 dma_addr 是物理地址吗？**
不是，是 IOVA；SMMU 页表负责 IOVA->PA。这也是"页表泄漏 -> dma_map 失败"链路（对应简历 SMMU 页表泄漏治理）。
