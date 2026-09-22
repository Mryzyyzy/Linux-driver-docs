# PCIe 枚举逻辑详细分析

## 1. 枚举入口点

### 1.1 驱动触发枚举

```c
// 在 Host Controller 驱动中
static int mtk_pcie_probe(struct platform_device *pdev)
{
    // ...
    err = pci_host_probe(host);  // ← 触发枚举
}
```

### 1.2 `pci_host_probe()` 调用链

```c
// drivers/pci/probe.c

int pci_host_probe(struct pci_host_bridge *bridge)
{
    // ...
    ret = pci_scan_root_bus_bridge(bridge);  // ← 开始扫描
    // ...
}
```

---

## 2. 根总线扫描：`pci_scan_root_bus_bridge()`

### 2.1 函数实现

```c
// drivers/pci/probe.c:3201

int pci_scan_root_bus_bridge(struct pci_host_bridge *bridge)
{
    struct resource_entry *window;
    struct pci_bus *b;
    int max, bus, ret;
    
    // 1. 创建根总线结构
    b = pci_alloc_child_bus(bridge->bus);
    
    // 2. 设置总线操作函数
    b->ops = bridge->ops;  // 配置空间访问接口
    
    // 3. 扫描总线上的设备
    max = pci_scan_child_bus(b);  // ← 关键：扫描子总线
    
    // 4. 分配资源（BAR、中断等）
    pci_bus_assign_resources(b);
    
    // 5. 添加设备到系统
    pci_bus_add_devices(b);  // ← 触发设备驱动加载
    
    return 0;
}
```

### 2.2 关键步骤

1. **创建总线结构**：`pci_alloc_child_bus()`
2. **扫描设备**：`pci_scan_child_bus()` ← 核心扫描逻辑
3. **分配资源**：`pci_bus_assign_resources()`
4. **添加设备**：`pci_bus_add_devices()` ← 触发驱动加载

---

## 3. 核心扫描逻辑：`pci_scan_child_bus()`

### 3.1 扫描流程

```c
// drivers/pci/probe.c:3050

unsigned int pci_scan_child_bus(struct pci_bus *bus)
{
    return pci_scan_child_bus_extend(bus, 0);
}
```

### 3.2 实际扫描实现：`pci_scan_child_bus_extend()`

```c
// drivers/pci/probe.c:2926

static unsigned int pci_scan_child_bus_extend(struct pci_bus *bus,
                                              unsigned int available_buses)
{
    unsigned int used_buses, normal_bridges = 0, hotplug_bridges = 0;
    unsigned int start = bus->busn_res.start;
    unsigned int devfn, cmax, max = start;
    struct pci_dev *dev;
    
    dev_dbg(&bus->dev, "scanning bus\n");
    
    /* 核心扫描循环：遍历所有可能的设备位置 */
    /* devfn 范围：0x00 - 0xFF (256 个位置) */
    /* devfn = (device << 3) | function */
    /* device: 0-31 (5位), function: 0-7 (3位) */
    /* 
     * 注意：每次增加 8，是因为每个设备位置（slot）最多可以有 8 个功能
     * 但并不是每个设备都有 8 个功能！
     * - 大多数设备只有功能 0（function 0）
     * - 只有多功能设备（multifunction device）才会有多个功能
     * - 扫描循环按设备位置遍历，然后在 pci_scan_slot() 中扫描该位置的所有功能
     */
    for (devfn = 0; devfn < 256; devfn += 8) {
        pci_scan_slot(bus, devfn);  // ← 扫描每个设备位置的所有功能
    }
    
    /* 处理桥接器（递归扫描下级总线） */
    for_each_pci_bridge(dev, bus) {
        cmax = max;
        max = pci_scan_bridge_extend(bus, dev, max, 0, 0);
        // 递归扫描下级总线
    }
    
    return max;
}
```

**关键点**：
- **扫描范围**：`devfn = 0` 到 `devfn = 255`（256 个位置）
- **步长**：每次增加 8（因为每个设备最多有 8 个功能）
- **扫描顺序**：先扫描所有设备，再处理桥接器

### 3.2 扫描槽位：`pci_scan_slot()`

```c
// drivers/pci/probe.c:2710

int pci_scan_slot(struct pci_bus *bus, int devfn)
{
    struct pci_dev *dev;
    int fn, nr = 0;
    
    // 1. 先扫描功能 0（主功能）
    dev = pci_scan_single_device(bus, devfn + 0);
    if (dev) {
        if (!pci_dev_is_added(dev))
            nr++;
    }
    
    // 2. 如果功能 0 存在，检查是否是多功能设备
    if (dev && dev->multifunction) {
        // 扫描其他功能 (1-7)
        for (fn = 1; fn < 8; fn++) {
            dev = pci_scan_single_device(bus, devfn + fn);
            if (dev) {
                if (!pci_dev_is_added(dev))
                    nr++;
                dev->multifunction = 1;
            }
        }
    }
    
    return nr;
}
```

