# PCIe 设备功能（Function）详解

## 核心概念

### 1. 设备位置（Slot）vs 功能（Function）

```
PCIe 总线结构：
├─ 设备位置 0 (Device 0)
│   ├─ 功能 0 (Function 0) ← 大多数设备只有这个
│   ├─ 功能 1 (Function 1) ← 只有多功能设备才有
│   ├─ 功能 2 (Function 2)
│   ├─ ...
│   └─ 功能 7 (Function 7)
│
├─ 设备位置 1 (Device 1)
│   └─ 功能 0 (Function 0) ← 单功能设备
│
└─ 设备位置 2 (Device 2)
    ├─ 功能 0 (Function 0)
    ├─ 功能 1 (Function 1) ← 多功能设备
    └─ 功能 2 (Function 2)
```

### 2. 关键点

**❌ 错误理解**：每个设备都有 8 个功能

**✅ 正确理解**：
- 每个**设备位置**最多可以有 8 个功能（0-7）
- 但**大多数设备只有功能 0**
- 只有**多功能设备（multifunction device）**才会有多个功能

---

## 1. 设备功能数量

### 1.1 单功能设备（大多数情况）

```
设备位置 0：
  └─ 功能 0：网卡控制器
      （功能 1-7 不存在）

设备位置 1：
  └─ 功能 0：存储控制器
      （功能 1-7 不存在）
```

**特点**：
- 只有一个功能（功能 0）
- 读取功能 1-7 的 Vendor ID 会返回 `0xffff`（设备不存在）

### 1.2 多功能设备（少数情况）

```
设备位置 0：
  ├─ 功能 0：网卡控制器（主功能）
  ├─ 功能 1：网卡控制器（第二个端口）
  └─ 功能 2：管理功能
      （功能 3-7 不存在）
```

**特点**：
- 有多个功能（功能 0 必须存在）
- 功能 0 的 Header Type 寄存器 bit 7 = 1，表示这是多功能设备
- 其他功能（1-7）可能存在，也可能不存在

---

## 2. 如何判断是否多功能设备？

### 2.1 读取 Header Type 寄存器

```c
// drivers/pci/probe.c:1932

hdr_type = pci_hdr_type(dev);
dev->hdr_type = hdr_type & 0x7f;        // 低 7 位：头部类型
dev->multifunction = !!(hdr_type & 0x80);  // bit 7：是否多功能
```

**Header Type 寄存器格式**：
```
Bit 7: 多功能标志
  - 0 = 单功能设备
  - 1 = 多功能设备

Bit 6-0: 头部类型
  - 0x00 = 标准设备
  - 0x01 = PCI-to-PCI 桥接器
  - 0x02 = CardBus 桥接器
```

### 2.2 判断逻辑

```c
// 读取 Header Type
pci_read_config_byte(dev, PCI_HEADER_TYPE, &hdr_type);

if (hdr_type & 0x80) {
    // 多功能设备
    // 需要扫描功能 1-7
} else {
    // 单功能设备
    // 只扫描功能 0
}
```

---

## 3. 扫描逻辑

### 3.1 扫描循环设计

```c
// drivers/pci/probe.c:2937

for (devfn = 0; devfn < 256; devfn += 8) {
    pci_scan_slot(bus, devfn);  // 扫描设备位置 devfn/8 的所有功能
}
```

**为什么每次增加 8？**

- `devfn = (device << 3) | function`
- 每个设备位置最多有 8 个功能（0-7）
- 所以 `devfn` 的范围是：
  - 设备 0：devfn = 0x00-0x07
  - 设备 1：devfn = 0x08-0x0F
  - 设备 2：devfn = 0x10-0x17
  - ...
  - 设备 31：devfn = 0xF8-0xFF

**扫描顺序**：
```
devfn = 0x00 → 扫描设备 0，功能 0
devfn = 0x08 → 扫描设备 1，功能 0
devfn = 0x10 → 扫描设备 2，功能 0
...
devfn = 0xF8 → 扫描设备 31，功能 0
```

### 3.2 扫描槽位：`pci_scan_slot()`

```c
// drivers/pci/probe.c:2710

int pci_scan_slot(struct pci_bus *bus, int devfn)
{
    struct pci_dev *dev;
    int fn = 0, nr = 0;
    
    // 1. 先扫描功能 0（所有设备都必须有功能 0）
    dev = pci_scan_single_device(bus, devfn + 0);
    if (dev) {
        if (!pci_dev_is_added(dev))
            nr++;
    } else if (fn == 0) {
        // 如果功能 0 不存在，说明整个设备位置都不存在
        // （除非是虚拟化环境，允许只有功能 1-7）
        if (!hypervisor_isolated_pci_functions())
            break;  // 跳过这个设备位置
    }
    
    // 2. 如果功能 0 存在，检查是否是多功能设备
    if (dev && dev->multifunction) {
        // 扫描其他7功能（1-7）
        fn = next_fn(bus, dev, fn);
        while (fn >= 0) {
            dev = pci_scan_single_device(bus, devfn + fn);
            if (dev) {
                if (!pci_dev_is_added(dev))
                    nr++;
                dev->multifunction = 1;
            }
            fn = next_fn(bus, dev, fn);
        }
    }
    
    return nr;
}
```

