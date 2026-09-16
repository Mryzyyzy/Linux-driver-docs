# Linux DMA API 使用指南与一致性保证

## 1. DMA 概述

**DMA (Direct Memory Access)** 允许外设直接访问内存，无需 CPU 参与，提高数据传输效率。

### DMA 传输类型

1. **一致性 DMA (Coherent DMA)**: 缓存一致性由硬件保证
2. **流式 DMA (Streaming DMA)**: 需要软件管理缓存一致性

---

## 2. DMA API 分类

### 2.1 一致性 DMA 接口

用于需要**长期共享**的内存区域（如描述符、控制块）。

#### 分配/释放

```c
#include <linux/dma-mapping.h>

// 分配一致性 DMA 内存
void *dma_alloc_coherent(struct device *dev, size_t size,
                         dma_addr_t *dma_handle, gfp_t flag);

// 释放一致性 DMA 内存
void dma_free_coherent(struct device *dev, size_t size,
                       void *cpu_addr, dma_addr_t dma_handle);
```

**示例**:

```c
struct device *dev = &pdev->dev;
dma_addr_t dma_addr;
void *cpu_addr;
size_t size = PAGE_SIZE;

// 分配
cpu_addr = dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
if (!cpu_addr) {
    dev_err(dev, "Failed to allocate coherent DMA memory\n");
    return -ENOMEM;
}

// 使用
memset(cpu_addr, 0, size);
// ... 配置 DMA 描述符等

// 释放
dma_free_coherent(dev, size, cpu_addr, dma_addr);
```

#### 一致性保证机制

**硬件保证**:
- CPU 和 DMA 看到的是**同一份数据**
- 硬件自动处理缓存一致性（通过 cache coherency 协议）
- 无需手动刷新缓存

**适用场景**:
- DMA 描述符
- 控制寄存器结构
- 长期共享的数据结构

---

### 2.2 流式 DMA 接口

用于**一次性传输**的数据缓冲区。

#### 映射/取消映射

```c
// 映射（CPU → DMA）
dma_addr_t dma_map_single(struct device *dev, void *ptr,
                          size_t size, enum dma_data_direction dir);

// 取消映射
void dma_unmap_single(struct device *dev, dma_addr_t dma_addr,
                      size_t size, enum dma_data_direction dir);

// 映射（支持 scatter-gather）
int dma_map_sg(struct device *dev, struct scatterlist *sg,
               int nents, enum dma_data_direction dir);

// 取消映射
void dma_unmap_sg(struct device *dev, struct scatterlist *sg,
                  int nents, enum dma_data_direction dir);
```

**传输方向**:

```c
enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,  // 双向
    DMA_TO_DEVICE = 1,      // CPU → 设备（写）
    DMA_FROM_DEVICE = 2,    // 设备 → CPU（读）
    DMA_NONE = 3,
};
```

#### 一致性保证机制

**软件保证**（需要手动处理）:

1. **映射时** (`dma_map_single/sg`):
   - 如果方向是 `DMA_TO_DEVICE`: 刷新 CPU 写缓存（确保设备看到最新数据）
   - 如果方向是 `DMA_FROM_DEVICE`: 使缓存失效（确保 CPU 读取最新数据）

2. **取消映射时** (`dma_unmap_single/sg`):
   - 如果方向是 `DMA_FROM_DEVICE`: 刷新缓存（确保 CPU 看到设备写入的数据）
   - 如果方向是 `DMA_TO_DEVICE`: 通常不需要额外操作

**示例**:

```c
// 发送数据到设备
void send_data_to_device(struct device *dev, void *buffer, size_t len)
{
    dma_addr_t dma_addr;
    
    // 1. 准备数据（CPU 写入）
    memcpy(buffer, source_data, len);
    
    // 2. 映射（自动刷新 CPU 写缓存）
    dma_addr = dma_map_single(dev, buffer, len, DMA_TO_DEVICE);
    if (dma_mapping_error(dev, dma_addr)) {
        dev_err(dev, "DMA mapping failed\n");
        return;
    }
    
    // 3. 配置 DMA 并启动传输
    configure_dma(dev, dma_addr, len);
    start_dma_transfer(dev);
    
    // 4. 等待传输完成（中断或轮询）
    wait_for_dma_complete(dev);
    
    // 5. 取消映射
    dma_unmap_single(dev, dma_addr, len, DMA_TO_DEVICE);
}

// 从设备接收数据
void receive_data_from_device(struct device *dev, void *buffer, size_t len)
{
    dma_addr_t dma_addr;
    
    // 1. 映射（使缓存失效，准备接收）
    dma_addr = dma_map_single(dev, buffer, len, DMA_FROM_DEVICE);
    if (dma_mapping_error(dev, dma_addr)) {
        dev_err(dev, "DMA mapping failed\n");
        return;
    }
    
    // 2. 配置 DMA 并启动传输
    configure_dma(dev, dma_addr, len);
    start_dma_transfer(dev);
    
    // 3. 等待传输完成
    wait_for_dma_complete(dev);
    
    // 4. 取消映射（自动刷新缓存，确保 CPU 看到数据）
    dma_unmap_single(dev, dma_addr, len, DMA_FROM_DEVICE);
    
    // 5. 现在可以安全地读取数据
    process_received_data(buffer, len);
}
```

---

## 3. 一致性保证详解

### 3.1 缓存一致性问题

**问题场景**:

```
CPU 写入数据到内存 → 数据在 CPU 缓存中
DMA 从内存读取数据 → 可能读到旧数据（缓存未刷新）
```

**解决方案**:

1. **一致性 DMA**: 硬件自动处理
2. **流式 DMA**: 软件在映射/取消映射时处理

### 3.2 ARM 平台的缓存操作

在 ARM 平台上，DMA API 内部会调用：

```c
// DMA_TO_DEVICE: 刷新缓存
dma_cache_sync(dev, cpu_addr, size, DMA_TO_DEVICE);
// 内部调用: __dma_map_area() → 刷新 CPU 写缓存

// DMA_FROM_DEVICE: 使缓存失效
dma_cache_sync(dev, cpu_addr, size, DMA_FROM_DEVICE);
// 内部调用: __dma_unmap_area() → 使缓存失效

**缓存重新生效的时机**:
- 缓存失效后，CPU 下次访问该地址时会**自动从内存重新加载**到缓存
- 但更关键的是：`dma_unmap_single(..., DMA_FROM_DEVICE)` 时会**刷新缓存**，
  确保 CPU 读取时看到设备写入的最新数据
- 因此，**必须在取消映射后才能安全读取数据**
```

**底层实现** (ARM):

```c
// arch/arm/mm/dma-mapping.c

static void __dma_map_area(const void *start, size_t size, enum dma_data_direction dir)
{
    if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL)
        dmac_inv_range(start, start + size);  // 使缓存失效
    else
        dmac_clean_range(start, start + size);  // 刷新缓存
}

static void __dma_unmap_area(const void *start, size_t size, enum dma_data_direction dir)
{
    if (dir != DMA_TO_DEVICE)
        dmac_clean_range(start, start + size);  // 刷新缓存
}
```

**⚠️ 重要：缓存失效的范围**

- **不是整个 cache 失效**，而是**按地址范围失效相关的缓存行**
- 只失效 `buffer` 地址范围内的缓存行（通常是 64 字节对齐）
- **其他内存地址的缓存完全不受影响**
- 详见: [dma_cache_invalidate_scope.md](dma_cache_invalidate_scope.md)

### 3.3 内存屏障和同步

**DMA 传输中的内存屏障**:

```c
// 1. 写入 DMA 描述符后，需要内存屏障
write_dma_descriptor(desc);
wmb();  // 写内存屏障，确保描述符写入完成
start_dma();

// 2. DMA 完成后，读取数据前需要内存屏障
wait_for_dma_complete();
rmb();  // 读内存屏障，确保数据读取完成
read_dma_data();
```

