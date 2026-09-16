# GPIO 子系统

> 基于 gpiolib（`drivers/gpio/`）+ descriptor API。代码摘自本地内核树 6.12.58。旧的全局整数 API（`gpio_request(nr)`）已废弃，面试统一讲 descriptor API。

## 1. 全景分层

```
消费者驱动      gpiod_get / gpiod_set_value / gpiod_to_irq
                     │ struct gpio_desc（不透明句柄）
gpiolib 核心    drivers/gpio/gpiolib.c   字符设备 /dev/gpiochipN、sysfs 兼容、
                                     irqchip 桥接（gpio-to-irq）
                     │ struct gpio_chip
控制器驱动      gpio-pl061 / 厂商 GPIO     寄存器级方向/电平/中断
```

三层各司其职：消费者只见 desc；gpiolib 管编号空间与并发；控制器驱动写寄存器。

## 2. 核心结构（源码摘录）

### `struct gpio_chip` - 控制器驱动要实现的

```c
/* include/linux/gpio/driver.h */
struct gpio_chip {
	const char		*label;
	struct gpio_device	*gpiodev;
	struct device		*parent;
	struct fwnode_handle	*fwnode;
	struct module		*owner;

	int		(*request)(struct gpio_chip *gc, unsigned int offset);
	void		(*free)(struct gpio_chip *gc, unsigned int offset);
	int		(*get_direction)(struct gpio_chip *gc, unsigned int offset);
	int		(*direction_input)(struct gpio_chip *gc, unsigned int offset);
	int		(*direction_output)(struct gpio_chip *gc, unsigned int offset, int value);
	int		(*get)(struct gpio_chip *gc, unsigned int offset);
	int		(*get_multiple)(struct gpio_chip *gc, unsigned long *mask,
					unsigned long *bits);
	void		(*set)(struct gpio_chip *gc, unsigned int offset, int value);
	void		(*set_multiple)(struct gpio_chip *gc, unsigned long *mask,
					unsigned long *bits);
	int		(*set_config)(struct gpio_chip *gc, unsigned int offset,
					unsigned long config);   /* 上拉/开漏/去抖 */
	int		(*to_irq)(struct gpio_chip *gc, unsigned int offset);
	...
};
```

- `set/get` 操作的是**原始电平**（raw value），跟 active-low 无关；消费者语义（logical value）由 gpiolib 按 DT 极性翻转--见下
- `to_irq`：GPIO 中断的桥（实现后 `gpiod_to_irq()` 可用，底层 irqchip 是 gpiolib 自动建的层级域）

## 3. 消费者 API（驱动里就这么写）

```c
/* probe：按 DT 的 xxx-gpios 属性拿 */
struct gpio_desc *pwdn = gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);  /* 逻辑低=断电? 由 DT 决定 */
if (IS_ERR(pwdn)) return PTR_ERR(pwdn);

gpiod_set_value_cansleep(pwdn, 1);   /* 逻辑值：DT 里 active-low 会自动翻 */
int lv = gpiod_get_value(pwdn);

int irq = gpiod_to_irq(pwdn);        /* 转 IRQ 号，走 request_threaded_irq */
devm_gpio_free / gpiod_put(pwdn);
```

规则（面试点）：
- **逻辑值 vs 原始值**：DT `gpio = <&gpio2 5 GPIO_ACTIVE_LOW>` 时，`gpiod_set_value(d,1)` 输出低电平。写驱动想的是"使能有效"，不是"输出高"
- `_cansleep` 后缀：控制器挂在 I2C/SPI 扩展芯片（如 IO 扩展器）时会睡眠，**原子上下文只能用无后缀版**，且必须是 MMIO 控制器
- DT 里 `-gpios` 后缀的 key 就是 `gpiod_get` 的 con_id

## 4. 设备树描述

```dts
sensor@10 {
    compatible = "xxx";
    pwdn-gpios = <&gpio2 5 GPIO_ACTIVE_HIGH>;
    interrupt-parent = <&gpio2>;
    interrupts = <4 IRQ_TYPE_EDGE_FALLING>;   /* 也可直接走 GPIO 控制器 irqchip */
};
```

一条脚同时被 GPIO 和中断用：gpiolib 与 irqchip 共享同一 offset，`gpiod_to_irq` 后线被 irq 占用时 set_direction 会被拒。

## 5. 调试

| 手段 | 用法 |
|---|---|
| `/dev/gpiochipN` + libgpiod | `gpiodetect`、`gpioinfo`、`gpioset gpiochip2 5=1`（用户态直接操作，bringup 试线神器） |
| debugfs `/sys/kernel/debug/gpio` | 所有线状态/消费者名 |
| `/sys/class/gpio`（旧） | 已废弃但仍可用，新代码别依赖 |

常见坑：
- gpiod_get 返回 `-ENOENT`：DT 里 key 名不带 `-gpios` 或后缀不匹配
- 电平逻辑反了：DT 极性标错（低有效器件写了 ACTIVE_HIGH）
- 原子上下文调 `_cansleep` 版本 -> `might_sleep` 报错

## 6. 面试问答

**Q: gpiod 和旧 gpio 号 API 的区别？**
desc 是句柄制：引用计数、DT 极性语义、防全局编号冲突；旧 API 全局整数号+手动 request/free，已被标记废弃。

**Q: gpiod_to_irq 原理？**
gpiolib 为每个控制器建一个 irq_domain（层级挂在父中断控制器下），to_irq 即找该 domain 里 offset 对应的 virq；控制器驱动提供 irq_chip 的 ack/mask 回调。

**Q: GPIO 中断和外部 IRQ 线怎么选？**
本质同一机制；SoC 引脚复用成 GPIO 后其中断就是 GPIO irqchip 的，`request_threaded_irq(gpiod_to_irq(...))`。

**Q: 为什么有 set_value_cansleep 两个版本？**
MMIO GPIO 一条写指令完成，可原子；I2C/SPI 扩展 GPIO 一趟总线事务会睡眠。驱动作者必须声明自己的调用上下文，内核用 `_cansleep` 强制自证。
