/*
 * 缓存行操作工具函数
 * 用于精确控制单个缓存行的失效、刷新等操作
 */

#ifndef _CACHE_LINE_UTILS_H_
#define _CACHE_LINE_UTILS_H_

#include <linux/kernel.h>
#include <linux/cache.h>
#include <asm/cacheflush.h>

/**
 * 获取缓存行大小（字节）
 */
static inline size_t cache_line_size(void)
{
    return L1_CACHE_BYTES;  // 通常是 64 字节
}

/**
 * 将地址对齐到缓存行边界
 */
static inline void *cache_line_align(void *addr)
{
    return (void *)((unsigned long)addr & ~(L1_CACHE_BYTES - 1));
}

/**
 * 检查地址是否对齐到缓存行边界
 */
static inline bool cache_line_aligned(void *addr)
{
    return ((unsigned long)addr & (L1_CACHE_BYTES - 1)) == 0;
}

/**
 * 失效单个缓存行
 * @addr: 任意地址（会自动对齐到缓存行边界）
 * 
 * 注意：只失效 addr 所在的那个缓存行（64 字节）
 */
static inline void cache_line_invalidate(void *addr)
{
    void *aligned = cache_line_align(addr);
    
    __dma_inv_range(aligned, aligned + L1_CACHE_BYTES);
    dsb(sy);  // 数据同步屏障，确保操作完成
}

/**
 * 刷新单个缓存行（将脏数据写回内存）
 * @addr: 任意地址（会自动对齐）
 */
static inline void cache_line_clean(void *addr)
{
    void *aligned = cache_line_align(addr);
    
    __dma_clean_range(aligned, aligned + L1_CACHE_BYTES);
    dsb(sy);
}

/**
 * 刷新并失效单个缓存行
 * @addr: 任意地址（会自动对齐）
 */
static inline void cache_line_flush(void *addr)
{
    void *aligned = cache_line_align(addr);
    
    __dma_flush_range(aligned, aligned + L1_CACHE_BYTES);
    dsb(sy);
}

/**
 * 失效多个缓存行（精确控制）
 * @start: 起始地址
 * @size: 大小（字节）
 */
static inline void cache_range_invalidate(void *start, size_t size)
{
    void *aligned_start = cache_line_align(start);
    void *aligned_end = (void *)(((unsigned long)start + size + L1_CACHE_BYTES - 1) 
                                 & ~(L1_CACHE_BYTES - 1));
    
    __dma_inv_range(aligned_start, aligned_end);
    dsb(sy);
}

/**
 * 刷新多个缓存行
 */
static inline void cache_range_clean(void *start, size_t size)
{
    void *aligned_start = cache_line_align(start);
    void *aligned_end = (void *)(((unsigned long)start + size + L1_CACHE_BYTES - 1) 
                                 & ~(L1_CACHE_BYTES - 1));
    
    __dma_clean_range(aligned_start, aligned_end);
    dsb(sy);
}

/**
 * 刷新并失效多个缓存行
 */
static inline void cache_range_flush(void *start, size_t size)
{
    void *aligned_start = cache_line_align(start);
    void *aligned_end = (void *)(((unsigned long)start + size + L1_CACHE_BYTES - 1) 
                                 & ~(L1_CACHE_BYTES - 1));
    
    __dma_flush_range(aligned_start, aligned_end);
    dsb(sy);
}

#endif /* _CACHE_LINE_UTILS_H_ */





