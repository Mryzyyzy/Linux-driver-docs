# UART 子系统（tty/serial_core）

以 Linux 6.12（`drivers/tty/serial/serial_core.c` + `include/linux/serial_core.h`）源码为例整理。代码摘自本地内核树 `cix_6.12_master_Dev/linux`，示例驱动用 pl011（ARM 经典 IP，也是 CIX Sky1 用的）。

## 1. 硬件基础

### 异步串行帧

```
空闲(高) ┐┌─1──0──1──1──0──1──0──1─┬─P─┬─停止─┐
         └┘ START  8bit 数据(LSB先)│奇偶│(高)  │下一帧 START
```

- **无时钟线**：双方约定波特率各自采样（异步），采样点在 bit 中央
- 常见帧格式 `8N1`：8 数据位、无校验、1 停止位
- 波特率生成：` bauddiv = uartclk / (16 * baud)`（16 倍过采样）
- 电平：SoC 引脚是 TTL，长距离/抗干扰用 RS-232（±12V）或 RS-485（差分）

### 常见错误位（面试点）

| 错误 | 含义 | 常见原因 |
|---|---|---|
| overrun (OER) | 数据没及时取走被覆盖 | 中断被长时间屏蔽/波特率太高，DMA 或降低负载 |
| parity (PER) | 校验错 | 波特率偏差、干扰 |
| frame/break (FER) | 停止位采样不到 1 | 波特率不匹配、地线问题 |
| buf_overrun | 软件层 flip buffer 满 | ldisc 消费太慢 |

波特率偏差 >~3% 就开始错位--两边 `uartclk` 分频不整除时优先查这个。

## 2. 内核分层：四层洋葱

```
用户态         write()/read() /dev/ttyS0 (getty, sshd, minicom...)
                  │
tty 核心       drivers/tty/tty_io.c        字符设备、行规则挂接点
                  │ N_TTY 行规则：规范模式(回显/行缓冲/^C)、raw 模式直通
tty 核心       ────────────────────────────
                  │
uart 核心      drivers/tty/serial/serial_core.c   uart_add_one_port 等
                  │  uart_driver / uart_state / uart_port / uart_ops
控制器驱动     amba-pl011.c / 8250 / imx.c         寄存器级：FIFO、中断、波特率
```

要点：
- **uart 核心是 tty 核心和控制器驱动之间的适配层**：把 tty 的"字符流"翻译成 uart_ops 的回调
- 行规则（line discipline）是可替换插件：N_TTY（终端）、N_GSM（GSMTSY 多路复用）、N_NULL
- console 子系统（`register_console`）复用同一批 uart_port：`earlycon`（早期汇编级，MMIO 直写）-> console（`uart_console_write` 走 poll 模式）-> 正常 tty--bringup "三段式串口"就是它

## 3. 三个核心结构（源码摘录，6.12.58）

### `struct uart_driver` - 一个驱动的整体注册

```c
/* include/linux/serial_core.h */
struct uart_driver {
	struct module		*owner;
	const char		*driver_name;
	const char		*dev_name;   /* /dev/<dev_name>N */
	int			 major;
	int			 minor;
	int			 nr;          /* 端口数 */
	struct console		*cons;

	/* 私有，低层驱动别碰 */
	struct uart_state	*state;
	struct tty_driver	*tty_driver;
};
```

注册后 `nr` 个端口按需生成 `/dev/ttyS0..N`（由 tty core 注册 tty_driver，devtmpfs/udev 负责设备节点）。

### `struct uart_port` - 一个物理端口

关键字段（节选）：
- `iobase/membase`：寄存器基址（MMIO 用 membase + `readl/writel`）
- `irq`：RX/TX 共用中断号
- `uartclk`：输入时钟（算分频的基准）
- `fifosize`：硬件 FIFO 深度（pl011 常见 32）
- `ops`：回调表；`line`：端口编号
- `state`：回指 uart_state（含 tty 与 xmit 环形缓冲）

### `struct uart_ops` - 控制器驱动要实现的回调

```c
/* include/linux/serial_core.h */
struct uart_ops {
	unsigned int	(*tx_empty)(struct uart_port *);
	void		(*set_mctrl)(struct uart_port *, unsigned int mctrl);
	unsigned int	(*get_mctrl)(struct uart_port *);
	void		(*stop_tx)(struct uart_port *);
	void		(*start_tx)(struct uart_port *);
	void		(*throttle)(struct uart_port *);
	void		(*unthrottle)(struct uart_port *);
	void		(*stop_rx)(struct uart_port *);
	void		(*start_rx)(struct uart_port *);
	int		(*startup)(struct uart_port *);
	void		(*shutdown)(struct uart_port *);
	void		(*set_termios)(struct uart_port *, struct ktermios *new,
				       const struct ktermios *old);
	void		(*pm)(struct uart_port *, unsigned int state,
			      unsigned int oldstate);
	int		(*request_port)(struct uart_port *);
	void		(*release_port)(struct uart_port *);
	void		(*config_port)(struct uart_port *, int);
	...
#ifdef CONFIG_CONSOLE_POLL
	int		(*poll_init)(struct uart_port *);
	void		(*poll_put_char)(struct uart_port *, unsigned char);
	int		(*poll_get_char)(struct uart_port *);
#endif
};
```

