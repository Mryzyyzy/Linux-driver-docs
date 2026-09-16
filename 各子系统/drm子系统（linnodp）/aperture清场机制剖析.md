# aperture 清场机制：aperture_remove_all_conflicting_devices 剖析

linlon-dp 驱动 bind 时的"清场"动作，以及用它做范例演示 VS Code 读内核代码的 Go to Definition 实战技巧。

## 一、函数逐层剖析

### 调用背景

在 linlondp_drv.c 里：

```c
err = aperture_remove_all_conflicting_devices("linlondp");
```

目的：在自家 DRM 驱动 bind 之前，**把之前占用同一段显存（framebuffer aperture）的"占位驱动"踢掉**，避免冲突。典型的"占位驱动"是：

- **efifb / vesafb / simplefb / simpledrm**：BIOS/UEFI 阶段把固件 framebuffer 暴露给内核，让启动早期就能看到 logo
- 当真正的 GPU/DP 驱动加载时，必须先卸载这些占位驱动，自己接管显存

### 第 0 层：入口（inline wrapper）

文件：include/linux/aperture.h

```c
static inline int aperture_remove_all_conflicting_devices(const char *name)
{
    return aperture_remove_conflicting_devices(0, (resource_size_t)-1, name);
}
```

- 只是个 inline 包装，传入 `[0, ~0]` 这个**覆盖整个物理地址空间**的范围
- 等价于"不管在哪里的 framebuffer aperture，全踢掉"

### 第 1 层：`aperture_remove_conflicting_devices`

文件：drivers/video/aperture.c

```c
int aperture_remove_conflicting_devices(resource_size_t base, resource_size_t size,
                                        const char *name)
{
    sysfb_disable(NULL);                      // ① 阻止 sysfb 之后再注册占位驱动
    aperture_detach_devices(base, size);      // ② 真正干活：把现有占位驱动卸载
    return 0;
}
```

两件事：

1. **`sysfb_disable()`**：sysfb（system framebuffer）是负责注册 efifb/vesafb/simplefb/simpledrm 等"固件 framebuffer 平台设备"的子系统。调用它防止以后再创建新占位设备。
2. **`aperture_detach_devices()`**：从已注册的 aperture 列表中，把和 `[base, base+size)` 重叠的所有占位设备卸载掉。

### 第 2 层：`aperture_detach_devices`

```c
static void aperture_detach_devices(resource_size_t base, resource_size_t size)
{
    resource_size_t end = base + size;
    struct list_head *pos, *n;

    mutex_lock(&apertures_lock);

    list_for_each_safe(pos, n, &apertures) {                    // ① 遍历全局 aperture 链表
        struct aperture_range *ap = container_of(pos, struct aperture_range, lh);
        struct device *dev = ap->dev;

        if (WARN_ON_ONCE(!dev))
            continue;

        if (!overlap(base, end, ap->base, ap->base + ap->size)) // ② 不重叠就跳过
            continue;

        ap->dev = NULL;          /* detach from device */        // ③ 标记已卸载
        list_del(&ap->lh);
        ap->detach(dev);                                        // ④ 调用注册时给的卸载回调
    }

    mutex_unlock(&apertures_lock);
}
```

核心三件事：

- 遍历全局 `apertures` 链表（所有"占有 framebuffer 物理范围"的设备）
- 用 `overlap()` 判断是否落在要清理的范围内
- 调用注册时挂上去的 `ap->detach(dev)` 回调进行真正的"卸载"

### 第 3 层：关键数据结构 `struct aperture_range`

```c
struct aperture_range {
    struct device *dev;                  // 占用这段显存的设备
    resource_size_t base;                // 起始物理地址
    resource_size_t size;                // 大小
    struct list_head lh;                 // 挂到全局 apertures 链表的节点
    void (*detach)(struct device *dev);  // 卸载回调（关键!）
};

static LIST_HEAD(apertures);             // 全局链表头
static DEFINE_MUTEX(apertures_lock);     // 保护链表
```

这是**整个 aperture 子系统的核心数据结构**--理解它就理解了机制：