**关键逻辑**：
1. **先扫描功能 0**：所有设备都必须有功能 0
2. **检查是否多功能**：读取功能 0 的 Header Type，bit 7 = 1？
3. **如果是多功能**：继续扫描功能 1-7
4. **如果是单功能**：只扫描功能 0，跳过功能 1-7

---

## 4. 实际示例

### 4.1 单功能设备示例

```
设备位置 0（devfn = 0x00）：
  ├─ 功能 0：Vendor ID = 0x10ec, Device ID = 0x8168（Realtek 网卡）
  │   └─ Header Type = 0x00（单功能设备）
  │
  ├─ 功能 1：Vendor ID = 0xffff（不存在）
  ├─ 功能 2：Vendor ID = 0xffff（不存在）
  └─ ...（功能 3-7 都不存在）

扫描结果：
  - 只发现功能 0
  - 功能 1-7 读取 Vendor ID 返回 0xffff，跳过
```

### 4.2 多功能设备示例

```
设备位置 0（devfn = 0x00）：
  ├─ 功能 0：Vendor ID = 0x8086, Device ID = 0x1234
  │   └─ Header Type = 0x80（多功能设备，bit 7 = 1）
  │
  ├─ 功能 1：Vendor ID = 0x8086, Device ID = 0x1235（存在）
  ├─ 功能 2：Vendor ID = 0x8086, Device ID = 0x1236（存在）
  ├─ 功能 3：Vendor ID = 0xffff（不存在）
  └─ ...（功能 4-7 都不存在）

扫描结果：
  - 发现功能 0（多功能标志 = 1）
  - 继续扫描功能 1-7
  - 发现功能 1 和功能 2
  - 功能 3-7 读取 Vendor ID 返回 0xffff，跳过
```

---

## 5. 为什么这样设计？

### 5.1 性能优化

如果每个设备位置都扫描 8 个功能，会非常慢：

```
256 个设备位置 × 8 个功能 = 2048 次配置空间读取
```

但实际优化后：

```
1. 先扫描功能 0（256 次读取）
2. 只对多功能设备扫描其他功能（假设 10% 是多功能）
   = 256 × 0.1 × 7 = 179 次读取
总计：约 435 次读取（而不是 2048 次）
```

### 5.2 多功能设备标志

通过 Header Type bit 7，可以快速判断：
- **单功能设备**：只扫描功能 0，跳过功能 1-7
- **多功能设备**：扫描功能 0，然后继续扫描功能 1-7

---

## 6. 常见设备类型

### 6.1 单功能设备（大多数）

- **网卡**：通常只有功能 0
- **存储控制器**：通常只有功能 0
- **USB 控制器**：通常只有功能 0
- **显卡**：通常只有功能 0

### 6.2 多功能设备（少数）

- **多端口网卡**：
  - 功能 0：端口 1
  - 功能 1：端口 2
  - 功能 2：端口 3
  - ...

- **组合设备**：
  - 功能 0：网卡
  - 功能 1：音频控制器
  - 功能 2：管理功能

- **某些服务器网卡**：
  - 功能 0：主控制器
  - 功能 1：辅助控制器
  - 功能 2：管理接口

---

## 7. 总结

### 7.1 关键点

| 方面 | 说明 |
|------|------|
| **设备位置** | 每个总线最多 32 个设备位置（0-31） |
| **功能数量** | 每个设备位置最多 8 个功能（0-7） |
| **单功能设备** | 大多数设备只有功能 0 |
| **多功能设备** | 少数设备有多个功能，通过 Header Type bit 7 标识 |
| **扫描优化** | 先扫描功能 0，只有多功能设备才扫描其他功能 |

### 7.2 扫描策略

```
1. 遍历所有设备位置（devfn += 8）
2. 对每个设备位置：
   a. 扫描功能 0（必须存在）
   b. 检查 Header Type bit 7
   c. 如果是多功能设备，扫描功能 1-7
   d. 如果是单功能设备，跳过功能 1-7
```

### 7.3 代码位置

- **扫描循环**：`drivers/pci/probe.c:2937`
- **扫描槽位**：`drivers/pci/probe.c:2710`
- **判断多功能**：`drivers/pci/probe.c:1938`

---

## 8. 参考资料

- [PCI Local Bus Specification](https://pcisig.com/)
- [PCIe Base Specification](https://pcisig.com/)
- Linux 内核源码：`drivers/pci/probe.c`

