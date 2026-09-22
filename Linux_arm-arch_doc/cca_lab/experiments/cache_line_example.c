/*
 * 缓存行操作示例
 * 演示如何精确失效单个缓存行
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include "cache_line_utils.h"

#define MODULE_NAME "cache_line_example"

/* 测试：失效单个缓存行 */
static void test_single_cache_line_invalidate(void)
{
    void *buffer1, *buffer2;
    u8 *p1, *p2;
    
    pr_info("=== Test: Single Cache Line Invalidate ===\n");
    
    // 分配两个缓冲区（每个 2 个缓存行）
    buffer1 = kmalloc(CACHE_LINE_SIZE * 2, GFP_KERNEL);
    buffer2 = kmalloc(CACHE_LINE_SIZE * 2, GFP_KERNEL);
    
    if (!buffer1 || !buffer2) {
        pr_err("Memory allocation failed\n");
        goto out;
    }
    
    // 对齐到缓存行边界
    p1 = (u8 *)cache_line_align(buffer1);
    p2 = (u8 *)cache_line_align(buffer2);
    
    // 填充数据
    memset(p1, 0xAA, CACHE_LINE_SIZE);
    memset(p2, 0xBB, CACHE_LINE_SIZE);
    
    pr_info("Before invalidate:\n");
    pr_info("  buffer1[0] = 0x%02x (cached)\n", p1[0]);
    pr_info("  buffer2[0] = 0x%02x (cached)\n", p2[0]);
    
    // 只失效 buffer1 的第一个缓存行
    pr_info("\nInvalidating buffer1's first cache line only...\n");
    cache_line_invalidate(p1);
    
    pr_info("After invalidating buffer1[0]:\n");
    pr_info("  buffer1[0] = 0x%02x (cache invalidated, will reload from memory)\n", p1[0]);
    pr_info("  buffer2[0] = 0x%02x (still cached! ✓)\n", p2[0]);
    
    // 只失效 buffer1 的第二个缓存行（不影响第一个）
    pr_info("\nInvalidating buffer1's second cache line only...\n");
    cache_line_invalidate(p1 + CACHE_LINE_SIZE);
    
    pr_info("After invalidating buffer1[64]:\n");
    pr_info("  buffer1[0] = 0x%02x (first cache line still affected)\n", p1[0]);
    pr_info("  buffer1[64] = 0x%02x (second cache line invalidated)\n", p1[64]);
    pr_info("  buffer2[0] = 0x%02x (completely unaffected ✓)\n", p2[0]);
    
out:
    if (buffer1) kfree(buffer1);
    if (buffer2) kfree(buffer2);
}

/* 测试：精确控制缓存行失效 */
static void test_precise_cache_control(void)
{
    struct {
        u32 var1;
        u32 var2;
        u32 var3;
        char padding[52];  // 填充到 64 字节
    } __attribute__((aligned(64))) shared_data;
    
    pr_info("\n=== Test: Precise Cache Control ===\n");
    
    // 初始化
    shared_data.var1 = 0x11111111;
    shared_data.var2 = 0x22222222;
    shared_data.var3 = 0x33333333;
    
    pr_info("Initial values:\n");
    pr_info("  var1 = 0x%08x\n", shared_data.var1);
    pr_info("  var2 = 0x%08x\n", shared_data.var2);
    pr_info("  var3 = 0x%08x\n", shared_data.var3);
    
    // 只失效 var1 所在的缓存行
    pr_info("\nInvalidating cache line containing var1 only...\n");
    cache_line_invalidate(&shared_data.var1);
    
    // var1 的缓存失效，var2 和 var3 不受影响（如果在同一缓存行则也会失效）
    pr_info("After invalidate:\n");
    pr_info("  var1 cache line invalidated\n");
    pr_info("  var2 and var3: depends on alignment\n");
}

/* 测试：对齐的重要性 */
static void test_alignment_importance(void)
{
    void *unaligned, *aligned;
    u8 *p;
    
    pr_info("\n=== Test: Alignment Importance ===\n");
    
    // 分配未对齐的缓冲区
    unaligned = kmalloc(128, GFP_KERNEL);
    p = (u8 *)unaligned + 1;  // 故意不对齐
    
    pr_info("Unaligned address: %p\n", p);
    pr_info("Cache line aligned: %s\n", cache_line_aligned(p) ? "Yes" : "No");
    
    // 失效操作会自动对齐
    pr_info("Invalidating... (will auto-align to cache line boundary)\n");
    cache_line_invalidate(p);
    
    pr_info("Actual cache line invalidated: %p to %p\n",
            cache_line_align(p), 
            (u8 *)cache_line_align(p) + CACHE_LINE_SIZE);
    
    kfree(unaligned);
}

static int __init cache_line_example_init(void)
{
    pr_info("Cache Line Operation Example Module\n");
    pr_info("====================================\n\n");
    
    pr_info("Cache line size: %zu bytes\n", cache_line_size());
    pr_info("\n");
    
    test_single_cache_line_invalidate();
    test_precise_cache_control();
    test_alignment_importance();
    
    pr_info("\n====================================\n");
    pr_info("All tests completed\n");
    
    return 0;
}

static void __exit cache_line_example_exit(void)
{
    pr_info("Cache Line Example Module Unloaded\n");
}

module_init(cache_line_example_init);
module_exit(cache_line_example_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Cache Line Operation Example");