- 每个占用 framebuffer 的驱动，在注册时往全局链表里挂一个 `aperture_range`
- 后来者要踢它们时，遍历这个链表逐个调 `detach()`

### 第 4 层：`detach` 回调从哪里来？-- 注册侧

"占位设备是怎么注册进来的"：

```c
int devm_aperture_acquire_for_platform_device(struct platform_device *pdev,
                                              resource_size_t base,
                                              resource_size_t size)
{
    return devm_aperture_acquire(&pdev->dev, base, size,
                                 aperture_detach_platform_device);  // ← 注册 detach 回调
}
```

而 `aperture_detach_platform_device` 真正的卸载动作就是：

```c
static void aperture_detach_platform_device(struct device *dev)
{
    struct platform_device *pdev = to_platform_device(dev);
    platform_device_unregister(pdev);   // 把平台设备从内核注销
}
```

`devm_aperture_acquire` 则把信息打包成 `aperture_range`，挂到全局链表：

```c
ap->dev    = dev;
ap->base   = base;
ap->size   = size;
ap->detach = detach;
list_add(&ap->lh, &apertures);
```

### 第 5 层：完整数据流

```text
启动早期：sysfb 探测到 EFI framebuffer，注册一个 simpledrm/efifb 平台设备
   │
   ▼
该占位驱动 probe 时调用：
   devm_aperture_acquire_for_platform_device(pdev, fb_base, fb_size)
   │
   ▼
往全局 apertures[] 链表挂一个 aperture_range：
   { dev=pdev, base=fb_base, size=fb_size,
     detach=aperture_detach_platform_device }
   │
   ▼ … 系统正常运行，屏幕显示 EFI 的内容 …
   │
   ▼ 当 linlon-dp 驱动加载时：
linlondp_bind()
   └─ aperture_remove_all_conflicting_devices("linlondp")
        └─ aperture_remove_conflicting_devices(0, -1, "linlondp")
             ├─ sysfb_disable(NULL)         (防止再创建新占位)
             └─ aperture_detach_devices(0, -1)
                  └─ 遍历 apertures[]
                       └─ ap->detach(dev)
                            └─ aperture_detach_platform_device(dev)
                                 └─ platform_device_unregister(pdev)
                                      -> 触发该占位驱动的 remove()
                                      -> 释放 framebuffer 显存所有权
   │
   ▼
linlon-dp 安全接管显存，开始自己的 KMS 初始化
```

### 一句话总结

`aperture_remove_all_conflicting_devices` = "**遍历内核里所有声明占用过 framebuffer 物理内存的设备，把和指定地址范围重叠的，统统调用它们注册时留下的 detach 回调进行卸载**"。是 DRM 驱动 probe 时的标准"清场"动作。

## 二、`(resource_size_t)-1`：覆盖整个物理地址空间的技巧

inline 包装里第 2 个参数 `(resource_size_t)-1` 是关键。

### 类型链

```c
#ifdef CONFIG_PHYS_ADDR_T_64BIT
typedef u64 phys_addr_t;       // = unsigned 64-bit
#else
typedef u32 phys_addr_t;       // = unsigned 32-bit
#endif

typedef phys_addr_t resource_size_t;
```

`resource_size_t` 是**无符号整数**（u32 或 u64），由 `CONFIG_PHYS_ADDR_T_64BIT` 决定宽度。

### `-1` 转成无符号会变成什么

C 标准规定：**有符号 `-1` 转成无符号整型 T，结果是 T 类型的最大值**（所有 bit 全为 1）。

```c
(u32)-1  ->  0xFFFFFFFF              = 2^32 - 1
(u64)-1  ->  0xFFFFFFFFFFFFFFFF      = 2^64 - 1
```

二进制视角：

```
-1 (补码) =  1111...1111
强转无符号  ->  保留这些 bit
           ->  所有位为 1，即该类型最大值
```

### 为什么这就"覆盖整个物理地址空间"

```c
resource_size_t end = base + size;   // 0 + (~0) -> 因为无符号溢出，end 也变成 ~0
```

