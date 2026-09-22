# PCIe MSI 中断机制详解

## 一、概述

MSI (Message Signaled Interrupts) 是 PCIe 设备使用内存写操作来触发中断的机制，相比传统的 INTx 中断，具有以下优势：

- **性能更好**：不需要共享中断线，减少中断延迟
- **可扩展**：支持多个中断向量（MSI-X 最多支持 2048 个）
- **精确路由**：每个中断可以路由到不同的 CPU
- **无共享冲突**：每个设备有独立的中断向量

## 二、MSI 与 MSI-X

### 2.1 MSI (Message Signaled Interrupts)

- **向量数量**：1-32 个（2 的幂次）
- **配置方式**：通过 PCI 配置空间的 MSI Capability 结构
- **消息格式**：地址 + 数据（32 位或 64 位地址）

### 2.2 MSI-X (Extended Message Signaled Interrupts)

- **向量数量**：1-2048 个
- **配置方式**：通过内存映射的 MSI-X Table
- **独立配置**：每个向量可以独立配置地址和数据
- **更灵活**：支持每个向量不同的目标 CPU

## 三、MSI 中断的注册流程

### 3.1 驱动层 API

#### 3.1.1 现代 API（推荐）

```c
// 分配 MSI 中断向量
int pci_alloc_irq_vectors(struct pci_dev *dev, 
                          unsigned int min_vecs,
                          unsigned int max_vecs, 
                          unsigned int flags);

// 获取 Linux IRQ 号
int pci_irq_vector(struct pci_dev *dev, unsigned int nr);

// 注册中断处理函数
int request_threaded_irq(unsigned int irq, 
                         irq_handler_t handler,
                         irq_handler_t thread_fn,
                         unsigned long flags,
                         const char *name, 
                         void *dev);

// 释放中断向量
void pci_free_irq_vectors(struct pci_dev *dev);
```

**使用示例：**

```c
static int my_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int ret, irq, nvecs;
    
    // 1. 分配 MSI 中断向量（尝试 MSI-X，失败则尝试 MSI）
    nvecs = pci_alloc_irq_vectors(pdev, 1, 4, 
                                  PCI_IRQ_MSIX | PCI_IRQ_MSI);
    if (nvecs < 0)
        return nvecs;
    
    // 2. 获取 Linux IRQ 号
    irq = pci_irq_vector(pdev, 0);
    if (irq < 0)
        goto err_free_vectors;
    
    // 3. 注册中断处理函数
    ret = request_threaded_irq(irq, my_irq_handler, my_irq_thread,
                                IRQF_ONESHOT, "my_device", priv);
    if (ret)
        goto err_free_vectors;
    
    return 0;
    
err_free_vectors:
    pci_free_irq_vectors(pdev);
    return ret;
}
```

#### 3.1.2 传统 API（已弃用但仍支持）

```c
// 启用 MSI
int pci_enable_msi(struct pci_dev *dev);

// 启用 MSI-X
int pci_enable_msix_range(struct pci_dev *dev, 
                          struct msix_entry *entries,
                          int minvec, int maxvec);

// 禁用 MSI/MSI-X
void pci_disable_msi(struct pci_dev *dev);
void pci_disable_msix(struct pci_dev *dev);
```

### 3.2 内部实现流程

#### 3.2.1 MSI 启用流程

```c
// drivers/pci/msi/api.c:30
int pci_enable_msi(struct pci_dev *dev)
{
    return __pci_enable_msi_range(dev, 1, 1, NULL);
}
```

**详细流程：**

```
pci_enable_msi()
    ↓
__pci_enable_msi_range()
    ↓
1. 检查设备是否支持 MSI
   - pci_msi_supported()
   - 检查全局 MSI 使能标志
   - 检查设备标志和总线标志
    ↓
2. 设置 MSI 上下文
   - pci_setup_msi_context()
    ↓
3. 创建设备 MSI 域
   - pci_setup_msi_device_domain()
    ↓
4. 初始化 MSI Capability
   - msi_capability_init()
     ├─ 创建 MSI 描述符
     ├─ 配置 MSI 寄存器
     ├─ 分配 Linux IRQ
     └─ 写入 MSI 消息
    ↓
5. 设置 MSI 使能位
   - pci_msi_set_enable(dev, 1)
```

#### 3.2.2 MSI Capability 初始化

```c
// drivers/pci/msi/msi.c:350
static int msi_capability_init(struct pci_dev *dev, int nvec,
                               struct irq_affinity *affd)
{
    // 1. 禁用 MSI（在配置期间）
    pci_msi_set_enable(dev, 0);
    dev->msi_enabled = 1;
    
    // 2. 创建 MSI 描述符
    ret = msi_setup_msi_desc(dev, nvec, masks);
    
    // 3. 屏蔽所有 MSI 中断
    entry = msi_first_desc(&dev->dev, MSI_DESC_ALL);
    pci_msi_mask(entry, msi_multi_mask(entry));
    
    // 4. 分配 Linux IRQ（通过 irqdomain）
    ret = pci_msi_setup_msi_irqs(dev, nvec, PCI_CAP_ID_MSI);
    
    // 5. 启用 MSI
    pci_intx_for_msi(dev, 0);  // 禁用 INTx
    pci_msi_set_enable(dev, 1); // 启用 MSI
    
    // 6. 更新设备 IRQ
    dev->irq = entry->irq;
}
```

#### 3.2.3 IRQ Domain 分配

```c
// drivers/pci/msi/irqdomain.c:11
int pci_msi_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
    struct irq_domain *domain;
    
    // 获取设备的 MSI 域
    domain = dev_get_msi_domain(&dev->dev);
    
    if (domain && irq_domain_is_hierarchy(domain)) {
        // 使用分层 irqdomain（现代方式）
        return msi_domain_alloc_irqs_all_locked(&dev->dev, 
                                                MSI_DEFAULT_DOMAIN, nvec);
    }
    
    // 使用传统方式（架构特定）
    return pci_msi_legacy_setup_msi_irqs(dev, nvec, type);
}
```

