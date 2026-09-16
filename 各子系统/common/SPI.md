# SPI 子系统

以 Linux 6.12（`drivers/spi/spi.c` + `include/linux/spi/spi.h`）源码为例，从硬件到内核分层整理。代码均摘自本地内核树 `cix_6.12_master_Dev/linux`。

## 1. 硬件基础

### 物理层：四根线，全双工

```
    SCLK ───────────────────────────┐（主机驱动）
    MOSI ──────主机出主机入─────────┤（主机 -> 从机）
    MISO ──────从机出主机入─────────┤（从机 -> 主机）
    CS/n ──────每从机一根（或译码器）┘（主机拉低选中）
```

- **同步**串行：时钟由主机产生，收发同步进行（对比 UART 异步、I2C 半双工）
- **全双工**：MOSI/MISO 独立，一次传输同时完成收发
- **没有应答机制**：不像 I2C 有 ACK，SPI 发出去不保证对端收到--可靠性靠协议层（CRC/回读校验）
- 一个控制器（`spi_controller`）可挂多个从机，靠独立 CS 区分

### 四种工作模式（CPOL/CPHA）

| Mode | CPOL | CPHA | 含义 |
|---|---|---|---|
| 0 | 0 | 0 | 空闲低电平，第 1 个边沿采样 |
| 1 | 0 | 1 | 空闲低电平，第 2 个边沿采样 |
| 2 | 1 | 0 | 空闲高电平，第 1 个边沿采样 |
| 3 | 1 | 1 | 空闲高电平，第 2 个边沿采样 |

- **CPOL**：空闲时时钟极性；**CPHA**：第几个边沿采样
- 驱动里对应 `spi_device.mode` 的 `SPI_CPOL` / `SPI_CPHA` 位
- 面试高频：数据手册说 "SPI mode 0" 就是 CPOL=0、CPHA=0；**两边模式不一致是首查项**（现象：读到的数据移位/全 0/全 FF）

### 对比 I2C

| | I2C | SPI |
|---|---|---|
| 线数 | 2（SCL/SDA） | 4+（含每从机一根 CS） |
| 双工 | 半双工 | 全双工 |
| 应答 | 有 ACK | 无 |
| 寻址 | 7-bit 地址总线广播 | 硬件片选 |
| 速率 | 100k/400k/1M/3.4M | 几十 MHz 起步 |
| 典型用途 | 低速配置类（RTC/EEPROM/sensor 寄存器） | 高吞吐（Flash/屏/ADC） |

## 2. 内核分层与三个核心结构

```
用户态            spidev（用户态测试口 /dev/spidevB.C）
                     │
协议驱动层        spi_driver（外设驱动：Flash/sensor/屏）── 用 spi_sync/spi_write 收发
                     │  spi_transfer / spi_message（传输描述）
核心层            drivers/spi/spi.c（排队、优化、统计、resume 检查）
                     │  spi_controller + controller->transfer_one
控制器驱动层      spi-cadence.c / spi-pl022.c / spi-imx.c（真实搬数：FIFO/POLL/DMA）
```

定义在 `include/linux/spi/spi.h`（源码摘录，6.12.58）：

### `struct spi_driver` - 外设协议驱动

```c
struct spi_driver {
	const struct spi_device_id *id_table;
	int			(*probe)(struct spi_device *spi);
	void			(*remove)(struct spi_device *spi);
	void			(*shutdown)(struct spi_device *spi);
	struct device_driver	driver;
};
```

- 总线模型：`spi_bus_type` 上 device（`spi_device`）与 driver 按 `of_match_table`/`id_table` 匹配，匹配上调 probe--与 platform/i2c 完全同构

### `struct spi_transfer` - 一次传输

```c
struct spi_transfer {
	const void	*tx_buf;
	void		*rx_buf;
	unsigned	len;
	...
	unsigned	dummy_data:1;
	unsigned	cs_off:1;
	unsigned	cs_change:1;	/* 该 transfer 结束后拉高 CS */
	unsigned	tx_nbits:4;
	unsigned	rx_nbits:4;
	...
	dma_addr_t	tx_dma;
	dma_addr_t	rx_dma;
};
```

- 全双工语义：tx_buf/rx_buf 可只给一个（只读/只写），但控制器层面**时钟是照常发的**--只读时发 dummy
- `cs_change`：message 中途抬起片选（某些器件要求"命令段结束-重启时序"）
- `len` 单位字节；位宽由 `bits_per_word`（spi_device 或 transfer 级）决定

### `struct spi_message` - 一组传输的容器

```c
struct spi_message {
	struct list_head	transfers;
	struct spi_device	*spi;
	...
	/* 完成回调（spi_async 用） */
	void			(*complete)(void *context);
	void			*context;
	unsigned		frame_length;
	unsigned		actual_length;
	int			status;
};
```

