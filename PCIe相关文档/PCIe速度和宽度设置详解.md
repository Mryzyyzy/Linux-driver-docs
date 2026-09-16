# PCIe Speed 和 Width 设置详解

## 概述

**Speed（速度）** 和 **Width（宽度）** 是 PCIe 链路训练后协商得到的两个关键参数，决定了链路的带宽。

## Speed（链路速度）

### 1. 什么是 Speed？

**Speed** 表示每个 Lane 的数据传输速率。

```
Speed = 每个 Lane 的传输速率
```

### 2. PCIe 速度等级

| Speed 值 | 速度 | 标准名称 | 说明 |
|---------|------|---------|------|
| 1 | 2.5 GT/s | Gen1 | PCIe 1.0 |
| 2 | 5.0 GT/s | Gen2 | PCIe 2.0 |
| 3 | 8.0 GT/s | Gen3 | PCIe 3.0 |
| 4 | 16.0 GT/s | Gen4 | PCIe 4.0 |
| 5 | 32.0 GT/s | Gen5 | PCIe 5.0 |
| 6 | 64.0 GT/s | Gen6 | PCIe 6.0 |

**注意**：
- GT/s = Giga Transfers per second（每秒千兆传输）
- 实际有效带宽 = GT/s × 编码效率（8b/10b 或 128b/130b）

### 3. 有效带宽计算

```
Gen1/Gen2: 使用 8b/10b 编码，效率 = 80%
  - Gen1: 2.5 GT/s × 0.8 = 2.0 Gbps 有效带宽
  - Gen2: 5.0 GT/s × 0.8 = 4.0 Gbps 有效带宽

Gen3+: 使用 128b/130b 编码，效率 ≈ 98.5%
  - Gen3: 8.0 GT/s × 0.985 = 7.88 Gbps 有效带宽
  - Gen4: 16.0 GT/s × 0.985 = 15.76 Gbps 有效带宽
```

## Width（链路宽度）

### 1. 什么是 Width？

**Width** 表示链路使用的 Lane 数量。

```
Width = 使用的 Lane 数量
```

### 2. PCIe 宽度选项

| Width 值 | 说明 | Lane 数量 |
|---------|------|----------|
| 1 | x1 | 1 个 Lane |
| 2 | x2 | 2 个 Lane |
| 4 | x4 | 4 个 Lane |
| 8 | x8 | 8 个 Lane |
| 16 | x16 | 16 个 Lane |
| 32 | x32 | 32 个 Lane（较少见）|

### 3. 总带宽计算

```
总带宽 = Speed × Width × 编码效率

示例：
- Gen3 x4: 8.0 GT/s × 4 × 0.985 = 31.52 Gbps ≈ 3.94 GB/s
- Gen4 x16: 16.0 GT/s × 16 × 0.985 = 252.16 Gbps ≈ 31.52 GB/s
```

## Speed 和 Width 的寄存器

### 1. Link Status 寄存器（当前值）

**寄存器位置：** `PCIe Capability + 0x12`

```
Link Status (LnkSta) - 16位寄存器
─────────────────────────────────────────
Bit  字段              说明
─────────────────────────────────────────
0-3   Current Speed    当前链路速度（只读）
4-9   Negotiated Width 协商的链路宽度（只读）
10    Training        链路训练状态
11    Slot Clock      插槽时钟状态
12    DLLActive       数据链路层激活
...
```

**读取当前 Speed 和 Width：**

```c
// ATF 环境示例
uint16_t link_status = pcie_read16(cfg_base, pos + 0x12);

uint8_t current_speed = link_status & 0x0F;      // Bit 0-3
uint8_t current_width = (link_status >> 4) & 0x3F; // Bit 4-9

// 解析速度
const char *speed_str;
switch (current_speed) {
    case 1: speed_str = "2.5 GT/s (Gen1)"; break;
    case 2: speed_str = "5.0 GT/s (Gen2)"; break;
    case 3: speed_str = "8.0 GT/s (Gen3)"; break;
    case 4: speed_str = "16.0 GT/s (Gen4)"; break;
    case 5: speed_str = "32.0 GT/s (Gen5)"; break;
    default: speed_str = "Unknown"; break;
}

INFO("Current Speed: %s\n", speed_str);
INFO("Current Width: x%d\n", current_width);
```

