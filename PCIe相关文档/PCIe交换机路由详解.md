# PCIe Switch 路由机制

## 概述

**Switch 和 RC 类似，但功能不同**：
- **RC**：连接 CPU 和 PCIe 总线，负责地址路由
- **Switch**：扩展 PCIe 总线，负责 TLP 路由

---

## Switch 的路由机制

### 1. Switch 如何识别设备

**Switch 通过 TLP 中的路由信息识别设备：**

```
TLP 路由信息:
═══════════════════════════════════════════════════════════════════

Memory Read/Write TLP:
┌─────────────────────────────────────┐
│ TLP Header                          │
│ ├─> Type: Memory Read/Write        │
│ ├─> Address: 0xFEA00000            │
│ └─> Routing: Bus 2, Dev 0, Func 0 │ ← Switch 看这个
└─────────────────────────────────────┘

Configuration Read/Write TLP:
┌─────────────────────────────────────┐
│ TLP Header                          │
│ ├─> Type: Config Read/Write        │
│ ├─> Bus: 2                          │ ← Switch 看这个
│ ├─> Dev: 0                          │
│ └─> Func: 0                         │
└─────────────────────────────────────┘
```

### 2. Switch 的路由表

**Switch 内部有路由表（类似 RC）：**

```
Switch 路由表:
═══════════════════════════════════════════════════════════════════

┌─────────────────────────────────────────────────────────────┐
│ Switch 内部路由表                                           │
├─────────────────────────────────────────────────────────────┤
│ Bus 范围        │ 输出端口                                  │
├─────────────────────────────────────────────────────────────┤
│ Bus 1           │ Port 1 (上游，连接 RC)                   │
│ Bus 2           │ Port 2 (下游，连接 Device A)             │
│ Bus 3           │ Port 3 (下游，连接 Device B)              │
│ Bus 4-5         │ Port 4 (下游，连接另一个 Switch)          │
└─────────────────────────────────────────────────────────────┘
```

### 3. Switch 路由过程

```
TLP 路由流程:
═══════════════════════════════════════════════════════════════════

1. Switch 接收 TLP
   ├─> 提取路由信息（Bus/Dev/Func 或 Address）
   └─> 查找路由表

2. Switch 查找路由表
   ├─> 根据 Bus 号确定输出端口
   └─> 如果 Bus 在上游 → Port 1
      如果 Bus 在下游 → 对应的下游 Port

3. Switch 转发 TLP
   └─> 从对应的端口发送出去
```

---

## Switch vs RC 的区别

### 1. 功能对比

| 特性 | Root Complex (RC) | Switch |
|------|------------------|--------|
| **位置** | CPU 和 PCIe 之间 | PCIe 总线中间 |
| **主要功能** | 地址路由、协议转换 | TLP 路由、总线扩展 |
| **路由依据** | 地址范围 | Bus 号 |
| **路由表** | 地址 → Bus/Dev/Func | Bus 号 → 端口 |
| **配置** | 软件配置地址路由表 | 硬件自动学习或软件配置 |

### 2. 路由方式对比

**RC 的路由（基于地址）：**
```
CPU 访问 0xFEA00000
    │
    ├─> RC 查找地址路由表
    ├─> 匹配: 0xFEA00000 → Bus 1, Dev 0, Func 0
    └─> 生成 TLP，包含 Bus/Dev/Func
```

**Switch 的路由（基于 Bus 号）：**
```
Switch 收到 TLP (Bus 2, Dev 0, Func 0)
    │
    ├─> Switch 查找 Bus 路由表
    ├─> 匹配: Bus 2 → Port 2
    └─> 从 Port 2 转发 TLP
```

---

## Switch 的路由表创建

### 1. 自动学习（硬件）

**某些 Switch 支持自动学习：**

```
Switch 自动学习过程:
═══════════════════════════════════════════════════════════════════

1. 设备枚举时
   ├─> 配置空间访问 TLP 经过 Switch
   ├─> Switch 记录: Bus 2 → Port 2
   └─> 自动更新路由表

2. 后续 TLP
   ├─> Switch 根据 Bus 号查找路由表
   └─> 自动转发到对应端口
```

### 2. 软件配置

**某些 Switch 需要软件配置：**

```c
/**
 * 配置 Switch 路由表
 */
void configure_switch_routing_table(uint8_t switch_bus, uint8_t switch_dev,
                                     uint8_t port, uint8_t downstream_bus)
{
    // 访问 Switch 的配置空间
    uint8_t pos = pcie_find_capability(switch_bus, switch_dev, 0, PCI_CAP_ID_EXP);
    
    // Switch 有特殊的配置寄存器（平台/厂商特定）
    // 配置下游总线和端口的映射关系
    pcie_write_config8(switch_bus, switch_dev, 0, 
                       SWITCH_ROUTING_TABLE_BASE + port, downstream_bus);
}
```

---

## Switch 拓扑示例

```
PCIe 拓扑:
═══════════════════════════════════════════════════════════════════

CPU
│
└─> Root Complex (Bus 0)
    │
    └─> Root Port 0
        │
        └─> Switch (Bus 1, Dev 0, Func 0)
            │
            ├─> Port 1 (上游) ──> RC
            │
            ├─> Port 2 (下游) ──> Bus 2 ──> Device A
            │
            ├─> Port 3 (下游) ──> Bus 3 ──> Device B
            │
            └─> Port 4 (下游) ──> Bus 4 ──> Switch 2
                                    │
                                    └─> Bus 5 ──> Device C
```

**Switch 路由表：**
```
Bus 0-1  → Port 1 (上游)
Bus 2    → Port 2 (Device A)
Bus 3    → Port 3 (Device B)
Bus 4-5  → Port 4 (Switch 2)
```

---

## Switch 如何识别设备

### 1. 基于 Bus 号

**Switch 主要根据 Bus 号路由：**

```c
// Switch 路由逻辑（简化）
void switch_route_tlp(struct tlp *tlp)
{
    uint8_t target_bus = extract_bus_from_tlp(tlp);
    
    // 查找路由表
    uint8_t output_port = switch_routing_table[target_bus];
    
    // 转发到对应端口
    switch_send_to_port(output_port, tlp);
}
```

### 2. 基于地址（某些 Switch）

**某些 Switch 也支持基于地址的路由：**

```
Memory TLP (地址: 0xFEA00000)
    │
    ├─> Switch 查找地址路由表
    ├─> 匹配: 0xFEA00000 → Port 2
    └─> 转发到 Port 2
```

---

## 总结

### Switch 和 RC 的相似点

1. **都有路由表**：用于决定 TLP 的转发方向
2. **都负责路由**：将 TLP 路由到正确的目标
3. **都需要配置**：路由表需要软件配置或硬件学习

### Switch 和 RC 的不同点

| 方面 | RC | Switch |
|------|----|----|
| **路由依据** | 地址范围 | Bus 号 |
| **路由表内容** | 地址 → Bus/Dev/Func | Bus → 端口 |
| **主要功能** | 协议转换、地址路由 | TLP 转发、总线扩展 |
| **位置** | CPU 侧 | PCIe 总线中间 |

### 关键点

- **Switch 通过 Bus 号识别设备**：TLP 中包含目标 Bus 号
- **Switch 有路由表**：Bus 号 → 输出端口
- **Switch 和 RC 类似但不完全一样**：RC 负责地址路由，Switch 负责 TLP 路由