**IRQ Domain 的作用：**
- 将硬件中断号（hwirq）映射到 Linux IRQ 号（virq）
- 管理中断控制器的层次结构
- 提供统一的中断分配接口

## 四、MSI 消息的配置

### 4.1 MSI 消息结构

```c
struct msi_msg {
    union {
        struct {
            u32 address_lo;  // 低 32 位地址
            u32 address_hi;  // 高 32 位地址（64 位模式）
            u16 data;        // 数据字段
        };
        u64 address;         // 完整地址（64 位）
    };
};
```

### 4.2 MSI 消息写入

```c
// drivers/pci/msi/msi.c:239
void __pci_write_msi_msg(struct msi_desc *entry, struct msi_msg *msg)
{
    struct pci_dev *dev = msi_desc_to_pci_dev(entry);
    
    if (dev->current_state != PCI_D0 || pci_dev_is_disconnected(dev)) {
        // 设备不在 D0 状态，不写入硬件
        return;
    }
    
    if (entry->pci.msi_attrib.is_msix) {
        // MSI-X：写入内存映射的 MSI-X Table
        pci_write_msg_msix(entry, msg);
    } else {
        // MSI：写入 PCI 配置空间
        pci_write_msg_msi(dev, entry, msg);
    }
    
    // 保存消息到描述符
    entry->msg = *msg;
    
    // 调用平台特定的写入函数（如果有）
    if (entry->write_msi_msg)
        entry->write_msi_msg(entry, entry->write_msi_msg_data);
}
```

#### 4.2.1 MSI 消息写入（配置空间）

```c
// drivers/pci/msi/msi.c:187
static inline void pci_write_msg_msi(struct pci_dev *dev, 
                                      struct msi_desc *desc,
                                      struct msi_msg *msg)
{
    int pos = dev->msi_cap;
    u16 msgctl;
    
    // 1. 更新 Multiple Message Enable 字段
    pci_read_config_word(dev, pos + PCI_MSI_FLAGS, &msgctl);
    msgctl &= ~PCI_MSI_FLAGS_QSIZE;
    msgctl |= FIELD_PREP(PCI_MSI_FLAGS_QSIZE, desc->pci.msi_attrib.multiple);
    pci_write_config_word(dev, pos + PCI_MSI_FLAGS, msgctl);
    
    // 2. 写入地址（低 32 位）
    pci_write_config_dword(dev, pos + PCI_MSI_ADDRESS_LO, msg->address_lo);
    
    // 3. 如果是 64 位模式，写入高 32 位地址和数据
    if (desc->pci.msi_attrib.is_64) {
        pci_write_config_dword(dev, pos + PCI_MSI_ADDRESS_HI, msg->address_hi);
        pci_write_config_word(dev, pos + PCI_MSI_DATA_64, msg->data);
    } else {
        // 32 位模式，只写入低 16 位数据
        pci_write_config_word(dev, pos + PCI_MSI_DATA_32, msg->data);
    }
    
    // 4. 确保写入可见（读回验证）
    pci_read_config_word(dev, pos + PCI_MSI_FLAGS, &msgctl);
}
```

#### 4.2.2 MSI-X 消息写入（内存映射）

```c
// drivers/pci/msi/msi.c:209
static inline void pci_write_msg_msix(struct msi_desc *desc, 
                                       struct msi_msg *msg)
{
    void __iomem *base = pci_msix_desc_addr(desc);
    u32 ctrl = desc->pci.msix_ctrl;
    bool unmasked = !(ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);
    
    // 1. 如果条目未屏蔽，先屏蔽它
    if (unmasked)
        pci_msix_write_vector_ctrl(desc, ctrl | PCI_MSIX_ENTRY_CTRL_MASKBIT);
    
    // 2. 写入地址和数据
    writel(msg->address_lo, base + PCI_MSIX_ENTRY_LOWER_ADDR);
    writel(msg->address_hi, base + PCI_MSIX_ENTRY_UPPER_ADDR);
    writel(msg->data, base + PCI_MSIX_ENTRY_DATA);
    
    // 3. 如果之前未屏蔽，恢复屏蔽状态
    if (unmasked)
        pci_msix_write_vector_ctrl(desc, ctrl);
    
    // 4. 确保写入可见
    readl(base + PCI_MSIX_ENTRY_DATA);
}
```

### 4.3 MSI 消息内容

MSI 消息由中断控制器（如 GIC、IOAPIC）生成，包含：

- **地址**：指向中断控制器的特定寄存器
- **数据**：中断向量号或中断 ID

**示例（ARM GIC）：**
- 地址：GIC 的 GITS_TRANSLATER 寄存器地址
- 数据：中断 ID（SPI 中断号）

## 五、MSI 中断的触发机制

### 5.1 硬件触发流程

```
PCIe 设备产生中断事件
    ↓
设备执行内存写操作（Memory Write TLP）
    ↓
写入地址 = MSI 消息中的地址
写入数据 = MSI 消息中的数据
    ↓
TLP 通过 PCIe 链路传输
    ↓
到达 Root Complex
    ↓
路由到中断控制器（如 GIC）
    ↓
中断控制器识别中断 ID
    ↓
触发 CPU 中断
    ↓
CPU 跳转到中断处理程序
```

### 5.2 软件处理流程

```
CPU 收到中断
    ↓
中断控制器分发中断
    ↓
调用通用中断处理程序
    ↓
查找对应的 irq_desc
    ↓
调用驱动注册的 handler
    ↓
执行中断处理函数
    ↓
（可选）唤醒中断线程
    ↓
中断处理完成，返回
```