### 2. Link Capabilities 寄存器（最大支持值）

**寄存器位置：** `PCIe Capability + 0x0C`

```
Link Capabilities (LnkCap) - 32位寄存器
─────────────────────────────────────────
Bit  字段              说明
─────────────────────────────────────────
0-3   Max Speed        最大支持速度（只读）
4-9   Max Width        最大支持宽度（只读）
...
```

**读取最大支持值：**

```c
uint32_t link_cap = pcie_read32(cfg_base, pos + 0x0C);

uint8_t max_speed = link_cap & 0x0F;              // Bit 0-3
uint8_t max_width = (link_cap >> 4) & 0x3F;       // Bit 4-9

INFO("Max Speed: Gen%d\n", max_speed);
INFO("Max Width: x%d\n", max_width);
```

## Speed 和 Width 的设置

### 重要说明

**Speed 和 Width 是链路训练时自动协商的，软件通常不能直接设置！**

但是，可以通过以下方式影响 Speed 和 Width：

### 1. 触发重新训练（Retrain Link）

**通过 Link Control 寄存器触发重新训练：**

```c
/**
 * 触发链路重新训练
 * 重新训练可能会改变 Speed 和 Width（如果链路条件改变）
 */
void pcie_retrain_link(uintptr_t cfg_base)
{
    uint8_t pos = pcie_find_capability(cfg_base);
    if (pos == 0)
        return;
    
    uint16_t link_control = pcie_read16(cfg_base, pos + 0x10);
    
    // 设置 Retrain Link 位 (Bit 4)
    link_control |= (1 << 4);
    
    pcie_write16(cfg_base, pos + 0x10, link_control);
    
    INFO("Link retrain requested\n");
}
```

### 2. 禁用硬件自主宽度切换

**某些平台支持硬件自主调整宽度，可以禁用：**

```c
/**
 * 禁用硬件自主宽度切换
 * 这样宽度就不会自动改变
 */
void pcie_disable_hw_autonomous_width(uintptr_t cfg_base)
{
    uint8_t pos = pcie_find_capability(cfg_base);
    if (pos == 0)
        return;
    
    uint16_t link_control = pcie_read16(cfg_base, pos + 0x10);
    
    // 设置 Hardware Autonomous Width Disable 位 (Bit 8)
    link_control |= (1 << 8);
    
    pcie_write16(cfg_base, pos + 0x10, link_control);
}
```

### 3. 限制最大速度（某些平台支持）

**某些 PCIe 控制器支持限制最大速度：**

```c
/**
 * 限制最大速度（平台特定）
 * 注意：这需要平台特定的寄存器，不是标准 PCIe 寄存器
 */
void pcie_limit_max_speed_platform_specific(uintptr_t controller_base, uint8_t max_gen)
{
    // 这是平台特定的实现
    // 例如：写入 PCIe 控制器的配置寄存器
    // mmio_write_32(controller_base + PLATFORM_SPEED_LIMIT_REG, max_gen);
}
```

## 完整示例代码

### 读取 Speed 和 Width

