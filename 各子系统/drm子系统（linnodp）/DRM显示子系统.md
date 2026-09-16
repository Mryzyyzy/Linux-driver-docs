
LinLon-DP 驱动结构总览，用于快速建立学习地图，便于在 Xen 上调试屏幕显示。

## 模块定位

LinLon-DP（也叫 Komeda/Linlon Display Processor）是 CIX SoC（Sky1）的显示控制器（DPU）KMS 驱动。它产出 DRM 设备，向下游 dptx（DisplayPort TX）通过 `drm_bridge`/`component` 模型相连，最终点亮 DP/eDP。

代码根目录：drivers/gpu/drm/cix/linlon-dp/

## 分层结构

```
linlondp_drv.c     ── platform_driver + component_master，绑定/解绑
   │
   ├─ linlondp_dev.c/.h   ── struct linlondp_dev：核心 device 抽象
   │      ├─ 时钟/复位/IRQ/IOMMU/SMCCC（GOP、MMHUB 恢复）
   │      ├─ format_table、debugfs、sysfs
   │      └─ chip 识别 → 调用 hw/dp_dev.c 的 *_identify()
   │
   ├─ linlondp_pipeline.[ch]            ── 抽象 pipeline / component 拓扑
   ├─ linlondp_pipeline_state.c         ── atomic check/state
   ├─ linlondp_plane.c / crtc.c / kms.c ── DRM KMS 对接
   ├─ linlondp_wb_connector.c           ── Writeback 连接器
   ├─ linlondp_framebuffer.[ch]         ── FB/AFBC 描述
   ├─ linlondp_format_caps.[ch]         ── 像素格式能力
   ├─ linlondp_color_mgmt.* / coeffs.*  ── CSC / Gamma / Degamma
   ├─ linlondp_event.c                  ── 中断事件分发
   └─ hw/
        ├─ dp_dev.c/.h    ── chip 实现：寄存器读写、enum_resources、irq_handler、flush
        ├─ dp_axi.c       ── AXI/SMMU 相关初始化
        ├─ dp_component.c ── 各级 HW component（LPU/CU/IPS/Layer/Scaler/Compiz/Improc/Timing）
        └─ dp_regs.h      ── 寄存器宏定义
```

## 绑定流程（你最该先读的）

1. linlondp_drv.c `linlondp_bind()`：
   - `aperture_remove_all_conflicting_devices("linlondp")`：踢掉 efifb / simpledrm
   - `linlondp_dev_create()` → 解析 DT/ACPI，识别 chip，初始化 pipeline/component
   - `linlondp_kms_attach()` → 注册 CRTC/Plane/Connector/Encoder，绑定 dptx bridge
   - `drm_fbdev_client_setup(...DRM_FORMAT_XRGB8888)` → 起 fbcon
   - `enabled_by_gop` 路径：UEFI GOP 已点屏时走 runtime PM "set_active"，跳过完整初始化以保持画面

2. component_master 等待 dptx 子设备就绪后才回调 `linlondp_bind`；匹配通过 OF graph 远端节点。

## 与 Xen dom0 屏幕相关的关键点

你在 Xen 上点屏时，下列几处通常会"踩坑"，建议重点看：

| 关注点 | 文件/位置 | Xen 影响 |
|---|---|---|
| GOP 接管（保留 UEFI framebuffer） | linlondp_dev.h 的 `CIX_SIP_DP_GOP_CTRL` (0xc200000f)；`hw/dp_dev.c` 中 `close_gop` 系列 | 触发 SMC，dom0 走 Xen 时需要 Xen 转发 SiP SMC；GOP framebuffer 物理地址段必须在 dom0 reserved-memory 中可访问 |
| MMHUB ASNI 恢复 | `CIX_SIP_RESTORE_MMHUB_ASNI` (0xc2000017) | 同样是 SMC，runtime resume 时调用 |
| SMMU / IOMMU | `hw/dp_axi.c`、`linlondp_dev_funcs.connect_iommu` | Xen 下 SMMU 由 hypervisor 管，dom0 dma 走 swiotlb 或 Xen-IOMMU 直通；CIX 上常需要 SMMU-v3 bypass，与你 xen-smmu-v3-bypass-preload.patch 相关 |
| IRQ | `linlondp_dev->irq`，`linlondp_event.c` | DTB 中 DPU irq 必须由 Xen 路由给 dom0（SPI 直通） |
| reserved-memory (CMA/dma-pool) | `of_reserved_mem` 在 linlondp_dev.c 顶部 include | 与你已有的 0001-xen-arm-dom0-reserved-memory-and-module-placement.patch 紧密相关：DPU 显存大块往往位于 reserved-memory |
| fbdev 接管 efifb | `aperture_remove_all_conflicting_devices` | Xen + GOP 场景下 efifb 占用的地址要在 dom0 内存映射中保持一致 |

## 推荐学习顺序

1. linlondp_drv.c（probe + bind 全貌）
2. linlondp_dev.c 中 `linlondp_dev_create()` / `linlondp_dev_resume()` / `linlondp_dev_suspend()`
3. `hw/dp_dev.c` 中的 chip identify、irq_handler、flush、close_gop
4. `linlondp_kms.c` + `linlondp_crtc.c`（atomic commit 路径，理解 vblank/flush 时序）
5. `linlondp_pipeline.c` + `hw/dp_component.c`（component graph：LPU→CU→Compiz→Improc→Timing）
6. `linlondp_wb_connector.c` 与 dptx 的对接（OF graph 端口/endpoint）

## 调试建议（Xen 场景）

- 加 `drm.debug=0x1ff` + `dyndbg="file linlondp_* +p"` 内核命令行，看 bind 阶段在哪一步停住。
- `cat /sys/kernel/debug/dri/0/state` 看是否完成 atomic enable。
- `cat /sys/kernel/debug/linlondp0/register` 转储 DPU 寄存器（即上面 `linlondp_register_show`）。
- 确认 Xen 是否转发了 `0xc200000f` / `0xc2000017` SiP SMC；若未转发，`close_gop`/`restore_mmhub` 会卡住或硬件状态紊乱。
- 确认 DPU 的 reserved-memory 节点已被 Xen 透传，并且 dom0 p2m 保留 RAM 类型（这正是 patch 0001 在 `map_range_to_domain` 里做的事）。