- `CONFIG_CONSOLE_POLL` 三件套 = kgdb/netconsole 依赖的轮询接口，**也是 earlycon/console 能在"中断还没好"的阶段工作的基础**
- `set_termios`：用户 `tcsetattr` 一路传到硬件（波特率/数据位/校验/流控全在这配）

## 4. 收发数据路径（面试核心）

### 写路径

```
write(/dev/ttyS0)
  -> tty_write -> n_tty_write（canonical 则等行缓冲规则）
  -> uart_write               # 写入 port->state->xmit 循环缓冲
  -> __uart_start -> ops->start_tx()    # 使能 TX 中断
  -> 硬件 TX FIFO 有空位触发中断 -> pl011_tx_chars() 从 xmit 搬 FIFO
  -> xmit 空 -> ops->stop_tx()          # 关 TX 中断省功耗
```

### 读路径

```
RX 数据到 -> 中断 -> pl011_rx_chars()
  -> 逐字符 uart_insert_char(port, flag, ch)   # 处理 OER/PER/FER，计数进 port->icount
  -> tty_flip_buffer_push()             # 推给 tty 核心
  -> ldisc (N_TTY) -> canonical/raw 分流 -> 唤醒 read() 等待者
```

- 中断处理要点：RX 中断必须读到 FIFO 空（电平触发语义），否则中断风暴
- `port->icount`（`struct uart_icount`：rx/tx/frame/overrun/parity/brk...）是统计来源，`/proc/tty/driver/<name>` 可读

### 流控

- 硬件流控 RTS/CTS：`set_mctrl` 拉 RTS，对端 CTS 满-> `stop_tx`；`termios CRTSCTS` 开启
- 软件流控 XON/XOFF：N_TTY 层处理，不碰硬件

## 5. 写一个最小 UART 驱动的骨架

```c
static struct uart_driver foo_uart = {
    .owner       = THIS_MODULE,
    .driver_name = "foo-uart",
    .dev_name    = "ttyFOO",
    .nr          = 2,
};

static const struct uart_ops foo_ops = {
    .tx_empty   = foo_tx_empty,
    .start_tx   = foo_start_tx,
    .stop_tx    = foo_stop_tx,
    .start_rx   = foo_start_rx,
    .stop_rx    = foo_stop_rx,
    .startup    = foo_startup,      /* request_irq 在这 */
    .shutdown   = foo_shutdown,
    .set_termios = foo_set_termios, /* 波特率分频计算在这 */
    .type       = foo_type,
    .request_port = foo_request_port, /* ioremap 资源 */
    .release_port = foo_release_port,
    .config_port = foo_config_port,
};

static int foo_probe(struct platform_device *pdev)
{
    struct uart_port *port;
    ...
    port->membase  = devm_platform_ioremap_resource(pdev, 0);
    port->irq      = platform_get_irq(pdev, 0);
    port->uartclk  = clk_get_rate(clk);
    port->fifosize = 32;
    port->ops      = &foo_ops;
    port->line     = line++;        /* 对应 ttyFOO<line> */
    uart_add_one_port(&foo_uart, port);
    ...
}
/* module: uart_register_driver(&foo_uart) + platform_driver_register */
```

## 6. 调试

| 手段 | 用法 |
|---|---|
| `/proc/tty/driver/<driver_name>` | 每端口 icount、信号线状态 |
| `/dev/ttyFOOn` + `stty -F ... raw -echo 115200` | raw 模式下 `cat`/`echo` 快速回环测试 |
| earlycon 参数 | `earlycon=pl011,mmio32,0x28001000`（boot 最早的输出） |
| `console=ttyFOO0` | 正式 console 绑定 |
| 逻辑分析仪/示波器 | 量一帧实际位宽算波特率：`baud = 1/(最小电平宽度)` |

## 7. 面试问答

**Q: UART 和 I2C/SPI 最大区别？**
异步无时钟线、点对点（不是总线）、全双工但只有一根 TX 一根 RX；可靠性靠帧格式（起始/停止位）提供最小同步。

**Q: earlycon、console、tty 三者关系？**
earlycon 在 arch 初始化极早期用"write-only 轮询"输出（不依赖任何驱动框架）；console 走 `register_console` 的驱动（中断可能还没起来，仍是 poll）；tty 是完整运行时的字符设备。三者共用同一硬件但代码路径独立--串口 bringup 三阶段调的就是这三层。

**Q: 串口打印为什么会丢字？**
TX FIFO 满后 CPU 忙等被抢占/关中断太久；console 打印路径不可睡眠所以不重试，直接丢。降低波特率反而更容易丢（占用时间长）；解法：RAM ring buffer（如 pstore/ramoops）。

**Q: overrun 和 buf_overrun 区别？**
overrun 是硬件 FIFO/移位寄存器级覆盖（软件取数不够快）；buf_overrun 是 tty flip buffer 级（ldisc 消费不够快）。

**Q: 波特率怎么算？**
`div = uartclk / (16 * baud)`，16 倍过采样；分频不整除产生累积误差，>3% 就不可靠。设置 termios 时对非法值驱动应取最近似并回填真实值。
