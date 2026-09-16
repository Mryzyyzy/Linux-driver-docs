# PCIe 完整初始化流程详解

本文档详细说明 PCIe 从系统上电到设备可用的完整初始化流程，包括链路训练、枚举、驱动加载等所有步骤。

## 目录

1. [概述](#概述)
2. [完整初始化流程](#完整初始化流程)
3. [阶段1：硬件上电和链路训练](#阶段1硬件上电和链路训练)
4. [阶段2：BIOS/UEFI 枚举](#阶段2biosuefi-枚举)
5. [阶段3：操作系统初始化](#阶段3操作系统初始化)
6. [阶段4：驱动加载](#阶段4驱动加载)
7. [时间线总结](#时间线总结)
8. [各阶段的关系](#各阶段的关系)
9. [故障排查](#故障排查)
10. [实际代码示例](#实际代码示例)

---

## 概述

### PCIe 初始化的完整流程

```
系统上电
    │
    ├─> 阶段1：硬件上电 + 链路训练（硬件自动）
    │
    ├─> 阶段2：BIOS/UEFI 枚举（固件）
    │
    ├─> 阶段3：操作系统初始化（内核）
    │
    └─> 阶段4：驱动加载（驱动模块）
```

### 关键概念

- **链路训练**：硬件自动完成，建立物理连接
- **枚举**：软件/固件完成，发现设备并分配资源
- **驱动加载**：操作系统匹配并加载设备驱动

---

## 完整初始化流程

### 流程图

```
┌─────────────────────────────────────────────────────────────┐
│                    PCIe 完整初始化流程                        │
└─────────────────────────────────────────────────────────────┘

时间轴:  0ms    50ms   100ms   150ms   200ms   250ms
         │      │      │       │       │       │
         ├──────┼──────┼───────┼───────┼───────┤
         │训练  │枚举  │OS启动 │驱动   │设备   │
         │      │      │       │匹配   │可用   │
         │      │      │       │       │       │
阶段1:   │硬件上电 + 链路训练（硬件自动）                      │
         │                                                      │
         │  Detect ──> Polling ──> Configuration ──> L0       │
         │  (~24ms)   (~12ms)    (~16ms)        (完成)        │
         │                                                      │
         └──────────────────────────────────────────────────────┘
                          │
阶段2:                     │ BIOS/UEFI 枚举（固件）            │
                          │                                    │
                          │  扫描总线 ──> 发现设备 ──> 分配资源 │
                          │                                    │
                          └────────────────────────────────────┘
                                       │
阶段3:                                  │ 操作系统初始化（内核）  │
                                       │                          │
                                       │  PCI 子系统 ──> 设备列表 │
                                       │                          │
                                       └──────────────────────────┘
                                                  │
阶段4:                                             │ 驱动加载      │
                                                  │                │
                                                  │  匹配驱动      │
                                                  │  probe()       │
                                                  │  设备可用      │
                                                  └────────────────┘
```

---

## 阶段1：硬件上电和链路训练

### 1.1 设备上电

**触发条件：**
- 系统上电
- PCIe 插槽供电
- 设备复位释放

**硬件自动执行：**
- PCIe 控制器初始化
- PHY/SerDes 初始化
- 开始链路训练

### 1.2 链路训练（Link Training）

**执行者：** 硬件自动（LTSSM 状态机）

**训练序列：**

```
┌─────────┐
│ Detect  │ ──> 检测链路是否存在 (~24ms)
└────┬────┘
     │
┌────▼────┐
│ Polling │ ──> 发送 TS1，CDR 锁定 (~12ms)
└────┬────┘
     │
┌────▼──────────┐
│ Configuration │ ──> 协商参数，对齐通道 (~16ms)
└────┬──────────┘
     │
┌────▼────┐
│   L0    │ ──> 链路可用，可以传输数据
└─────────┘
```

#### Detect 阶段

**目的：** 检测链路是否存在

**过程：**
- 发送电气空闲信号
- 检测接收端是否有响应
- 如果检测到信号，进入下一阶段

**时间：** 最大 24ms

#### Polling 阶段

**目的：** CDR 锁定，建立基本同步

**过程：**
- 发送 TS1 训练序列
- CDR 从 TS1 中恢复时钟频率
- CDR 锁定相位
- 检测到有效的 TS1

**关键操作：**
- CDR 频率锁定
- CDR 相位对齐
- 符号锁定（8b/10b 边界）

**时间：** 最大 12ms

#### Configuration 阶段

**目的：** 协商参数，对齐通道

**过程：**
- 发送 TS1，包含链路参数（速度、宽度）
- 协商双方都支持的最高参数
- 多通道时对齐所有通道
- 发送 TS2 确认参数

**协商内容：**
- **链路速度**：Gen1/2/3/4/5
- **链路宽度**：x1/x2/x4/x8/x16
- **通道对齐**：多通道时对齐所有通道

**时间：** 最大 16ms

#### L0 状态

**结果：**
- 链路训练完成
- 可以传输数据包（TLP、DLLP）
- CDR 持续跟踪相位

**训练结果保存：**
- 写入 Link Status 寄存器
- Speed（速度）
- Width（宽度）
- DLLActive（数据链路层激活）

### 1.3 训练结果和可访问信息

**硬件自动写入配置空间的所有信息：**

#### A. Link Status 寄存器（训练结果的主要信息）

**寄存器位置：** `PCIe Capability + 0x12`

```
Link Status (LnkSta) - 16位寄存器
─────────────────────────────────────────
Bit  字段              说明                软件可访问
─────────────────────────────────────────
0-3   Speed           当前链路速度          ✅ 是
4-9   Width           当前链路宽度          ✅ 是
10    Link Training   链路训练状态          ✅ 是
11    Slot Clock      插槽时钟状态          ✅ 是
12    DLLActive       数据链路层激活        ✅ 是
13    Link Bandwidth  链路带宽管理状态      ✅ 是
14    Link Autonomous 链路自主带宽状态      ✅ 是
15    Clock           时钟配置状态          ✅ 是
```

**软件读取方法：**

```c
int pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
u16 link_status;
pci_read_config_word(pdev, pos + PCI_EXP_LNKSTA, &link_status);

// 解析各个字段
u8 speed = link_status & PCI_EXP_LNKSTA_SPEED;
u8 width = (link_status & PCI_EXP_LNKSTA_WIDTH) >> 4;
bool training = !!(link_status & PCI_EXP_LNKSTA_LT);
bool clock = !!(link_status & PCI_EXP_LNKSTA_SLC);
bool dll_active = !!(link_status & PCI_EXP_LNKSTA_DLLLA);
bool bw_mgmt = !!(link_status & PCI_EXP_LNKSTA_LBMS);
bool auto_bw = !!(link_status & PCI_EXP_LNKSTA_LABS);
bool clock_cfg = !!(link_status & PCI_EXP_LNKSTA_CLK);
```

#### B. Link Capabilities 寄存器（设备能力）

**寄存器位置：** `PCIe Capability + 0x0C`

```
Link Capabilities (LnkCap) - 32位寄存器
─────────────────────────────────────────
Bit  字段              说明                软件可访问
─────────────────────────────────────────
0-3   Max Speed       最大支持速度          ✅ 是
4-9   Max Width       最大支持宽度          ✅ 是
10-11 ASPM Support    ASPM 支持能力        ✅ 是
12    L0s Exit Latency L0s 退出延迟         ✅ 是
13-15 L1 Exit Latency  L1 退出延迟         ✅ 是
16    Clock PM        时钟电源管理          ✅ 是
17    Surprise Down    意外关闭支持          ✅ 是
18    DLL Link Active  数据链路层报告       ✅ 是
19    Link Bandwidth   链路带宽通知         ✅ 是
20    Link Autonomous  链路自主带宽         ✅ 是
21    Clock PM Sync    时钟 PM 同步         ✅ 是
22-23 Data Link Layer  数据链路层状态       ✅ 是
24    Port Number      端口号               ✅ 是
25-27 PHY Version      PHY 版本            ✅ 是
```

**软件读取方法：**

```c
u32 link_cap;
pci_read_config_dword(pdev, pos + PCI_EXP_LNKCAP, &link_cap);

// 解析各个字段
u8 max_speed = link_cap & PCI_EXP_LNKCAP_SLS;
u8 max_width = (link_cap & PCI_EXP_LNKCAP_MLW) >> 4;
u8 aspm_support = (link_cap >> 10) & 0x3;
u8 l0s_latency = (link_cap >> 12) & 0x7;
u8 l1_latency = (link_cap >> 15) & 0x7;
bool clock_pm = !!(link_cap & PCI_EXP_LNKCAP_CLKPM);
bool surprise_down = !!(link_cap & PCI_EXP_LNKCAP_SD);
```

#### C. Link Control 寄存器（链路控制）

**寄存器位置：** `PCIe Capability + 0x10`

```
Link Control (LnkCtl) - 16位寄存器
─────────────────────────────────────────
Bit  字段              说明                软件可访问
─────────────────────────────────────────
0-1   ASPM Control    ASPM 控制            ✅ 是（可读写）
2     RCB             读完成边界           ✅ 是（可读写）
3     Link Disable    链路禁用             ✅ 是（可读写）
4     Retrain Link    重新训练链路         ✅ 是（可写）
5     Common Clock    公共时钟配置         ✅ 是（可读写）
6     Extended Synch  扩展同步             ✅ 是（可读写）
7     Clock PM Enable 时钟 PM 使能         ✅ 是（可读写）
8     Hardware Auto   硬件自主宽度         ✅ 是（可读写）
9     Bandwidth Int   带宽中断使能         ✅ 是（可读写）
10    Auto BW Int     自主带宽中断         ✅ 是（可读写）
```

**软件读取/写入方法：**

```c
u16 link_control;
pci_read_config_word(pdev, pos + PCI_EXP_LNKCTL, &link_control);

// 读取 ASPM 设置
u8 aspm_control = link_control & PCI_EXP_LNKCTL_ASPMC;

// 写入（例如：触发重新训练）
link_control |= PCI_EXP_LNKCTL_RL;  // Request Retrain
pci_write_config_word(pdev, pos + PCI_EXP_LNKCTL, link_control);
```

#### D. Device Status 寄存器（设备状态）

**寄存器位置：** `PCIe Capability + 0x08`

```
Device Status (DevSta) - 16位寄存器
─────────────────────────────────────────
Bit  字段              说明                软件可访问
─────────────────────────────────────────
0     Correctable     可纠正错误           ✅ 是
1     Non-Fatal       非致命错误           ✅ 是
2     Fatal           致命错误             ✅ 是
3     Unsupported     不支持请求           ✅ 是
4     Aux Power       辅助电源             ✅ 是
5     Trans Pending    传输挂起             ✅ 是
```

**软件读取方法：**

```c
u16 device_status;
pci_read_config_word(pdev, pos + PCI_EXP_DEVSTA, &device_status);

bool correctable_err = !!(device_status & PCI_EXP_DEVSTA_CED);
bool non_fatal_err = !!(device_status & PCI_EXP_DEVSTA_NFED);
bool fatal_err = !!(device_status & PCI_EXP_DEVSTA_FED);
bool unsupported_req = !!(device_status & PCI_EXP_DEVSTA_URD);
bool aux_power = !!(device_status & PCI_EXP_DEVSTA_AUXPD);
bool trans_pending = !!(device_status & PCI_EXP_DEVSTA_TRPND);
```

#### E. Device Capabilities 寄存器（设备能力）

**寄存器位置：** `PCIe Capability + 0x04`

```
Device Capabilities (DevCap) - 32位寄存器
─────────────────────────────────────────
Bit  字段              说明                软件可访问
─────────────────────────────────────────
0-2   Max Payload      最大有效载荷         ✅ 是
3-5   Phantom Func    幻象功能             ✅ 是
6     Extended Tag     扩展标签            ✅ 是
7     Endpoint L0s     端点 L0s 延迟       ✅ 是
8-10  Endpoint L1      端点 L1 延迟        ✅ 是
11    Role Based ERR   基于角色的错误报告  ✅ 是
12    Captured Slot    捕获插槽电源        ✅ 是
13    Captured Slot    捕获插槽功率限制    ✅ 是
14-15 Function Level  功能级重置          ✅ 是
```

**软件读取方法：**

```c
u32 device_cap;
pci_read_config_dword(pdev, pos + PCI_EXP_DEVCAP, &device_cap);

u8 max_payload = device_cap & PCI_EXP_DEVCAP_PAYLOAD;
u8 phantom_func = (device_cap >> 3) & 0x7;
bool extended_tag = !!(device_cap & PCI_EXP_DEVCAP_EXT_TAG);
u8 endpoint_l0s = (device_cap >> 7) & 0x7;
u8 endpoint_l1 = (device_cap >> 8) & 0x7;
```

### 1.4 完整的训练信息读取函数

**软件可以读取所有训练相关信息的完整函数：**

```c
static void read_all_training_info(struct pci_dev *pdev)
{
    int pos;
    u16 link_status, link_control, device_status;
    u32 link_cap, device_cap;
    
    // 查找 PCIe Capability
    pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (!pos) {
        pr_info("Not a PCIe device\n");
        return;
    }
    
    // ========== 1. 读取链路状态（训练结果）==========
    pci_read_config_word(pdev, pos + PCI_EXP_LNKSTA, &link_status);
    
    u8 speed = link_status & PCI_EXP_LNKSTA_SPEED;
    u8 width = (link_status & PCI_EXP_LNKSTA_WIDTH) >> 4;
    bool training = !!(link_status & PCI_EXP_LNKSTA_LT);
    bool clock = !!(link_status & PCI_EXP_LNKSTA_SLC);
    bool dll_active = !!(link_status & PCI_EXP_LNKSTA_DLLLA);
    bool bw_mgmt = !!(link_status & PCI_EXP_LNKSTA_LBMS);
    bool auto_bw = !!(link_status & PCI_EXP_LNKSTA_LABS);
    bool clock_cfg = !!(link_status & PCI_EXP_LNKSTA_CLK);
    
    pr_info("=== Link Status (Training Results) ===\n");
    pr_info("Current Speed: Gen%d (%s)\n", speed,
            speed == 1 ? "2.5 GT/s" :
            speed == 2 ? "5.0 GT/s" :
            speed == 3 ? "8.0 GT/s" :
            speed == 4 ? "16.0 GT/s" :
            speed == 5 ? "32.0 GT/s" : "Unknown");
    pr_info("Current Width: x%d\n", width);
    pr_info("Link Training: %s\n", training ? "In Progress" : "Complete");
    pr_info("Slot Clock: %s\n", clock ? "Active" : "Inactive");
    pr_info("Data Link Layer: %s\n", dll_active ? "Active" : "Inactive");
    pr_info("Link Bandwidth Management: %s\n", bw_mgmt ? "Active" : "Inactive");
    pr_info("Link Autonomous Bandwidth: %s\n", auto_bw ? "Active" : "Inactive");
    pr_info("Clock Configuration: %s\n", clock_cfg ? "Supported" : "Not Supported");
    
    // ========== 2. 读取链路能力 ==========
    pci_read_config_dword(pdev, pos + PCI_EXP_LNKCAP, &link_cap);
    
    u8 max_speed = link_cap & PCI_EXP_LNKCAP_SLS;
    u8 max_width = (link_cap & PCI_EXP_LNKCAP_MLW) >> 4;
    u8 aspm_support = (link_cap >> 10) & 0x3;
    u8 l0s_latency = (link_cap >> 12) & 0x7;
    u8 l1_latency = (link_cap >> 15) & 0x7;
    bool clock_pm = !!(link_cap & PCI_EXP_LNKCAP_CLKPM);
    bool surprise_down = !!(link_cap & PCI_EXP_LNKCAP_SD);
    
    pr_info("\n=== Link Capabilities ===\n");
    pr_info("Max Speed: Gen%d\n", max_speed);
    pr_info("Max Width: x%d\n", max_width);
    pr_info("ASPM Support: %s\n",
            aspm_support == 0 ? "None" :
            aspm_support == 1 ? "L0s" :
            aspm_support == 2 ? "L1" : "L0s + L1");
    pr_info("L0s Exit Latency: %d\n", l0s_latency);
    pr_info("L1 Exit Latency: %d\n", l1_latency);
    pr_info("Clock PM: %s\n", clock_pm ? "Supported" : "Not Supported");
    pr_info("Surprise Down: %s\n", surprise_down ? "Supported" : "Not Supported");
    
    // ========== 3. 读取链路控制 ==========
    pci_read_config_word(pdev, pos + PCI_EXP_LNKCTL, &link_control);
    
    u8 aspm_control = link_control & PCI_EXP_LNKCTL_ASPMC;
    bool rcb = !!(link_control & PCI_EXP_LNKCTL_RCB);
    bool link_disable = !!(link_control & PCI_EXP_LNKCTL_LD);
    bool retrain = !!(link_control & PCI_EXP_LNKCTL_RL);
    bool common_clock = !!(link_control & PCI_EXP_LNKCTL_CCC);
    
    pr_info("\n=== Link Control ===\n");
    pr_info("ASPM Control: %s\n",
            aspm_control == 0 ? "Disabled" :
            aspm_control == 1 ? "L0s Enabled" :
            aspm_control == 2 ? "L1 Enabled" : "L0s + L1 Enabled");
    pr_info("Read Completion Boundary: %s\n", rcb ? "128B" : "64B");
    pr_info("Link Disable: %s\n", link_disable ? "Yes" : "No");
    pr_info("Retrain Link: %s\n", retrain ? "Requested" : "No");
    pr_info("Common Clock: %s\n", common_clock ? "Yes" : "No");
    
    // ========== 4. 读取设备状态 ==========
    pci_read_config_word(pdev, pos + PCI_EXP_DEVSTA, &device_status);
    
    bool correctable_err = !!(device_status & PCI_EXP_DEVSTA_CED);
    bool non_fatal_err = !!(device_status & PCI_EXP_DEVSTA_NFED);
    bool fatal_err = !!(device_status & PCI_EXP_DEVSTA_FED);
    bool unsupported_req = !!(device_status & PCI_EXP_DEVSTA_URD);
    bool aux_power = !!(device_status & PCI_EXP_DEVSTA_AUXPD);
    bool trans_pending = !!(device_status & PCI_EXP_DEVSTA_TRPND);
    
    pr_info("\n=== Device Status ===\n");
    pr_info("Correctable Error: %s\n", correctable_err ? "Detected" : "No");
    pr_info("Non-Fatal Error: %s\n", non_fatal_err ? "Detected" : "No");
    pr_info("Fatal Error: %s\n", fatal_err ? "Detected" : "No");
    pr_info("Unsupported Request: %s\n", unsupported_req ? "Detected" : "No");
    pr_info("Aux Power: %s\n", aux_power ? "Present" : "Not Present");
    pr_info("Transaction Pending: %s\n", trans_pending ? "Yes" : "No");
    
    // ========== 5. 读取设备能力 ==========
    pci_read_config_dword(pdev, pos + PCI_EXP_DEVCAP, &device_cap);
    
    u8 max_payload = device_cap & PCI_EXP_DEVCAP_PAYLOAD;
    u8 phantom_func = (device_cap >> 3) & 0x7;
    bool extended_tag = !!(device_cap & PCI_EXP_DEVCAP_EXT_TAG);
    u8 endpoint_l0s = (device_cap >> 7) & 0x7;
    u8 endpoint_l1 = (device_cap >> 8) & 0x7;
    
    pr_info("\n=== Device Capabilities ===\n");
    pr_info("Max Payload Size: %d bytes\n", 128 << max_payload);
    pr_info("Phantom Functions: %d\n", phantom_func);
    pr_info("Extended Tag: %s\n", extended_tag ? "Supported" : "Not Supported");
    pr_info("Endpoint L0s Acceptable Latency: %d\n", endpoint_l0s);
    pr_info("Endpoint L1 Acceptable Latency: %d\n", endpoint_l1);
    
    // ========== 6. 检查是否降级 ==========
    pr_info("\n=== Training Result Analysis ===\n");
    if (speed < max_speed || width < max_width) {
        pr_warn("⚠ Link operating below maximum capability!\n");
        pr_warn("  Max: Gen%d x%d\n", max_speed, max_width);
        pr_warn("  Current: Gen%d x%d\n", speed, width);
        pr_warn("  Possible reasons:\n");
        pr_warn("    - Signal integrity issues\n");
        pr_warn("    - Incompatible device\n");
        pr_warn("    - Power/thermal constraints\n");
    } else {
        pr_info("✓ Link operating at maximum capability\n");
    }
    
    if (!dll_active) {
        pr_err("✗ Data Link Layer not active - link may not be usable\n");
    } else {
        pr_info("✓ Data Link Layer active - link is ready\n");
    }
}
```

### 1.5 使用 lspci 查看训练信息

**命令行查看所有训练信息：**

```bash
# 查看完整的 PCIe Capability 信息
lspci -vvv -s 00:01.0 | grep -A 30 "Capabilities"

# 输出示例：
# Capabilities: [40] Express (v2) Endpoint, MSI 00
#         DevCap: MaxPayload 512 bytes, PhantFunc 0, Latency L0s <1us, L1 <16us
#                 ExtTag+ AttnBtn- AttnInd- PwrInd- RBE+ FLReset+
#         DevSta: CorrErr- NonFatalErr- FatalErr- UnsuppReq- AuxPwr- TransPend-
#         LnkCap: Port #0, Speed 16GT/s, Width x16, ASPM L0s L1, Exit Latency L0s <1us, L1 <16us
#                 ClockPM+ ClockPM- Surprise- LLActRep- BwNot- ASPMOptComp+
#         LnkSta: Speed 8GT/s, Width x4, TrErr- Train- SlotClk+ DLActive+ BWMgmt- ABWMgmt-
#         LnkCtl: ASPM L1 Enabled; RCB 64 bytes Disabled- CommClk+
#                 ExtSynch- ClockPM- AutWidDis- BWInt- AutBWInt-
```

**解读输出：**

- **LnkCap**: 设备支持的最大能力
- **LnkSta**: 训练完成后的实际状态（**这是训练结果**）
- **LnkCtl**: 当前的链路控制设置
- **DevCap**: 设备能力
- **DevSta**: 设备状态（错误信息等）

### 1.6 训练过程中的实时信息

**虽然训练是硬件自动的，但软件可以监控训练过程：**

```c
// 监控链路训练过程
static void monitor_link_training(struct pci_dev *pdev)
{
    int pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    int timeout = 100;  // 100ms 超时
    
    pr_info("Monitoring link training...\n");
    
    while (timeout-- > 0) {
        u16 link_status;
        pci_read_config_word(pdev, pos + PCI_EXP_LNKSTA, &link_status);
        
        bool training = !!(link_status & PCI_EXP_LNKSTA_LT);
        bool dll_active = !!(link_status & PCI_EXP_LNKSTA_DLLLA);
        
        if (training) {
            pr_info("Link training in progress...\n");
        } else if (dll_active) {
            u8 speed = link_status & PCI_EXP_LNKSTA_SPEED;
            u8 width = (link_status & PCI_EXP_LNKSTA_WIDTH) >> 4;
            pr_info("Link training complete: Gen%d x%d\n", speed, width);
            return;
        }
        
        msleep(1);
    }
    
    pr_err("Link training timeout!\n");
}
```

### 1.7 训练信息总结表

**所有软件可访问的训练相关信息：**

| 寄存器 | 偏移 | 主要信息 | 软件访问 |
|--------|------|---------|---------|
| **Link Status** | 0x12 | 训练结果（速度、宽度、状态） | ✅ 只读 |
| **Link Capabilities** | 0x0C | 设备最大能力 | ✅ 只读 |
| **Link Control** | 0x10 | 链路控制（ASPM、重训练等） | ✅ 读写 |
| **Device Status** | 0x08 | 设备状态（错误信息） | ✅ 只读 |
| **Device Capabilities** | 0x04 | 设备能力 | ✅ 只读 |

**关键训练结果（在 Link Status 中）：**
- ✅ 当前链路速度（Speed）
- ✅ 当前链路宽度（Width）
- ✅ 训练状态（Link Training）
- ✅ 时钟状态（Slot Clock）
- ✅ 数据链路层状态（DLLActive）
- ✅ 带宽管理状态
- ✅ 时钟配置状态

### 1.8 快速访问训练信息的代码模板

**在你的驱动中可以直接使用的代码：**

```c
// 快速读取训练结果的简化函数
static void quick_check_training_result(struct pci_dev *pdev)
{
    int pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (!pos)
        return;
    
    u16 link_status;
    u32 link_cap;
    
    // 读取训练结果
    pci_read_config_word(pdev, pos + PCI_EXP_LNKSTA, &link_status);
    pci_read_config_dword(pdev, pos + PCI_EXP_LNKCAP, &link_cap);
    
    // 提取关键信息
    u8 speed = link_status & PCI_EXP_LNKSTA_SPEED;
    u8 width = (link_status & PCI_EXP_LNKSTA_WIDTH) >> 4;
    bool dll_active = !!(link_status & PCI_EXP_LNKSTA_DLLLA);
    
    u8 max_speed = link_cap & PCI_EXP_LNKCAP_SLS;
    u8 max_width = (link_cap & PCI_EXP_LNKCAP_MLW) >> 4;
    
    // 打印结果
    pr_info("Link: Gen%d x%d (Max: Gen%d x%d), DLLActive: %s\n",
            speed, width, max_speed, max_width,
            dll_active ? "Yes" : "No");
    
    // 检查降级
    if (speed < max_speed || width < max_width) {
        pr_warn("Link degraded from maximum capability\n");
    }
}
```

### 1.9 训练信息访问的完整示例

**在驱动 probe() 函数中使用：**

```c
static int my_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    // ... 其他初始化代码 ...
    
    // 读取所有训练信息
    read_all_training_info(pdev);
    
    // 或者只读取关键信息
    quick_check_training_result(pdev);
    
    // ... 继续初始化 ...
}
```

---

## 阶段2：BIOS/UEFI 枚举

### 2.1 等待链路训练完成

**BIOS 代码逻辑：**

```c
void bios_wait_for_link_training(int port)
{
    int timeout = 100;  // 100ms 超时
    
    while (timeout-- > 0) {
        u16 link_status = read_link_status(port);
        
        if (link_status & LINK_ACTIVE) {
            // 链路训练完成
            return SUCCESS;
        }
        
        delay(1ms);
    }
    
    // 超时，训练失败
    return TIMEOUT;
}
```

### 2.2 枚举过程

**枚举步骤：**

```
1. 扫描总线
   │
   ├─> 从 Root Complex 开始
   ├─> 检查每个 Bus/Device/Function
   └─> 读取 Vendor ID 判断设备是否存在
   
2. 发现设备
   │
   ├─> 读取 Vendor ID (0x00)
   │   - 0xFFFF = 没有设备
   │   - 有效值 = 有设备
   │
   └─> 读取 Device ID (0x02)
       - 确定具体设备型号
       
3. 读取设备信息
   │
   ├─> Class Code (设备类型)
   ├─> BAR0~BAR5 (需要多少地址空间)
   ├─> Interrupt Pin (中断引脚)
   └─> Capabilities (支持的功能)
   
4. 分配资源
   │
   ├─> 分配 BAR 地址空间
   │   - 探测 BAR 大小
   │   - 分配物理地址
   │   - 写入 BAR 寄存器
   │
   ├─> 分配中断号
   │   - 根据 Interrupt Pin
   │   - 分配 IRQ
   │   - 写入 Interrupt Line
   │
   └─> 分配总线号（如果有 Switch）
       - 分配下游总线号
       
5. 配置设备
   │
   ├─> 写入 BAR 地址
   ├─> 配置中断
   └─> 使能设备（可能触发重新训练）
   
6. 递归枚举（如果有 Switch）
   │
   └─> 如果发现 Switch，继续扫描下游总线
```

### 2.3 枚举代码示例（伪代码）

```c
void bios_pcie_enumeration(void)
{
    // 1. 初始化 PCIe 控制器
    pcie_controller_init();
    
    // 2. 从 Root Complex 开始枚举
    for (bus = 0; bus < MAX_BUS; bus++) {
        for (device = 0; device < 32; device++) {
            for (function = 0; function < 8; function++) {
                
                // 3. 等待链路训练完成（如果是新发现的设备）
                if (is_new_device(bus, device, function)) {
                    wait_for_link_training(bus, device, function);
                }
                
                // 4. 读取 Vendor ID
                vendor_id = pci_read_config(bus, device, function, 0x00);
                
                // 5. 检查设备是否存在
                if (vendor_id == 0xFFFF) {
                    continue;  // 没有设备
                }
                
                // 6. 发现设备！
                device_id = pci_read_config(bus, device, function, 0x02);
                class_code = pci_read_config(bus, device, function, 0x09);
                
                pr_info("Found: %04x:%04x at %02x:%02x.%d\n",
                        vendor_id, device_id, bus, device, function);
                
                // 7. 探测和分配 BAR
                for (bar = 0; bar < 6; bar++) {
                    bar_value = pci_read_config(bus, device, function,
                                                 0x10 + bar * 4);
                    
                    if (bar_value == 0) {
                        continue;  // BAR 未使用
                    }
                    
                    // 探测 BAR 大小
                    size = probe_bar_size(bus, device, function, bar);
                    
                    if (size > 0) {
                        // 分配地址空间
                        if (bar_value & 0x01) {
                            // I/O 空间
                            io_addr = allocate_io_space(size);
                            pci_write_config(bus, device, function,
                                             0x10 + bar * 4, io_addr);
                        } else {
                            // 内存空间
                            mem_addr = allocate_memory_space(size);
                            pci_write_config(bus, device, function,
                                             0x10 + bar * 4, mem_addr);
                        }
                    }
                }
                
                // 8. 分配中断
                interrupt_pin = pci_read_config(bus, device, function, 0x3D);
                if (interrupt_pin) {
                    irq = allocate_irq(bus, device, function, interrupt_pin);
                    pci_write_config(bus, device, function, 0x3C, irq);
                }
                
                // 9. 如果是 Switch，递归枚举下游总线
                if (is_pcie_switch(bus, device, function)) {
                    downstream_bus = allocate_bus_number();
                    configure_switch(bus, device, function, downstream_bus);
                    enumerate_bus(downstream_bus);  // 递归
                }
            }
        }
    }
}
```

### 2.4 枚举结果

**BIOS 枚举完成后：**

- 所有设备被发现
- 资源已分配（BAR、IRQ、Bus）
- 设备信息写入 ACPI 表（或设备树）
- 传递给操作系统

---

## 阶段3：操作系统初始化

### 3.1 Linux 内核 PCI 子系统

**内核启动时的初始化：**

```
内核启动
    │
    ├─> PCI 子系统初始化
    │   ├─> drivers/pci/pci.c
    │   ├─> drivers/pci/probe.c
    │   └─> drivers/pci/setup-bus.c
    │
    ├─> 从 ACPI 读取设备信息（或重新枚举）
    │
    ├─> 创建设备结构 (struct pci_dev)
    │
    ├─> 建立设备列表
    │
    └─> 准备驱动匹配
```

### 3.2 设备列表建立

**内核代码（简化）：**

```c
// drivers/pci/probe.c

void pci_scan_bus(struct pci_bus *bus)
{
    // 扫描总线，发现设备
    for (devfn = 0; devfn < 256; devfn++) {
        struct pci_dev *dev;
        
        // 读取配置空间
        if (pci_read_config_dword(bus, devfn, PCI_VENDOR_ID, &id))
            continue;
        
        if (id == 0xffffffff || id == 0x00000000)
            continue;
        
        // 创建设备结构
        dev = pci_scan_single_device(bus, devfn);
        if (dev) {
            // 添加到设备列表
            list_add_tail(&dev->bus_list, &bus->devices);
        }
    }
}
```

### 3.3 驱动匹配准备

**内核建立设备-驱动匹配机制：**

```c
// 内核维护两个列表：
// 1. 设备列表：所有发现的 PCIe 设备
// 2. 驱动列表：所有注册的 PCIe 驱动

// 当驱动注册时，内核会尝试匹配
pci_register_driver(&my_driver);
    │
    ├─> 遍历所有设备
    ├─> 比较 pci_device_id
    └─> 如果匹配，调用驱动的 probe()
```

---

## 阶段4：驱动加载

### 4.1 驱动注册

**驱动模块加载时：**

```c
// 驱动代码
static struct pci_driver my_driver = {
    .name = "my_device",
    .id_table = my_device_ids,
    .probe = my_probe,
    .remove = my_remove,
};

static int __init my_init(void)
{
    // 注册驱动
    return pci_register_driver(&my_driver);
}
module_init(my_init);
```

### 4.2 驱动匹配

**内核匹配过程：**

```
驱动注册
    │
    ├─> 遍历所有已发现的设备
    │
    ├─> 比较 Vendor ID / Device ID
    │
    ├─> 如果匹配
    │   └─> 调用驱动的 probe() 函数
    │
    └─> 如果不匹配
        └─> 继续下一个设备
```

### 4.3 驱动 probe() 函数

**驱动需要做的事情：**

```c
static int my_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct my_device *dev;
    int ret;
    
    // ========== 1. 基本初始化 ==========
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    dev->pdev = pdev;
    pci_set_drvdata(pdev, dev);
    
    // ========== 2. 使能设备 ==========
    // 注意：链路训练在 BIOS 阶段已完成
    // 这里只是使能设备，不会重新训练
    ret = pci_enable_device(pdev);
    if (ret) {
        pr_err("Failed to enable device\n");
        goto err_free_dev;
    }
    
    // ========== 3. 启用总线主控 ==========
    pci_set_master(pdev);  // 允许 DMA
    
    // ========== 4. 请求资源 ==========
    ret = pci_request_regions(pdev, DRIVER_NAME);
    if (ret) {
        pr_err("Failed to request regions\n");
        goto err_disable_device;
    }
    
    // ========== 5. 映射 BAR ==========
    // BIOS 已经分配了物理地址，这里映射到虚拟地址
    dev->bar0 = pci_iomap(pdev, 0, 0);
    if (!dev->bar0) {
        pr_err("Failed to map BAR0\n");
        ret = -ENOMEM;
        goto err_release_regions;
    }
    
    // ========== 6. 检查链路状态（可选）==========
    int pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (pos) {
        u16 link_status;
        pci_read_config_word(pdev, pos + PCI_EXP_LNKSTA, &link_status);
        
        u8 speed = link_status & PCI_EXP_LNKSTA_SPEED;
        u8 width = (link_status & PCI_EXP_LNKSTA_WIDTH) >> 4;
        bool dll_active = !!(link_status & PCI_EXP_LNKSTA_DLLLA);
        
        pr_info("Link status: Gen%d x%d, DLLActive: %s\n",
                speed, width, dll_active ? "Yes" : "No");
        
        if (!dll_active) {
            pr_err("Link not active!\n");
            ret = -ENODEV;
            goto err_unmap_bars;
        }
    }
    
    // ========== 7. 分配中断 ==========
    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_LEGACY | PCI_IRQ_MSI);
    if (ret < 0) {
        pr_err("Failed to allocate IRQ vectors\n");
        goto err_unmap_bars;
    }
    
    dev->irq = pci_irq_vector(pdev, 0);
    ret = request_irq(dev->irq, my_interrupt_handler,
                      IRQF_SHARED, DRIVER_NAME, dev);
    if (ret) {
        pr_err("Failed to request IRQ\n");
        goto err_free_irq_vectors;
    }
    
    // ========== 8. 设备特定初始化 ==========
    // 根据你的设备做特定配置
    iowrite32(INIT_VALUE, dev->bar0 + DEVICE_CONTROL_REG);
    
    pr_info("Device initialized successfully\n");
    return 0;
    
    // 错误处理...
err_free_irq_vectors:
    pci_free_irq_vectors(pdev);
err_unmap_bars:
    pci_iounmap(pdev, dev->bar0);
err_release_regions:
    pci_release_regions(pdev);
err_disable_device:
    pci_disable_device(pdev);
err_free_dev:
    kfree(dev);
    return ret;
}
```

### 4.4 驱动不需要做的事情

**这些已经在之前阶段完成：**

- ❌ 链路训练（硬件自动，BIOS 等待完成）
- ❌ 枚举设备（BIOS/内核完成）
- ❌ 分配 BAR 地址空间（BIOS/内核完成）
- ❌ 分配中断号（BIOS/内核完成）

**驱动只需要：**
- ✅ 使用已分配的资源
- ✅ 映射 BAR 到虚拟地址
- ✅ 注册中断处理函数
- ✅ 初始化设备特定功能

---

## 时间线总结

### 完整时间线

```
时间轴:  0ms    50ms   100ms   150ms   200ms   250ms   300ms
         │      │      │       │       │       │       │
         ├──────┼──────┼───────┼───────┼───────┼───────┤
         │训练  │枚举  │OS启动 │驱动   │设备   │       │
         │      │      │       │匹配   │可用   │       │
         │      │      │       │       │       │       │
0-50ms:  硬件上电 + 链路训练
         ├─> Detect: ~24ms
         ├─> Polling: ~12ms
         └─> Configuration: ~16ms
         └─> L0: 完成
         
50-150ms: BIOS/UEFI 枚举
         ├─> 等待训练完成: ~10ms
         ├─> 扫描总线: ~20ms
         ├─> 分配资源: ~30ms
         └─> 配置设备: ~40ms
         
150-200ms: 操作系统启动
         ├─> PCI 子系统初始化: ~10ms
         ├─> 从 ACPI 读取信息: ~20ms
         └─> 建立设备列表: ~20ms
         
200-250ms: 驱动加载
         ├─> 驱动注册: ~5ms
         ├─> 设备匹配: ~10ms
         └─> probe() 执行: ~35ms
         
250ms+: 设备可用
         └─> 可以正常使用设备
```

### 各阶段持续时间

| 阶段 | 持续时间 | 说明 |
|------|---------|------|
| **链路训练** | 50-100ms | 硬件自动，取决于链路质量 |
| **BIOS 枚举** | 50-100ms | 取决于设备数量 |
| **OS 初始化** | 20-50ms | 内核 PCI 子系统 |
| **驱动加载** | 10-50ms | 取决于驱动复杂度 |
| **总计** | 130-300ms | 从系统上电到设备可用 |

---

## 各阶段的关系

### 依赖关系

```
阶段1（链路训练）
    │
    │ 必须完成
    ▼
阶段2（枚举）
    │
    │ 需要链路已建立
    ▼
阶段3（OS 初始化）
    │
    │ 使用枚举结果
    ▼
阶段4（驱动加载）
    │
    │ 使用已分配的资源
    ▼
设备可用
```

### 关键点

1. **链路训练必须先完成**
   - 枚举需要访问配置空间
   - 配置空间访问需要链路已建立

2. **枚举必须在驱动加载前完成**
   - 驱动需要知道设备在哪里
   - 驱动需要知道分配了什么资源

3. **驱动使用已分配的资源**
   - BAR 地址已分配（物理地址）
   - 驱动映射到虚拟地址
   - 中断号已分配
   - 驱动注册中断处理函数

---

## 故障排查

### 1. 链路训练失败

**症状：**
- 设备无法被发现
- `lspci` 看不到设备
- 枚举时读取 Vendor ID = 0xFFFF

**可能原因：**
- 物理连接问题（设备未插好）
- 信号完整性问题（PCB 走线问题）
- 电源问题（供电不稳定）
- 设备故障

**排查方法：**
```bash
# 查看链路状态
lspci -vvv | grep -A 5 "LnkSta"

# 查看内核日志
dmesg | grep -i pcie

# 检查物理连接
# 检查电源
```

### 2. 枚举失败

**症状：**
- 设备被发现但资源未分配
- BAR 地址为 0
- 中断未分配

**可能原因：**
- 资源冲突
- BIOS 配置问题
- 设备配置空间损坏

**排查方法：**
```bash
# 查看设备信息
lspci -vvv -s 00:01.0

# 查看资源分配
cat /proc/iomem | grep pci
cat /proc/ioports | grep pci

# 查看中断
cat /proc/interrupts | grep pci
```

### 3. 驱动加载失败

**症状：**
- 设备被发现但无驱动
- `lsmod` 看不到驱动模块
- `dmesg` 显示 probe 失败

**可能原因：**
- 驱动未编译/未加载
- Vendor ID / Device ID 不匹配
- probe() 函数返回错误

**排查方法：**
```bash
# 查看设备信息
lspci -k -s 00:01.0

# 查看驱动匹配
dmesg | grep -i "my_device"

# 查看驱动模块
lsmod | grep my_driver
modinfo my_driver.ko
```

---

## 实际代码示例

### 完整的驱动初始化代码

```c
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/io.h>

#define DRIVER_NAME "my_pcie_device"

static int my_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct my_device *dev;
    int ret;
    int pos;
    u16 link_status;
    
    pr_info("Probing device %04x:%04x\n", pdev->vendor, pdev->device);
    
    // 1. 分配设备结构
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;
    
    dev->pdev = pdev;
    pci_set_drvdata(pdev, dev);
    
    // 2. 使能设备（链路训练已在 BIOS 阶段完成）
    ret = pci_enable_device(pdev);
    if (ret) {
        pr_err("Failed to enable device\n");
        goto err_free_dev;
    }
    
    // 3. 启用总线主控
    pci_set_master(pdev);
    
    // 4. 请求资源
    ret = pci_request_regions(pdev, DRIVER_NAME);
    if (ret) {
        pr_err("Failed to request regions\n");
        goto err_disable_device;
    }
    
    // 5. 映射 BAR
    dev->bar0 = pci_iomap(pdev, 0, 0);
    if (!dev->bar0) {
        pr_err("Failed to map BAR0\n");
        ret = -ENOMEM;
        goto err_release_regions;
    }
    
    // 6. 检查链路状态（验证训练结果）
    pos = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (pos) {
        pci_read_config_word(pdev, pos + PCI_EXP_LNKSTA, &link_status);
        
        u8 speed = link_status & PCI_EXP_LNKSTA_SPEED;
        u8 width = (link_status & PCI_EXP_LNKSTA_WIDTH) >> 4;
        bool dll_active = !!(link_status & PCI_EXP_LNKSTA_DLLLA);
        
        pr_info("Link status: Gen%d x%d, DLLActive: %s\n",
                speed, width, dll_active ? "Yes" : "No");
        
        if (!dll_active) {
            pr_err("Link not active after training!\n");
            ret = -ENODEV;
            goto err_unmap_bars;
        }
    }
    
    // 7. 分配中断
    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_LEGACY | PCI_IRQ_MSI);
    if (ret < 0) {
        pr_err("Failed to allocate IRQ vectors\n");
        goto err_unmap_bars;
    }
    
    dev->irq = pci_irq_vector(pdev, 0);
    ret = request_irq(dev->irq, my_interrupt_handler,
                      IRQF_SHARED, DRIVER_NAME, dev);
    if (ret) {
        pr_err("Failed to request IRQ %d\n", dev->irq);
        goto err_free_irq_vectors;
    }
    
    // 8. 设备特定初始化
    // 根据你的设备做特定配置
    iowrite32(0x1234, dev->bar0 + 0x00);
    
    pr_info("Device initialized successfully\n");
    return 0;
    
err_free_irq_vectors:
    pci_free_irq_vectors(pdev);
err_unmap_bars:
    pci_iounmap(pdev, dev->bar0);
err_release_regions:
    pci_release_regions(pdev);
err_disable_device:
    pci_disable_device(pdev);
err_free_dev:
    kfree(dev);
    return ret;
}

static void my_remove(struct pci_dev *pdev)
{
    struct my_device *dev = pci_get_drvdata(pdev);
    
    if (!dev)
        return;
    
    // 清理资源（与 probe 相反的顺序）
    free_irq(dev->irq, dev);
    pci_free_irq_vectors(pdev);
    pci_iounmap(pdev, dev->bar0);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    kfree(dev);
}

static const struct pci_device_id my_device_ids[] = {
    { PCI_DEVICE(0x1234, 0x5678), },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, my_device_ids);

static struct pci_driver my_driver = {
    .name = DRIVER_NAME,
    .id_table = my_device_ids,
    .probe = my_probe,
    .remove = my_remove,
};

static int __init my_init(void)
{
    return pci_register_driver(&my_driver);
}

static void __exit my_exit(void)
{
    pci_unregister_driver(&my_driver);
}

module_init(my_init);
module_exit(my_exit);
```

---

## 总结

### 关键要点

1. **链路训练是硬件自动的**
   - 设备上电时自动开始
   - 不需要软件干预
   - 结果保存在 Link Status 寄存器

2. **枚举是软件/固件完成的**
   - BIOS/UEFI 在启动时执行
   - 内核也可能重新枚举
   - 分配资源，建立设备列表

3. **驱动使用已分配的资源**
   - 不需要重新训练链路
   - 不需要重新枚举
   - 只需要映射 BAR、注册中断、初始化设备

4. **执行顺序是固定的**
   - 必须先训练链路
   - 然后才能枚举
   - 最后加载驱动

### 学习建议

1. **理解各阶段的职责**
   - 硬件做什么
   - 固件做什么
   - 内核做什么
   - 驱动做什么

2. **理解各阶段的关系**
   - 为什么必须先训练链路
   - 为什么必须先枚举
   - 驱动如何使用已分配的资源

3. **实践**
   - 查看 `lspci -vvv` 输出
   - 查看内核日志
   - 编写和调试驱动

### 相关文档

- [PCIe_LINK_TRAINING.md](PCIe_LINK_TRAINING.md) - 链路训练详细说明
- [PCIe_OVERVIEW.md](PCIe_OVERVIEW.md) - PCIe 协议总览
- [RUNTIME_OPERATIONS.md](RUNTIME_OPERATIONS.md) - 运行时操作

---

**本文档提供了 PCIe 从系统上电到设备可用的完整初始化流程，包括所有关键步骤和代码示例，适合深入学习 PCIe 初始化机制。**

