# PCIe Bus 号的来源

## 关键问题

**你说得对！** 如果系统要读取 Bus 1，那 Bus 1 的位置必须是已知的、固定的，不能是"枚举时临时分配"的。

---

## Bus 号的真实来源

### 1. Bus 0 是固定的

**Bus 0 是硬件/规范定义的固定值：**

```
Root Complex 总是挂在 Bus 0 上
```

### 2. 其他 Bus 号来自"桥"的配置空间

**每个 Root Port / Switch Port 的配置空间中有 Bus Number 寄存器：**

```
桥（Bridge）配置空间:
═══════════════════════════════════════════════════════════════════

偏移    寄存器名称                说明
─────────────────────────────────────────────────────────────
0x18    Primary Bus Number      上游总线号（固定，通常是 Bus 0）
0x19    Secondary Bus Number     下游总线号（这个就是 Bus 1/2/3...）
0x1A    Subordinate Bus Number   下游最大总线号
```

### 3. 系统如何知道 Bus 1 在哪里？

**系统通过读取桥的配置空间来知道：**

```
枚举流程:
═══════════════════════════════════════════════════════════════════

1. 扫描 Bus 0
   ├─> 发现 Root Port (Bus 0, Dev 0, Func 0)
   └─> 读取它的配置空间

2. 读取桥的 Bus Number 寄存器
   ├─> Primary Bus = 0 (上游是 Bus 0)
   ├─> Secondary Bus = 1 (下游是 Bus 1) ← 从这里知道 Bus 1！
   └─> Subordinate Bus = 1 (下游最大是 Bus 1)

3. 现在系统知道：
   └─> 通过这个 Root Port 可以访问 Bus 1

4. 扫描 Bus 1
   └─> 读取 (Bus 1, Dev 0, Func 0) 的配置空间
```

---

## Bus 号的来源（三种可能）

### 1. 硬件默认值（某些平台）

**某些平台的桥在硬件初始化时就有默认的 Bus 号：**

```c
// 硬件初始化后，桥的配置空间可能已经有值
Root Port (Bus 0, Dev 0, Func 0):
  Primary Bus = 0
  Secondary Bus = 1  ← 硬件默认值
  Subordinate Bus = 1
```

### 2. 固件配置（BIOS/UEFI/ATF）

**固件在枚举前就配置好 Bus 号：**

```c
/**
 * 固件配置 Bus 号（ATF/BIOS）
 */
void configure_bus_numbers(void)
{
    // 配置 Root Port 0
    pcie_write_config8(0, 0, 0, 0x18, 0);  // Primary Bus = 0
    pcie_write_config8(0, 0, 0, 0x19, 1);  // Secondary Bus = 1
    pcie_write_config8(0, 0, 0, 0x1A, 1);  // Subordinate Bus = 1
    
    // 配置 Root Port 1
    pcie_write_config8(0, 1, 0, 0x18, 0);  // Primary Bus = 0
    pcie_write_config8(0, 1, 0, 0x19, 2);  // Secondary Bus = 2
    pcie_write_config8(0, 1, 0, 0x1A, 2);  // Subordinate Bus = 2
}
```

### 3. 枚举时动态分配（Linux 内核）

**Linux 内核在枚举时动态分配 Bus 号：**

```c
/**
 * Linux 内核枚举流程（简化）
 */
void pci_scan_bus(int bus)
{
    // 1. 扫描当前 Bus 上的所有设备
    for (dev = 0; dev < 32; dev++) {
        for (func = 0; func < 8; func++) {
            // 2. 检查设备是否存在
            if (pci_device_exists(bus, dev, func)) {
                // 3. 如果是桥设备
                if (is_bridge(bus, dev, func)) {
                    // 4. 分配新的 Bus 号
                    int new_bus = allocate_bus_number();
                    
                    // 5. 写入桥的配置空间
                    pci_write_config8(bus, dev, func, 0x19, new_bus);  // Secondary Bus
                    pci_write_config8(bus, dev, func, 0x1A, new_bus);  // Subordinate Bus
                    
                    // 6. 递归扫描新 Bus
                    pci_scan_bus(new_bus);
                }
            }
        }
    }
}
```

