只读保护的是 **函数指针变量本身**，不是中断处理函数，也不是 GIC 硬件接收能力。

所以它变只读后，仍然可以中断响应。

---

## 1. 关键区别：读和写

这行：

```c
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init = default_handle_irq;
```

`__ro_after_init` 保护的是变量：

```c
handle_arch_irq
```

初始化后：

```text
不能再写 handle_arch_irq
可以继续读 handle_arch_irq
```

中断响应时只是读取这个函数指针，然后跳过去执行。

类似：

```c
handler = handle_arch_irq;  // 读，允许
handler(regs);              // 调用，允许
```

只读不是“不让访问”，而是“不让修改”。

---

## 2. 中断响应路径里并没有修改它

初始化阶段：

```c
set_handle_irq(gic_handle_irq);
```

这里会写：

```c
handle_arch_irq = handle_irq;
```

这一步发生在 init 早期，内存还没变只读，所以允许。

初始化完成后：

```text
handle_arch_irq 已经等于 gic_handle_irq
```

后续中断来了，入口代码只是使用它：

```c
el1_interrupt(regs, handle_arch_irq);
```

再往下是调用：

```c
gic_handle_irq(regs);
```

没有再执行：

```c
handle_arch_irq = xxx;
```

所以不会被只读权限影响。

---

## 3. 类比

假设有个只读变量：

```c
const int x = 100;
```

你不能：

```c
x = 200;   // 不允许
```

但你可以：

```c
printf("%d\n", x);  // 可以读
```

函数指针也是一样：

```c
void (*handle_arch_irq)(struct pt_regs *) = gic_handle_irq;
```

初始化后不能改成别的：

```c
handle_arch_irq = other_handler;  // 不允许
```

但可以读取并调用：

```c
handle_arch_irq(regs);            // 可以
```

---

## 4. “只读”只影响页表写权限

`__ro_after_init` 后期让该区域页表变成：

```text
Readable: yes
Writable: no
Executable: no
```

对 `handle_arch_irq` 来说，它只是一个数据变量，里面保存一个地址：

```text
handle_arch_irq 变量地址处保存：gic_handle_irq 的地址
```

中断入口只需要读出这个地址：

```text
从 handle_arch_irq 变量读取 gic_handle_irq 地址
跳转到 gic_handle_irq
```

读是允许的。

---

## 5. 真正执行的是代码段里的函数

`handle_arch_irq` 本身不是函数代码，它只是一个“保存函数地址的变量”。

可以理解成：

```text
handle_arch_irq 变量
  内容 = gic_handle_irq 的地址

gic_handle_irq 函数
  位于 .text 代码段
```

中断来了：

```text
读 handle_arch_irq 里的地址
  → 得到 gic_handle_irq
  → 跳到 .text 里的 gic_handle_irq 执行
```

`.data..ro_after_init` 只读不会阻止 `.text` 代码执行。

---

## 6. 怎么从代码层面证明

看 entry-common.c 的调用：

```c
el1_interrupt(regs, handle_arch_irq);
```

这里是把 `handle_arch_irq` 当参数传进去。

这一步是读取函数指针值，不是修改函数指针。

而修改只发生在：

```c
set_handle_irq()
```

```c
handle_arch_irq = handle_irq;
```

这个函数带 `__init`：

```c
int __init set_handle_irq(...)
```

说明它只应该在初始化阶段调用。

所以代码上可以证明：

```text
init 阶段：
  set_handle_irq() 写 handle_arch_irq

runtime 阶段：
  el1h_64_irq_handler() 读 handle_arch_irq 并调用

只读保护：
  禁止 runtime 再写，但不禁止 runtime 读/调用
```

---

## 7. 如果运行时真的去写会怎样

如果 init 完成后还有代码尝试：

```c
handle_arch_irq = xxx;
```

由于这个变量所在页已经只读，ARM64 会触发页权限异常，大概率 kernel oops/panic。

这正是它想要的保护效果。

---

一句话总结：

> `__ro_after_init` 只是不允许 init 后修改 `handle_arch_irq` 这个函数指针；中断响应只需要读取这个指针并跳到 `gic_handle_irq()` 执行，所以只读不会影响中断响应，反而能保护中断入口不被后续篡改。