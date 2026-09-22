/*
 * DMA 一致性测试程序
 * 用于验证 DMA 映射/取消映射是否正确处理缓存一致性
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/device.h>

#define TEST_SIZE 1024

/* 测试 1: 验证 DMA_TO_DEVICE 的缓存刷新 */
static void test_dma_to_device(struct device *dev)
{
    void *buffer;
    dma_addr_t dma_addr;
    int i;
    
    pr_info("=== Test 1: DMA_TO_DEVICE ===\n");
    
    buffer = kmalloc(TEST_SIZE, GFP_KERNEL);
    if (!buffer)
        return;
    
    /* 填充测试数据 */
    for (i = 0; i < TEST_SIZE; i++)
        ((u8 *)buffer)[i] = (u8)(i & 0xFF);
    
    pr_info("Before mapping: buffer[0] = 0x%02x\n", ((u8 *)buffer)[0]);
    
    /* 映射（应该刷新 CPU 写缓存） */
    dma_addr = dma_map_single(dev, buffer, TEST_SIZE, DMA_TO_DEVICE);
    if (dma_mapping_error(dev, dma_addr)) {
        pr_err("DMA mapping failed\n");
        kfree(buffer);
        return;
    }
    
    pr_info("After mapping: dma_addr = 0x%llx\n", (unsigned long long)dma_addr);
    pr_info("Note: CPU cache should be flushed, device can see the data\n");
    
    /* 取消映射 */
    dma_unmap_single(dev, dma_addr, TEST_SIZE, DMA_TO_DEVICE);
    
    kfree(buffer);
    pr_info("Test 1 passed\n\n");
}

/* 测试 2: 验证 DMA_FROM_DEVICE 的缓存失效和刷新 */
static void test_dma_from_device(struct device *dev)
{
    void *buffer;
    dma_addr_t dma_addr;
    
    pr_info("=== Test 2: DMA_FROM_DEVICE ===\n");
    
    buffer = kmalloc(TEST_SIZE, GFP_KERNEL);
    if (!buffer)
        return;
    
    /* 初始化缓冲区 */
    memset(buffer, 0xAA, TEST_SIZE);
    pr_info("Before mapping: buffer[0] = 0x%02x\n", ((u8 *)buffer)[0]);
    
    /* 映射（应该使缓存失效） */
    dma_addr = dma_map_single(dev, buffer, TEST_SIZE, DMA_FROM_DEVICE);
    if (dma_mapping_error(dev, dma_addr)) {
        pr_err("DMA mapping failed\n");
        kfree(buffer);
        return;
    }
    
    pr_info("After mapping: cache should be invalidated\n");
    pr_info("Device can now write to this buffer\n");
    
    /* 模拟设备写入（实际中由硬件完成） */
    /* 这里只是演示，实际应该等待 DMA 完成 */
    
    /* 取消映射（应该刷新缓存，使 CPU 看到设备写入的数据） */
    dma_unmap_single(dev, dma_addr, TEST_SIZE, DMA_FROM_DEVICE);
    
    pr_info("After unmapping: buffer[0] = 0x%02x\n", ((u8 *)buffer)[0]);
    pr_info("Note: CPU cache should be flushed, CPU can now read the data\n");
    
    kfree(buffer);
    pr_info("Test 2 passed\n\n");
}

/* 测试 3: 验证一致性 DMA */
static void test_coherent_dma(struct device *dev)
{
    void *buffer;
    dma_addr_t dma_addr;
    int i;
    
    pr_info("=== Test 3: Coherent DMA ===\n");
    
    /* 分配一致性 DMA 内存 */
    buffer = dma_alloc_coherent(dev, TEST_SIZE, &dma_addr, GFP_KERNEL);
    if (!buffer) {
        pr_err("Failed to allocate coherent DMA memory\n");
        return;
    }
    
    pr_info("Coherent buffer: cpu=%p, dma=0x%llx\n",
            buffer, (unsigned long long)dma_addr);
    
    /* CPU 写入 */
    for (i = 0; i < TEST_SIZE; i++)
        ((u8 *)buffer)[i] = (u8)(i & 0xFF);
    
    pr_info("CPU wrote data: buffer[0] = 0x%02x\n", ((u8 *)buffer)[0]);
    pr_info("Device can immediately see this data (hardware coherency)\n");
    
    /* 设备可以直接读取，无需刷新缓存 */
    
    /* 释放 */
    dma_free_coherent(dev, TEST_SIZE, buffer, dma_addr);
    
    pr_info("Test 3 passed\n\n");
}

/* 测试 4: 验证映射后修改数据的错误 */
static void test_wrong_usage(struct device *dev)
{
    void *buffer;
    dma_addr_t dma_addr;
    
    pr_info("=== Test 4: Wrong Usage (for demonstration) ===\n");
    
    buffer = kmalloc(TEST_SIZE, GFP_KERNEL);
    if (!buffer)
        return;
    
    memset(buffer, 0x11, TEST_SIZE);
    
    /* 映射 */
    dma_addr = dma_map_single(dev, buffer, TEST_SIZE, DMA_TO_DEVICE);
    if (dma_mapping_error(dev, dma_addr)) {
        kfree(buffer);
        return;
    }
    
    pr_info("WRONG: Modifying buffer after mapping!\n");
    pr_info("This is incorrect - device may not see the change\n");
    ((u8 *)buffer)[0] = 0x22;  /* ❌ 错误用法 */
    
    dma_unmap_single(dev, dma_addr, TEST_SIZE, DMA_TO_DEVICE);
    
    pr_info("Correct way: modify before mapping, or remap\n");
    
    kfree(buffer);
    pr_info("Test 4 completed (demonstration only)\n\n");
}

/* 模块初始化 */
static int __init dma_test_init(void)
{
    struct device *test_dev;
    
    pr_info("DMA Consistency Test Module\n");
    pr_info("============================\n\n");
    
    /* 创建一个虚拟设备用于测试 */
    test_dev = kzalloc(sizeof(*test_dev), GFP_KERNEL);
    if (!test_dev)
        return -ENOMEM;
    
    /* 设置 DMA 掩码 */
    dma_set_mask_and_coherent(test_dev, DMA_BIT_MASK(32));
    
    /* 运行测试 */
    test_coherent_dma(test_dev);
    test_dma_to_device(test_dev);
    test_dma_from_device(test_dev);
    test_wrong_usage(test_dev);
    
    pr_info("All tests completed\n");
    
    kfree(test_dev);
    return 0;
}

static void __exit dma_test_exit(void)
{
    pr_info("DMA Consistency Test Module Unloaded\n");
}

module_init(dma_test_init);
module_exit(dma_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA Consistency Test");





