# set_handle_irq 注册与解耦逻辑

疑问来源：架构层注册"根 IRQ 处理函数"为什么要这样做？看到两个"重复的" `int set_handle_fiq(void (*handle_fiq)(struct pt_regs *));`，机理是什么？`#define set_handle_irq set_handle_irq` 这块也看不懂。

不是重复逻辑，核心是 Linux 把 **CPU 异常入口** 和 **具体中断控制器驱动** 解耦了。

---

## 1. 为什么要注册“根 IRQ 处理函数”

ARM64 收到 IRQ 后，最早进入的是架构异常入口：

```c
el1h_64_irq_handler()
```

在 `arch/arm64/kernel/entry-common.c` 里：

```c
asmlinkage void noinstr el1h_64_irq_handler(struct pt_regs *regs)
{
	el1_interrupt(regs, handle_arch_irq);
}
```

这里的 `handle_arch_irq` 是一个函数指针：

```c
void (*handle_arch_irq)(struct pt_regs *) = default_handle_irq;
```

默认值是：

```c
static void default_handle_irq(struct pt_regs *regs)
{
	panic("IRQ taken without a root IRQ handler\n");
}
```
注册之前会赋值:
```c
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init = default_handle_irq;

void (*handle_arch_fiq)(struct pt_regs *) __ro_after_init = default_handle_fiq;
```
[[关于中断中_ro_after_init的解释]]

也就是说，架构层只知道：

```text
CPU 收到 IRQ exception 了
```

但它不知道具体中断控制器是：

- GICv2
- GICv3
- Apple AIC
- RISC-V INTC
- 其他 irqchip

所以 irqchip 驱动初始化时要注册自己的顶层处理函数：

```c
set_handle_irq(gic_handle_irq);
```

注册后路径变成：

```text
硬件 IRQ
  → ARM64 exception vector
  → el1h_64_irq_handler()
  → handle_arch_irq()
  → gic_handle_irq()
  → 读 ICC_IAR1_EL1 得到 INTID
  → generic_handle_domain_irq()
  → 设备驱动 handler
```

所以 `set_handle_irq()` 的目的就是：

> 让架构异常入口知道“收到 IRQ 后该交给哪个 irqchip 顶层 handler”。

---

## 2. `set_handle_irq()` 的机理

定义在 `arch/arm64/kernel/irq.c`：

```c
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init = default_handle_irq;

int __init set_handle_irq(void (*handle_irq)(struct pt_regs *))
{
	if (handle_arch_irq != default_handle_irq)
		return -EBUSY;

	handle_arch_irq = handle_irq;
	pr_info("Root IRQ handler: %ps\n", handle_irq);
	return 0;
}
```

机制很简单：

1. 初始 `handle_arch_irq = default_handle_irq`
2. GIC 初始化时调用：

   ```c
   set_handle_irq(gic_handle_irq);
   ```

3. `handle_arch_irq` 被改成 `gic_handle_irq`
4. 后续 IRQ exception 直接走 GIC handler

并且它只允许注册一次：

```c
if (handle_arch_irq != default_handle_irq)
	return -EBUSY;
```

这是为了防止多个 root irqchip 抢顶层入口。

---

## 3. 为什么要有 `set_handle_fiq()`

ARM64 异常类型里 IRQ 和 FIQ 是两条不同入口：

```text
IRQ: mask bit 是 DAIF.I
FIQ: mask bit 是 DAIF.F
```

所以代码里也有两套路由：

```c
void (*handle_arch_irq)(struct pt_regs *) = default_handle_irq;
void (*handle_arch_fiq)(struct pt_regs *) = default_handle_fiq;
```

对应注册函数：

```c
int set_handle_irq(void (*handle_irq)(struct pt_regs *));
int set_handle_fiq(void (*handle_fiq)(struct pt_regs *));
```

FIQ 入口在 entry-common.c：

```c
asmlinkage void noinstr el1h_64_fiq_handler(struct pt_regs *regs)
{
	trace_android_rvh_fiq_dump(regs);
	el1_interrupt(regs, handle_arch_fiq);
}
```

所以 FIQ 路径是：

```text
FIQ exception
  → el1h_64_fiq_handler()
  → handle_arch_fiq()
```