### 5.3 中断处理函数注册

```c
// 注册中断处理函数
int request_threaded_irq(unsigned int irq,
                         irq_handler_t handler,      // 硬中断处理函数
                         irq_handler_t thread_fn,    // 线程处理函数（可选）
                         unsigned long flags,
                         const char *name,
                         void *dev_id);

// handler: 在中断上下文中执行，必须快速返回
static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
    struct my_device *dev = dev_id;
    
    // 快速处理：清除中断标志、读取状态等
    u32 status = readl(dev->regs + STATUS_REG);
    
    // 如果需要在进程上下文中处理，返回 IRQ_WAKE_THREAD
    if (status & NEED_THREAD_HANDLING)
        return IRQ_WAKE_THREAD;
    
    // 快速处理完成
    return IRQ_HANDLED;
}

// thread_fn: 在进程上下文中执行，可以睡眠
static irqreturn_t my_irq_thread(int irq, void *dev_id)
{
    struct my_device *dev = dev_id;
    
    // 可以执行耗时操作：I/O、睡眠、调度等
    process_data(dev);
    
    return IRQ_HANDLED;
}
```

## 六、MSI 中断的屏蔽和取消屏蔽

### 6.1 MSI 屏蔽机制

```c
// drivers/pci/msi/irqdomain.c:167
static void pci_irq_mask_msi(struct irq_data *data)
{
    struct msi_desc *desc = irq_data_get_msi_desc(data);
    
    // 屏蔽 MSI 中断
    pci_msi_mask(desc, BIT(data->irq - desc->irq));
    
    // 如果支持，屏蔽父中断
    cond_mask_parent(data);
}
```

**MSI 屏蔽实现：**

```c
// drivers/pci/msi/msi.c
void pci_msi_mask(struct msi_desc *desc, u32 mask)
{
    struct pci_dev *dev = msi_desc_to_pci_dev(desc);
    u32 mask_bits = desc->pci.msi_mask;
    
    if (!desc->pci.msi_attrib.can_mask)
        return;
    
    // 更新屏蔽位
    mask_bits |= mask;
    desc->pci.msi_mask = mask_bits;
    
    // 写入 PCI 配置空间
    pci_write_config_dword(dev, desc->pci.mask_pos, mask_bits);
}
```

### 6.2 MSI-X 屏蔽机制

```c
// drivers/pci/msi/irqdomain.c:213
static void pci_irq_mask_msix(struct irq_data *data)
{
    // MSI-X 每个向量独立屏蔽
    pci_msix_mask(irq_data_get_msi_desc(data));
    cond_mask_parent(data);
}
```

**MSI-X 屏蔽实现：**

```c
void pci_msix_mask(struct msi_desc *desc)
{
    u32 ctrl = desc->pci.msix_ctrl;
    
    // 设置屏蔽位
    ctrl |= PCI_MSIX_ENTRY_CTRL_MASKBIT;
    desc->pci.msix_ctrl = ctrl;
    
    // 写入 MSI-X Table
    pci_msix_write_vector_ctrl(desc, ctrl);
}
```

## 七、完整示例：驱动中的 MSI 使用

### 7.1 设备驱动示例

```c
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

struct my_device {
    struct pci_dev *pdev;
    void __iomem *regs;
    int irq;
    int nvecs;
};

static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
    struct my_device *dev = dev_id;
    u32 status;
    
    // 读取中断状态
    status = readl(dev->regs + STATUS_REG);
    
    if (status & INT_MASK) {
        // 处理中断
        handle_interrupt(dev, status);
        
        // 清除中断标志
        writel(status, dev->regs + STATUS_REG);
        
        return IRQ_HANDLED;
    }
    
    return IRQ_NONE;
}

static int my_pci_probe(struct pci_dev *pdev, 
                        const struct pci_device_id *id)
{
    struct my_device *dev;
    int ret, i;
    
    // 1. 分配设备结构
    dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;
    
    dev->pdev = pdev;
    pci_set_drvdata(pdev, dev);
    
    // 2. 启用设备
    ret = pci_enable_device(pdev);
    if (ret)
        return ret;
    
    // 3. 请求内存区域
    ret = pci_request_regions(pdev, "my_device");
    if (ret)
        goto err_disable;
    
    // 4. 映射 I/O 内存
    dev->regs = pci_iomap(pdev, 0, 0);
    if (!dev->regs) {
        ret = -ENOMEM;
        goto err_release;
    }
    
    // 5. 分配 MSI 中断向量
    ret = pci_alloc_irq_vectors(pdev, 1, 4, 
                                 PCI_IRQ_MSIX | PCI_IRQ_MSI);
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to allocate MSI vectors\n");
        goto err_iounmap;
    }
    
    dev->nvecs = ret;
    dev_info(&pdev->dev, "Allocated %d MSI vectors\n", dev->nvecs);
    
    // 6. 注册中断处理函数（为每个向量）
    for (i = 0; i < dev->nvecs; i++) {
        int irq = pci_irq_vector(pdev, i);
        
        ret = request_threaded_irq(irq, my_irq_handler, NULL,
                                    IRQF_ONESHOT, 
                                    dev_name(&pdev->dev), dev);
        if (ret) {
            dev_err(&pdev->dev, "Failed to request IRQ %d\n", irq);
            goto err_free_irqs;
        }
    }
    
    // 7. 初始化设备
    init_device(dev);
    
    return 0;
    
err_free_irqs:
    // 释放已注册的中断
    for (i--; i >= 0; i--) {
        int irq = pci_irq_vector(pdev, i);
        free_irq(irq, dev);
    }
    pci_free_irq_vectors(pdev);
    
err_iounmap:
    pci_iounmap(pdev, dev->regs);
    
err_release:
    pci_release_regions(pdev);
    
err_disable:
    pci_disable_device(pdev);
    
    return ret;
}

static void my_pci_remove(struct pci_dev *pdev)
{
    struct my_device *dev = pci_get_drvdata(pdev);
    int i;
    
    // 1. 停止设备
    stop_device(dev);
    
    // 2. 释放中断
    for (i = 0; i < dev->nvecs; i++) {
        int irq = pci_irq_vector(pdev, i);
        free_irq(irq, dev);
    }
    
    // 3. 释放 MSI 向量
    pci_free_irq_vectors(pdev);
    
    // 4. 取消映射 I/O 内存
    pci_iounmap(pdev, dev->regs);
    
    // 5. 释放资源
    pci_release_regions(pdev);
    
    // 6. 禁用设备
    pci_disable_device(pdev);
}

static const struct pci_device_id my_pci_ids[] = {
    { PCI_DEVICE(PCI_VENDOR_ID_MY, PCI_DEVICE_ID_MY) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, my_pci_ids);

static struct pci_driver my_pci_driver = {
    .name       = "my_device",
    .id_table   = my_pci_ids,
    .probe      = my_pci_probe,
    .remove     = my_pci_remove,
};

module_pci_driver(my_pci_driver);
```