- 一个 message 内 CS 保持有效（除非 transfer 带 cs_change），从机看到的原子操作单元
- `actual_length`：完成后回填的实际传输字节数

## 3. 传输 API 与核心路径

### 驱动侧标准写法

```c
/* 只写 */
u8 cmd = CMD_READ_STATUS;
spi_write(spi, &cmd, 1);

/* 组合传输：写寄存器地址 + 读数据（一条 message 两个 transfer，CS 不断） */
struct spi_transfer xfers[2] = {};
struct spi_message m;
u8 tx[1] = { reg }, rx[2];

xfers[0].tx_buf = tx;  xfers[0].len = 1;
xfers[1].rx_buf = rx;  xfers[1].len = 2;

spi_message_init(&m);
spi_message_add_tail(&xfers[0], &m);
spi_message_add_tail(&xfers[1], &m);
spi_sync(spi, &m);     /* 阻塞等完成 */
```

### `__spi_sync` 核心路径（`drivers/spi/spi.c`）

```c
static int __spi_sync(struct spi_device *spi, struct spi_message *message)
{
	DECLARE_COMPLETION_ONSTACK(done);
	...
	if (__spi_check_suspended(ctlr)) {
		dev_warn_once(&spi->dev, "Attempted to sync while suspend\n");
		return -ESHUTDOWN;
	}

	status = spi_maybe_optimize_message(spi, message);
	...
	if (READ_ONCE(ctlr->queue_empty) && !ctlr->must_async) {
		message->actual_length = 0;
		message->status = -EINPROGRESS;
		trace_spi_message_submit(message);
		...
```

读点（面试讲）：
- **suspend 保护**：suspend 中访问直接 `-ESHUTDOWN`，不是挂死
- **快路径**：队列空时当前上下文直接调用控制器驱动，绕过 kthread，降延迟
- `spi_sync` 本质 = `spi_async` + `wait_for_completion`，且**同一消息不能在两个上下文并发**

### 控制器驱动要实现的（`struct spi_controller` 关键回调）

| 回调 | 作用 |
|---|---|
| `transfer_one(spi, xfer)` | 传输单个 transfer（现代接口，核心层帮你组队列） |
| `max_transfer_size` / `max_dma_len` | 分段上限 |
| `set_cs` | 片选控制（时序敏感器件在这里加延迟） |
| `optimize_message` / `prepare_message` | message 级预处理（如 DMA 映射 sg） |

## 4. 设备描述（Device Tree）

```dts
spi@f0100000 {
    compatible = "cdns,qspi";        /* 控制器 */
    #address-cells = <1>;
    #size-cells = <0>;

    flash@0 {
        compatible = "jedec,spi-nor";
        reg = <0>;                    /* 片选 0 */
        spi-max-frequency = <50000000>;
        spi-cpol; spi-cpha;           /* mode 3 */
        spi-rx-bus-width = <4>;       /* Quad 读 */
    };
};
```

- `reg` = CS 编号；`spi-max-frequency` 必填（默认会拒绝无上限设备）
- 双/四线（`spi-{rx,tx}-bus-width`）对应 `SPI_NBITS_*`

## 5. 调试

| 手段 | 用法 |
|---|---|
| sysfs | `/sys/bus/spi/devices/spiB.C/`、`modalias`、`statistics/`（传输计数/错误数/bytes） |
| spidev | DT 里挂 spidev 节点，用户态 `write()/read()/ioctl(SPI_IOC_MESSAGE_n)` 直接发原始波形 |
| ftrace | `trace_spi_message_submit/done`、`trace_spi_transfer_done` |
| 逻辑分析仪 | 模式不匹配/相位偏移这类"软件看起来都对"的问题只有波形能定论 |

## 6. 面试问答

**Q: SPI 和 I2C 怎么选？**
吞吐高、引脚够 -> SPI；引脚紧张、多从机、低速配置 -> I2C。SPI 无 ACK，可靠性要靠协议层校验。

**Q: spi_sync 和 spi_async 区别？能原子上下文用吗？**
sync = async + completion 等待；都不能在原子上下文用（可能走 DMA/睡眠）。中断上下文要用 `spi_async` + 回调。

**Q: 只读为什么还要 tx_buf？**
SPI 是时钟驱动的全双工移位，主机不发货不了时钟；只读时核心层自动补 dummy（`dummy_data`）。

**Q: 读回来全是 0x00 或 0xFF 说明什么？**
0xFF 常见 CS 没选中/MISO 悬空（上拉）；0x00 常见 CPOL/CPHA 不匹配或位宽错。先用逻辑分析仪看第一笔。

**Q: DMA 什么时候介入？**
长度超过阈值（控制器驱动自定，常见 32/64 字节）且缓冲区 dma 映射可用；短传输走 FIFO 轮询反而更快（省映射开销）。
