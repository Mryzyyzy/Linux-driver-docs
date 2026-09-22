# PCIe Bus/Dev/Func 参数说明

## 概述

**Bus、Dev、Func** 是 PCIe 设备的**三层地址标识**，用于唯一标识系统中的每个 PCIe 设备。

```
Bus 1, Dev 0, Func 0
│    │  │    │  │    │
│    │  │    │  │    └─> Function（功能号，0-7）
│    │  │    │  └──────> Device（设备号，0-31）
│    │  │    └─────────> Bus（总线号，0-255）
```

---

## 三个参数的含义

### 1. Bus（总线号）

**Bus = 总线编号**

- **范围**：0-255（8位）
- **作用**：标识设备所在的 PCIe 总线
- **说明**：
  - 每个 Root Port 或 Switch 下游端口创建一条新总线
  - Root Complex 通常从 Bus 0 开始
  - 通过 Switch 扩展时，总线号递增

**示例：**
```
Root Complex (Bus 0)
    │
    ├─> Root Port 0 ──> Bus 1 ──> Device A
    │
    └─> Root Port 1 ──> Bus 2 ──> Switch ──> Bus 3 ──> Device B
```

### 2. Dev（设备号）

**Dev = 设备编号**

- **范围**：0-31（5位）
- **作用**：标识总线上的设备
- **说明**：
  - 每条总线上最多 32 个设备
  - 设备号由硬件决定（通常从 0 开始）

**示例：**
```
Bus 1:
├─> Dev 0: 网卡
├─> Dev 1: SSD
└─> Dev 2: GPU
```

### 3. Func（功能号）

**Func = 功能编号**

- **范围**：0-7（3位）
- **作用**：标识设备内的功能
- **说明**：
  - 一个物理设备可以有多个功能（Multi-Function Device）
  - 单功能设备通常为 Func 0
  - 多功能设备：Func 0, Func 1, Func 2...

**示例：**
```
物理设备（Dev 0）:
├─> Func 0: 网卡功能
├─> Func 1: 音频功能
└─> Func 2: 其他功能
```

---

## 完整地址示例

### 示例1：简单设备

```
Bus 1, Dev 0, Func 0
│    │  │    │  │    │
│    │  │    │  │    └─> Func 0: 第一个功能
│    │  │    │  └──────> Dev 0: 第一个设备
│    │  │    └─────────> Bus 1: 第一条总线
```

**含义**：Bus 1 上的第 0 个设备的第 0 个功能

### 示例2：多功能设备

```
Bus 1, Dev 0, Func 1
│    │  │    │  │    │
│    │  │    │  │    └─> Func 1: 第二个功能
│    │  │    │  └──────> Dev 0: 第一个设备
│    │  │    └─────────> Bus 1: 第一条总线
```

**含义**：Bus 1 上的第 0 个设备的第 1 个功能

---

## 如何查看 Bus/Dev/Func

### Linux 系统

```bash
# 查看所有 PCIe 设备
lspci

# 输出示例：
# 01:00.0 Network controller: Intel Corporation ...
# │ │ │ │
# │ │ │ └─> Func 0
# │ │ └───> Dev 0
# │ └─────> Bus 1
```

### 代码中访问

```c
// 读取配置空间
uint32_t vendor_id = pcie_read_config32(bus, dev, func, 0x00);

// 示例：访问 Bus 1, Dev 0, Func 0
uint32_t vid = pcie_read_config32(1, 0, 0, 0x00);
```

---

## Bus/Dev/Func 的组合

### 地址编码

**Bus/Dev/Func 组合成一个 16 位地址：**

```c
// 编码方式
uint16_t bdf = (bus << 8) | (dev << 3) | func;

// 示例：Bus 1, Dev 0, Func 0
uint16_t bdf = (1 << 8) | (0 << 3) | 0;
// = 0x0100

// 解码方式
uint8_t bus  = (bdf >> 8) & 0xFF;
uint8_t dev  = (bdf >> 3) & 0x1F;
uint8_t func = bdf & 0x07;
```

### 地址范围

```
总地址空间:
═══════════════════════════════════════════════════════════════════

Bus:  256 个总线 (0-255)
Dev:  32 个设备/总线 (0-31)
Func: 8 个功能/设备 (0-7)

总容量: 256 × 32 × 8 = 65,536 个设备功能
```

---

## 实际应用

### 1. 设备枚举

```c
// 扫描所有可能的 Bus/Dev/Func
for (bus = 0; bus < 256; bus++) {
    for (dev = 0; dev < 32; dev++) {
        for (func = 0; func < 8; func++) {
            // 检查设备是否存在
            uint16_t vendor_id = pcie_read_config16(bus, dev, func, 0x00);
            if (vendor_id != 0xFFFF) {
                // 设备存在
                printf("Found device at %02X:%02X.%X\n", bus, dev, func);
            }
        }
    }
}
```

### 2. 路由表配置

```c
// 配置路由表时使用 Bus/Dev/Func
configure_rc_routing_table(
    bus,   // 1
    dev,   // 0
    func,  // 0
    bar_base,
    bar_size
);
```

---

## 总结

| 参数 | 范围 | 说明 | 示例 |
|------|------|------|------|
| **Bus** | 0-255 | 总线号，标识设备所在的总线 | Bus 1 |
| **Dev** | 0-31 | 设备号，标识总线上的设备 | Dev 0 |
| **Func** | 0-7 | 功能号，标识设备内的功能 | Func 0 |

**Bus 1, Dev 0, Func 0** 表示：
- 第 1 条总线上的
- 第 0 个设备的
- 第 0 个功能

**这是 PCIe 设备的唯一标识！**


