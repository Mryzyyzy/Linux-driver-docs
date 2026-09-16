# 如何只失效单个缓存行

## 1. ARM 平台的缓存行操作指令

### 1.1 ARM64 缓存操作指令

ARM64 提供了精确的缓存操作指令，可以针对单个缓存行进行操作：

```assembly
; 使单个缓存行失效（Invalidate）
dc ivac, x0    ; Invalidate by Virtual Address to Point of Coherency
               ; x0 = 虚拟地址
               ; 只失效 x0 所在的那个缓存行（64 字节）

; 刷新单个缓存行（Clean）
dc cvac, x0    ; Clean by Virtual Address to PoC
               ; 将脏数据写回内存，但缓存行仍然有效

; 刷新并失效（Clean and Invalidate）
dc civac, x0   ; Clean and Invalidate by Virtual Address to PoC
               ; 先刷新，再失效
```

### 1.2 ARM 32位缓存操作指令

```assembly
; ARMv7/v8 32位
mcr p15, 0, r0, c7, c6, 1    ; 使缓存行失效（Invalidate）
                              ; r0 = 虚拟地址

mcr p15, 0, r0, c7, c10, 1   ; 刷新缓存行（Clean）
                              ; r0 = 虚拟地址

mcr p15, 0, r0, c7, c14, 1   ; 刷新并失效（Clean and Invalidate）
                              ; r0 = 虚拟地址
```

---

## 2. Linux 内核提供的接口

### 2.1 内联汇编封装

Linux 内核提供了内联汇编封装，可以在驱动中使用：

```c
#include <asm/cacheflush.h>

// ARM64
void __dma_inv_range(const void *start, const void *end);
void __dma_clean_range(const void *start, const void *end);
void __dma_flush_range(const void *start, const void *end);

// ARM 32位
void dmac_inv_range(const void *start, const void *end);
void dmac_clean_range(const void *start, const void *end);
void dmac_flush_range(const void *start, const void *end);
```

### 2.2 单个缓存行操作

虽然内核主要提供范围操作，但底层实现是逐个缓存行处理的：

```c
// arch/arm64/mm/cache.S

ENTRY(__dma_inv_range)
    add     x1, x0, x1          // x1 = 结束地址
    bic     x0, x0, #63         // 对齐到缓存行边界（64 字节）
    
loop:
    dc      ivac, x0            // 失效当前缓存行
    add     x0, x0, #64         // 移动到下一个缓存行
    cmp     x0, x1
    b.lo    loop
    
    dsb     sy                  // 数据同步屏障
    ret
END(__dma_inv_range)
```

**关键**：每次 `dc ivac, x0` 只失效一个缓存行（64 字节）。

---

## 3. 在驱动中失效单个缓存行

### 3.1 方法 1: 使用内核提供的接口（推荐）

```c
#include <linux/cacheflush.h>
#include <asm/cacheflush.h>

void invalidate_single_cache_line(void *addr)
{
    // 对齐到缓存行边界
    void *aligned_addr = (void *)((unsigned long)addr & ~(L1_CACHE_BYTES - 1));
    
    // 失效单个缓存行（实际上失效 aligned_addr 到 aligned_addr + 64 的范围）
    __dma_inv_range(aligned_addr, aligned_addr + L1_CACHE_BYTES);
    
    // 数据同步屏障，确保操作完成
    dsb(sy);
}
```

### 3.2 方法 2: 直接使用内联汇编

```c
#include <asm/cacheflush.h>

void invalidate_cache_line_arm64(void *addr)
{
    void *aligned = (void *)((unsigned long)addr & ~63UL);
    
    asm volatile(
        "dc ivac, %0\n\t"      // 使缓存行失效
        "dsb sy\n\t"           // 数据同步屏障
        :
        : "r" (aligned)
        : "memory"
    );
}

void invalidate_cache_line_arm32(void *addr)
{
    unsigned long aligned = (unsigned long)addr & ~63UL;
    
    asm volatile(
        "mcr p15, 0, %0, c7, c6, 1\n\t"  // 使缓存行失效
        "dsb\n\t"                        // 数据同步屏障
        :
        : "r" (aligned)
        : "memory"
    );
}
```

### 3.3 方法 3: 使用 DMA API（间接方式）

```c
// 虽然 DMA API 会失效整个范围，但底层是逐个缓存行处理的
void *buffer = kmalloc(64, GFP_KERNEL);  // 只分配一个缓存行大小

dma_map_single(dev, buffer, 64, DMA_FROM_DEVICE);
// 内部会失效 buffer 所在的那个缓存行
```

---

## 4. 实际使用示例

### 4.1 示例：失效单个变量的缓存行