---

## 4. 扫描单个设备：`pci_scan_single_device()`

### 4.1 函数实现

```c
// drivers/pci/probe.c:2622

struct pci_dev *pci_scan_single_device(struct pci_bus *bus, int devfn)
{
    struct pci_dev *dev;
    
    // 1. 检查设备是否已存在
    dev = pci_get_slot(bus, devfn);
    if (dev) {
        pci_dev_put(dev);
        return dev;  // 已存在，直接返回
    }
    
    // 2. 扫描设备（读取 Vendor ID，判断是否存在）
    dev = pci_scan_device(bus, devfn);
    if (!dev)
        return NULL;  // 设备不存在
    
    // 3. 添加到总线
    pci_device_add(dev, bus);
    
    return dev;
}
```

### 4.2 设备扫描：`pci_scan_device()`

```c
// drivers/pci/probe.c:2457

static struct pci_dev *pci_scan_device(struct pci_bus *bus, int devfn)
{
    struct pci_dev *dev;
    u32 l;
    
    // 1. 读取 Vendor ID（带超时和错误处理）
    if (!pci_bus_read_dev_vendor_id(bus, devfn, &l, 60*1000))
        return NULL;  // 设备不存在或读取失败
    
    // 2. 设备存在！创建 pci_dev 结构
    dev = pci_alloc_dev(bus);
    if (!dev)
        return NULL;
    
    dev->devfn = devfn;
    dev->vendor = l & 0xffff;           // Vendor ID (低 16 位)
    dev->device = (l >> 16) & 0xffff;  // Device ID (高 16 位)
    
    // 3. 读取更多设备信息（Class Code、BAR、中断等）
    if (pci_setup_device(dev)) {
        pci_bus_put(dev->bus);
        kfree(dev);
        return NULL;
    }
    
    return dev;
}
```

### 4.3 Vendor ID 读取：`pci_bus_read_dev_vendor_id()`

```c
// drivers/pci/probe.c:2417

bool pci_bus_generic_read_dev_vendor_id(struct pci_bus *bus, int devfn, u32 *l,
                                       int timeout)
{
    // 1. 读取 Vendor ID（通过 Host Controller 驱动接口）
    if (pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, l))
        return false;
    
    // 2. 判断设备是否存在
    // 以下情况表示设备不存在：
    // - 0xffffffff: 全 1（PCI 规范规定）
    // - 0x00000000: 全 0（某些硬件返回）
    // - 0x0000ffff: 低 16 位全 1
    // - 0xffff0000: 高 16 位全 1
    if (PCI_POSSIBLE_ERROR(*l) || 
        *l == 0x00000000 ||
        *l == 0x0000ffff || 
        *l == 0xffff0000)
        return false;  // 设备不存在
    
    // 3. 处理 RRS (Request Retry Status) 情况
    // 某些设备需要等待才能读取到正确的 Vendor ID
    if (pci_bus_rrs_vendor_id(*l))
        return pci_bus_wait_rrs(bus, devfn, l, timeout);
    
    return true;  // 设备存在
}
```

### 4.2 关键判断：设备是否存在

```c
// 读取 Vendor ID
pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, &l);

// 判断设备是否存在
if (l == 0xffffffff ||      // 全 1
    l == 0x00000000 ||      // 全 0
    l == 0x0000ffff ||      // 低 16 位全 1
    (l & 0xffff) == 0xffff) // Vendor ID = 0xffff
    return NULL;  // 设备不存在
```

**原理**：
- PCIe 规范规定：读取不存在的设备配置空间会返回 `0xffff`
- 如果 Vendor ID = `0xffff`，说明该位置没有设备

---

## 5. 设备信息读取：`pci_setup_device()`

### 5.1 读取配置空间

