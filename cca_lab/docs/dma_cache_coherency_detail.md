# DMA 缓存一致性详解：失效与重新生效

## 问题：缓存失效后什么时候重新生效？

这是一个关于 ARM 平台缓存一致性的关键问题。

---

## 1. 缓存失效（Invalidate）的含义

### 1.1 什么是缓存失效？

**缓存失效（Cache Invalidate）** 是指：
- 将缓存中的**旧数据标记为无效**
- **不立即**从内存加载新数据
- 下次 CPU 访问时，会触发**缓存未命中（cache miss）**，然后从内存加载

### 1.2 ARM 平台的实现

```c
// arch/arm/mm/dma-mapping.c

static void __dma_map_area(const void *start, size_t size, enum dma_data_direction dir)
{
    if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL)
        dmac_inv_range(start, start + size);  // 使缓存失效
    // ...
}
```

**底层实现** (ARM):
```assembly
; dmac_inv_range 的简化逻辑
loop:
    dc ivac, x0    ; 使缓存行失效（Invalidate by Virtual Address to PoC）
    add x0, x0, #64
    cmp x0, x1
    blt loop
```

---

## 2. 缓存重新生效的时机

### 2.1 自动重新加载（Lazy Loading）

**关键点**：缓存失效后，**不会立即**从内存加载数据。而是在：

1. **CPU 下次访问该地址时**
   - 触发缓存未命中（cache miss）
   - CPU 自动从内存加载数据到缓存
   - 这就是"重新生效"的时机

2. **但这里有个问题**：
   - 如果设备**还没写完**数据，CPU 访问时会加载**部分旧数据**
   - 因此需要**等待 DMA 完成**后再访问

### 2.2 显式刷新（在取消映射时）

**更安全的做法**：在 `dma_unmap_single(..., DMA_FROM_DEVICE)` 时：

```c
static void __dma_unmap_area(const void *start, size_t size, enum dma_data_direction dir)
{
    if (dir != DMA_TO_DEVICE)
        dmac_clean_range(start, start + size);  // 刷新缓存
}
```

**刷新缓存（Clean）** 的含义：
- 如果缓存中有**脏数据**（被 CPU 修改过），写回内存
- **使缓存失效**，强制下次访问从内存读取
- 确保 CPU 看到内存中的**最新数据**（设备写入的）

---

## 3. 完整流程示例

### 场景：从设备接收数据

```c
void receive_data_from_device(struct device *dev, void *buffer, size_t len)
{
    dma_addr_t dma_addr;
    
    /* 步骤 1: 映射（使缓存失效） */
    dma_addr = dma_map_single(dev, buffer, len, DMA_FROM_DEVICE);
    // 此时：
    // - 缓存被标记为无效
    // - CPU 如果访问 buffer，会触发 cache miss，从内存加载
    // - 但此时内存中可能还是旧数据（设备还没写）
    
    /* 步骤 2: 启动 DMA 传输 */
    start_dma(dev, dma_addr);
    // 设备开始写入数据到内存（物理地址）
    
    /* 步骤 3: 等待 DMA 完成 */
    wait_for_dma_complete(dev);
    // 此时设备已经写完数据到内存
    
    /* 步骤 4: 取消映射（刷新缓存） */
    dma_unmap_single(dev, dma_addr, len, DMA_FROM_DEVICE);
    // 此时：
    // - 缓存被刷新（如果有脏数据，写回内存）
    // - 缓存被失效
    // - 下次 CPU 访问时，会从内存加载设备写入的新数据
    
    /* 步骤 5: 读取数据（现在安全了） */
    memcpy(dest, buffer, len);
    // CPU 访问 buffer[0] 时：
    // 1. 检查缓存 → 缓存失效（未命中）
    // 2. 从内存加载 buffer[0] → 得到设备写入的新数据
    // 3. 数据加载到缓存 → 缓存重新生效
    // 4. 后续访问 buffer[1..n] 同样过程
}
```