```c
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/cacheflush.h>

struct shared_data {
    volatile u32 value;
    char padding[60];  // 填充到 64 字节（一个缓存行）
} __attribute__((aligned(64)));  // 对齐到缓存行边界

static void invalidate_shared_data_cache(struct shared_data *data)
{
    // 失效 data 所在的缓存行
    __dma_inv_range(data, data + sizeof(*data));
    dsb(sy);
    
    pr_info("Invalidated cache line for data at %p\n", data);
}

// 使用示例
static int example_probe(struct platform_device *pdev)
{
    struct shared_data *data;
    
    // 分配对齐到缓存行边界
    data = kzalloc(sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;
    
    // 确保对齐
    data = (struct shared_data *)ALIGN((unsigned long)data, 64);
    
    // 失效缓存行（例如，设备可能修改了这个内存）
    invalidate_shared_data_cache(data);
    
    // 现在可以安全读取（会从内存加载最新数据）
    pr_info("Value: %u\n", data->value);
    
    return 0;
}
```

### 4.2 示例：精确控制缓存行失效

```c
#include <linux/cache.h>

#define CACHE_LINE_SIZE  L1_CACHE_BYTES  // 通常是 64

void precise_cache_invalidate(void *addr, size_t size)
{
    void *start = (void *)((unsigned long)addr & ~(CACHE_LINE_SIZE - 1));
    void *end = (void *)(((unsigned long)addr + size + CACHE_LINE_SIZE - 1) 
                         & ~(CACHE_LINE_SIZE - 1));
    
    // 逐个失效缓存行
    for (void *p = start; p < end; p += CACHE_LINE_SIZE) {
        __dma_inv_range(p, p + CACHE_LINE_SIZE);
    }
    
    dsb(sy);  // 确保所有操作完成
}
```

---

## 5. 缓存行对齐的重要性

### 5.1 为什么需要对齐？

```c
// ❌ 未对齐的情况
void *addr = (void *)0x80001001;  // 未对齐到 64 字节边界

// 失效操作会失效包含 0x80001000-0x8000103F 的缓存行
// 可能影响不相关的数据

// ✅ 对齐的情况
void *addr = (void *)0x80001000;  // 对齐到 64 字节边界

// 失效操作只影响 0x80001000-0x8000103F
// 精确控制
```

### 5.2 对齐方法

```c
// 方法 1: 编译时对齐
struct data {
    u32 value;
} __attribute__((aligned(64)));  // 对齐到 64 字节

// 方法 2: 运行时对齐
void *buffer = kmalloc(size + 63, GFP_KERNEL);
buffer = (void *)ALIGN((unsigned long)buffer, 64);

// 方法 3: 使用 kmem_cache（对齐的分配器）
struct kmem_cache *cache = kmem_cache_create(
    "my_cache", 
    sizeof(struct data), 
    64,  // 对齐到 64 字节
    0, 
    NULL
);
void *data = kmem_cache_alloc(cache, GFP_KERNEL);
```

---

## 6. 完整示例：精确的缓存行操作

```c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <asm/cacheflush.h>

#define CACHE_LINE_SIZE  64

/* 失效单个缓存行 */
static inline void invalidate_one_cache_line(void *addr)
{
    void *aligned = (void *)((unsigned long)addr & ~(CACHE_LINE_SIZE - 1));
    
    __dma_inv_range(aligned, aligned + CACHE_LINE_SIZE);
    dsb(sy);
}

/* 刷新单个缓存行 */
static inline void clean_one_cache_line(void *addr)
{
    void *aligned = (void *)((unsigned long)addr & ~(CACHE_LINE_SIZE - 1));
    
    __dma_clean_range(aligned, aligned + CACHE_LINE_SIZE);
    dsb(sy);
}

/* 刷新并失效单个缓存行 */
static inline void clean_invalidate_one_cache_line(void *addr)
{
    void *aligned = (void *)((unsigned long)addr & ~(CACHE_LINE_SIZE - 1));
    
    __dma_flush_range(aligned, aligned + CACHE_LINE_SIZE);
    dsb(sy);
}

/* 测试代码 */
static int __init cache_line_test_init(void)
{
    void *buffer1, *buffer2;
    
    // 分配两个缓存行大小的缓冲区
    buffer1 = kmalloc(CACHE_LINE_SIZE * 2, GFP_KERNEL);
    buffer2 = kmalloc(CACHE_LINE_SIZE * 2, GFP_KERNEL);
    
    // 对齐
    buffer1 = (void *)ALIGN((unsigned long)buffer1, CACHE_LINE_SIZE);
    buffer2 = (void *)ALIGN((unsigned long)buffer2, CACHE_LINE_SIZE);
    
    // 填充数据
    memset(buffer1, 0xAA, CACHE_LINE_SIZE);
    memset(buffer2, 0xBB, CACHE_LINE_SIZE);
    
    pr_info("Before invalidate:\n");
    pr_info("  buffer1[0] = 0x%02x\n", ((u8 *)buffer1)[0]);
    pr_info("  buffer2[0] = 0x%02x\n", ((u8 *)buffer2)[0]);
    
    // 只失效 buffer1 的第一个缓存行
    invalidate_one_cache_line(buffer1);
    
    pr_info("After invalidating buffer1's first cache line:\n");
    pr_info("  buffer1[0] = 0x%02x (cache invalidated)\n", ((u8 *)buffer1)[0]);
    pr_info("  buffer2[0] = 0x%02x (still cached)\n", ((u8 *)buffer2)[0]);
    
    // 只失效 buffer1 的第二个缓存行
    invalidate_one_cache_line(buffer1 + CACHE_LINE_SIZE);
    
    kfree(buffer1);
    kfree(buffer2);
    
    return 0;
}

module_init(cache_line_test_init);
MODULE_LICENSE("GPL");
```

