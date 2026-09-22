# DMA API 快速参考

## 一致性保证机制速查表

| 操作 | 一致性 DMA | 流式 DMA (TO_DEVICE) | 流式 DMA (FROM_DEVICE) |
|------|-----------|---------------------|----------------------|
| **分配** | `dma_alloc_coherent()` | `kmalloc()` + `dma_map_single()` | `kmalloc()` + `dma_map_single()` |
| **缓存处理** | 硬件自动 | 映射时刷新写缓存 | 映射时失效缓存，取消映射时刷新 |
| **修改时机** | 随时可修改 | 映射前修改 | 取消映射后读取 |
| **一致性保证** | 硬件保证 | 软件在映射时保证 | 软件在取消映射时保证 |

## 常用模式

### 模式 1: 发送数据到设备

```c
// 1. 准备数据
memcpy(buffer, data, len);

// 2. 映射（刷新 CPU 缓存）
dma_addr = dma_map_single(dev, buffer, len, DMA_TO_DEVICE);
if (dma_mapping_error(dev, dma_addr))
    return -EFAULT;

// 3. 启动 DMA
start_dma(dev, dma_addr);

// 4. 等待完成
wait_for_complete();

// 5. 取消映射
dma_unmap_single(dev, dma_addr, len, DMA_TO_DEVICE);
```

### 模式 2: 从设备接收数据

```c
// 1. 映射（使缓存失效）
dma_addr = dma_map_single(dev, buffer, len, DMA_FROM_DEVICE);
if (dma_mapping_error(dev, dma_addr))
    return -EFAULT;

// 2. 启动 DMA
start_dma(dev, dma_addr);

// 3. 等待完成
wait_for_complete();

// 4. 取消映射（刷新缓存）
dma_unmap_single(dev, dma_addr, len, DMA_FROM_DEVICE);

// 5. 读取数据
memcpy(data, buffer, len);
```

### 模式 3: 长期共享的描述符

```c
// 分配
desc = dma_alloc_coherent(dev, sizeof(*desc), &desc_dma, GFP_KERNEL);

// 随时修改（硬件自动保证一致性）
desc->addr = dma_addr;
desc->len = len;
wmb();  // 内存屏障
start_dma(dev, desc_dma);

// 释放
dma_free_coherent(dev, sizeof(*desc), desc, desc_dma);
```

## 关键检查点

- ✅ 映射前准备数据（TO_DEVICE）
- ✅ 取消映射后读取数据（FROM_DEVICE）
- ✅ 始终检查 `dma_mapping_error()`
- ✅ 匹配映射和取消映射的方向
- ✅ 使用内存屏障（wmb/rmb）

## 常见错误

- ❌ 映射后修改发送缓冲区
- ❌ 取消映射前读取接收缓冲区
- ❌ 忘记检查映射错误
- ❌ 方向不匹配（映射 TO，取消映射 FROM）