---

## 4. 实际驱动示例

### 4.1 PCIe 驱动中的 DMA 使用

```c
#include <linux/pci.h>
#include <linux/dma-mapping.h>

struct my_pcie_device {
    struct pci_dev *pdev;
    void __iomem *regs;
    
    // 一致性 DMA: 描述符
    struct dma_descriptor *desc;
    dma_addr_t desc_dma;
    
    // 流式 DMA: 数据缓冲区
    void *tx_buffer;
    void *rx_buffer;
    dma_addr_t tx_dma;
    dma_addr_t rx_dma;
    size_t buffer_size;
};

static int my_pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct my_pcie_device *dev;
    struct device *device = &pdev->dev;
    int err;
    
    dev = devm_kzalloc(device, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;
    
    dev->pdev = pdev;
    dev->buffer_size = PAGE_SIZE;
    
    // 1. 分配一致性 DMA 内存（描述符）
    dev->desc = dma_alloc_coherent(device, sizeof(*dev->desc),
                                   &dev->desc_dma, GFP_KERNEL);
    if (!dev->desc) {
        dev_err(device, "Failed to allocate descriptor\n");
        return -ENOMEM;
    }
    
    // 2. 分配流式 DMA 缓冲区
    dev->tx_buffer = kzalloc(dev->buffer_size, GFP_KERNEL);
    dev->rx_buffer = kzalloc(dev->buffer_size, GFP_KERNEL);
    if (!dev->tx_buffer || !dev->rx_buffer) {
        err = -ENOMEM;
        goto err_free_desc;
    }
    
    pci_set_drvdata(pdev, dev);
    return 0;
    
err_free_desc:
    dma_free_coherent(device, sizeof(*dev->desc), dev->desc, dev->desc_dma);
    return err;
}

// 发送数据
static int my_pcie_send_data(struct my_pcie_device *dev, const void *data, size_t len)
{
    struct device *device = &dev->pdev->dev;
    dma_addr_t dma_addr;
    
    if (len > dev->buffer_size)
        return -EINVAL;
    
    // 1. 复制数据到缓冲区
    memcpy(dev->tx_buffer, data, len);
    
    // 2. 映射到 DMA 地址（自动刷新 CPU 缓存）
    dma_addr = dma_map_single(device, dev->tx_buffer, len, DMA_TO_DEVICE);
    if (dma_mapping_error(device, dma_addr)) {
        dev_err(device, "DMA mapping failed\n");
        return -EFAULT;
    }
    
    // 3. 配置 DMA 描述符（一致性内存，无需映射）
    dev->desc->src_addr = dma_addr;
    dev->desc->dst_addr = dev->regs + TX_FIFO;
    dev->desc->length = len;
    dev->desc->control = DESC_VALID;
    
    // 4. 内存屏障：确保描述符写入完成
    wmb();
    
    // 5. 启动 DMA
    writel(dev->desc_dma, dev->regs + DMA_DESC_ADDR);
    writel(DMA_START, dev->regs + DMA_CONTROL);
    
    // 注意：这里不立即取消映射，等待传输完成后再取消
    return 0;
}

// DMA 传输完成中断处理
static irqreturn_t my_pcie_irq_handler(int irq, void *dev_id)
{
    struct my_pcie_device *dev = dev_id;
    u32 status = readl(dev->regs + DMA_STATUS);
    
    if (status & DMA_COMPLETE) {
        // 取消映射（如果是接收，会刷新缓存）
        dma_unmap_single(&dev->pdev->dev, dev->tx_dma,
                         dev->buffer_size, DMA_TO_DEVICE);
        
        // 通知上层
        complete(&dev->tx_complete);
    }
    
    return IRQ_HANDLED;
}

// 接收数据
static int my_pcie_receive_data(struct my_pcie_device *dev, void *data, size_t len)
{
    struct device *device = &dev->pdev->dev;
    dma_addr_t dma_addr;
    
    // 1. 映射接收缓冲区（使缓存失效）
    dma_addr = dma_map_single(device, dev->rx_buffer, len, DMA_FROM_DEVICE);
    if (dma_mapping_error(device, dma_addr)) {
        dev_err(device, "DMA mapping failed\n");
        return -EFAULT;
    }
    
    // 2. 配置 DMA
    dev->desc->src_addr = dev->regs + RX_FIFO;
    dev->desc->dst_addr = dma_addr;
    dev->desc->length = len;
    dev->desc->control = DESC_VALID;
    
    wmb();
    
    // 3. 启动 DMA
    writel(dev->desc_dma, dev->regs + DMA_DESC_ADDR);
    writel(DMA_START, dev->regs + DMA_CONTROL);
    
    // 4. 等待完成
    wait_for_completion(&dev->rx_complete);
    
    // 5. 取消映射（自动刷新缓存，确保 CPU 看到数据）
    dma_unmap_single(device, dma_addr, len, DMA_FROM_DEVICE);
    
    // 6. 现在可以安全读取
    memcpy(data, dev->rx_buffer, len);
    
    return len;
}
```

