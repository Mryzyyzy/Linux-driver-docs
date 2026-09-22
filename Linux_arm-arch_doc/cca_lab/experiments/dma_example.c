/*
 * DMA API 使用示例
 * 演示一致性 DMA 和流式 DMA 的正确使用方法
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/slab.h>

#define DEVICE_NAME "dma_example"
#define BUFFER_SIZE PAGE_SIZE

struct dma_example_device {
    struct pci_dev *pdev;
    void __iomem *regs;
    
    /* 一致性 DMA: 用于 DMA 描述符 */
    struct dma_descriptor {
        dma_addr_t src_addr;
        dma_addr_t dst_addr;
        u32 length;
        u32 control;
    } *desc;
    dma_addr_t desc_dma;
    
    /* 流式 DMA: 用于数据传输 */
    void *tx_buffer;
    void *rx_buffer;
    dma_addr_t tx_dma;
    dma_addr_t rx_dma;
    
    struct completion tx_complete;
    struct completion rx_complete;
    
    int irq;
};

/* 示例 1: 一致性 DMA 的使用 */
static int setup_coherent_dma(struct dma_example_device *dev)
{
    struct device *device = &dev->pdev->dev;
    
    /* 分配一致性 DMA 内存（描述符） */
    dev->desc = dma_alloc_coherent(device, sizeof(*dev->desc),
                                   &dev->desc_dma, GFP_KERNEL);
    if (!dev->desc) {
        dev_err(device, "Failed to allocate coherent DMA memory\n");
        return -ENOMEM;
    }
    
    /* 初始化描述符（CPU 和 DMA 看到同一份数据） */
    dev->desc->src_addr = 0;
    dev->desc->dst_addr = 0;
    dev->desc->length = 0;
    dev->desc->control = 0;
    
    dev_info(device, "Coherent DMA allocated: cpu=%p, dma=0x%llx\n",
             dev->desc, (unsigned long long)dev->desc_dma);
    
    return 0;
}

/* 示例 2: 流式 DMA 发送（CPU → 设备） */
static int dma_send_data(struct dma_example_device *dev,
                         const void *data, size_t len)
{
    struct device *device = &dev->pdev->dev;
    dma_addr_t dma_addr;
    int err;
    
    if (len > BUFFER_SIZE) {
        dev_err(device, "Data too large: %zu > %d\n", len, BUFFER_SIZE);
        return -EINVAL;
    }
    
    /* 步骤 1: 准备数据（在映射之前） */
    memcpy(dev->tx_buffer, data, len);
    
    /* 步骤 2: 映射到 DMA 地址空间
     * DMA_TO_DEVICE: 自动刷新 CPU 写缓存，确保设备看到最新数据
     */
    dma_addr = dma_map_single(device, dev->tx_buffer, len, DMA_TO_DEVICE);
    if (dma_mapping_error(device, dma_addr)) {
        dev_err(device, "DMA mapping failed\n");
        return -EFAULT;
    }
    
    /* 步骤 3: 配置 DMA 描述符（一致性内存，无需映射） */
    dev->desc->src_addr = dma_addr;
    dev->desc->dst_addr = dev->regs ? (dma_addr_t)dev->regs : 0;
    dev->desc->length = len;
    dev->desc->control = 0x1; /* VALID */
    
    /* 步骤 4: 内存屏障 - 确保描述符写入完成 */
    wmb();
    
    /* 步骤 5: 启动 DMA 传输 */
    if (dev->regs) {
        writel(dev->desc_dma, dev->regs + 0x0); /* DMA_DESC_ADDR */
        writel(0x1, dev->regs + 0x4);          /* DMA_CONTROL */
    }
    
    dev->tx_dma = dma_addr; /* 保存用于后续取消映射 */
    
    dev_info(device, "DMA send started: len=%zu, dma=0x%llx\n",
             len, (unsigned long long)dma_addr);
    
    return 0;
}

/* 示例 3: 流式 DMA 接收（设备 → CPU） */
static int dma_receive_data(struct dma_example_device *dev,
                            void *data, size_t len)
{
    struct device *device = &dev->pdev->dev;
    dma_addr_t dma_addr;
    int err;
    
    if (len > BUFFER_SIZE) {
        dev_err(device, "Buffer too small: %zu > %d\n", len, BUFFER_SIZE);
        return -EINVAL;
    }
    
    /* 步骤 1: 映射接收缓冲区
     * DMA_FROM_DEVICE: 使缓存失效，准备接收新数据
     */
    dma_addr = dma_map_single(device, dev->rx_buffer, len, DMA_FROM_DEVICE);
    if (dma_mapping_error(device, dma_addr)) {
        dev_err(device, "DMA mapping failed\n");
        return -EFAULT;
    }
    
    /* 步骤 2: 配置 DMA 描述符 */
    dev->desc->src_addr = dev->regs ? (dma_addr_t)dev->regs : 0;
    dev->desc->dst_addr = dma_addr;
    dev->desc->length = len;
    dev->desc->control = 0x1; /* VALID */
    
    wmb();
    
    /* 步骤 3: 启动 DMA 传输 */
    if (dev->regs) {
        writel(dev->desc_dma, dev->regs + 0x0);
        writel(0x1, dev->regs + 0x4);
    }
    
    dev->rx_dma = dma_addr;
    
    dev_info(device, "DMA receive started: len=%zu, dma=0x%llx\n",
             len, (unsigned long long)dma_addr);
    
    return 0;
}

