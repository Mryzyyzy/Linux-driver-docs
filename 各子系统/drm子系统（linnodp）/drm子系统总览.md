# 系统学习 Linux DRM 子系统：路线图 + 定位指南

一份**可以照着走完的系统学习路线**：每个阶段列出 **学什么概念、对应哪些源码文件、用什么工具去自己定位、做什么实验验证**。

---

## 总览：DRM 三层结构

```text
┌──────────────────────────────────────────────────────────┐
│  用户态 (libdrm + Mesa + 合成器)                          │
│  /usr/lib/libdrm.so                                      │
├──────────────────────────────────────────────────────────┤  ioctl / mmap
│  DRM Core (内核通用框架)                                  │
│  drivers/gpu/drm/drm_*.c                                 │
│  include/drm/drm_*.h                                     │
├──────────────────────────────────────────────────────────┤  drm_driver 回调
│  Vendor Driver (厂商驱动)                                 │
│  drivers/gpu/drm/<vendor>/                               │
│  (你这里是 cix/linlon-dp/)                                │
├──────────────────────────────────────────────────────────┤  寄存器读写
│  硬件 IP                                                  │
└──────────────────────────────────────────────────────────┘
```

学习顺序就是**从中间层(Core)入手 → 向下看厂商驱动 → 向上对照用户态**。

---

## 阶段 0：建立"自己定位代码"的工具箱

在动手读代码前，先把这些定位手法练熟，整个学习过程会快 10 倍。

### 文件级定位

| 想找什么                  | 怎么定位                                                                                        |
| --------------------- | ------------------------------------------------------------------------------------------- |
| 某个**结构体定义**           | 光标停在类型名上 `F12`；或全工程符号搜 `Ctrl+T` 输入 `struct drm_crtc`                                        |
| 某个**函数的真正 .c 实现**     | `F12` 通常先跳到头文件声明，**再 F12 一次** 才到 .c；或在 drivers/gpu/drm/ 用 grep 正则 `^\w+\s+drm_xxx\s*\(` 找定义 |
| **谁调用了某函数 / 谁实现了某回调** | 光标停在符号上 `Shift+F12`（Find All References）                                                    |
| **某 ioctl 号对应内核哪个函数** | 在 drm_ioctl.c 里搜 `drm_ioctls[]` 表                                                           |
| **回调字段的赋值点**          | 光标停在 `.atomic_enable` 这种字段名上 `Shift+F12`                                                    |
| **某宏展开**              | `Alt+F12` Peek 一眼，不要 `F12` 进去深陷                                                             |

### 头文件 vs C 文件分工

```text
include/drm/drm_xxx.h     ← 结构体、函数原型、宏定义、kernel-doc
drivers/gpu/drm/drm_xxx.c ← 真正的实现
```

**先读 .h 看接口契约，再读 .c 看实现**。.h 顶部往往有大段 kernel-doc 注释解释整个子模块设计。

### 主线追踪法

为什么从 ioctl 入手？
因为整条链上几乎每个 `→` 都是**函数指针间接调用**，唯独 **ioctl 号 → 函数** 这一步是**静态 1:1 映射**，不用追指针、grep 必中。所以它是唯一适合"上车"的固定锚点：
[[为什么从 ioctl 入手最稳]]
随手一个 ioctl 入手，从 `drm_ioctl.c` 一直跟到驱动回调，就能贯穿一条主线。例如：

```
DRM_IOCTL_MODE_SETCRTC
  → drm_ioctl.c::drm_ioctls[] 表
  → drm_mode_setcrtc()                         (drm_crtc.c)
  → crtc->funcs->set_config()
  → drm_atomic_helper_set_config()             (drm_atomic_helper.c)
  → drm_atomic_commit()                        (drm_atomic.c)
  → drv->mode_config.funcs->atomic_commit()
  → linlondp_kms_atomic_commit                 (cix/linlon-dp/linlondp_kms.c)
  → crtc_helper_funcs->atomic_enable           (cix/linlon-dp/linlondp_crtc.c)
  → 寄存器写入                                  (cix/linlon-dp/hw/...)
```