---

## 5. Scatter-Gather DMA

用于处理**分散的内存区域**（如网络数据包）。

```c
// 分配 scatterlist
struct scatterlist *sg;
int nents;

sg = kcalloc(MAX_SG_ENTRIES, sizeof(*sg), GFP_KERNEL);
sg_init_table(sg, MAX_SG_ENTRIES);

// 设置 scatterlist 条目
sg_set_buf(&sg[0], buffer1, len1);
sg_set_buf(&sg[1], buffer2, len2);
nents = 2;

// 映射
nents = dma_map_sg(dev, sg, nents, DMA_TO_DEVICE);
if (nents == 0) {
    dev_err(dev, "DMA map failed\n");
    return -EFAULT;
}

// 获取每个段的 DMA 地址
for (i = 0; i < nents; i++) {
    dma_addr_t dma_addr = sg_dma_address(&sg[i]);
    size_t len = sg_dma_len(&sg[i]);
    // 配置 DMA 描述符
}

// 取消映射
dma_unmap_sg(dev, sg, nents, DMA_TO_DEVICE);
```

---

## 6. 一致性保证检查清单

### 6.1 一致性 DMA

- [x] 使用 `dma_alloc_coherent()` 分配
- [x] CPU 和 DMA 都访问同一块内存
- [x] 硬件自动保证一致性
- [x] 无需手动刷新缓存

### 6.2 流式 DMA

**发送 (CPU → 设备)**:
- [x] 使用 `dma_map_single/sg()` 映射（方向：`DMA_TO_DEVICE`）
- [x] 映射后立即启动 DMA，不要修改缓冲区
- [x] 传输完成后 `dma_unmap_single/sg()`

**接收 (设备 → CPU)**:
- [x] 使用 `dma_map_single/sg()` 映射（方向：`DMA_FROM_DEVICE`）
- [x] 映射后启动 DMA
- [x] 传输完成后 `dma_unmap_single/sg()`（刷新缓存）
- [x] 取消映射后才能读取数据

**双向传输**:
- [x] 使用 `DMA_BIDIRECTIONAL`
- [x] 映射和取消映射都会处理缓存

### 6.3 内存屏障

- [x] 写入 DMA 描述符后使用 `wmb()`
- [x] 读取 DMA 数据前使用 `rmb()`
- [x] 必要时使用 `mb()`（全屏障）

---

## 7. 常见错误和陷阱

### 错误 1: 映射后修改数据

```c
// ❌ 错误
dma_addr = dma_map_single(dev, buffer, len, DMA_TO_DEVICE);
buffer[0] = 0x42;  // 错误！映射后不应修改
start_dma(dev, dma_addr);

// ✅ 正确
buffer[0] = 0x42;  // 先修改
dma_addr = dma_map_single(dev, buffer, len, DMA_TO_DEVICE);
start_dma(dev, dma_addr);
```