---

## 7. ARM 平台特定实现

### 7.1 ARM64 实现

```c
// arch/arm64/include/asm/cacheflush.h

static inline void __inv_dcache_by_line(unsigned long start, unsigned long end)
{
    unsigned long cur = start & ~(CACHE_LINE_SIZE - 1);
    
    do {
        asm volatile("dc ivac, %0" : : "r" (cur) : "memory");
        cur += CACHE_LINE_SIZE;
    } while (cur < end);
    
    dsb(sy);
}
```

### 7.2 ARM 32位实现

```c
// arch/arm/include/asm/cacheflush.h

static inline void __inv_dcache_by_line(unsigned long start, unsigned long end)
{
    unsigned long cur = start & ~(CACHE_LINE_SIZE - 1);
    
    do {
        asm volatile("mcr p15, 0, %0, c7, c6, 1" : : "r" (cur) : "memory");
        cur += CACHE_LINE_SIZE;
    } while (cur < end);
    
    dsb();
}
```

---

## 8. 使用场景

### 8.1 场景 1: 共享内存同步

```c
// CPU 和 DMA 共享的数据结构
struct shared_desc {
    volatile u32 status;
    volatile u32 data;
    char padding[56];  // 填充到 64 字节
} __attribute__((aligned(64)));

// DMA 完成后，失效缓存行，确保 CPU 看到最新状态
void dma_complete_handler(struct shared_desc *desc)
{
    // 只失效这个描述符的缓存行
    invalidate_one_cache_line(desc);
    
    // 现在可以安全读取
    if (desc->status == DMA_COMPLETE) {
        process_data(desc->data);
    }
}
```

### 8.2 场景 2: 精确的缓存控制

```c
// 只失效特定变量的缓存行，不影响其他数据
void update_shared_variable(volatile u32 *var)
{
    // 设备可能修改了这个变量
    // 只失效这个变量所在的缓存行
    invalidate_one_cache_line(var);
    
    // 读取最新值
    u32 value = *var;
}
```

---

## 9. 性能考虑

### 9.1 单个缓存行失效的开销

```c
// 单个缓存行失效的典型开销（ARM Cortex-A57）
// - dc ivac 指令: ~10-20 个时钟周期
// - dsb sy 屏障: ~10-50 个时钟周期（取决于系统负载）
// 总计: ~20-70 个时钟周期

// 对比：失效 1KB 缓冲区（16 个缓存行）
// - 16 * 20-70 = 320-1120 个时钟周期
```

### 9.2 优化建议

```c
// ✅ 好的做法：只失效需要的缓存行
invalidate_one_cache_line(needed_var);

// ❌ 不好的做法：失效整个缓冲区
dma_map_single(dev, large_buffer, 1MB, DMA_FROM_DEVICE);
// 失效 16,384 个缓存行，开销很大
```

---

## 10. 总结

### 10.1 失效单个缓存行的方法

1. **使用内核接口**（推荐）:
   ```c
   __dma_inv_range(addr, addr + 64);
   ```

2. **直接内联汇编**:
   ```c
   asm volatile("dc ivac, %0" : : "r" (aligned_addr));
   ```

3. **对齐很重要**:
   ```c
   addr = (void *)((unsigned long)addr & ~63UL);
   ```

### 10.2 关键点

- ✅ 每次 `dc ivac` 只失效一个缓存行（64 字节）
- ✅ 必须对齐到缓存行边界
- ✅ 需要数据同步屏障（dsb）确保操作完成
- ✅ 只影响指定的缓存行，其他缓存不受影响

---

## 11. 完整工具函数

```c
#include <linux/cache.h>
#include <asm/cacheflush.h>

/**
 * 失效单个缓存行
 * @addr: 任意地址（会自动对齐到缓存行边界）
 */
static inline void cache_line_invalidate(void *addr)
{
    void *aligned = (void *)((unsigned long)addr & ~(L1_CACHE_BYTES - 1));
    __dma_inv_range(aligned, aligned + L1_CACHE_BYTES);
    dsb(sy);
}

/**
 * 刷新单个缓存行
 */
static inline void cache_line_clean(void *addr)
{
    void *aligned = (void *)((unsigned long)addr & ~(L1_CACHE_BYTES - 1));
    __dma_clean_range(aligned, aligned + L1_CACHE_BYTES);
    dsb(sy);
}

/**
 * 刷新并失效单个缓存行
 */
static inline void cache_line_flush(void *addr)
{
    void *aligned = (void *)((unsigned long)addr & ~(L1_CACHE_BYTES - 1));
    __dma_flush_range(aligned, aligned + L1_CACHE_BYTES);
    dsb(sy);
}
```

---

## 参考资料

- [ARM Architecture Reference Manual - Cache Operations](https://developer.arm.com/documentation/ddi0487/latest)
- [Linux Cache Management API](https://www.kernel.org/doc/html/latest/core-api/cachetlb.html)