然后用 `overlap()` 判断范围重叠：

```c
static bool overlap(resource_size_t base1, resource_size_t end1,
                    resource_size_t base2, resource_size_t end2)
{
    return (base1 < end2) && (end1 > base2);
}
```

- `base1 = 0`，`end1 = 最大值`
- 对任何合法的 `ap->base / ap->size`：`0 < end2` 几乎总成立、`最大值 > base2` 总成立

**结果：跟任何一个已注册的 aperture_range 都会"重叠"，全部命中、全部 detach。**

"覆盖整个物理地址空间"不是真的有这么大物理内存，而是构造一个**逻辑上无所不包**的区间，让 overlap 判断对任何已注册条目都返回 true。

### 扩展：内核里这种写法非常常见

```c
#define U32_MAX   ((u32)~0U)         // include/linux/limits.h
#define U64_MAX   ((u64)~0ULL)
#define ULONG_MAX (~0UL)
#define SIZE_MAX  (~(size_t)0)

memset(buf, 0xff, n);                  // 把内存全填 1
ioremap(addr, (size_t)-1);             // 类似"最大"用法
```

> **`(无符号类型 T)-1` ≡ T 的最大值 ≡ T 所有 bit 为 1**

## 三、用这个函数练 Go to Definition（VS Code 读内核实战）

### 技巧 1：分清"声明"与"定义"，按需跳

| 快捷键 | 作用 | 用途 |
|--------|------|------|
| `F12` / `Ctrl+Click` | **Go to Definition** | 跳到函数实现 |
| `Ctrl+F12` | **Go to Implementation** | 跳到接口/虚函数的实现（C 里少用） |
| `Alt+F12` | **Peek Definition** | 弹小窗预览，不离开当前文件 |
| `Shift+F12` | **Find All References** | 看谁调用了它 |
| `Ctrl+T` | **Go to Symbol in Workspace** | 全工程符号搜索（应对 IntelliSense 失败） |
| `Ctrl+Shift+O` | **Go to Symbol in File** | 当前文件内跳转 |

### 技巧 2：第一次跳容易掉进 inline 包装/头文件

在 linlondp_drv.c 光标停在 `aperture_remove_all_conflicting_devices`，按 `F12` -> 会跳到 include/linux/aperture.h 里的 inline 包装。

**关键观察**：内核里大量函数是 `static inline` 写在头文件里的薄包装。**别停在这里以为读完了**--继续在 `aperture_remove_conflicting_devices` 上按 `F12` 才会跳到真正的 `.c` 实现。

> 学习节奏：**每跳一次先扫一眼是不是 inline/宏/extern 声明**，是的话立刻再跳一层。

### 技巧 3：碰到 `#if defined(CONFIG_xxx)` 双实现，要看哪个生效

aperture.h 里有：

```c
#if defined(CONFIG_APERTURE_HELPERS)
int aperture_remove_conflicting_devices(...);   // 真实现，声明
#else
static inline int aperture_remove_conflicting_devices(...) { return 0; } // stub
#endif
```

IntelliSense **可能跳到 stub 那个空实现**，让你以为这个函数啥也没干。

**应对方法**：
- `grep -rn "^int aperture_remove_conflicting_devices" drivers/` 找真正的 `.c` 实现
- 确认 `.config` 里 `CONFIG_APERTURE_HELPERS=y` 才有真实现

### 技巧 4：跳到 `.c` 后，立刻"上下浏览全文件"建立心智模型

按 `Ctrl+Shift+O` 看 aperture.c 的符号目录，能立刻看到围绕 `struct aperture_range` 展开的整个函数群。

**经验**：一个 `.c` 文件里围绕**一个数据结构**展开的函数群，构成一个"小子系统"。先识别核心数据结构（这里是 `struct aperture_range` 和全局 `apertures` 链表），然后把函数分类成"注册"和"销毁"两组，就能快速看懂。

### 技巧 5：用 Find References 反向追"谁调用了它"

光标在 `aperture_detach_platform_device` 上按 `Shift+F12`：只看到一个引用--`devm_aperture_acquire_for_platform_device` 把它作为 `detach` 回调传进去。