### 错误 2: 取消映射前读取数据

```c
// ❌ 错误
wait_for_dma();
memcpy(dest, buffer, len);  // 错误！缓存可能未刷新
dma_unmap_single(dev, dma_addr, len, DMA_FROM_DEVICE);

// ✅ 正确
wait_for_dma();
dma_unmap_single(dev, dma_addr, len, DMA_FROM_DEVICE);
memcpy(dest, buffer, len);  // 取消映射后读取
```

### 错误 3: 忘记检查映射错误

```c
// ❌ 错误
dma_addr = dma_map_single(dev, buffer, len, DMA_TO_DEVICE);
start_dma(dev, dma_addr);  // 如果映射失败会出问题

// ✅ 正确
dma_addr = dma_map_single(dev, buffer, len, DMA_TO_DEVICE);
if (dma_mapping_error(dev, dma_addr)) {
    return -EFAULT;
}
start_dma(dev, dma_addr);
```

---

## 8. ARM 平台特殊考虑

### 8.1 缓存行对齐

```c
// 确保 DMA 缓冲区按缓存行对齐
buffer = kmalloc(size + L1_CACHE_BYTES, GFP_KERNEL);
buffer = PTR_ALIGN(buffer, L1_CACHE_BYTES);
```

### 8.2 DMA 地址限制

```c
// 检查 DMA 掩码
if (!dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32))) {
    dev_err(dev, "32-bit DMA not supported\n");
    return -EIO;
}
```

### 8.3 设备树中的 DMA 配置

```dts
my_device {
    compatible = "vendor,device";
    dma-coherent;  // 声明设备支持一致性 DMA
    dma-ranges = <0x0 0x0 0x0 0x80000000>;  // DMA 地址范围
};
```

---

## 9. 调试技巧

### 9.1 启用 DMA 调试

```bash
# 在内核启动参数添加
dma_debug=on
dma_debug_entries=65536
```

### 9.2 检查 DMA 映射泄漏

```bash
# 查看 DMA 调试信息
cat /sys/kernel/debug/dma-api/dump
```

### 9.3 使用 DMA 调试工具

```c
// 在代码中添加
#include <linux/dma-debug.h>

// 检查映射
dma_debug_add_bus(&pci_bus_type);
```

---

## 10. 性能优化

### 10.1 使用 DMA 池

```c
// 分配小块 DMA 内存（避免碎片）
struct dma_pool *pool;

pool = dma_pool_create("my_pool", dev, size, align, 0);
buffer = dma_pool_alloc(pool, GFP_KERNEL, &dma_addr);
// ... 使用
dma_pool_free(pool, buffer, dma_addr);
dma_pool_destroy(pool);
```

### 10.2 预分配缓冲区

```c
// 避免频繁分配/释放
static void *dma_buffer;
static dma_addr_t dma_buffer_addr;

// 在 probe 时分配
dma_buffer = dma_alloc_coherent(dev, size, &dma_buffer_addr, GFP_KERNEL);

// 在 remove 时释放
dma_free_coherent(dev, size, dma_buffer, dma_buffer_addr);
```

---

## 11. 总结

### 一致性保证机制

1. **一致性 DMA**: 硬件自动保证，使用 `dma_alloc_coherent()`
2. **流式 DMA**: 软件在映射/取消映射时处理缓存，使用 `dma_map_single/sg()`

### 关键原则

- ✅ 映射后不要修改发送缓冲区
- ✅ 取消映射后才能读取接收缓冲区
- ✅ 始终检查 `dma_mapping_error()`
- ✅ 使用适当的内存屏障
- ✅ 匹配映射和取消映射的方向

### 最佳实践

- 长期共享的数据 → 一致性 DMA
- 一次性传输 → 流式 DMA
- 分散数据 → scatter-gather
- 小块频繁分配 → DMA 池