**这条链路自己跟一遍**比看任何教程都管用。

---

## 阶段 1：DRM Core 入口 —— 设备模型 / 文件操作 / ioctl 分发

### 要理解的概念
- 一个 DRM 驱动如何向内核注册自己
- `/dev/dri/cardX` 是怎么产生的
- ioctl 是怎么从用户态分发到驱动

### 该读的文件（按顺序）

| 文件                          | 关键内容                                       | 推荐读法                            |
| --------------------------- | ------------------------------------------ | ------------------------------- |
| include/drm/drm_drv.h       | `struct drm_driver`（驱动入口结构）                | 看字段含义和 kernel-doc，建立"驱动要填什么"的全图 |
| drivers/gpu/drm/drm_drv.c   | `drm_dev_alloc`、`drm_dev_register`，设备注册主流程 | 跟 `drm_dev_register` 看注册过程      |
| drivers/gpu/drm/drm_file.c  | `drm_open`、`drm_release`，文件操作              | 看 `/dev/dri/cardX` 的 open 钩子    |
| drivers/gpu/drm/drm_ioctl.c | `drm_ioctls[]` 巨表，所有 ioctl 号到处理函数的映射       | **核心定位表**，以后查任何 ioctl 都从这里入手    |

### 验证实验
```bash
ls /sys/class/drm/                # 看注册的 card / connector
cat /sys/kernel/debug/dri/0/name  # 看驱动名
modetest -M linlondp              # 用户态查 driver 资源
```

---

## 阶段 2：KMS 五大对象 —— Framebuffer / Plane / CRTC / Encoder / Connector

### 要理解的概念
- 五个对象的职责和关系（你之前已经懂了，这里学源码结构）
- 每个对象都有 `funcs`（必填基础）和 `helper_funcs`（atomic 回调）两组钩子
- 都是 `drm_mode_object` 的派生

### 一个对象一组文件

| 对象 | 头文件（接口） | 源文件（实现） | 关键结构 |
|------|----------------|----------------|----------|
| **CRTC** | drm_crtc.h | drm_crtc.c | `struct drm_crtc`、`drm_crtc_funcs`、`drm_crtc_helper_funcs` |
| **Plane** | drm_plane.h | drm_plane.c | `struct drm_plane`、`drm_plane_funcs`、`drm_plane_helper_funcs` |
| **Encoder** | drm_encoder.h | drm_encoder.c | `struct drm_encoder` |
| **Connector** | drm_connector.h | drm_connector.c | `struct drm_connector`、`drm_connector_funcs` |
| **Framebuffer** | drm_framebuffer.h | drm_framebuffer.c | `struct drm_framebuffer` |
| **回调原型集合** | drm_modeset_helper_vtables.h | — | **所有 helper_funcs 在这里一次看全**——极重要的总览文件 |

### 自学手法
1. 打开 drm_crtc.h，搜 `struct drm_crtc {`，逐字段看 kernel-doc
2. 打开 drm_modeset_helper_vtables.h，搜 `drm_crtc_helper_funcs`，看每个 atomic_xxx 回调的语义
3. **找一个最简单的驱动作样板对照**：
   - drivers/gpu/drm/tiny/simpledrm.c — 几百行实现完整 DRM 驱动
   - drivers/gpu/drm/vkms/ — 纯虚拟 DRM，没有真实硬件，逻辑最干净

### 在 linlon-dp 里对照
| 对象 | 驱动文件 |
|------|----------|
| CRTC | cix/linlon-dp/linlondp_crtc.c |
| Plane | cix/linlon-dp/linlondp_plane.c |
| Connector | cix/linlon-dp/linlondp_wb_connector.c + cix/dptx/ |
| Framebuffer | cix/linlon-dp/linlondp_framebuffer.c |