## 八、关键数据结构

### 8.1 struct msi_desc

```c
struct msi_desc {
    struct list_head list;           // 链表节点
    struct irq_alloc_info info;       // 分配信息
    struct msi_msg msg;               // MSI 消息
    struct irq_domain *irq_domain;    // IRQ 域
    unsigned int irq;                 // Linux IRQ 号
    unsigned int msi_index;           // MSI 索引
    
    union {
        struct {
            u8 is_msix:1;             // 是否为 MSI-X
            u8 is_64:1;               // 是否 64 位地址
            u8 can_mask:1;             // 是否支持屏蔽
            u8 multiple:3;            // Multiple Message Enable
            u8 multi_cap:3;           // Multiple Message Capable
        } msi_attrib;
        
        struct {
            u32 mask_pos;             // 屏蔽位位置
            u32 msi_mask;             // 屏蔽位值
        } pci;
    };
    
    // MSI-X 特定字段
    struct {
        u32 ctrl;                     // MSI-X 控制字
        void __iomem *mask_base;       // 屏蔽寄存器基址
    } msix_ctrl;
};
```

### 8.2 struct irq_domain

```c
struct irq_domain {
    struct list_head link;            // 域链表
    const char *name;                 // 域名
    const struct irq_domain_ops *ops; // 域操作
    void *host_data;                  // 主机数据
    unsigned int flags;                // 标志
    
    // MSI 相关
    struct msi_domain_info *msi;      // MSI 域信息
    struct irq_domain *parent;        // 父域
};
```

## 九、MSI 域层次结构

### 9.1 分层 IRQ Domain

```
┌─────────────────────────────────┐
│   PCIe Root Complex Domain      │  (平台特定)
│   - 管理所有 PCIe 设备           │
└─────────────────────────────────┘
              ↓
┌─────────────────────────────────┐
│   PCIe Device Domain            │  (设备特定)
│   - 管理单个设备的 MSI 向量      │
└─────────────────────────────────┘
              ↓
┌─────────────────────────────────┐
│   Linux IRQ                     │
│   - 虚拟中断号                   │
└─────────────────────────────────┘
```

### 9.2 域创建流程

```c
// 1. 平台创建根 MSI 域
struct irq_domain *pci_msi_create_irq_domain(
    struct fwnode_handle *fwnode,
    struct msi_domain_info *info,
    struct irq_domain *parent);

// 2. 设备创建设备 MSI 域
bool pci_setup_msi_device_domain(struct pci_dev *pdev);
bool pci_setup_msix_device_domain(struct pci_dev *pdev, unsigned int hwsize);
```

## 十、总结

### 10.1 MSI 中断流程总结

1. **注册阶段**：
   - 驱动调用 `pci_alloc_irq_vectors()` 分配 MSI 向量
   - 内核创建 MSI 描述符和 IRQ Domain
   - 配置 PCI 配置空间或 MSI-X Table
   - 写入 MSI 消息（地址和数据）

2. **触发阶段**：
   - 设备产生中断事件
   - 设备执行内存写操作（MSI 消息）
   - 中断控制器接收并路由中断
   - CPU 执行中断处理程序

3. **处理阶段**：
   - 调用驱动注册的 handler
   - 执行中断处理逻辑
   - 清除中断标志
   - 返回中断处理结果

### 10.2 关键 API 总结

| API | 功能 | 使用场景 |
|-----|------|----------|
| `pci_alloc_irq_vectors()` | 分配 MSI 向量 | 设备初始化 |
| `pci_irq_vector()` | 获取 Linux IRQ 号 | 注册中断处理函数 |
| `request_threaded_irq()` | 注册中断处理函数 | 设备初始化 |
| `pci_free_irq_vectors()` | 释放 MSI 向量 | 设备移除 |
| `pci_write_msi_msg()` | 写入 MSI 消息 | 中断控制器配置 |

### 10.3 最佳实践

1. **优先使用现代 API**：`pci_alloc_irq_vectors()` 而不是 `pci_enable_msi()`
2. **使用线程化中断**：`request_threaded_irq()` 提高响应性
3. **正确处理错误**：检查所有返回值，正确释放资源
4. **支持多向量**：如果设备支持，使用多个 MSI 向量提高性能
5. **考虑亲和性**：使用 `PCI_IRQ_AFFINITY` 标志分散中断到不同 CPU

## 十一、设备树中的 MSI 配置

### 11.1 概述

在设备树（Device Tree）中配置 MSI 中断，需要定义 MSI 控制器和 PCIe 根复合体之间的关系。设备树提供了两种主要方式：