/* DMA 传输完成中断处理 */
static irqreturn_t dma_irq_handler(int irq, void *dev_id)
{
    struct dma_example_device *dev = dev_id;
    u32 status = 0;
    
    if (dev->regs) {
        status = readl(dev->regs + 0x8); /* DMA_STATUS */
    }
    
    if (status & 0x1) { /* TX_COMPLETE */
        struct device *device = &dev->pdev->dev;
        
        /* 取消映射（发送完成） */
        if (dev->tx_dma) {
            dma_unmap_single(device, dev->tx_dma, BUFFER_SIZE, DMA_TO_DEVICE);
            dev->tx_dma = 0;
        }
        
        complete(&dev->tx_complete);
    }
    
    if (status & 0x2) { /* RX_COMPLETE */
        struct device *device = &dev->pdev->dev;
        
        /* 取消映射（接收完成，自动刷新缓存） */
        if (dev->rx_dma) {
            dma_unmap_single(device, dev->rx_dma, BUFFER_SIZE, DMA_FROM_DEVICE);
            dev->rx_dma = 0;
        }
        
        /* 读内存屏障 - 确保数据可见 */
        rmb();
        
        complete(&dev->rx_complete);
    }
    
    return IRQ_HANDLED;
}

/* 接收数据完成后的处理 */
static int dma_receive_complete(struct dma_example_device *dev,
                                void *data, size_t len)
{
    /* 等待 DMA 完成 */
    if (wait_for_completion_timeout(&dev->rx_complete, HZ) == 0) {
        dev_err(&dev->pdev->dev, "DMA receive timeout\n");
        return -ETIMEDOUT;
    }
    
    /* 现在可以安全地读取数据（取消映射已刷新缓存） */
    memcpy(data, dev->rx_buffer, len);
    
    return len;
}

/* 清理函数 */
static void cleanup_dma(struct dma_example_device *dev)
{
    struct device *device = &dev->pdev->dev;
    
    /* 释放一致性 DMA */
    if (dev->desc) {
        dma_free_coherent(device, sizeof(*dev->desc),
                         dev->desc, dev->desc_dma);
        dev->desc = NULL;
    }
    
    /* 释放流式 DMA 缓冲区 */
    if (dev->tx_buffer) {
        kfree(dev->tx_buffer);
        dev->tx_buffer = NULL;
    }
    
    if (dev->rx_buffer) {
        kfree(dev->rx_buffer);
        dev->rx_buffer = NULL;
    }
}

/* 初始化函数 */
static int dma_example_probe(struct pci_dev *pdev,
                             const struct pci_device_id *id)
{
    struct dma_example_device *dev;
    struct device *device = &pdev->dev;
    int err;
    
    dev = devm_kzalloc(device, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;
    
    dev->pdev = pdev;
    pci_set_drvdata(pdev, dev);
    
    init_completion(&dev->tx_complete);
    init_completion(&dev->rx_complete);
    
    /* 1. 设置 DMA 掩码 */
    err = dma_set_mask_and_coherent(device, DMA_BIT_MASK(32));
    if (err) {
        dev_err(device, "DMA mask setup failed\n");
        return err;
    }
    
    /* 2. 分配一致性 DMA（描述符） */
    err = setup_coherent_dma(dev);
    if (err)
        return err;
    
    /* 3. 分配流式 DMA 缓冲区 */
    dev->tx_buffer = kzalloc(BUFFER_SIZE, GFP_KERNEL);
    dev->rx_buffer = kzalloc(BUFFER_SIZE, GFP_KERNEL);
    if (!dev->tx_buffer || !dev->rx_buffer) {
        err = -ENOMEM;
        goto err_cleanup;
    }
    
    dev_info(device, "DMA example device initialized\n");
    return 0;
    
err_cleanup:
    cleanup_dma(dev);
    return err;
}

static void dma_example_remove(struct pci_dev *pdev)
{
    struct dma_example_device *dev = pci_get_drvdata(pdev);
    
    cleanup_dma(dev);
    
    dev_info(&pdev->dev, "DMA example device removed\n");
}

static const struct pci_device_id dma_example_id_table[] = {
    { PCI_DEVICE(0x1234, 0x5678) }, /* 替换为你的设备 ID */
    { 0, }
};
MODULE_DEVICE_TABLE(pci, dma_example_id_table);

static struct pci_driver dma_example_driver = {
    .name = DEVICE_NAME,
    .id_table = dma_example_id_table,
    .probe = dma_example_probe,
    .remove = dma_example_remove,
};

module_pci_driver(dma_example_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("DMA API Usage Example");





