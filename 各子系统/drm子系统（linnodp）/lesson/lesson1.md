# DRM 课程 · 第 0001 课 · 基础

# DRM 三层架构与设备节点

你在 CIX Sky1 上把显示打通 Xen dom0 时，改的其实是一个 DRM 厂商驱动（`linlondp_kms.c`）——给它接上 fbdev 模拟，控制台才能在 Weston 启动前亮起来。要理解这个改动为什么有效、为什么 6.12 要把 `.lastclose` 换成 `.fbdev_probe`，得先看清 DRM 整体是怎么分层的。这一课只做一件事：建立 **三层** 的全局地图，并能在你自己的源码树里指出每一层落在哪。

```text
┌──────────────────────────────────────────────────────────┐
│ 用户态 libdrm / Mesa / 合成器(Weston)                    │
│ /usr/lib/libdrm.so                                       │
├──────────────────────────────────────────────────────────┤ ← ioctl / mmap
│ DRM Core 内核通用框架（与硬件无关）                       │
│ drivers/gpu/drm/drm_*.c ← include/drm/*.h                │
├──────────────────────────────────────────────────────────┤ ← drm_driver 回调
│ 厂商驱动 针对具体硬件 IP                                 │
│ drivers/gpu/drm/cix/linlon-dp/ (linlondp)                │
├──────────────────────────────────────────────────────────┤ ← 寄存器读写
│ 硬件 IP DPU / DP PHY / GOP ...                           │
└──────────────────────────────────────────────────────────┘
```

## 三层各是什么

### 用户态

应用通过 `libdrm` 封装的 ioctl 访问 DRM。合成器（Weston）用 `libdrm` 做 page flip、提交 atomic state。这一层我们只关心一件事：它 **怎么调进来** ——即 ioctl 号如何穿过 Core 到达厂商驱动。后续课程会从 ioctl 入手追踪这条链。
[[weston追到linlondp链路]]
### DRM Core

位于 `drivers/gpu/drm/drm_*.c` 与 `include/drm/*.h`，与具体硬件无关。它做三件通用的事：

- **设备模型**：注册字符设备，产生 `/dev/dri/cardX`（`drm_drv.c`）。
    
- **文件操作**：`open/release/mmap` 钩子（`drm_file.c`）。
    
- **ioctl 分发**：一张 `drm_ioctls[]` 巨表把 ioctl 号静态映射到处理函数（`drm_ioctl.c`）。  
    Core 里还分出一个子部分 **KMS**（Kernel Mode Setting），专门管“显示管线配置”。Core 是大框架，KMS 是其中负责把 framebuffer 推到屏幕的那块，后面单独讲。
    

### 厂商驱动

针对具体硬件 IP，位于 `drivers/gpu/drm/cix/linlon-dp/`。它不直接处理 ioctl，而是 **填写一个入口结构体 `struct drm_driver`**，声明自己支持哪些能力、提供哪些回调；Core 在需要时通过函数指针回调到厂商驱动。这就是“分层”的接缝。

## 接缝长什么样：linlondp 的 `drm_driver`

在你的源码树里，`linlondp_kms.c` 第 68 行就是这个接缝：

```c
static struct drm_driver linlondp_kms_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	...
	.lastclose       = drm_fb_helper_lastclose,   /* 第 71 行；6.12+ 会被换成 .fbdev_probe */
	...
};
```

这个结构体被两处使用，构成厂商驱动接入 Core 的完整生命周期：

```c
/* linlondp_kms.c:463 —— 分配 drm_device，绑定到 platform device */
kms = devm_drm_dev_alloc(mdev->dev, &linlondp_kms_driver, ...);
/* linlondp_kms.c:511 —— 注册，此时 /dev/dri/cardX 才出现 */
err = drm_dev_register(drm, 0);
```

Mission 锚点第 71 行的 `.lastclose` 正是你 patches 里要改的那行。在 6.12+ 内核上，旧的 `drm_fb_helper_lastclose` 机制被移除，驱动若不提供 `.fbdev_probe`，fbdev 模拟就不会被实例化——控制台就亮不起来。这是后面“fbdev 模拟”那课的核心，现在只需记住：**这个结构体就是厂商驱动和 Core 之间的合同**。

## 设备节点：`/dev/dri/cardX` 与 `/sys/class/drm`

当 `drm_dev_register()` 跑完，内核会创建字符设备 `/dev/dri/card0`，并在 `/sys/class/drm/` 下露出 connector 等 KMS 对象的属性。用户态一切访问都从这个设备节点进入，再被 Core 的 ioctl 分发表路由出去。**这是用户态和内核态之间唯一的门。**

## 动手 · 在你的树里找到这三层

打开你的源码树，确认下面三件事：

1. Core 的 ioctl 分发表：在 `drivers/gpu/drm/drm_ioctl.c` 里搜 `drm_ioctls[]`。
    
2. 厂商驱动的入口：打开 `drivers/gpu/drm/cix/linlon-dp/linlondp_kms.c`，定位第 68 行的 `linlondp_kms_driver`。
    
3. 看一个最小 DRM 驱动长什么样：打开 `drivers/gpu/drm/tiny/` 里的一个，比如 `simpledrm.c` 或 `gm12u320.c`，找它的 `drm_driver`。  
    对比 linlondp，你会发现 tiny 驱动小得多——这正是官方推荐的入门读物。
    

## 小测 · 检验分层

1. `libdrm.so` 属于三层中的哪一层？  
    答案：**用户态**
    
2. `drivers/gpu/drm/drm_ioctl.c` 属于哪一层？  
    答案：**核心层**
    
3. `drivers/gpu/drm/cix/linlon-dp/` 属于哪一层？  
    答案：**驱动层**
    

## 推荐主源

这一课的主源是 kernel.org 的 DRM Introduction：[https://docs.kernel.org/gpu/introduction.html](https://docs.kernel.org/gpu/introduction.html)。官方对 DRM Core 的定位，并明确推荐从 `drivers/gpu/drm/tiny/` 单文件驱动读起。辅读你自己的笔记：`/home/q/workspace/note/04_学习-刷题/各子系统/drm子系统（linnodp）/drm子系统总览.md`。  
本课属于 **DRM 基础**。下一课：**KMS 五大对象**（fb / plane / crtc / encoder / connector）。参考：**DRM 分层速查**（待建）。