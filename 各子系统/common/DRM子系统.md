# DRM 子系统（通用版）

> 通用 DRM/KMS 框架速览；CIX linlon-dp 实战系列见 [../drm子系统（linnodp）/](../drm子系统（linnodp）/) 目录（KMS 五对象详解、atomic commit、mmap、aperture 清场等 8 篇）。代码摘自本地内核树 6.12.58。

## 1. 全景分层

```
用户态        Weston/GPU 厂商 SDK（libdrm ioctl）
                   │ /dev/dri/card0
DRM 核心      drm_drv / drm_ioctl（ioctl 分发表）
                   │
 ┌─ KMS（模式设置）── drm_crtc/drm_encoder/drm_connector/drm_plane/drm_framebuffer
 ├─ GEM（显存）───── drm_gem_*：buffer 对象、handle、mmap
 └─ atomic commit ── drm_atomic_*：一次性提交一帧的全部状态
                   │
驱动          简单显式驱动用 drm_simple_kms_helper；复杂 DPU 自己实现
```

## 2. KMS 五大对象（显示管线怎么拼）

```
 ┌────────┐   ┌──────────┐   ┌──────────┐   ┌───────────┐
 │ FB/Plane├──>│  CRTC    ├──>│ Encoder  ├──>│ Connector │──> 屏幕
 └────────┘   │ 时序+混叠 │   │ 信号编码  │   │ HDMI/DP/DSI│
  像素来源     │ (vblank) │   │ TMDS/eDP │   │ 热插拔/EDID │
```

| 对象 | 职责 | 关键点 |
|---|---|---|
| `drm_framebuffer` | 像素容器（宽高/格式/对 GEM 的引用） | 用户 createfb 时从 GEM handle 组装 |
| `drm_plane` | 扫描源（可多层叠） | primary/cursor/overlay |
| `drm_crtc` | 时序发生 + 混合 | vblank 中断的归属者（page flip 的节拍器） |
| `drm_encoder` | 信号编码桥 | 可链 bridge（DPI->HDMI 转换芯片） |
| `drm_connector` | 物理口 | 热插拔检测、EDID/模式列表 |

## 3. Atomic Commit（现代 DRM 的心脏）

- 一次 ioctl（`DRM_IOCTL_MODE_ATOMIC`）提交"下一帧的全部状态"：plane 换 FB、CRTC 改模式、connector 开关，**全或无（test-only 可校验）**
- 与旧 setcrtc/page_flip 的区别：旧接口多次调用中间态可能撕裂；atomic 保证帧级原子性
- 流程：用户态 ioctl -> `drm_atomic_check_only`（校验）-> commit（vblank 时生效）-> `drm_crtc_send_vblank_event` 通知用户态

### 驱动侧入口（源码摘录）

```c
/* include/drm/drm_mode_config.h */
struct drm_mode_config_funcs {
	struct drm_framebuffer *(*fb_create)(...);
	const uint32_t *(*get_format_list)(...);
	int (*atomic_check)(struct drm_device *dev, struct drm_atomic_state *state);
	void (*atomic_commit)(struct drm_device *dev, struct drm_atomic_state *state,
			      bool nonblock);   /* 提交入口 */
	...
};
```

- 简单设备：`drm_simple_kms_helper` 或 `drm_atomic_helper_*` 家族把 check/commit 的大头全包了
- UDL/虚拟设备用 `drm_gem_dma` 走 CMA；GPU 类设备走 GEM + 自己的分配器

## 4. GEM 显存管理

| 概念 | 一句话 |
|---|---|
| GEM object | 一块显存对象（`struct drm_gem_object`），带引用计数 |
| handle | 用户态 32bit 句柄（per-fd 表），`drm_ioctl` 层维护 |
| prime/dma-buf | GEM 对象导出为 dma-buf 跨设备/进程共享（零拷贝合成的基础） |
| mmap | `drm_gem_mmap`：一次性映射或 fault-based（CMA/shmem 后端决定） |
| vma/node | GEM 的 VM 映射管理（GPU VM 对应 drm_gpuva，新架构） |

零拷贝合成路径（面试加分）：camera/NPU 产出 dma-buf -> drm_prime 导入成 FB -> plane 直接扫描输出，全程无 memcpy--与简历 DMA-BUF IPC 通路同一套底座。

## 5. 用户态标准流程（KMS）

```
open /dev/dri/card0
drmModeGetResources / Connector / Encoder / CRTC   // 摸清管线
drmModeCreateFB(dumb buffer 或 prime导入)           // 准备像素
drmModeSetCrtc 或 drmModePageFlip                   // 上屏（旧API）
  现代替代：drmModeAtomicCommit                      // 一步提交全部状态
poll(vblank event) -> 下一帧 flip                    // 双缓冲循环
```

## 6. 调试

| 手段 | 用法 |
|---|---|
| `modetest` | 列管线/打测试图（不依赖 compositor 的最小验证） |
| debugfs `/sys/kernel/debug/dri/0/` | state、framebuffer dump、vblank 统计 |
| `drm.debug=0x1e` + dmesg | 分级日志（driver/kms/atomic 全开） |
| ftrace | `drm_vblank_event`, `drm_atomic_commit` |
| 常见症状 | 黑屏但 commit 成功 -> 看 connector 检测状态/EDID；flip 超时 -> vblank 中断没到（查 GIC 路由，对应总览 GIC↔DRM 关联） |

## 7. 面试问答

**Q: KMS 五对象各自职责？连接器和编码器为什么分开？**
见上表。编码器归 SoC 侧信号制式，connector 归物理口与热插拔；一个 connector 可换接不同编码器路径（转接桥），分开建模才能表达。

**Q: atomic commit 相比旧接口好在哪？**
帧级原子（无中间撕裂）、test-only 校验、多 plane 一致更新；依赖 vblank 事件驱动用户态调度，是 Wayland 合成器的性能基础。

**Q: vblank 中断丢了会怎样？**
page flip 完成事件发不出去 -> 合成器超时/掉帧。这是 DRM 调试里最常见的中断路由问题（所以 bringup 顺序里 GIC 在 DRM 前）。

**Q: GEM 和 dma-buf 什么关系？**
GEM 是 DRM 内的显存对象；prime 层把 GEM 导出/导入为 dma-buf，实现跨设备（渲染 GPU <-> 显示 DPU）和跨进程零拷贝。