---

## 4. 时间线分析

### 4.1 详细时间线

```
时间点 T0: dma_map_single(..., DMA_FROM_DEVICE)
├─ 缓存失效（invalidate）
├─ 缓存状态: 无效（invalid）
└─ CPU 如果访问: cache miss → 从内存加载（可能是旧数据）

时间点 T1: start_dma()
├─ 设备开始写入内存
└─ 内存中的数据正在被设备更新

时间点 T2: wait_for_dma_complete()
├─ 设备写入完成
└─ 内存中现在是新数据

时间点 T3: dma_unmap_single(..., DMA_FROM_DEVICE)
├─ 刷新缓存（clean，如果有脏数据）
├─ 使缓存失效（invalidate）
├─ 缓存状态: 无效（invalid）
└─ 强制下次访问从内存读取

时间点 T4: CPU 读取 buffer[0]
├─ 检查缓存 → 无效（cache miss）
├─ 从内存加载 buffer[0] → 新数据
├─ 数据加载到缓存
└─ 缓存重新生效 ✓

时间点 T5: CPU 读取 buffer[1]
├─ 检查缓存 → 可能命中（如果 buffer[0] 和 buffer[1] 在同一缓存行）
└─ 从缓存读取 → 快速访问
```

---

## 5. 为什么需要两步：失效 + 刷新？

### 5.1 只失效不刷新的问题

如果只失效不刷新：

```c
// ❌ 假设只失效，不刷新
dma_map_single(..., DMA_FROM_DEVICE);  // 只失效
// ... DMA 传输 ...
// 如果缓存中有 CPU 之前写入的脏数据，这些数据可能还在缓存中
// 失效后，脏数据丢失，但内存中可能还有旧数据
```

### 5.2 正确的做法：失效 + 刷新

```c
// ✅ 正确的流程
dma_map_single(..., DMA_FROM_DEVICE);
// 1. 失效缓存（清除旧数据）
// 2. 准备接收新数据

dma_unmap_single(..., DMA_FROM_DEVICE);
// 1. 刷新缓存（如果有脏数据，写回内存）
// 2. 失效缓存（强制下次从内存读取）
// 3. 确保 CPU 看到设备写入的新数据
```

---

## 6. ARM 缓存操作指令

### 6.1 缓存失效（Invalidate）

```assembly
; ARM64: 使缓存失效
dc ivac, x0    ; Invalidate by Virtual Address to Point of Coherency
```

**效果**:
- 标记缓存行为无效
- **不**从内存加载数据
- 下次访问时触发 cache miss

### 6.2 缓存刷新（Clean）

```assembly
; ARM64: 刷新缓存
dc cvac, x0    ; Clean by Virtual Address to Point of Coherency
```

**效果**:
- 如果缓存行是脏的（dirty），写回内存
- 标记缓存行为干净（clean）
- **不**使缓存失效

### 6.3 刷新并失效（Clean and Invalidate）

```assembly
; ARM64: 刷新并失效
dc civac, x0   ; Clean and Invalidate by Virtual Address to PoC
```

**效果**:
- 先刷新（写回脏数据）
- 再失效（标记为无效）
- 这是 `dma_unmap_single(..., DMA_FROM_DEVICE)` 使用的操作

---

## 7. 实际代码中的处理

### 7.1 Linux 内核的实现

```c
// arch/arm64/mm/dma-mapping.c

static void __dma_map_area(const void *start, size_t size, enum dma_data_direction dir)
{
    if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL) {
        __dma_inv_area(start, size);  // 使缓存失效
    } else {
        __dma_clean_area(start, size);  // 刷新缓存
    }
}

static void __dma_unmap_area(const void *start, size_t size, enum dma_data_direction dir)
{
    if (dir == DMA_TO_DEVICE) {
        // 发送完成，不需要额外操作
        return;
    }
    
    // FROM_DEVICE 或 BIDIRECTIONAL
    __dma_clean_inv_area(start, size);  // 刷新并失效
    // 确保 CPU 下次访问时从内存读取最新数据
}
```