- **msi-parent**：简单的父节点引用
- **msi-map**：基于 Requester ID (RID) 的映射

### 11.2 MSI 控制器节点

MSI 控制器节点必须包含以下属性：

```dts
gic_its: msi-controller@1820000 {
    compatible = "arm,gic-v3-its";
    reg = <0x00 0x01820000 0x00 0x10000>;
    msi-controller;                    // 标识为 MSI 控制器
    #msi-cells = <1>;                  // MSI 说明符的单元数
};
```

**关键属性：**
- `msi-controller`：标识该节点为 MSI 控制器（必需）
- `#msi-cells`：MSI 说明符的单元数（可选，默认为 0）
  - 对于 GIC ITS，通常为 1（表示设备 ID）
  - 对于简单的 MSI 控制器，可以为 0

### 11.3 PCIe 根复合体配置

#### 11.3.1 使用 msi-map（推荐）

`msi-map` 属性用于将 PCIe 设备的 Requester ID (RID) 映射到 MSI 控制器和 MSI 说明符。

**格式：**
```
msi-map = <rid-base msi-controller msi-base length>,
          <rid-base msi-controller msi-base length>, ...;
```

**参数说明：**
- `rid-base`：起始 RID（Requester ID）
- `msi-controller`：MSI 控制器的 phandle
- `msi-base`：起始 MSI 说明符
- `length`：映射的 RID 范围长度

**示例 1：简单映射（所有设备使用同一个 MSI 控制器）**

```dts
/ {
    gic_its: msi-controller@1820000 {
        compatible = "arm,gic-v3-its";
        reg = <0x00 0x01820000 0x00 0x10000>;
        msi-controller;
        #msi-cells = <1>;
    };

    pcie0_rc: pcie@5500000 {
        compatible = "ti,am654-pcie-rc";
        reg = <0x0 0x5500000 0x0 0x1000>;
        device_type = "pci";
        #address-cells = <3>;
        #size-cells = <2>;
        
        /* 
         * 所有 RID [0x0, 0x10000) 映射到 gic_its，
         * MSI 说明符从 0x0 开始
         */
        msi-map = <0x0 &gic_its 0x0 0x10000>;
    };
};
```

**示例 2：多个 PCIe 控制器使用不同的 MSI 范围**

```dts
/ {
    gic_its: msi-controller@1820000 {
        compatible = "arm,gic-v3-its";
        reg = <0x00 0x01820000 0x00 0x10000>;
        msi-controller;
        #msi-cells = <1>;
    };

    /* PCIe 控制器 0：使用 MSI ID 范围 [0x0, 0x10000) */
    pcie0_rc: pcie@5500000 {
        compatible = "ti,am654-pcie-rc";
        reg = <0x0 0x5500000 0x0 0x1000>;
        device_type = "pci";
        msi-map = <0x0 &gic_its 0x0 0x10000>;
    };

    /* PCIe 控制器 1：使用 MSI ID 范围 [0x10000, 0x20000) */
    pcie1_rc: pcie@5600000 {
        compatible = "ti,am654-pcie-rc";
        reg = <0x0 0x5600000 0x0 0x1000>;
        device_type = "pci";
        msi-map = <0x0 &gic_its 0x10000 0x10000>;
    };
};
```

**示例 3：使用 msi-map-mask 进行位掩码**

```dts
pcie@f {
    compatible = "vendor,pcie-root-complex";
    device_type = "pci";
    
    /*
     * 只使用 RID 的低 8 位（Device 和 Function）
     * 忽略 Bus 号
     */
    msi-map = <0x0 &msi 0x0 0x100>;
    msi-map-mask = <0xff>;  // 只匹配低 8 位
};
```

#### 11.3.2 使用 msi-parent（简单场景）

`msi-parent` 属性用于简单的场景，当根复合体和 MSI 控制器之间不需要传递侧带数据时。

```dts
pcie@f {
    compatible = "vendor,pcie-root-complex";
    device_type = "pci";
    
    /* 直接指定 MSI 父节点 */
    msi-parent = <&gic_its>;
};
```

**注意：** `msi-parent` 主要用于不支持侧带数据的简单 MSI 控制器。

### 11.4 Requester ID (RID) 格式

RID 是一个 16 位值，格式如下：

```
Bits [15:8]  - Bus number
Bits [7:3]   - Device number
Bits [2:0]   - Function number
```

**示例：**
- Bus 0, Device 1, Function 0 → RID = 0x0008
- Bus 1, Device 5, Function 2 → RID = 0x014A

### 11.5 内核中的设备树解析

#### 11.5.1 MSI 域查找

```c
// drivers/pci/of.c:92
struct irq_domain *pci_host_bridge_of_msi_domain(struct pci_bus *bus)
{
    struct irq_domain *d;
    
    if (!bus->dev.of_node)
        return NULL;
    
    // 1. 通过 msi-parent 或 msi-map 查找 MSI 域
    d = of_msi_get_domain(&bus->dev, bus->dev.of_node, DOMAIN_BUS_PCI_MSI);
    if (d)
        return d;
    
    // 2. 查找直接附加到 host bridge 的域
    d = irq_find_matching_host(bus->dev.of_node, DOMAIN_BUS_PCI_MSI);
    if (d)
        return d;
    
    // 3. 查找任何匹配的 irq domain
    return irq_find_host(bus->dev.of_node);
}
```

#### 11.5.2 RID 到 MSI ID 的映射

```c
// drivers/of/irq.c:701
u32 of_msi_map_id(struct device *dev, struct device_node *msi_np, u32 id_in)
{
    // 向上遍历设备树，查找 msi-map 属性
    // 应用映射规则，返回映射后的 MSI ID
    return __of_msi_map_id(dev, &msi_np, id_in);
}
```