> 这种"通过 Find References 反查注册点"对**理解回调机制至关重要**。看到 `ap->detach(dev)` 时不知道它是啥，但 Find References 一查 `detach` 字段就知道是 `aperture_detach_platform_device`。

### 技巧 6：跳到结构体定义看字段是定位的关键

在 `ap->detach(dev)` 这一行，光标停在 `ap` 上按 `F12` 看不到结构体（因为 `ap` 是变量）；正确做法：

- 光标停在 `aperture_range` 这个**类型名**上按 `F12`
- 或者光标停在 `detach` 这个**字段名**上按 `F12`

跳到 `struct aperture_range { ... void (*detach)(...); ... }`，立刻明白 `detach` 是个函数指针，**再用 Find References 找谁给它赋了值**，就找到 `devm_aperture_acquire` 里的 `ap->detach = detach;`，再反追这个 `detach` 参数从哪传来。

> **C 内核的"虚函数"全靠函数指针字段**，看到 `xxx->func(...)` 就要：①跳到结构体看字段类型 ②Find References 找赋值点

### 技巧 7：宏 / `container_of` 类代码用 Peek 不要 F12

`container_of` 是个宏，`F12` 进去看的是宏定义，离主线远。**用 `Alt+F12` 弹窗 Peek 一眼即可**：知道它是"从结构体内某字段的指针反推回结构体起始指针"就行，不必跳进去。

### 技巧 8：IntelliSense 失败时退而用 grep / Go to Symbol

内核代码体量大、宏多、`CONFIG_xx` 条件多，IntelliSense（clangd / C/C++ 插件）经常跳错或不跳。常用退路：

1. `Ctrl+T` 全工程符号搜索：直接输函数名，按文件路径选 `.c` 那个
2. grep 搜定义模式：搜 `^\w+\s+函数名\s*\(` 强制找定义不找调用
3. 使用 `compile_commands.json`：让 clangd 知道用的是哪个 `.config`，可大幅减少跳错

### 技巧 9：用调用栈反推主线

- **Go Back** (`Alt+←`) / **Go Forward** (`Alt+→`)：在跳转历史里前进/后退
- VS Code 顶部的 **Breadcrumbs**：显示当前位置的文件->函数路径

记一条主线：

```
linlondp_bind
 └─ aperture_remove_all_conflicting_devices  (inline)
     └─ aperture_remove_conflicting_devices  (.c 真实现)
         └─ aperture_detach_devices
             └─ ap->detach()  -> aperture_detach_platform_device
                 └─ platform_device_unregister
```

迷路时 `Alt+←` 退回任一层，重新捋。

### 技巧 10：先看注释和函数 docstring 再读代码

内核函数定义上方的 `/** ... */` kernel-doc 注释通常已经说清楚设计意图。**先读注释 -> 再看实现**，可以在跳进 100 行代码前先建立预期。

### 练习路径

1. 光标停 linlondp_drv.c 的 `aperture_remove_all_conflicting_devices`，`F12` -> 落到 aperture.h inline
2. 在 inline 里光标停 `aperture_remove_conflicting_devices`，再 `F12` -> 落到 aperture.c
3. 落到 aperture.c 后 `Ctrl+Shift+O` 浏览整个文件结构
4. 在 `aperture_detach_devices` 里光标停 `aperture_range`，`F12` -> 看结构体
5. 光标停结构体里的 `detach` 字段，`Shift+F12` -> 看谁给它赋值 -> 跳到 `devm_aperture_acquire`
6. 光标停 `devm_aperture_acquire` 函数名，`Shift+F12` -> 看谁调用 -> 跳到 `devm_aperture_acquire_for_platform_device`
7. 再 `Shift+F12` -> 看谁调用了 acquire -> 跳到 efifb/simpledrm 等占位驱动（验证"谁先注册的"）
8. 路径上多次 `Alt+←` 退回主线，确认自己没迷路

完成这 8 步，就掌握了用 Go to Definition + Find References + Symbol Search 三件套阅读内核任意子系统的通用方法。