---

## 完整的枚举流程

```
系统启动 → 枚举 PCIe
═══════════════════════════════════════════════════════════════════

步骤1: 扫描 Bus 0（固定）
─────────────────────────────────────
系统知道 Bus 0 的位置（硬件固定）
├─> 扫描 Bus 0 上的所有 Dev/Func
└─> 发现 Root Port (Bus 0, Dev 0, Func 0)

步骤2: 读取桥的 Bus Number 寄存器
─────────────────────────────────────
读取 Root Port 的配置空间:
├─> Primary Bus = 0
├─> Secondary Bus = ? ← 需要读取或配置
└─> Subordinate Bus = ?

步骤3: 确定下游 Bus 号
─────────────────────────────────────
情况A: 硬件已有默认值
├─> Secondary Bus = 1 (硬件默认)
└─> 直接使用

情况B: 固件已配置
├─> Secondary Bus = 1 (固件写入)
└─> 直接读取

情况C: 枚举时分配
├─> 分配 Bus 1
├─> 写入 Secondary Bus = 1
└─> 然后使用

步骤4: 扫描 Bus 1
─────────────────────────────────────
现在系统知道 Bus 1 的位置:
├─> 通过 Root Port (Bus 0, Dev 0, Func 0) 访问
└─> 扫描 Bus 1 上的所有 Dev/Func
```

---

## 关键点总结

### 1. Bus 0 是固定的

**Bus 0 的位置是硬件/规范定义的，系统启动时就知道。**

### 2. 其他 Bus 号来自桥的配置空间

**系统通过读取桥（Root Port/Switch）的配置空间中的 Bus Number 寄存器来知道下游 Bus 号。**

### 3. Bus 号的来源

- **硬件默认值**：某些平台硬件初始化时就有
- **固件配置**：BIOS/UEFI/ATF 在枚举前配置
- **动态分配**：Linux 内核在枚举时分配并写入

### 4. 系统如何知道 Bus 1 在哪里？

**系统不是"凭空知道"Bus 1，而是：**
1. 先扫描 Bus 0（固定位置）
2. 发现桥设备（Root Port/Switch）
3. **读取桥的配置空间**，得到 Secondary Bus 号
4. 通过这个桥访问下游 Bus

---

## 代码示例

```c
/**
 * 读取桥的下游 Bus 号
 */
uint8_t get_downstream_bus(uint8_t bridge_bus, uint8_t bridge_dev, uint8_t bridge_func)
{
    // 读取桥的 Secondary Bus Number 寄存器
    uint8_t secondary_bus = pcie_read_config8(bridge_bus, bridge_dev, bridge_func, 0x19);
    
    return secondary_bus;  // 这就是下游的 Bus 号
}

/**
 * 通过桥访问下游设备
 */
void access_downstream_device(uint8_t bridge_bus, uint8_t bridge_dev, uint8_t bridge_func,
                              uint8_t target_dev, uint8_t target_func)
{
    // 1. 读取下游 Bus 号
    uint8_t downstream_bus = get_downstream_bus(bridge_bus, bridge_dev, bridge_func);
    
    // 2. 现在知道目标设备在哪个 Bus 上了
    // 3. 通过桥访问下游设备
    uint32_t value = pcie_read_config32(downstream_bus, target_dev, target_func, 0x00);
}
```

---

## 总结

**你的理解是对的！**

- **Bus 0**：硬件固定，系统启动时就知道
- **Bus 1/2/3...**：存储在桥的配置空间中，系统通过读取桥的配置空间来知道
- **系统不是"分配"Bus 号，而是"读取"或"配置"桥的 Bus Number 寄存器**

**所以 Bus 号在系统读取时，要么是硬件默认值，要么是固件/内核已经配置好的！**