### 11.6 完整设备树示例

```dts
/ {
    #address-cells = <2>;
    #size-cells = <2>;

    /* GIC 中断控制器 */
    gic500: interrupt-controller@1800000 {
        compatible = "arm,gic-v3";
        #interrupt-cells = <3>;
        interrupt-controller;
        reg = <0x00 0x01800000 0x00 0x10000>,  /* GICD */
              <0x00 0x01880000 0x00 0x90000>;  /* GICR */
        interrupts = <GIC_PPI 9 IRQ_TYPE_LEVEL_HIGH>;

        /* GIC ITS (MSI 控制器) */
        gic_its: msi-controller@1820000 {
            compatible = "arm,gic-v3-its";
            reg = <0x00 0x01820000 0x00 0x10000>;
            msi-controller;
            #msi-cells = <1>;
        };
    };

    /* PCIe 根复合体 0 */
    pcie0_rc: pcie@5500000 {
        compatible = "ti,am654-pcie-rc";
        reg = <0x0 0x5500000 0x0 0x1000>,
              <0x0 0x5501000 0x0 0x1000>,
              <0x0 0x10000000 0x0 0x2000>,
              <0x0 0x5506000 0x0 0x1000>;
        reg-names = "app", "dbics", "config", "atu";
        
        #address-cells = <3>;
        #size-cells = <2>;
        device_type = "pci";
        
        /* PCIe 配置 */
        bus-range = <0x0 0xff>;
        num-viewport = <16>;
        max-link-speed = <2>;
        dma-coherent;
        
        /* 中断配置 */
        interrupts = <GIC_SPI 340 IRQ_TYPE_EDGE_RISING>;
        
        /* MSI 映射：所有设备使用 gic_its，MSI ID 从 0x0 开始 */
        msi-map = <0x0 &gic_its 0x0 0x10000>;
        
        /* 内存映射 */
        ranges = <0x81000000 0 0          0x0 0x10020000 0 0x00010000>,
                 <0x82000000 0 0x10030000 0x0 0x10030000 0 0x07FD0000>;
        
        status = "okay";
    };
};
```

### 11.7 调试和验证

#### 11.7.1 检查设备树配置

```bash
# 查看设备树中的 MSI 配置
cat /proc/device-tree/pcie@5500000/msi-map

# 查看 MSI 控制器
ls -la /proc/device-tree/*/msi-controller
```

#### 11.7.2 内核日志

启用 MSI 相关的调试日志：

```bash
# 查看 MSI 分配信息
dmesg | grep -i msi

# 查看 IRQ domain 信息
cat /proc/interrupts | grep MSI
```

#### 11.7.3 常见问题

1. **MSI 域未找到**
   - 检查 `msi-map` 或 `msi-parent` 属性是否正确
   - 确认 MSI 控制器节点存在且包含 `msi-controller` 属性

2. **RID 映射错误**
   - 检查 `msi-map` 的格式和范围
   - 确认 RID 计算是否正确

3. **MSI ID 冲突**
   - 确保不同 PCIe 控制器使用不同的 MSI ID 范围
   - 检查 `msi-base` 和 `length` 参数

### 11.8 平台特定示例

#### 11.8.1 ARM GIC ITS

```dts
gic_its: msi-controller@1820000 {
    compatible = "arm,gic-v3-its";
    reg = <0x00 0x01820000 0x00 0x10000>;
    msi-controller;
    #msi-cells = <1>;  // 设备 ID
};
```

#### 11.8.2 其他 MSI 控制器

```dts
/* 简单的 MSI 控制器（无侧带数据） */
msi_controller: msi-controller@a {
    reg = <0xa 0x1000>;
    compatible = "vendor,simple-msi";
    msi-controller;
    /* #msi-cells = 0 (默认) */
};
```

### 11.9 总结

设备树中的 MSI 配置要点：

1. **MSI 控制器节点**：
   - 必须包含 `msi-controller` 属性
   - 可选 `#msi-cells` 属性指定说明符单元数

2. **PCIe 根复合体配置**：
   - 使用 `msi-map` 进行 RID 到 MSI ID 的映射（推荐）
   - 或使用 `msi-parent` 进行简单引用

3. **映射规则**：
   - `msi-map` 支持多个映射条目
   - 可以使用 `msi-map-mask` 进行位掩码操作
   - RID 格式：Bus[15:8] + Device[7:3] + Function[2:0]

4. **内核支持**：
   - 内核自动解析设备树配置
   - 通过 `of_msi_map_id()` 进行 RID 映射
   - 通过 `pci_host_bridge_of_msi_domain()` 查找 MSI 域

## 十二、MSI 中断识别机制详解

### 12.1 核心问题

当设备往一个地址写一个值时，系统如何知道这个中断号是什么？

**关键点：**
- MSI 消息只包含**地址**和**数据**
- 地址指向中断控制器的特定寄存器
- 数据是 event_id（事件 ID）
- **侧带信息（Sideband Information）**用于识别设备

### 12.2 MSI 消息的组成

```c
struct msi_msg {
    u32 address_lo;  // 低 32 位地址
    u32 address_hi;  // 高 32 位地址（64 位模式）
    u16 data;        // 数据字段（event_id）
};
```

**示例（ARM GIC ITS）：**

```c
// drivers/irqchip/irq-gic-v3-its.c:1725
static void its_irq_compose_msi_msg(struct irq_data *d, struct msi_msg *msg)
{
    struct its_device *its_dev = irq_data_get_irq_chip_data(d);
    struct its_node *its;
    u64 addr;
    
    its = its_dev->its;
    addr = its->get_msi_base(its_dev);  // GITS_TRANSLATER 地址
    
    msg->address_lo = lower_32_bits(addr);
    msg->address_hi = upper_32_bits(addr);
    msg->data = its_get_event_id(d);    // event_id
}
```