### 7.2 为什么取消映射时要刷新并失效？

```c
dma_unmap_single(..., DMA_FROM_DEVICE);
// 内部调用: __dma_clean_inv_area()
// 
// 原因：
// 1. 刷新（clean）: 如果缓存中有 CPU 写入的脏数据，写回内存
// 2. 失效（invalidate）: 标记缓存无效，强制下次从内存读取
// 3. 结果: CPU 下次访问时，会从内存加载设备写入的新数据
```

---

## 8. 验证缓存重新生效

### 8.1 测试代码

```c
void test_cache_revalidation(struct device *dev, void *buffer, size_t len)
{
    dma_addr_t dma_addr;
    u8 old_value, new_value;
    
    // 初始值
    buffer[0] = 0xAA;
    old_value = buffer[0];
    pr_info("Initial value: 0x%02x\n", old_value);
    
    // 映射（失效缓存）
    dma_addr = dma_map_single(dev, buffer, len, DMA_FROM_DEVICE);
    pr_info("After map: cache invalidated\n");
    
    // 模拟设备写入（实际中由硬件完成）
    // 这里只是演示，实际应该等待 DMA 完成
    // buffer[0] = 0x55;  // 设备写入（在内存中）
    
    // 如果此时 CPU 读取（错误示例）
    // new_value = buffer[0];  // 可能读到旧值（如果缓存还有数据）
    
    // 取消映射（刷新并失效）
    dma_unmap_single(dev, dma_addr, len, DMA_FROM_DEVICE);
    pr_info("After unmap: cache cleaned and invalidated\n");
    
    // 现在读取（缓存重新生效）
    new_value = buffer[0];
    pr_info("After unmap, CPU reads: 0x%02x\n", new_value);
    // 此时：
    // 1. CPU 访问 buffer[0]
    // 2. 缓存检查 → 无效（cache miss）
    // 3. 从内存加载 → 得到设备写入的新值（0x55）
    // 4. 数据加载到缓存 → 缓存重新生效
}
```

---

## 9. 总结

### 9.1 缓存失效后的重新生效时机

1. **自动重新加载**：
   - CPU 下次访问该地址时
   - 触发 cache miss
   - 从内存加载数据到缓存
   - 缓存重新生效

2. **关键时机**：
   - **必须在 DMA 完成后**再访问
   - **必须在取消映射后**再读取数据
   - 取消映射时的刷新+失效确保数据一致性

### 9.2 正确的使用模式

```c
// ✅ 正确：等待完成 + 取消映射 + 读取
dma_map_single(..., DMA_FROM_DEVICE);     // 失效缓存
start_dma();
wait_for_complete();                       // 等待设备写完
dma_unmap_single(..., DMA_FROM_DEVICE);   // 刷新+失效，缓存准备重新生效
read_data();                               // 此时访问，缓存重新生效，读到新数据

// ❌ 错误：取消映射前读取
dma_map_single(..., DMA_FROM_DEVICE);
start_dma();
wait_for_complete();
read_data();                               // 错误！缓存可能还有旧数据
dma_unmap_single(..., DMA_FROM_DEVICE);
```

### 9.3 核心原理

- **失效（Invalidate）**: 清除缓存中的旧数据标记
- **刷新（Clean）**: 将脏数据写回内存
- **重新生效**: CPU 访问时自动从内存加载（cache miss）
- **关键**: 取消映射时的刷新+失效确保 CPU 看到最新数据

---

## 10. 参考资料

- [ARM Architecture Reference Manual - Cache Operations](https://developer.arm.com/documentation/ddi0487/latest)
- [Linux DMA API Documentation](https://www.kernel.org/doc/html/latest/core-api/dma-api.html)