```c
/**
 * 读取并显示当前的 Speed 和 Width
 */
void pcie_read_speed_width(uintptr_t cfg_base)
{
    uint8_t pos = pcie_find_capability(cfg_base);
    if (pos == 0)
        return;
    
    // 读取 Link Status
    uint16_t link_status = pcie_read16(cfg_base, pos + 0x12);
    uint8_t current_speed = link_status & 0x0F;
    uint8_t current_width = (link_status >> 4) & 0x3F;
    
    // 读取 Link Capabilities
    uint32_t link_cap = pcie_read32(cfg_base, pos + 0x0C);
    uint8_t max_speed = link_cap & 0x0F;
    uint8_t max_width = (link_cap >> 4) & 0x3F;
    
    // 显示结果
    INFO("Link Status:\n");
    INFO("  Current Speed: Gen%d\n", current_speed);
    INFO("  Current Width: x%d\n", current_width);
    INFO("  Max Speed: Gen%d\n", max_speed);
    INFO("  Max Width: x%d\n", max_width);
    
    // 检查是否降级
    if (current_speed < max_speed || current_width < max_width) {
        WARN("Link degraded!\n");
        WARN("  Max: Gen%d x%d\n", max_speed, max_width);
        WARN("  Current: Gen%d x%d\n", current_speed, current_width);
    }
}
```

### 触发重新训练

```c
/**
 * 触发链路重新训练并等待完成
 */
int pcie_retrain_and_wait(uintptr_t cfg_base, uint32_t timeout_ms)
{
    uint8_t pos = pcie_find_capability(cfg_base);
    if (pos == 0)
        return -1;
    
    // 1. 触发重新训练
    uint16_t link_control = pcie_read16(cfg_base, pos + 0x10);
    link_control |= (1 << 4);  // Retrain Link
    pcie_write16(cfg_base, pos + 0x10, link_control);
    
    // 2. 等待训练完成
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        uint16_t link_status = pcie_read16(cfg_base, pos + 0x12);
        
        // 检查训练是否完成（Training bit = 0 且 DLLActive = 1）
        if (!(link_status & (1 << 10)) && (link_status & (1 << 12))) {
            INFO("Link retrain completed\n");
            return 0;
        }
        
        mdelay(10);  // 等待 10ms
        elapsed += 10;
    }
    
    ERROR("Link retrain timeout\n");
    return -1;
}
```

## Speed 和 Width 的协商过程

### 链路训练时的协商

```
1. 双方设备读取对方的 Link Capabilities
   ├─> 获取对方支持的最大 Speed
   └─> 获取对方支持的最大 Width

2. 选择双方都支持的最高值
   ├─> Speed = min(设备A最大Speed, 设备B最大Speed)
   └─> Width = min(设备A最大Width, 设备B最大Width)

3. 考虑物理限制
   ├─> 如果链路质量不好，可能降级
   └─> 如果某些 Lane 故障，Width 可能减少

4. 协商完成
   └─> 结果写入 Link Status 寄存器
```

### 示例

```
设备 A: 支持 Gen4 x16
设备 B: 支持 Gen3 x8
物理链路: 质量良好，所有 Lane 正常

协商结果:
├─> Speed = min(Gen4, Gen3) = Gen3
└─> Width = min(x16, x8) = x8

最终: Gen3 x8
```

## 常见问题

### Q1: 为什么 Speed/Width 低于预期？

**可能原因：**
1. 对方设备不支持更高的 Speed/Width
2. 物理链路质量不好（降级）
3. 某些 Lane 故障（Width 减少）
4. 电源管理限制

### Q2: 如何强制使用特定的 Speed/Width？

**通常不能直接强制**，但可以：
1. 触发重新训练（可能改变）
2. 检查物理连接（确保所有 Lane 正常）
3. 检查对方设备的能力

### Q3: Speed 和 Width 可以动态改变吗？

**可以，但有限制：**
- **Speed**: 通常需要重新训练才能改变
- **Width**: 某些平台支持硬件自主调整（可以禁用）

## 总结

- **Speed**: 每个 Lane 的传输速率（Gen1-Gen6）
- **Width**: 使用的 Lane 数量（x1-x32）
- **总带宽**: Speed × Width × 编码效率
- **设置方式**: 主要通过链路训练自动协商，软件可以触发重新训练
- **读取方式**: 通过 Link Status 寄存器读取当前值，通过 Link Capabilities 读取最大支持值