```c
// drivers/pci/probe.c:1923

int pci_setup_device(struct pci_dev *dev)
{
    u32 class;
    u16 cmd;
    u8 hdr_type;
    
    // 1. 读取头部类型
    hdr_type = pci_hdr_type(dev);
    dev->hdr_type = hdr_type & 0x7f;
    dev->multifunction = !!(hdr_type & 0x80);  // 是否多功能设备
    
    // 2. 设置设备名称
    dev_set_name(&dev->dev, "%04x:%02x:%02x.%d", 
                 pci_domain_nr(dev->bus),
                 dev->bus->number, 
                 PCI_SLOT(dev->devfn),
                 PCI_FUNC(dev->devfn));
    
    // 3. 读取类代码
    class = pci_class(dev);
    dev->revision = class & 0xff;      // 修订版本
    dev->class = class >> 8;            // 类代码（高 24 位）
    
    // 4. 根据头部类型处理
    switch (dev->hdr_type) {
    case PCI_HEADER_TYPE_NORMAL:  // 标准设备
        pci_read_irq(dev);        // 读取中断信息
        pci_read_bases(dev, 6, PCI_ROM_ADDRESS);  // 读取 6 个 BAR
        break;
        
    case PCI_HEADER_TYPE_BRIDGE:  // 桥接器
        pci_read_irq(dev);
        pci_read_bases(dev, 2, PCI_ROM_ADDRESS1);  // 读取 2 个 BAR
        pci_read_bridge_windows(dev);  // 读取桥接器窗口
        break;
        
    case PCI_HEADER_TYPE_CARDBUS:  // CardBus 桥接器
        pci_read_irq(dev);
        pci_read_bases(dev, 1, 0);
        break;
    }
    
    return 0;
}
```

### 5.2 读取 BAR

```c
static void pci_read_bases(struct pci_dev *dev, unsigned int how_many,
                           int rom)
{
    unsigned int pos;
    u32 l;
    int reg;
    
    // 读取每个 BAR
    for (pos = 0; pos < how_many && pos < PCI_STD_RESOURCE_END; pos++) {
        reg = PCI_BASE_ADDRESS_0 + (pos << 2);
        
        // 读取 BAR 值
        pci_read_config_dword(dev, reg, &l);
        dev->resource[pos].start = l;
        
        // 判断 BAR 类型
        if (l == 0)
            continue;  // BAR 未使用
        
        // 写入全 1 来探测 BAR 大小
        pci_write_config_dword(dev, reg, 0xffffffff);
        pci_read_config_dword(dev, reg, &l);
        
        // 解析 BAR 大小和类型
        if (l & PCI_BASE_ADDRESS_SPACE_IO) {
            // I/O 空间 BAR
            dev->resource[pos].flags = IORESOURCE_IO;
        } else {
            // 内存空间 BAR
            dev->resource[pos].flags = IORESOURCE_MEM;
            if (l & PCI_BASE_ADDRESS_MEM_PREFETCH)
                dev->resource[pos].flags |= IORESOURCE_PREFETCH;
        }
        
        // 恢复原始值
        pci_write_config_dword(dev, reg, dev->resource[pos].start);
    }
}
```

---

## 6. 桥接器处理

### 6.1 发现桥接器

```c
// 如果设备是桥接器（PCI-to-PCI Bridge）
if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
    // 读取桥接器配置
    pci_read_bridge_io(bus, dev);
    
    // 创建下级总线
    child_bus = pci_add_new_bus(bus, dev);
    
    // 递归扫描下级总线
    pci_scan_child_bus(child_bus);  // ← 递归扫描
}
```

### 6.2 桥接器扫描

```c
// drivers/pci/probe.c:1535

int pci_scan_bridge(struct pci_bus *bus, struct pci_dev *dev, int max, int pass)
{
    struct pci_bus *child;
    int is_cardbus = (dev->hdr_type == PCI_HEADER_TYPE_CARDBUS);
    u32 buses, i, j = 0;
    u16 bctl;
    u8 primary, secondary, subordinate;
    int broken = 0;
    
    // 1. 读取桥接器配置
    pci_read_config_dword(dev, PCI_PRIMARY_BUS, &buses);
    primary = buses & 0xFF;
    secondary = (buses >> 8) & 0xFF;
    subordinate = (buses >> 16) & 0xFF;
    
    // 2. 创建下级总线
    child = pci_add_new_bus(bus, dev);
    if (!child)
        return max;
    
    // 3. 配置桥接器
    child->primary = secondary;
    child->number = secondary;
    child->secondary = secondary;
    
    // 4. 递归扫描下级总线
    max = pci_scan_child_bus(child);  // ← 递归扫描
    
    // 5. 更新桥接器的 subordinate 总线号
    pci_write_config_byte(dev, PCI_SUBORDINATE_BUS, max);
    
    return max;
}
```

---

## 7. 资源分配：`pci_bus_assign_resources()`

### 7.1 分配过程