**写入设备：**
- 地址：`GITS_TRANSLATER` 寄存器地址（所有设备共享）
- 数据：`event_id`（每个 MSI 向量不同）

### 12.3 侧带信息（Sideband Information）

**问题：** 如果所有设备都写入同一个地址，如何区分是哪个设备？

**答案：** 通过**侧带信息（Requester ID）**

#### 12.3.1 Requester ID (RID)

PCIe 事务包含 Requester ID，格式如下：

```
RID = Bus[15:8] + Device[7:3] + Function[2:0]
```

**示例：**
- Bus 0, Device 1, Function 0 → RID = 0x0008
- Bus 1, Device 5, Function 2 → RID = 0x014A

#### 12.3.2 RID 到 Device ID 的映射

```c
// drivers/pci/msi/irqdomain.c:415
u32 pci_msi_domain_get_msi_rid(struct irq_domain *domain, struct pci_dev *pdev)
{
    u32 rid = pci_dev_id(pdev);  // 获取 RID
    
    // 处理 DMA 别名
    pci_for_each_dma_alias(pdev, get_msi_id_cb, &rid);
    
    // 通过设备树或 ACPI 映射
    of_node = irq_domain_get_of_node(domain);
    rid = of_node ? of_msi_map_id(&pdev->dev, of_node, rid) :
                    iort_msi_map_id(&pdev->dev, rid);
    
    return rid;  // 返回 device_id
}
```

**映射过程：**
1. 从 PCIe 配置空间读取 RID
2. 通过设备树的 `msi-map` 映射到 device_id
3. device_id 用于 ITS 的设备识别

### 12.4 GIC ITS 的中断识别机制

#### 12.4.1 ITS 数据结构

```c
// drivers/irqchip/irq-gic-v3-its.c:164
struct its_device {
    struct list_head entry;
    struct its_node *its;           // ITS 节点
    struct event_lpi_map event_map; // 事件到 LPI 的映射
    u32 device_id;                  // 设备 ID（从 RID 映射）
    void *itt;                       // Interrupt Translation Table
    u32 nr_ites;                     // ITT 条目数
};
```

#### 12.4.2 Interrupt Translation Table (ITT)

ITT 是每个设备的中断翻译表，存储在内存中：

```
ITT[event_id] = {
    Physical ID (LPI ID),  // 物理中断 ID
    Collection ID,        // 目标 CPU Collection
    Valid bit             // 有效性标志
}
```

**创建 ITT：**

```c
// drivers/irqchip/irq-gic-v3-its.c:3390
static struct its_device *its_create_device(struct its_node *its,
                                            u32 dev_id, int nvecs,
                                            bool alloc_lpis)
{
    // 1. 分配 ITT 内存
    sz = nr_ites * (ITT_ENTRY_SIZE + 1);
    itt = kzalloc_node(sz, GFP_KERNEL, its->numa_node);
    
    // 2. 分配 LPI ID
    if (alloc_lpis) {
        lpi_map = its_lpi_alloc(nvecs, &lpi_base, &nr_lpis);
    }
    
    // 3. 创建设备结构
    dev->its = its;
    dev->itt = itt;
    dev->device_id = dev_id;
    dev->event_map.lpi_base = lpi_base;
    
    // 4. 发送 MAPD 命令到 ITS，注册设备
    its_send_mapd(dev, 1);
}
```

#### 12.4.3 中断映射（MAPTI）

当分配 MSI 中断时，需要建立 event_id 到 LPI ID 的映射：

```c
// drivers/irqchip/irq-gic-v3-its.c:3624
its_send_mapti(its_dev, d->hwirq, event);
```

**MAPTI 命令：**
- Device ID：设备的 device_id
- Event ID：MSI 消息中的 data 字段
- Physical ID：物理 LPI ID（Linux IRQ 对应的硬件中断号）
- Collection ID：目标 CPU Collection

### 12.5 完整的 MSI 中断识别流程

```
┌─────────────────────────────────────────────────────────┐
│ 1. 设备初始化阶段                                        │
└─────────────────────────────────────────────────────────┘
    ↓
驱动调用 pci_alloc_irq_vectors()
    ↓
内核分配 Linux IRQ 号（virq）
    ↓
ITS 分配 LPI ID（hwirq）
    ↓
创建 its_device 结构
    ├─ device_id = pci_msi_domain_get_msi_rid()  // 从 RID 映射
    ├─ 分配 ITT 表
    └─ 发送 MAPD 命令注册设备
    ↓
为每个 MSI 向量建立映射
    ├─ event_id = msi_index  // 0, 1, 2, ...
    ├─ hwirq = LPI ID        // 物理中断号
    └─ 发送 MAPTI 命令：device_id + event_id → hwirq
    ↓
生成 MSI 消息
    ├─ address = GITS_TRANSLATER 地址
    └─ data = event_id
    ↓
写入 PCI 配置空间或 MSI-X Table

┌─────────────────────────────────────────────────────────┐
│ 2. 中断触发阶段                                          │
└─────────────────────────────────────────────────────────┘
    ↓
设备产生中断事件
    ↓
设备执行内存写操作
    ├─ 地址 = GITS_TRANSLATER（所有设备共享）
    ├─ 数据 = event_id（每个向量不同）
    └─ 侧带信息 = Requester ID（自动携带）
    ↓
PCIe 总线传输 TLP（Transaction Layer Packet）
    ├─ 包含 Requester ID（Bus + Device + Function）
    └─ 包含地址和数据
    ↓
到达 Root Complex
    ↓
路由到 GIC ITS
    ↓
ITS 硬件处理
    ├─ 从 TLP 提取 Requester ID
    ├─ 通过 msi-map 映射得到 device_id
    ├─ 从 TLP 提取 data（event_id）
    └─ 查找 ITT[device_id][event_id]
    ↓
ITT 表查找
    ├─ 输入：device_id + event_id
    ├─ 输出：Physical ID (LPI ID) + Collection ID
    └─ 验证 Valid 位
    ↓
ITS 发送中断到目标 CPU
    ├─ LPI ID = 物理中断号
    └─ Collection = 目标 CPU
    ↓
CPU 接收中断
    ↓
GIC 分发中断
    ↓
查找 irq_desc[LPI_ID]
    ↓
调用驱动注册的 handler
```

