# pinctrl 子系统

> 引脚复用（pinmux）+ 引脚配置（pinconf）。代码摘自本地内核树 6.12.58。One SoC、万种引脚组合--pinctrl 就是"每个引脚当前归谁用、什么电气配置"的唯一权威。

## 1. 全景分层

```
消费者驱动      pinctrl_pm_select_state / devm_pinctrl_get（大多自动：probe 切 default）
                     │ DT: pinctrl-0 = <&uart2_pins>
pinctrl 核心    drivers/pinctrl/core.c        状态机/解析 DT
                drivers/pinctrl/pinmux.c      功能复用仲裁
                drivers/pinctrl/pinconf.c     电气配置
                     │ struct pinctrl_dev + pinctrl_desc
控制器驱动      pinctrl-xxx.c                 写 IOMUX/pad 寄存器
```

核心概念三件套：
- **pin**：物理引脚（编号）
- **function**：功能（uart2、i2c1、gpio...）
- **group**：实现某功能占用的引脚集合（一个 function 可多 group，如 uart2 可走 A 组脚或 B 组脚）

## 2. 核心结构（源码摘录）

### `struct pinctrl_desc` - 控制器驱动注册的描述符

```c
/* include/linux/pinctrl/pinctrl.h */
struct pinctrl_desc {
	const char *name;
	const struct pinctrl_pin_desc *pins;   /* 引脚表 */
	unsigned int npins;
	const struct pinctrl_ops *pctlops;     /* 枚举 pin/group */
	const struct pinmux_ops *pmxops;       /* 复用 */
	const struct pinconf_ops *confops;     /* 电气配置 */
	struct module *owner;
	...
};

extern int pinctrl_register_and_init(struct pinctrl_desc *pctldesc,
				     struct device *dev, void *driver_data,
				     struct pinctrl_dev **pctldev);
extern int pinctrl_enable(struct pinctrl_dev *pctldev);
```

### `struct pinmux_ops` - 复用操作

```c
/* include/linux/pinctrl/pinmux.h */
struct pinmux_ops {
	int (*request) (struct pinctrl_dev *pctldev, unsigned int offset);
	int (*free) (struct pinctrl_dev *pctldev, unsigned int offset);
	int (*get_functions_count) (struct pinctrl_dev *pctldev);
	const char *(*get_function_name) (struct pinctrl_dev *pctldev, unsigned int selector);
	int (*get_function_groups) (struct pinctrl_dev *pctldev, unsigned int selector,
				    const char * const **groups, unsigned int *num_groups);
	int (*set_mux) (struct pinctrl_dev *pctldev, unsigned int func_selector,
			unsigned int group_selector);
	int (*gpio_request_enable) (struct pinctrl_dev *pctldev,
				    struct pinctrl_gpio_range *range, unsigned int offset);
	...
};
```

- **set_mux 是最终写寄存器的地方**：func+group 选择落到 IOMUX 寄存器
- **request/free 是复用仲裁**：两个驱动抢同一引脚，第二个 request 被拒（"pin already requested by ..."）

## 3. 设备树怎么写（消费者视角，面试最常问）

```dts
uart2 {
    pinctrl-names = "default", "sleep";
    pinctrl-0 = <&uart2_default>;     /* probe 自动申请切到 default */
    pinctrl-1 = <&uart2_sleep>;       /* suspend 时 pinctrl 核心自动切 */
};

&pinctrl {
    uart2_default: uart2-default-pins {
        function = "uart2";
        pins = "PE5", "PE6";
        bias-pull-up;
        drive-strength = <8>;
    };
};
```

- `pinctrl-names` 定义状态名，**default/sleep 是核心约定名**，runtime PM 自动切换；自定义状态用 `pinctrl_select_state` 手动切
- pinconf 属性：`bias-pull-up/down/none`、`drive-strength`（mA）、`slew-rate`、`input-enable/schmitt-enable`、`power-source`（电压域）

## 4. 和 GPIO 子系统的关系（高频考点）

```
              ┌─ function="gpio" ──> gpiolib（gpio_request_enable 回调放行）
pinctrl ──────┤
              └─ function="uart2" ─> 外设控制器
```

- **GPIO 也是 pinctrl 的一个 function**：`gpio_request_enable` 让 pinctrl 把脚切到 GPIO 模式并移交给 gpiolib
- pinctrl 是底层唯一写 IOMUX 的层；gpiolib 不直接碰复用寄存器
- bringup 顺序上：pinctrl 必须先于依赖它的设备 probe（DT 依赖探测自动保证）

## 5. 调试

| 手段 | 用法 |
|---|---|
| debugfs `/sys/kernel/debug/pinctrl/` | `pins`（全部引脚）、`pingroups`、`pinmux-functions`、`pinctrl-devices`（每设备当前状态） |
| dmesg | "pin ... already requested by ..." = 复用冲突，直接给出冲突方 |
| pinctrl 状态没生效 | 查 `pinctrl-0` 引用的节点是否存在、selector 拼写、是否被更早的驱动占用 |

常见坑：
- 两个设备 DT 都声明同一引脚 -> 后 probe 的失败（这是保护不是 bug）
- sleep 态忘了配 -> suspend 后引脚保持默认态漏电（功耗测试抓的就是这个）
- 上电时引脚毛刺：bootloader 到内核 pinctrl 生效前的窗口，硬件要容忍或 bootloader 预配

## 6. 面试问答

**Q: pinctrl 解决什么问题？**
一颗 SoC 几百个引脚、每个引脚多种复用+电气参数，pinctrl 提供：统一 DT 描述、复用冲突仲裁、电气配置标准化、与 gpiolib 的移交协议。

**Q: function/group/pin 三者关系？**
function 是逻辑功能，group 是该功能在某组物理引脚上的实现，pin 是最小物理单元。set_mux(func, group) 完成实际复用。

**Q: 设备驱动需要主动调 pinctrl API 吗？**
一般不用：probe 时核心自动拿 default 状态；只有自定义状态（如"高速模式"重配驱动强度）才显式 `pinctrl_select_state`。

**Q: GPIO 请求时 pinctrl 发生了什么？**
gpiolib 的 request 走到 pinctrl 的 `gpio_request_enable`，把该 pin mux 成 GPIO function，之后 direction/output 由 gpiolib 管，复用权在 pinctrl 记账。