---

## 阶段 3：Atomic Modesetting 流水线（现代 DRM 的灵魂）

### 要理解的概念
- 每个对象有 `state`（drm_xxx_state），所有修改在 state 副本上做，最后原子 swap
- 整个 commit 流程的三个阶段：**check → swap → commit**
- helper 层（`drm_atomic_helper_*`）是驱动复用的代码

### 源码定位

| 文件 | 内容 |
|------|------|
| include/drm/drm_atomic.h | `drm_atomic_state`、`drm_xxx_state` 结构 |
| drivers/gpu/drm/drm_atomic.c | 核心：commit 主流程 |
| drivers/gpu/drm/drm_atomic_uapi.c | atomic ioctl 入口（用户态请求转 state） |
| drivers/gpu/drm/drm_atomic_helper.c | **最重要的 helper 集合**，几乎所有驱动都依赖 |
| drivers/gpu/drm/drm_atomic_state_helper.c | state 的 duplicate/destroy helper |

### 自学手法
1. 在 drm_atomic_uapi.c 找 `drm_mode_atomic_ioctl`，跟一遍主流程
2. 沿调用链跳到 `drm_atomic_commit` → `drm_atomic_helper_commit` → `commit_tail`
3. 看 helper 函数的 kernel-doc 顶部注释——内核里 atomic 部分文档质量极高
4. 重要：阅读 drm_atomic_helper.c 顶部的大段 **DOC** 注释，整个 atomic 哲学讲得清清楚楚

### 实验
```bash
modetest -M linlondp -P <plane>@<crtc>:<w>x<h>+<x>+<y>@<fmt>   # 测 plane
echo 1 > /sys/kernel/debug/tracing/events/drm/enable
cat /sys/kernel/debug/tracing/trace_pipe                       # 看 atomic 流程
```

---

## 阶段 4：显存管理 —— GEM / DMA / shmem / dumb buffer

### 要理解的概念
- GEM 对象 = 显存抽象 + handle 化命名
- handle / fd / mmap offset 三种引用方式
- 三大 helper：DMA(CMA)、shmem、TTM 各自适用场景
- dumb buffer 是 KMS 配的"最简单显存"

### 源码定位

| 文件 | 内容 |
|------|------|
| include/drm/drm_gem.h | `struct drm_gem_object` + 接口 |
| drivers/gpu/drm/drm_gem.c | GEM 核心：alloc、handle、mmap 入口 |
| drivers/gpu/drm/drm_dumb_buffers.c | dumb buffer ioctl 处理 |
| drivers/gpu/drm/drm_gem_dma_helper.c | **CMA 后端**——嵌入式最常用，linlon-dp 就用这个 |
| drivers/gpu/drm/drm_gem_shmem_helper.c | **shmem 后端**——不要求物理连续 |
| drivers/gpu/drm/drm_vma_manager.c | mmap offset 管理（之前讲的 fake offset cookie） |
| drivers/gpu/drm/drm_prime.c | DMA-BUF 跨设备共享（PRIME） |

### 在 linlon-dp 里对照
cix/linlon-dp/linlondp_kms.c 里：

```c
DEFINE_DRM_GEM_DMA_FOPS(linlondp_cma_fops);          // 用 CMA helper 的 fops
DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(linlondp_gem_dma_dumb_create), // dumb 走 CMA
```

→ 顺着 `DEFINE_DRM_GEM_DMA_FOPS` 跳到 drm_gem_dma_helper.h，看宏展开成什么 fops。

---

## 阶段 5：VBLANK / 事件 / PageFlip

### 要理解的概念
- vblank 中断驱动整个软件节拍
- DRM event = 通过 file 读出的异步事件（PageFlip 完成、vblank 等）