```c
void pci_bus_assign_resources(const struct pci_bus *bus)
{
    struct pci_bus *b;
    struct pci_dev *dev;
    
    // 1. 分配 I/O 空间
    pci_bus_assign_resources_io(bus);
    
    // 2. 分配内存空间
    pci_bus_assign_resources_mem(bus);
    
    // 3. 分配预取内存空间
    pci_bus_assign_resources_pref(bus);
    
    // 4. 分配总线号（对于桥接器）
    pci_bus_assign_busn_res(bus);
}
```

### 7.2 BAR 分配

```c
static void pci_assign_resource(struct pci_dev *dev, int resno)
{
    struct resource *res = dev->resource + resno;
    resource_size_t size, align, start;
    u32 flags;
    
    // 1. 获取 BAR 要求的大小和对齐
    size = resource_size(res);
    align = pci_resource_alignment(dev, res);
    
    // 2. 从可用资源中分配
    start = pci_find_resource(dev->bus, res, size, align);
    
    // 3. 写入 BAR
    pci_write_config_dword(dev, PCI_BASE_ADDRESS_0 + resno * 4, start);
    
    // 4. 更新资源信息
    res->start = start;
    res->end = start + size - 1;
}
```

---

## 8. 添加设备：`pci_bus_add_devices()`

### 8.1 函数实现

```c
void pci_bus_add_devices(struct pci_bus *bus)
{
    struct pci_dev *dev;
    struct pci_bus *child;
    int retval;
    
    // 1. 遍历总线上的所有设备
    list_for_each_entry(dev, &bus->devices, bus_list) {
        // 2. 创建设备的 sysfs 接口
        pci_bus_add_device(dev);
        
        // 3. 如果是桥接器，递归处理下级总线
        if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE ||
            dev->hdr_type == PCI_HEADER_TYPE_CARDBUS) {
            child = dev->subordinate;
            if (child)
                pci_bus_add_devices(child);  // 递归
        }
    }
    
    // 4. 触发设备驱动加载
    pci_bus_probe(bus);
}
```

### 8.2 设备驱动加载

```c
static void pci_bus_probe(struct pci_bus *bus)
{
    struct pci_dev *dev;
    
    // 遍历所有设备
    list_for_each_entry(dev, &bus->devices, bus_list) {
        // 如果不是桥接器，尝试加载驱动
        if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE &&
            dev->hdr_type != PCI_HEADER_TYPE_CARDBUS &&
            !dev->is_added) {
            // 触发驱动匹配和加载
            pci_device_probe(dev);
        }
    }
}
```

---

## 9. 完整枚举流程图

```
pci_host_probe(host)
    ↓
pci_scan_root_bus_bridge(bridge)
    ├─ 创建根总线结构
    ├─ 设置总线操作函数
    └─ pci_scan_child_bus(bus)  ← 开始扫描
        │
        ├─ 遍历 devfn (0x00 - 0xFF)
        │   │
        │   └─ pci_scan_slot(bus, devfn)
        │       │
        │       ├─ pci_scan_single_device(bus, devfn + 0)
        │       │   ├─ 读取 Vendor ID
        │       │   ├─ 判断设备是否存在 (Vendor ID != 0xffff)
        │       │   ├─ 创建 pci_dev
        │       │   ├─ pci_setup_device(dev)
        │       │   │   ├─ 读取 Device ID
        │       │   │   ├─ 读取 Class Code
        │       │   │   ├─ 读取 Header Type
        │       │   │   ├─ 读取 BAR (6个)
        │       │   │   └─ 读取中断信息
        │       │   └─ pci_device_add(dev, bus)
        │       │
        │       └─ 如果是多功能设备，扫描功能 1-7
        │
        └─ 如果是桥接器
            └─ pci_scan_bridge()
                └─ 递归扫描下级总线
    │
    ├─ pci_bus_assign_resources(bus)  ← 分配资源
    │   ├─ 分配 I/O 空间
    │   ├─ 分配内存空间
    │   └─ 写入 BAR
    │
    └─ pci_bus_add_devices(bus)  ← 添加设备
        ├─ 创建 sysfs 接口
        └─ pci_bus_probe(bus)
            └─ pci_device_probe(dev)  ← 加载设备驱动
```

---

## 10. 关键数据结构

### 10.1 pci_dev

```c
struct pci_dev {
    struct list_head bus_list;    // 总线设备列表
    struct pci_bus *bus;          // 所属总线
    struct pci_bus *subordinate; // 如果是桥接器，下级总线
    
    unsigned int devfn;           // 设备/功能号
    unsigned short vendor;        // Vendor ID
    unsigned short device;        // Device ID
    unsigned short class;         // 类代码
    u8 revision;                 // 修订版本
    u8 hdr_type;                 // 头部类型
    u8 multifunction;            // 是否多功能设备
    
    struct resource resource[DEVICE_COUNT_RESOURCE];  // BAR 资源
    unsigned int irq;            // 中断号
    // ...
};
```

