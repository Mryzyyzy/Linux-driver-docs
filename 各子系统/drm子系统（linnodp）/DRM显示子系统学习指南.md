# DRM 显示子系统学习指南

> **一句话总结**：DRM（Direct Rendering Manager）是 Linux 现代显示子系统，通过 KMS（Kernel Mode Setting）管理显示控制器的模式设置、帧缓冲、平面、合成、VBLANK 事件，配合 GEM（Graphics Execution Manager）管理显存，为用户态提供统一的 ioctl 接口。

---

## 📋 目录

- [DRM 三层结构](#drm-三层结构)
- [KMS 五大核心对象](#kms-五大核心对象)
- [Atomic Modesetting 流水线](#atomic-modesetting-流水线)
- [显存管理：GEM / DMA / CMA / dumb buffer](#显存管理gem--dma--cma--dumb-buffer)
- [DRM mmap 详解](#drm-mmap-详解)
- [VBLANK 与事件机制](#vblank-与事件机制)
- [Linlon-DP 驱动结构](#linlon-dp-驱动结构)
- [调试与工具](#调试与工具)
- [面试要点总结](#面试要点总结)

---

## DRM 三层结构

```
┌──────────────────────────────────────────────────────────┐
│  用户态（libdrm + Mesa + Weston / Xorg）                   │
│  /usr/lib/libdrm.so                                        │
├──────────────────────────────────────────────────────────┤  ioctl / mmap
│  DRM Core（内核通用框架）                                  │
│  drivers/gpu/drm/drm_*.c                                   │
│  include/drm/drm_*.h                                       │
├──────────────────────────────────────────────────────────┤  drm_driver 回调
│  Vendor Driver（厂商驱动）                                 │
│  drivers/gpu/drm/<vendor>/                                 │
│ （CIX 平台是 linlon-dp / komeda）                          │
├──────────────────────────────────────────────────────────┤  寄存器读写
│  硬件 IP（DPU / GPU）                                      │
└──────────────────────────────────────────────────────────┘
```

### 为什么从 ioctl 入手最稳？

整条链上几乎每个 `→` 都是函数指针间接调用，唯独 **ioctl 号 → 函数** 这一步是**静态 1:1 映射**，grep 必中。所以它是唯一适合"上车"的固定锚点。

典型链路：
```
DRM_IOCTL_MODE_SETCRTC
  → drm_ioctl.c::drm_ioctls[] 表
  → drm_mode_setcrtc()                         (drm_crtc.c)
  → crtc->funcs->set_config()
  → drm_atomic_helper_set_config()             (drm_atomic_helper.c)
  → drm_atomic_commit()                        (drm_atomic.c)
  → drv->mode_config.funcs->atomic_commit()
  → linlondp_kms_atomic_commit                 (linlon-dp)
  → crtc_helper_funcs->atomic_enable
  → 写 DPU 寄存器
```

---

## KMS 五大核心对象

KMS（Kernel Mode Setting）是 DRM 的核心功能，负责显示模式设置。它由五个核心对象组成显示流水线。

### 对象关系图

```
Framebuffer (像素数据来源)
      │
      ↓
Plane (图层、缩放、格式转换)
      │
      ↓
CRTC (显示控制器、扫描出、时序)
      │
      ↓
Encoder (编码输出格式：DP / HDMI / LVDS)
      │
      ↓
Connector (物理连接器、EDID 读取、Hotplug)
```

### 逐个详解

#### 1. Framebuffer (FB)

- **作用**：像素数据的容器，指向显存
- **核心属性**：宽度、高度、像素格式（如 XR24）、pitch（行跨距）
- **关键函数**：`drm_mode_addfb()` / `drm_mode_addfb2()`
- **注意**：FB 本身不分配显存，只是描述显存布局。显存由 GEM 对象提供。

#### 2. Plane（平面）

- **作用**：一个独立的图像图层，可以叠加、缩放、颜色转换
- **类型**：
  - Primary plane：主图层，通常全屏
  - Cursor plane：鼠标光标图层，通常小
  - Overlay plane：视频等叠加图层
- **关键能力**：Z-order（前后顺序）、缩放、裁剪、格式转换
- **关键回调**：`atomic_check()` / `atomic_update()` / `atomic_disable()`

#### 3. CRTC

- **作用**：显示控制器核心，负责从 Plane 读取像素、生成显示时序、扫描输出
- **核心属性**：模式（分辨率 + 刷新率）、VBLANK 中断源
- **关键回调**：`atomic_enable()` / `atomic_disable()` / `vblank()`
- **一句话**：CRTC 是"节拍器"，决定了整个显示流水线的节奏。

#### 4. Encoder

- **作用**：把 CRTC 的像素流编码成特定输出格式（DP / HDMI / LVDS / MIPI）
- **注意**：现代驱动中 Encoder 经常和 Connector 合并逻辑，不再单独操作

#### 5. Connector

- **作用**：代表物理连接器，处理热插拔、EDID 读取、模式探测
- **类型**：`DRM_MODE_CONNECTOR_DisplayPort` / `HDMI-A` / `eDP` / `LVDS` 等
- **关键回调**：`detect()` / `get_modes()` / `set_property()`
- **特殊类型**：Writeback Connector —— 把显示内容写回内存（录屏用）

### 所有对象的共性

每个 KMS 对象都有：
1. `funcs` 基础方法表（必选）
2. `helper_funcs` atomic 回调表（现代驱动）
3. `state` 状态对象（atomic 模式下）

都是 `struct drm_mode_object` 的"派生类"（通过内嵌实现）。

---

## Atomic Modesetting 流水线

现代 DRM 驱动都用 Atomic API。这是理解现代 DRM 的钥匙。

### 核心思想

> **所有修改先在副本上做，验证全部通过后，原子地一次性切换到新状态。**

好处：
- 不会出现"半设置好"的中间状态
- 所有资源依赖可以提前 check
- 失败可以完整回滚
- 支持异步提交

### 三个阶段

```
1. Check 阶段
   ├─ 复制所有对象的 state 到一个 atomic_state
   ├─ 每个 driver 实现 atomic_check() 做合法性校验
   ├─ 任意一个对象 check 失败 → 整体失败，全部回滚
   ↓
2. Prepare 阶段
   ├─ 分配需要的资源（如新的 FB 内存）
   ├─ 准备硬件需要的配置
   ↓
3. Commit 阶段
   ├─ atomically swap state 指针（所有状态一次性切换）
   ├─ 写硬件寄存器
   └─ VBLANK 到来时完成 page flip
```

### 关键数据结构

```c
struct drm_atomic_state {
    // 包含所有对象的新旧 state 指针
    struct drm_crtc_state **crtcs;
    struct drm_plane_state **planes;
    struct drm_connector_state **connectors;
    ...
};

struct drm_crtc_state {
    bool active;
    bool enable;
    struct drm_display_mode mode;
    struct drm_plane_state *plane_state[];  // 关联的 plane
    ...
};

struct drm_plane_state {
    struct drm_framebuffer *fb;  // 用哪个 FB
    struct drm_rect src;         // 源裁剪矩形
    struct drm_rect dst;         // 目标位置大小
    ...
};
```

### 关键文件

| 文件 | 作用 |
|------|------|
| `drm_atomic.c` | Atomic 核心：`drm_atomic_commit()`、state 管理 |
| `drm_atomic_helper.c` | **最重要的 helper 集合**，几乎所有驱动都依赖 |
| `drm_atomic_uapi.c` | Atomic ioctl 入口（用户态请求转 state） |
| `drm_atomic_state_helper.c` | state 的 duplicate/destroy helper |

> **必读**：`drm_atomic_helper.c` 顶部的大段 DOC 注释，整个 atomic 哲学讲得清清楚楚。

---

## 显存管理：GEM / DMA / CMA / dumb buffer

### GEM 是什么？

GEM（Graphics Execution Manager）是 DRM 的显存管理框架。

核心思想：
- **抽象**：GEM 对象 = 一块显存 + handle 化命名
- **跨进程**：同一个 GEM 对象可以在多个进程间共享（通过 PRIME fd）
- **统一管理**：不管显存是在 VRAM 还是在 CMA，上层接口统一

### 三种 GEM 后端对比

| 后端 | 文件 | 物理连续性 | 适用场景 |
|------|------|------------|---------|
| **DMA/CMA** | `drm_gem_dma_helper.c` | ✅ 连续 | 嵌入式显示控制器（DMA 要求连续）、linlon-dp |
| **SHMEM** | `drm_gem_shmem_helper.c` | ❌ 不连续 | GPU 渲染 buffer、不喂 DMA 的场景 |
| **TTM** | `ttm/` | 混合 | 独立显卡（有 VRAM）、AMD/NVIDIA |

> CIX linlon-dp 用的是 **DMA/CMA 后端**，因为 DPU 的 DMA 要求物理连续地址。

### Dumb Buffer：最简单的显存

Dumb buffer 是 DRM 规范定义的"最简单显存"，给用户态 modetest / fbdev 用。

```c
// 用户态 ioctl DRM_IOCTL_MODE_CREATE_DUMB
// → 内核 drm_mode_create_dumb()
// → 驱动 .dumb_create() 回调
// → 分配 CMA 内存
```

特点：
- 必然是物理连续的（因为显示 DMA 要）
- 格式通常是 XR24（32-bit RGBA）
- 可以 mmap 到用户态直接写像素

### CMA 是什么？

CMA（Contiguous Memory Allocator）是内核预留的一块**物理连续内存区**，专门给 DMA 设备用。

```
启动参数：cma=256M

启动时：预留 256M 物理连续内存，buddy system 不能用
运行时：drm_gem_dma_alloc() 从 CMA 里分配
```

为什么需要 CMA？
- 系统跑久了物理内存碎片化，很难再分配大块连续内存
- 显示控制器 DMA 往往要求物理连续（没有 IOMMU 映射时）
- 所以启动时就预留好，专专用

---

## DRM mmap 详解

### 两种 mmap 策略

在 kernel 驱动中，实现 mmap 离不开两个关键维度：

| 维度 | 方式 | 代表函数 |
|------|------|---------|
| **怎么映射** | 一次性映射 | `remap_pfn_range()` |
| | Page Fault 缺页映射 | `vm_insert_page()` |
| **何时分配** | mmap 之前分配 | `kzalloc()` / `cma_alloc()` |
| | mmap 过程中分配 | mmap 回调里分配 |
| | fault 中分配 | 缺页时现场分配 |

### 1. 两种映射方式对比

#### 一次性映射（remap_pfn_range）

在 mmap 回调里**一口气把整块内存的页表全部建好**，函数返回时映射已完整。

```c
static int my_mmap(struct file *file, struct vm_area_struct *vma)
{
    return remap_pfn_range(vma, vma->vm_start,
                           pfn, size, vma->vm_page_prot);
}
```

- 用户拿到地址后**第一次访问就不缺页**
- **要求物理连续**（remap_pfn_range 按连续 PFN 区间填表）
- 适合：显示 framebuffer、寄存器 MMIO、物理连续 DMA 缓冲

#### Page Fault 缺页映射（vm_insert_page）

mmap 回调里**不建映射**，只登记一个 `.fault` 钩子。用户**访问到哪一页**，才触发缺页异常，在 fault 里**补那一页**。

```c
static vm_fault_t my_fault(struct vm_fault *vmf)
{
    struct page *page = find_page(vmf->pgoff);
    return vmf_insert_pfn(vmf->vma, vmf->address,
                          page_to_pfn(page));
}
```

- 用户访问新页 → 缺页 → fault → 补页，**缺哪补哪**
- **允许物理不连续**（每页单独插）
- 适合：大 buffer、可能用不满、物理散乱的内存（如 shmem、GPU 渲染 buffer）

### 2. 三种分配时机

| 时机 | 说明 | 代表 |
|------|------|------|
| **mmap 之前** | 驱动初始化/CreateDumb 时就分配，mmap 只负责映射 | DRM dumb buffer、固定 framebuffer |
| **mmap 过程中** | mmap 回调里分配，然后立即映射 | 按需分配的小缓冲 |
| **fault 中** | 访问到某页才现场分配这一页 | 大稀疏映射、shmem |

### 3. 典型组合

| 组合 | 映射方式 | 分配时机 | 代表场景 |
|------|----------|----------|---------|
| 最"勤快" | 一次性 | mmap 前 | DRM 显示 framebuffer（要喂硬件 DMA，访问不能缺页） |
| 中间型 | 一次性 | mmap 中 | 按需分配的连续小缓冲 |
| 最"懒惰" | Page Fault | fault 中 | GPU 渲染 buffer、shmem、普通用户内存 |

### DRM 中的实际应用

```text
DRM dumb buffer (linlon-dp 显示用):
   分配时机 = CREATE_DUMB 时 (用到才分配但早于 mmap)
   映射方式 = 一次性 (drm_gem_dma_mmap → remap_pfn_range)
   原因：显示控制器 DMA 要物理连续 + 访问不能缺页

DRM shmem buffer (GPU 渲染用):
   分配时机 = 接近缺页分配 (get_pages 懒分配)
   映射方式 = Page Fault
   原因：buffer 大、可不连续、用不满
```

> **这就是为什么你在 Xen 下需要 patch 0002 保护 CMA 区域**：
> CMA 是物理连续的，而且正好落在 dom0 RAM 中间。如果 Xen 把它按 MMIO 映射了，或者 place_modules() 把 DTB/initrd 放进去了，DMA 就访问不了。

---

## VBLANK 与事件机制

### VBLANK 是什么？

显示器逐行扫描，扫完一帧最后一行到开始下一帧第一行之间的**空白期**叫 VBLANK（Vertical Blank）。

```
─────────────  上一帧最后一行
             │
    VBLANK   │  这段时间不扫描
             │
─────────────  下一帧第一行
```

VBLANK 期间切换 framebuffer**不会出现撕裂**（上半屏旧帧，下半屏新帧）。

### DRM Event 机制

用户态程序（如 Weston）不想轮询等 VBLANK，它想**睡等中断通知**。这就是 DRM Event。

完整流程：

```
Weston 调用 DRM_IOCTL_WAIT_VBLANK / DRM_IOCTL_MODE_PAGE_FLIP
    ↓
内核注册一个 event，把当前进程的 DRM 文件描述符挂到等待队列
    ↓
CPU 继续运行别的任务
    ↓
[ 硬件 VBLANK 中断来了 ]
    ↓
DPU ISR → drm_crtc_handle_vblank()
    ↓
遍历所有等待该 CRTC 的 event
    ↓
drm_send_event() → 给文件描述符发 POLLIN 信号
    ↓
用户态 select/poll/epoll 唤醒
    ↓
Weston 读 DRM event，知道可以开始画下一帧了
```

### 关键代码路径

```c
// 驱动调用：VBLANK 中断里通知 DRM Core
drm_crtc_handle_vblank(crtc);

// DRM Core 内部
drm_handle_vblank()
  → drm_send_event()
    → wake_up_interruptible() 唤醒等待的用户态
```

---

## Linlon-DP 驱动结构

CIX Sky1 的显示控制器叫 Linlon-DP（也叫 Komeda），代码位于 `drivers/gpu/drm/cix/linlon-dp/`。

### 整体结构

```
linlondp_drv.c     ── platform_driver + component_master，绑定/解绑
   │
   ├─ linlondp_dev.c/.h   ── struct linlondp_dev：核心 device 抽象
   │      ├─ 时钟/复位/IRQ/IOMMU/SMCCC（GOP、MMHUB 恢复）
   │      ├─ format_table、debugfs、sysfs
   │      └─ chip 识别 → 调用 hw/dp_dev.c 的 *_identify()
   │
   ├─ linlondp_pipeline.[ch]            ── 抽象 pipeline / component 拓扑
   ├─ linlondp_plane.c / crtc.c / kms.c ── DRM KMS 对接
   ├─ linlondp_wb_connector.c           ── Writeback 连接器
   ├─ linlondp_framebuffer.[ch]         ── FB/AFBC 描述
   ├─ linlondp_color_mgmt.* / coeffs.*  ── CSC / Gamma / Degamma
   ├─ linlondp_event.c                  ── 中断事件分发
   └─ hw/
        ├─ dp_dev.c/.h    ── chip 实现：寄存器读写、irq_handler、flush
        ├─ dp_axi.c       ── AXI/SMMU 相关初始化
        ├─ dp_component.c ── 各级 HW component（LPU/CU/IPS/Scaler/Compiz）
        └─ dp_regs.h      ── 寄存器宏定义
```

### 与 Xen 相关的关键代码

#### 1. GOP 接管（保留 UEFI framebuffer）

```c
// linlondp_dev.h: CIX_SIP_DP_GOP_CTRL (0xc200000f)
// hw/dp_dev.c: close_gop() 系列函数
```

触发 SMC 关闭 GOP。dom0 走 Xen 时需要 Xen 转发 SiP SMC。GOP framebuffer 物理地址段必须在 dom0 reserved-memory 中可访问。

> **这就是你 patch 0004 的由来**：把 GOP status page 映射给 dom0。

#### 2. SMMU / IOMMU

`hw/dp_axi.c`、`linlondp_dev_funcs.connect_iommu`

Xen 下 SMMU 由 hypervisor 管，dom0 dma 走 swiotlb 或 Xen-IOMMU 直通。CIX 上常需要 SMMU-v3 bypass。

> **这就是你 patch 0005 的相关点**：bypass 模式下 DPU DMA 才能正常工作。

#### 3. reserved-memory (CMA/dma-pool)

DPU 显存大块往往位于 reserved-memory。如果这块区域被 Xen 当成 MMIO 映射了，或者 DTB/initrd 放进去了，显示就会异常。

> **这就是你 patch 0002 的相关点**：保护 reserved-memory 的 P2M 映射。

---

## 调试与工具

### 必备工具

| 工具 | 用途 | 命令示例 |
|------|------|---------|
| **modetest** (libdrm 自带) | KMS 功能测试、模式设置、平面测试 | `modetest -M linlondp` |
| **kmscube** | KMS + GBM + EGL 完整示例 | `kmscube -M linlondp` |
| **debugfs** | 内核内部状态 | `cat /sys/kernel/debug/dri/0/state` |
| **ftrace** | 函数调用流 | `echo 1 > /sys/kernel/debug/tracing/events/drm/enable` |

### debugfs 常用文件

```bash
# 当前 atomic state（所有对象的状态快照）
cat /sys/kernel/debug/dri/0/state

# 当前 framebuffer 列表
cat /sys/kernel/debug/dri/0/framebuffer

# 各 plane 的状态
cat /sys/kernel/debug/dri/0/plane

# 厂商自定义寄存器 dump（linlon-dp 特有）
cat /sys/kernel/debug/linlondp0/register
```

### 常见现象排查

| 现象 | 可能原因 | 排查点 |
|------|----------|--------|
| **黑屏无输出** | 1. CRTC 没 enable<br>2. Plane 没贴 FB<br>3. DP 链路训练失败 | `/sys/kernel/debug/dri/0/state` 看 active 位；读 DP 寄存器 |
| **画面撕裂** | PageFlip 不在 VBLANK 期间完成 | 看 VBLANK 中断计数是否涨 |
| **画面卡顿** | 原子 commit 太慢 / GPU 渲染太慢 | ftrace 看 `drm_atomic_commit` 耗时 |
| **mmap 后访问 panic** | P2M 映射不对 / 内存被 reserved 了 | Xen 下看 EPT  violation；dump P2M 页表 |
| **Weston 启动失败** | DRM_CAP_ATOMIC 没设 / 没有合适的 plane/format | `strace -e ioctl weston` 看哪个 ioctl 失败 |

---

## 面试要点总结

### 1. KMS 基础概念

| 问题 | 标准回答 |
|------|----------|
| **KMS 五大对象？关系？** | Framebuffer（像素）→ Plane（图层）→ CRTC（时序）→ Encoder（编码）→ Connector（物理输出）。从内存到显示器的流水线。 |
| **Atomic API 比 legacy 好在哪？** | 1. 所有修改原子提交，无中间状态；2. 提前 check，失败可完整回滚；3. 支持异步提交；4. 多对象状态一致性有保证。 |
| **为什么需要 VBLANK？** | 显示器逐行扫描，VBLANK 期间切换 FB 不会出现撕裂（上半屏旧帧下半屏新帧）。 |
| **CMA 是什么？为什么需要？** | 启动时预留的物理连续内存区。系统跑久了内存碎片化，DMA 设备（如显示控制器）要大块连续物理内存，只能从 CMA 里分。 |

### 2. mmap 深度理解

| 问题 | 要点 |
|------|------|
| **remap_pfn_range vs vm_insert_page？** | remap 是一次性建整张页表，要求物理连续，适合显示 FB；vm_insert_page 是缺页时单页插，允许物理不连续，适合 GPU 渲染 buffer。 |
| **为什么 drm_gem_dma 用 remap？** | 显示控制器 DMA 要求物理连续，而且缺页会导致显示 underflow（画面撕裂/闪）。必须 mmap 时就建好所有页表。 |
| **mmap offset cookie 是什么？** | mmap 的 offset 参数不是物理地址偏移，而是 DRM 内部用来找哪个 GEM 对象的 cookie。这是 DRM 经典面试题。 |

### 3. 调试经验

> **面试官最爱问**："Weston 起来了但是黑屏，怎么查？"

标准回答框架（由外向内）：
1. **先看 sysfs 状态**：`cat /sys/kernel/debug/dri/0/state` 看 CRTC active 吗？plane 的 fb 对吗？
2. **再看硬件**：读 DPU 寄存器，确认帧缓冲地址、使能位、DP 链路状态
3. **再看中断**：VBLANK 中断计数涨不涨？
4. **最后看用户态**：Weston log 里有没有 drmModeAtomicCommit 失败？有没有选对 connector？

> **加分项**：提到 `/sys/class/drm/cardX-XXX/status` 先看 connector 是否 connected，再看 drm 内部状态。

---

**文档版本**: v1.0
**最后更新**: 2026-07-22