### 源码定位
| 文件 | 内容 |
|------|------|
| include/drm/drm_vblank.h | vblank API |
| drivers/gpu/drm/drm_vblank.c | `drm_crtc_handle_vblank`、`drm_wait_vblank_ioctl` |
| drivers/gpu/drm/drm_file.c | `drm_send_event` 系列，看事件如何送达用户态 |

---

## 阶段 6：Mode 与显示输出 —— mode 协商 / EDID / Bridge / Panel

### 要理解的概念
- Mode 从哪来：EDID 解析 vs panel 内建
- Bridge 链：CRTC→Encoder→[Bridge]+→Connector，DP/HDMI/DSI 多级转换
- Panel：固定显示器（eDP/LVDS）

### 源码定位
| 文件 | 内容 |
|------|------|
| drivers/gpu/drm/drm_modes.c | mode 操作 |
| drivers/gpu/drm/drm_edid.c | EDID 解析 |
| drivers/gpu/drm/drm_probe_helper.c | connector mode 探测 helper |
| drivers/gpu/drm/drm_bridge.c | Bridge 链管理 |
| drivers/gpu/drm/bridge/ | 各种 bridge 实现（DP/HDMI 转换桥） |
| drivers/gpu/drm/panel/ | 固定 panel 驱动 |

---

## 阶段 7：厂商驱动整体阅读（用 linlon-dp 实战）

把前面学的所有概念**对照真实驱动检查一遍**，每个文件能说出"它对应核心层的哪个机制"。

| linlon-dp 文件 | 对应 Core 概念 |
|----------------|----------------|
| linlondp_drv.c | 阶段 1：platform_driver + component + drm 设备注册 |
| linlondp_kms.c | 阶段 1+3：`drm_driver` 注册、atomic_commit 主流程 |
| linlondp_crtc.c | 阶段 2+3：CRTC + atomic helper_funcs |
| linlondp_plane.c | 阶段 2+3：Plane + atomic_update（写 fb 物理地址到寄存器） |
| linlondp_framebuffer.c | 阶段 4：fb 创建验证（pitch/format/modifier） |
| linlondp_wb_connector.c | 阶段 6：writeback connector（一种特殊 connector） |
| linlondp_pipeline.c | 厂商私有：硬件流水线建模 |
| linlondp_color_mgmt.c | gamma / CTM 色管 |
| linlondp_event.c | 阶段 5：vblank / flip 事件 |
| hw/ | 硬件抽象层（你正打开的 dp_dev.h 所在地） |

### 推荐读法
**走两遍**：
1. **第一遍主线**：`linlondp_platform_probe` → `linlondp_bind` → `linlondp_dev_create` → `linlondp_kms_attach`，看驱动起来的全过程
2. **第二遍业务**：拿 demo 触发的"SetCrtc → atomic_enable → 写寄存器"链路追到 `hw/` 里看寄存器操作

---

## 阶段 8：用户态对照 —— libdrm / modetest / Mesa / 合成器