### 10.2 pci_bus

```c
struct pci_bus {
    struct list_head node;        // 总线列表
    struct pci_bus *parent;      // 父总线
    struct list_head devices;     // 设备列表
    struct list_head children;    // 子总线列表
    
    struct pci_ops *ops;         // 配置空间访问接口
    void *sysdata;               // Host Controller 私有数据
    
    unsigned char number;        // 总线号
    unsigned char primary;       // 主总线号
    unsigned char secondary;      // 次总线号
    unsigned char subordinate;    // 从属总线号
    // ...
};
```

---

## 11. 枚举示例

### 11.1 扫描过程示例

```
扫描总线 0：

devfn = 0x00 (设备 0, 功能 0):
  ├─ 读取 Vendor ID → 0x10de (NVIDIA)
  ├─ 设备存在！
  ├─ 读取 Device ID → 0x1234
  ├─ 读取 Class Code → 0x060400 (PCI-to-PCI Bridge)
  ├─ 是桥接器，创建总线 1
  └─ 递归扫描总线 1

devfn = 0x08 (设备 1, 功能 0):
  ├─ 读取 Vendor ID → 0xffff
  └─ 设备不存在，跳过

devfn = 0x80 (设备 16, 功能 0):
  ├─ 读取 Vendor ID → 0x10ec (Realtek)
  ├─ 设备存在！
  ├─ 读取 Device ID → 0x8168
  ├─ 读取 Class Code → 0x020000 (Ethernet Controller)
  ├─ 读取 BAR0 → 0xfe000000 (内存空间)
  └─ 创建 pci_dev，添加到总线

继续扫描...
```

### 11.2 资源分配示例

```
设备: 0000:01:00.0 (网卡)
  BAR0: 需要 256KB 内存空间
    ├─ 从可用内存中分配
    ├─ 分配地址: 0xfe000000 - 0xfe003fff
    └─ 写入 BAR0: 0xfe000000

设备: 0000:01:00.1 (网卡功能 1)
  BAR0: 需要 64KB 内存空间
    ├─ 从可用内存中分配
    ├─ 分配地址: 0xfe004000 - 0xfe00ffff
    └─ 写入 BAR0: 0xfe004000
```

---

## 12. 驱动接口调用

### 12.1 配置空间读取

```c
// PCI 核心子系统调用
pci_bus_read_config_dword(bus, devfn, PCI_VENDOR_ID, &l);
    ↓
// 调用 Host Controller 驱动提供的接口
bus->ops->read(bus, devfn, PCI_VENDOR_ID, 4, &l);
    ↓
// MediaTek 驱动实现
pcie_mediatek_config_read(bus, devfn, PCI_VENDOR_ID, 4, &l);
    ↓
// 映射到硬件地址
addr = pcie_mediatek_map_bus(bus, devfn, PCI_VENDOR_ID);
    ↓
// 读取硬件寄存器
l = readl(addr);
```

---

## 13. 总结

### 13.1 枚举的关键步骤

1. **扫描总线**：遍历所有可能的设备位置 (devfn)
2. **读取 Vendor ID**：判断设备是否存在
3. **创建设备结构**：为存在的设备创建 `pci_dev`
4. **读取设备信息**：Device ID、Class Code、BAR 等
5. **处理桥接器**：递归扫描下级总线
6. **分配资源**：为每个设备的 BAR 分配地址
7. **添加设备**：创建 sysfs 接口，触发驱动加载

### 13.2 关键判断

- **设备是否存在**：`Vendor ID == 0xffff` → 不存在
- **是否多功能**：`Header Type & 0x80` → 多功能设备
- **是否桥接器**：`Header Type == 0x01` → PCI-to-PCI Bridge

### 13.3 驱动的作用

- ✅ **提供配置空间访问**：实现 `pci_ops.read/write/map_bus`
- ✅ **触发枚举**：调用 `pci_host_probe()`
- ❌ **不需要实现枚举逻辑**：由 PCI 核心子系统完成

---

## 14. 参考资料

- [Linux PCI 子系统源码](https://elixir.bootlin.com/linux/latest/source/drivers/pci/probe.c)
- [PCIe 规范 - 配置空间](https://pcisig.com/)
- [PCI 设备枚举文档](https://www.kernel.org/doc/html/latest/PCI/pci.html#device-enumeration)

