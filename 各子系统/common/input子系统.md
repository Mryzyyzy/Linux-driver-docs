# Input 子系统

> 键盘/触摸屏/摇杆等输入设备的统一框架。代码摘自本地内核树 6.12.58。结构小而美，是理解"内核事件流到 /dev/input"的最佳入门子系统。

## 1. 全景分层

```
用户态        evtest / libinput / getty(kbd)
                   │ read(/dev/input/eventX)
事件层        drivers/input/evdev.c     字符设备、每 open 一个 client 缓冲
                   │ struct input_event {time, type, code, value}
核心层        drivers/input/input.c     设备注册/事件分发/重复键
handler 层    evdev / keyboard / mousedev / joydev   按事件类型分流
                   │ input_report_*
设备驱动      gpio-keys / 触摸屏 I2C 驱动 / USB 键盘   产生原始事件
```

要点：**input_dev（设备）与 input_handler（消费者）多对多匹配**（类似总线模型），匹配后创建 handle 把两边连起来。

## 2. 核心结构（源码摘录）

### `struct input_dev` - 输入设备

```c
/* include/linux/input.h */
struct input_dev {
	const char *name;
	const char *phys;
	const char *uniq;
	struct input_id id;

	unsigned long evbit[BITS_TO_LONGS(EV_CNT)];   /* 支持的事件类型 */
	unsigned long keybit[BITS_TO_LONGS(KEY_CNT)]; /* 支持的键 */
	unsigned long relbit[BITS_TO_LONGS(REL_CNT)]; /* 相对轴（鼠标） */
	unsigned long absbit[BITS_TO_LONGS(ABS_CNT)]; /* 绝对轴（触摸屏） */
	unsigned long mscbit[BITS_TO_LONGS(MSC_CNT)];
	unsigned long ledbit[BITS_TO_LONGS(LED_CNT)];
	unsigned long swbit[BITS_TO_LONGS(SW_CNT)];   /* 开关（盖子/耳机） */

	unsigned int keycodemax;
	unsigned int keycodesize;
	void *keycode;                                 /* 键码表 */
	...
	struct input_dev_poller *poller;
	unsigned int repeat_key;   /* 重复键：软件自动重复 */
	struct timer_list timer;   /* rep[REP_DELAY]/rep[REP_PERIOD] */
	...
};
```

### 事件三元组（面试必背）

| 事件类型 (type) | code 例子 | value 含义 |
|---|---|---|
| `EV_KEY` | KEY_POWER, BTN_TOUCH | 1 按下 / 0 抬起 / 2 重复 |
| `EV_ABS` | ABS_X/Y, ABS_MT_POSITION_X | 坐标值（范围由 `input_set_abs_params` 声明） |
| `EV_REL` | REL_X/Y, REL_WHEEL | 增量 |
| `EV_SYN` | SYN_REPORT / SYN_MT_REPORT | 一帧事件结束分隔符 |
| `EV_SW` | SW_LID, SW_HEADPHONE_INSERT | 0/1 状态 |

**SYN_REPORT 语义是关键**：多次 report_* 只是积攒，SYN 才作为一个完整帧分发给用户态。

## 3. 设备驱动写法（gpio-keys 为模型的骨架）

```c
struct input_dev *idev = devm_input_allocate_device(dev);

idev->name = "foo-keys";
__set_bit(EV_KEY, idev->evbit);
__set_bit(KEY_POWER, idev->keybit);
/* 触摸屏另加：input_set_abs_params(idev, ABS_X, 0, MAX_X, 0, 0); */

input_register_device(idev);

/* 中断/轮询回调里上报 */
static irqreturn_t key_isr(int irq, void *dev)
{
	int pressed = !gpiod_get_value(pwdn);
	input_report_key(idev, KEY_POWER, pressed);
	input_sync(idev);                     /* = EV_SYN/SYN_REPORT，一帧封包 */
	return IRQ_HANDLED;
}
```

- 报事件三部曲：`input_report_*` -> `input_sync` -> 核心 `input_event()` 分发到所有 handle
- 多点触摸：`input_mt_init_slots` + `input_mt_report_slot_state`（type-B 协议），最终 `input_mt_sync_frame`
- 轮询设备（无中断）：`input_setup_polling` + poller 回调（ADC 摇杆常见）

## 4. 事件到用户态

```
input_report_key(dev, KEY_POWER, 1)
  -> input_event() -> input_handle_event()
       ├─ 重复键定时器管理（REP_DELAY 后每 REP_PERIOD 重发 value=2）
       └─ 遍历 dev->h_list -> handler->event()
            └─ evdev_event() -> 写入每个 client 的环形缓冲 -> wake up poll
用户态 read() <- struct input_event { struct timeval time; u16 type, code; s32 value; }
```

- 每个 `/dev/input/eventX` 对应一个 evdev client；缓冲满丢最旧事件（NEVER 阻塞生产者）
- libinput/Android inputflinger 就消费在这里，负责手势合成、按键映射等策略

## 5. 调试

| 手段 | 用法 |
|---|---|
| `evtest` | 打印所有设备的实时事件流（第一入口：先确认事件有没有到内核） |
| `/proc/bus/input/devices` | 每设备 name/phys/ids/支持位图/handlers（S: N: B:） |
| `getevent`（Android） | 同 evtest |
| 现象速查 | 事件没到 -> 驱动 ISR 没跑/没 sync；坐标跳变 -> abs 参数范围错；长按不重复 -> REP_* 没配 |

## 6. 面试问答

**Q: input_report_key 后用户态什么时候能看到？**
input_sync（SYN_REPORT）之后才整帧分发。不 sync 用户态永远收不到--经典 bug。

**Q: 为什么需要 SYN_REPORT？**
一次交互含多个原子事件（多点触控 N 个 slot 的坐标+压力），SYN 界定帧边界，消费方拿到一致快照。

**Q: input_dev 和 input_handler 的匹配机制？**
类似总线：handler 的 `id_table`/`match` 回调与设备 `input_id`（bus/vendor/product/version）匹配，匹配后建 input_handle，事件经 handle 分发。evdev 几乎全匹配（透传给用户态），keyboard 只配 EV_KEY。

**Q: 触摸屏驱动为什么常配 workqueue/threaded_irq？**
I2C 触摸芯片中断后要跑 I2C 读坐标（睡眠），硬中断上下文不允许 -> threaded_irq 或 schedule_work 后再上报。

**Q: 按键防抖在哪层做？**
gpio-keys 框架层（debounce 配置/定时器）；硬件去抖用 pinctrl 的 schmitt/去抖配置更好。input 核心不管防抖。