### 该读 / 该用
| 项目 | 作用 |
|------|------|
| **libdrm** [https://gitlab.freedesktop.org/mesa/drm](https://gitlab.freedesktop.org/mesa/drm) | ioctl 的薄封装，所有 `drmModeXxx` 函数实现 |
| **modetest** (libdrm 自带) | 板子上现成的 KMS 测试工具，最权威用法范例 |
| **drm-howto** [https://github.com/dvdhrm/docs](https://github.com/dvdhrm/docs) | 几百行的教学 demo 代码 |
| **kmscube** [https://gitlab.freedesktop.org/mesa/kmscube](https://gitlab.freedesktop.org/mesa/kmscube) | KMS + GBM + EGL 整套示例 |
| **Weston (libweston/backend-drm)** | 工业级合成器，看 DRM 实际复杂用法 |

### 实验组合
```bash
# 查能力
modetest -M linlondp                              # 列出所有 resource
cat /sys/kernel/debug/dri/0/state                 # 当前 atomic state
cat /sys/kernel/debug/dri/0/framebuffer           # 当前 fb 列表

# 跟内核流程
echo 1 > /sys/kernel/debug/tracing/events/drm/enable
echo 1 > /sys/kernel/debug/tracing/tracing_on
modetest -M linlondp -s <conn>@<crtc>:<mode>      # 触发 SetCrtc
cat /sys/kernel/debug/tracing/trace                # 看每个 KMS 事件
```

---

## 阶段 9：进阶专题（按需深入）

| 主题 | 入口文件 |
|------|----------|
| DMA-BUF 跨设备共享 | drm_prime.c + `drivers/dma-buf/` |
| GPU 渲染 (GEM 高级用法) | i915 / amdgpu / panfrost 任一现代 GPU 驱动 |
| TTM 显存管理 (独显) | `drivers/gpu/drm/ttm/` |
| Color management 详解 | drm_color_mgmt.c |
| Writeback connector | drm_writeback.c |
| VRR / Adaptive sync | grep `vrr_` in drm |
| HDR / metadata | grep `hdr_output_metadata` in drm |
| Modifier / AFBC 等帧格式 | drm_fourcc.c + `include/uapi/drm/drm_fourcc.h` |

---

## 学习方法总结：三条铁律

1. **概念 ↔ 文件 ↔ 实验三结合**
   - 学到一个概念 → 立刻打开它的源码看实现 → 在板子上跑命令观察现象
   - 三者闭环才能记住

2. **任何陌生函数都从 `.h` 读 kernel-doc 开始**
   - 不要直接 `F12` 跳进 `.c` 看实现
   - 顶部的 `/** DOC: ... */` 大段注释是设计者亲笔解释设计意图，价值远高于看代码

3. **始终走主线，迷路 `Alt+←` 退回**
   - 永远在脑里维护一条"用户态调用 → ioctl → core → driver → 寄存器"的主线
   - 偶尔跳出去看辅助函数，结束后立刻退回主线

---

## 一份具体的两周学习计划（按需调整）

| 阶段 | 目标 | 读什么 | 实验 |
|------|------|--------|------|
| Day 1-2 | DRM 入口 | drm_drv.c / drm_file.c / drm_ioctl.c | `strace -e ioctl modetest` |
| Day 3-4 | KMS 五大对象 | drm_crtc.h/.c, drm_plane.h/.c, drm_connector.h | modetest 列资源 |
| Day 5-6 | Atomic helper | drm_atomic_helper.c 顶部 DOC + simpledrm | enable ftrace + modetest -P |
| Day 7 | GEM / dumb / mmap | drm_gem.c + drm_gem_dma_helper.c | 跑你那份 demo 代码 |
| Day 8 | VBLANK / event | drm_vblank.c | modetest -v (PageFlip 循环) |
| Day 9-10 | EDID / Bridge / Panel | drm_probe_helper.c + bridge/ | 拔插 HDMI 看 sysfs 变化 |
| Day 11-14 | 通读 linlon-dp 驱动 | cix/linlon-dp/ 全部 | 加 pr_info 调试 + ftrace 看完整链路 |

---

## 概念对照清单

读到对应代码可直接对号入座：

- [ ] CRTC / Encoder / Connector / Plane / Framebuffer 概念
- [ ] dumb buffer + mmap 原理
- [ ] mmap offset 是 cookie 不是地址
- [ ] 缺页处理 + 伙伴系统 vs CMA
- [ ] VBLANK 与撕裂 / PageFlip / Atomic
- [ ] `DRM_CLIENT_CAP_ATOMIC` 的含义
- [ ] aperture 子系统清场机制
- [ ] YUV multi-plane 内存布局
- [ ] `drm_driver` 结构和注册（在 linlondp_kms.c 真实看过）

按照上面 9 个阶段顺序，把每个阶段都对照源码走一遍，就能从"会用 demo"进阶到"读懂任何 DRM 驱动"。