大多数普通 GIC 中断走 IRQ，不走 FIQ。  
但有些平台会用 FIQ 做特殊快速中断，例如 `drivers/irqchip/irq-apple-aic.c` 里就有：

```c
set_handle_irq(aic_handle_irq);
set_handle_fiq(aic_handle_fiq);
```

所以 `set_handle_fiq()` 是给有 FIQ 顶层处理需求的平台准备的。

---

## 4. "两个 `set_handle_fiq`"不是重复实现

头文件里的是声明：

```c
int set_handle_fiq(void (*handle_fiq)(struct pt_regs *));
```

真正实现是在：

```c
arch/arm64/kernel/irq.c
```

里面：

```c
int __init set_handle_fiq(void (*handle_fiq)(struct pt_regs *))
{
	if (handle_arch_fiq != default_handle_fiq)
		return -EBUSY;

	handle_arch_fiq = handle_fiq;
	pr_info("Root FIQ handler: %ps\n", handle_fiq);
	return 0;
}
```

一个是 **声明**，一个是 **定义**，不是重复。

可以理解为：

```text
头文件 irq.h:
  告诉其他 C 文件：有这个函数，可以调用

irq.c:
  真正实现这个函数
```

---

## 5. `#define set_handle_irq set_handle_irq` 是什么鬼？

这行确实看起来很怪：

```c
#define set_handle_irq	set_handle_irq
```

它不是为了替换代码逻辑，而是一个 **预处理器标记**。

它的作用是告诉通用代码：

```text
当前架构已经自己提供了 set_handle_irq
不要再定义默认版本
```

看 `include/linux/irq.h` 里面有这段：

```c
#ifndef set_handle_irq
#define set_handle_irq(handle_irq)		\
	do {					\
		(void)handle_irq;		\
		WARN_ON(1);			\
	} while (0)
#endif
```

意思是：

如果架构没有提供 `set_handle_irq`，通用代码就定义一个假的 fallback：

```c
set_handle_irq(x) {
	WARN_ON(1);
}
```

但 ARM64 在 `arch/arm64/include/asm/irq.h` 里提前写了：

```c
#define set_handle_irq set_handle_irq
```

所以：

```c
#ifndef set_handle_irq
```

判断为 false，通用 fallback 不会生效。

---

## 6. 自引用宏会不会无限展开？

不会。

```c
#define set_handle_irq set_handle_irq
```

这是一个 **自引用宏**。C 预处理器遇到它时，不会无限递归展开。

它主要效果是：

```text
让 set_handle_irq 这个宏名处于“已定义”状态
```

但实际代码里：

```c
set_handle_irq(gic_handle_irq);
```

预处理后基本还是：

```c
set_handle_irq(gic_handle_irq);
```

不会变成别的东西。

所以这行可以理解成：

```c
#define ARCH_HAS_SET_HANDLE_IRQ
```

只是 Linux 这里直接拿函数名本身当标记用。

---

## 7. 为什么 `set_handle_fiq` 没有类似的宏？

因为通用 `include/linux/irq.h` 里主要有 `set_handle_irq` 的 generic fallback 机制：

```c
#ifndef set_handle_irq
#define set_handle_irq(handle_irq) ...
#endif
```

但没有通用的 `set_handle_fiq` fallback。

`set_handle_fiq()` 是 ARM64 自己扩展出来的 FIQ 顶层注册接口，因此不需要：

```c
#define set_handle_fiq set_handle_fiq
```

---

## 8. 总体图

```text
                +----------------------+
                | ARM64 exception entry |
                +----------+-----------+
                           |
            IRQ exception  |  FIQ exception
                           |
             +-------------+-------------+
             |                           |
   el1h_64_irq_handler()      el1h_64_fiq_handler()
             |                           |
      handle_arch_irq              handle_arch_fiq
             |                           |
      set_handle_irq()             set_handle_fiq()
             |                           |
      gic_handle_irq()             platform FIQ handler
             |
      generic_handle_domain_irq()
             |
      device interrupt handler
```

---

一句话总结：

> `set_handle_irq()` 是 irqchip 驱动把自己的“顶层 IRQ 分发函数”挂到 ARM64 异常入口上的机制；`set_handle_fiq()` 是 FIQ 版本；`#define set_handle_irq set_handle_irq` 只是一个“我这个架构已经提供 set_handle_irq 了”的预处理器标记，不是实际逻辑替换。