### 12.6 关键数据结构映射关系

```
PCIe 设备
    ↓
Requester ID (RID)
    ├─ Bus:Device:Function
    └─ 例如：0x0008 (Bus 0, Dev 1, Func 0)
    ↓
设备树映射 (msi-map)
    ├─ RID → device_id
    └─ 例如：0x0008 → 0x0000
    ↓
ITS Device
    ├─ device_id = 0x0000
    ├─ ITT 表地址
    └─ event_map
    ↓
MSI 向量
    ├─ event_id = 0, 1, 2, ...
    ├─ Linux IRQ (virq) = 100, 101, 102, ...
    └─ LPI ID (hwirq) = 8192, 8193, 8194, ...
    ↓
ITT 表项
    ├─ ITT[device_id][event_id] = {
    │     Physical ID = LPI ID,
    │     Collection ID = CPU ID,
    │     Valid = 1
    │   }
    └─ 例如：ITT[0x0000][0] = {8192, 0, 1}
```

### 12.7 示例：具体的中断识别过程

假设有一个 PCIe 网卡设备：

**初始化阶段：**

```c
// 1. 设备 RID
RID = 0x0008  // Bus 0, Device 1, Function 0

// 2. 设备树映射
msi-map = <0x0 &gic_its 0x0 0x10000>
// RID 0x0008 映射到 device_id = 0x0008

// 3. 分配 MSI 向量
pci_alloc_irq_vectors(dev, 1, 4, PCI_IRQ_MSI)
// 返回 4 个向量

// 4. 为每个向量建立映射
// 向量 0: event_id=0, virq=100, hwirq=8192
its_send_mapti(its_dev, 8192, 0);
// ITT[0x0008][0] = {8192, CPU0, Valid}

// 向量 1: event_id=1, virq=101, hwirq=8193
its_send_mapti(its_dev, 8193, 1);
// ITT[0x0008][1] = {8193, CPU1, Valid}

// 5. 生成 MSI 消息
// 向量 0: address=GITS_TRANSLATER, data=0
// 向量 1: address=GITS_TRANSLATER, data=1
```

**中断触发阶段：**

```
1. 网卡接收数据包，触发向量 0
   ↓
2. 设备执行内存写：
   - 地址 = GITS_TRANSLATER (0x1820040)
   - 数据 = 0 (event_id)
   - 侧带信息 = RID 0x0008
   ↓
3. PCIe TLP 传输：
   - Requester ID = 0x0008
   - 地址 = 0x1820040
   - 数据 = 0
   ↓
4. ITS 硬件处理：
   - 提取 RID = 0x0008
   - 映射得到 device_id = 0x0008
   - 提取 data = 0 (event_id)
   - 查找 ITT[0x0008][0]
   ↓
5. ITT 查找结果：
   - Physical ID = 8192 (LPI ID)
   - Collection = CPU0
   - Valid = 1
   ↓
6. ITS 发送中断到 CPU0：
   - LPI ID = 8192
   ↓
7. CPU0 接收中断：
   - 查找 irq_desc[8192]
   - 找到 virq = 100
   - 调用驱动注册的 handler
```

### 12.8 为什么需要侧带信息？

**问题：** 如果所有设备都写入同一个地址（GITS_TRANSLATER），如何区分设备？

**答案：** PCIe 协议保证每个内存写事务都携带 Requester ID

1. **硬件自动携带**：PCIe 硬件在 TLP 中自动包含 Requester ID
2. **不可伪造**：Requester ID 由硬件设置，软件无法修改
3. **唯一标识**：每个 PCIe 设备有唯一的 RID

### 12.9 不同架构的 MSI 识别

#### 12.9.1 ARM GIC ITS

- **地址**：GITS_TRANSLATER 寄存器（所有设备共享）
- **数据**：event_id
- **侧带信息**：Requester ID → device_id
- **查找表**：ITT[device_id][event_id] → LPI ID

#### 12.9.2 x86 APIC

- **地址**：APIC MSI 地址（每个 CPU 不同）
- **数据**：中断向量号
- **侧带信息**：通过地址区分 CPU
- **查找**：直接使用数据作为中断向量

#### 12.9.3 其他 MSI 控制器

- **简单控制器**：可能不需要侧带信息，通过地址区分
- **复杂控制器**：类似 ITS，使用设备 ID + event_id

### 12.10 总结

MSI 中断识别的关键要素：

1. **MSI 消息**：
   - 地址：指向中断控制器寄存器
   - 数据：event_id（事件标识）

2. **侧带信息**：
   - Requester ID：硬件自动携带
   - 用于识别设备（device_id）

3. **查找表**：
   - ITT 表：device_id + event_id → LPI ID
   - 在初始化时建立映射

4. **完整流程**：
   - 初始化：建立 device_id + event_id → LPI ID 映射
   - 触发：设备写入地址+数据，硬件提取 RID
   - 识别：ITS 使用 RID→device_id + event_id 查找 ITT
   - 分发：ITS 发送 LPI ID 到目标 CPU

**核心答案：**
系统通过**侧带信息（Requester ID）**识别设备，通过**ITT 表**将 device_id + event_id 映射到具体的 LPI ID（物理中断号），从而知道是哪个中断。

