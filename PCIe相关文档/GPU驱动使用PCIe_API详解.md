# GPU 驱动如何使用 PCIe API

## 一、架构关系

### 1.1 驱动分层

```
┌─────────────────────────────────────────┐
│      GPU 驱动 (设备特定驱动)             │
│  例如: i915 (Intel), amdgpu, nouveau    │
│  - 实现 GPU 特定功能                     │
│  - 通过 PCIe API 访问硬件                │
└─────────────────────────────────────────┘
              ↓ 使用
┌─────────────────────────────────────────┐
│      PCIe 核心子系统 (PCI Core)        │
│  - 设备枚举和发现                        │
│  - 资源管理 (BAR、IRQ、DMA)              │
│  - 提供标准 API                          │
└─────────────────────────────────────────┘
              ↓ 管理
┌─────────────────────────────────────────┐
│      PCIe 硬件                          │
│  - PCIe Root Complex                   │
│  - GPU 设备                             │
└─────────────────────────────────────────┘
```

**关键点：**
- **PCIe 驱动**：内核核心子系统，负责 PCIe 总线管理
- **GPU 驱动**：设备驱动，通过 PCIe API 访问 GPU 硬件
- **两者分离**：GPU 驱动不直接操作 PCIe 硬件，而是调用 PCIe 子系统提供的 API

## 二、GPU 驱动使用的主要 PCIe API

### 2.1 驱动注册

#### 2.1.1 定义设备 ID 表

```c
// drivers/gpu/drm/mgag200/mgag200_drv.c
static const struct pci_device_id mgag200_pciidlist[] = {
    { PCI_VENDOR_ID_MATROX, 0x520, PCI_ANY_ID, PCI_ANY_ID, 0, 0, G200_PCI },
    { PCI_VENDOR_ID_MATROX, 0x521, PCI_ANY_ID, PCI_ANY_ID, 0, 0, G200_AGP },
    { 0, }  // 结束标记
};

MODULE_DEVICE_TABLE(pci, mgag200_pciidlist);
```

#### 2.1.2 定义 PCI 驱动结构

```c
static struct pci_driver mgag200_pci_driver = {
    .name = "mgag200",
    .id_table = mgag200_pciidlist,  // 设备 ID 表
    .probe = mgag200_pci_probe,     // 设备发现时调用
    .remove = mgag200_pci_remove,   // 设备移除时调用
    .shutdown = mgag200_pci_shutdown, // 系统关闭时调用
};
```

#### 2.1.3 注册驱动

```c
// 使用宏自动注册
drm_module_pci_driver_if_modeset(mgag200_pci_driver, mgag200_modeset);

// 或者手动注册
module_pci_driver(mgag200_pci_driver);
```

### 2.2 设备初始化（Probe 函数）

#### 2.2.1 启用 PCIe 设备

```c
static int gpu_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    int ret;
    
    // 1. 启用 PCIe 设备
    // 这会：
    //    - 启用设备的 I/O 和内存空间
    //    - 分配 IRQ
    //    - 设置设备为 D0 状态
    ret = pci_enable_device(pdev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to enable PCI device\n");
        return ret;
    }
    
    // 或者使用 pcim_enable_device（自动管理资源）
    ret = pcim_enable_device(pdev);
    if (ret)
        return ret;
}
```

#### 2.2.2 请求资源（BAR 空间）

```c
    // 2. 请求 PCIe 资源（BAR 空间）
    // 这会标记 BAR 为"已使用"，防止其他驱动冲突
    ret = pci_request_regions(pdev, "gpu_driver");
    if (ret) {
        dev_err(&pdev->dev, "Failed to request regions\n");
        goto err_disable;
    }
```

#### 2.2.3 设置 DMA 掩码

```c
    // 3. 设置 DMA 掩码（支持 64 位 DMA）
    ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
    if (ret) {
        // 如果不支持 64 位，尝试 32 位
        ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
        if (ret) {
            dev_err(&pdev->dev, "Failed to set DMA mask\n");
            goto err_release;
        }
    }
    pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
```

#### 2.2.4 映射 BAR（寄存器空间）

```c
    // 4. 映射 BAR 0（GPU 寄存器空间）
    // 方法 1：使用 pci_ioremap_bar（推荐）
    gpu->regs = pci_ioremap_bar(pdev, 0);
    if (!gpu->regs) {
        dev_err(&pdev->dev, "Failed to map BAR0\n");
        ret = -ENOMEM;
        goto err_release;
    }
    
    // 方法 2：手动映射
    resource_size_t start = pci_resource_start(pdev, 0);
    resource_size_t len = pci_resource_len(pdev, 0);
    gpu->regs = ioremap(start, len);
    
    // 方法 3：映射为 write-combining（用于帧缓冲区）
    gpu->fb = ioremap_wc(pci_resource_start(pdev, 1), 
                         pci_resource_len(pdev, 1));
```

#### 2.2.5 分配中断

```c
    // 5. 分配 MSI 中断向量
    ret = pci_alloc_irq_vectors(pdev, 1, 4, 
                                  PCI_IRQ_MSIX | PCI_IRQ_MSI);
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to allocate IRQ vectors\n");
        goto err_unmap;
    }
    
    // 6. 获取 Linux IRQ 号
    irq = pci_irq_vector(pdev, 0);
    if (irq < 0) {
        dev_err(&pdev->dev, "Failed to get IRQ\n");
        goto err_free_vectors;
    }
    
    // 7. 注册中断处理函数
    ret = request_threaded_irq(irq, gpu_irq_handler, gpu_irq_thread,
                                IRQF_ONESHOT, "gpu", gpu);
    if (ret) {
        dev_err(&pdev->dev, "Failed to request IRQ\n");
        goto err_free_vectors;
    }
```

### 2.3 配置空间访问

#### 2.3.1 读取配置寄存器

```c
    // 读取设备 ID
    u16 vendor = pdev->vendor;
    u16 device = pdev->device;
    
    // 读取配置空间寄存器
    u8 revision;
    pci_read_config_byte(pdev, PCI_REVISION_ID, &revision);
    
    // 读取 PCIe 能力寄存器
    u16 pcie_cap = pci_find_capability(pdev, PCI_CAP_ID_EXP);
    if (pcie_cap) {
        u16 pcie_status;
        pci_read_config_word(pdev, pcie_cap + PCI_EXP_LNKSTA, &pcie_status);
    }
```

#### 2.3.2 写入配置寄存器

```c
    // 写入配置寄存器
    pci_write_config_byte(pdev, PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    
    // 配置 MSI
    pci_write_config_dword(pdev, pos + PCI_MSI_ADDRESS_LO, msg->address_lo);
    pci_write_config_word(pdev, pos + PCI_MSI_DATA_64, msg->data);
```

### 2.4 资源管理

#### 2.4.1 获取资源信息

```c
    // 获取 BAR 的物理地址和大小
    resource_size_t bar0_start = pci_resource_start(pdev, 0);
    resource_size_t bar0_len = pci_resource_len(pdev, 0);
    
    // 检查资源类型
    unsigned long flags = pci_resource_flags(pdev, 0);
    if (flags & IORESOURCE_MEM) {
        // 内存资源
    }
    if (flags & IORESOURCE_IO) {
        // I/O 资源
    }
    if (flags & IORESOURCE_PREFETCH) {
        // 可预取的内存
    }
```

#### 2.4.2 释放资源

```c
static void gpu_pci_remove(struct pci_dev *pdev)
{
    struct gpu_device *gpu = pci_get_drvdata(pdev);
    
    // 1. 释放中断
    free_irq(pci_irq_vector(pdev, 0), gpu);
    pci_free_irq_vectors(pdev);
    
    // 2. 取消映射 BAR
    iounmap(gpu->regs);
    iounmap(gpu->fb);
    
    // 3. 释放资源
    pci_release_regions(pdev);
    
    // 4. 禁用设备
    pci_disable_device(pdev);
}
```

### 2.5 电源管理

#### 2.5.1 运行时电源管理

```c
    // 启用运行时电源管理
    pm_runtime_enable(&pdev->dev);
    
    // 请求电源（设备需要工作时）
    pm_runtime_get_sync(&pdev->dev);
    
    // 释放电源（设备不需要工作时）
    pm_runtime_put_sync(&pdev->dev);
```

#### 2.5.2 系统级电源管理

```c
static int gpu_pci_suspend(struct pci_dev *pdev, pm_message_t state)
{
    struct gpu_device *gpu = pci_get_drvdata(pdev);
    
    // 1. 保存设备状态
    gpu_save_state(gpu);
    
    // 2. 禁用中断
    pci_disable_msi(pdev);
    
    // 3. 将设备设置为低功耗状态
    pci_set_power_state(pdev, PCI_D3hot);
    
    return 0;
}

static int gpu_pci_resume(struct pci_dev *pdev)
{
    struct gpu_device *gpu = pci_get_drvdata(pdev);
    
    // 1. 恢复电源状态
    pci_set_power_state(pdev, PCI_D0);
    pci_restore_state(pdev);
    
    // 2. 重新启用中断
    pci_enable_msi(pdev);
    
    // 3. 恢复设备状态
    gpu_restore_state(gpu);
    
    return 0;
}

static struct pci_driver gpu_pci_driver = {
    // ...
    .suspend = gpu_pci_suspend,
    .resume = gpu_pci_resume,
};
```

## 三、完整的 GPU 驱动示例

```c
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>

struct gpu_device {
    struct pci_dev *pdev;
    void __iomem *regs;      // GPU 寄存器空间
    void __iomem *fb;        // 帧缓冲区
    int irq;                 // 中断号
    // ... 其他 GPU 特定数据
};

// 设备 ID 表
static const struct pci_device_id gpu_pci_tbl[] = {
    { PCI_DEVICE(0x10de, 0x1b80) },  // NVIDIA GPU
    { PCI_DEVICE(0x1002, 0x67df) },  // AMD GPU
    { 0, }
};
MODULE_DEVICE_TABLE(pci, gpu_pci_tbl);

// 中断处理函数
static irqreturn_t gpu_irq_handler(int irq, void *dev_id)
{
    struct gpu_device *gpu = dev_id;
    u32 status;
    
    // 读取中断状态
    status = readl(gpu->regs + GPU_INT_STATUS);
    
    // 处理中断
    if (status & GPU_INT_DMA_COMPLETE) {
        // DMA 完成处理
    }
    
    // 清除中断标志
    writel(status, gpu->regs + GPU_INT_CLEAR);
    
    return IRQ_HANDLED;
}

// Probe 函数
static int gpu_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    struct gpu_device *gpu;
    int ret, irq;
    
    // 1. 分配设备结构
    gpu = devm_kzalloc(&pdev->dev, sizeof(*gpu), GFP_KERNEL);
    if (!gpu)
        return -ENOMEM;
    gpu->pdev = pdev;
    pci_set_drvdata(pdev, gpu);
    
    // 2. 启用设备
    ret = pcim_enable_device(pdev);
    if (ret)
        return ret;
    
    // 3. 请求资源
    ret = pci_request_regions(pdev, "gpu_driver");
    if (ret)
        return ret;
    
    // 4. 设置 DMA 掩码
    ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
    if (ret) {
        ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
        if (ret)
            return ret;
    }
    pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
    
    // 5. 映射 BAR
    gpu->regs = pcim_iomap(pdev, 0, 0);
    if (!gpu->regs)
        return -ENOMEM;
    
    gpu->fb = pcim_iomap(pdev, 1, 0);
    if (!gpu->fb)
        return -ENOMEM;
    
    // 6. 分配中断
    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI);
    if (ret < 0)
        return ret;
    
    irq = pci_irq_vector(pdev, 0);
    ret = devm_request_irq(&pdev->dev, irq, gpu_irq_handler,
                           IRQF_ONESHOT, "gpu", gpu);
    if (ret)
        goto err_free_vectors;
    gpu->irq = irq;
    
    // 7. 初始化 GPU 硬件
    gpu_init_hw(gpu);
    
    // 8. 启用运行时电源管理
    pm_runtime_enable(&pdev->dev);
    
    dev_info(&pdev->dev, "GPU initialized\n");
    return 0;
    
err_free_vectors:
    pci_free_irq_vectors(pdev);
    return ret;
}

// Remove 函数
static void gpu_pci_remove(struct pci_dev *pdev)
{
    struct gpu_device *gpu = pci_get_drvdata(pdev);
    
    pm_runtime_disable(&pdev->dev);
    gpu_cleanup_hw(gpu);
    pci_free_irq_vectors(pdev);
    // pcim_iomap 会自动释放，不需要手动 iounmap
}

// PCI 驱动结构
static struct pci_driver gpu_pci_driver = {
    .name = "gpu_driver",
    .id_table = gpu_pci_tbl,
    .probe = gpu_pci_probe,
    .remove = gpu_pci_remove,
};

// 注册驱动
module_pci_driver(gpu_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("GPU PCIe Driver");
```

## 四、常用的 PCIe API 总结

### 4.1 设备管理

| API | 功能 |
|-----|------|
| `pci_enable_device()` | 启用 PCIe 设备 |
| `pcim_enable_device()` | 启用设备（自动管理资源） |
| `pci_disable_device()` | 禁用设备 |
| `pci_set_drvdata()` | 保存驱动私有数据 |
| `pci_get_drvdata()` | 获取驱动私有数据 |

### 4.2 资源管理

| API | 功能 |
|-----|------|
| `pci_request_regions()` | 请求 BAR 资源 |
| `pci_release_regions()` | 释放 BAR 资源 |
| `pci_resource_start()` | 获取 BAR 起始地址 |
| `pci_resource_len()` | 获取 BAR 大小 |
| `pci_resource_flags()` | 获取 BAR 标志 |
| `pci_ioremap_bar()` | 映射 BAR 到虚拟地址 |
| `pcim_iomap()` | 映射 BAR（自动管理） |

### 4.3 中断管理

| API | 功能 |
|-----|------|
| `pci_alloc_irq_vectors()` | 分配 MSI/MSI-X 向量 |
| `pci_free_irq_vectors()` | 释放中断向量 |
| `pci_irq_vector()` | 获取 Linux IRQ 号 |
| `pci_enable_msi()` | 启用 MSI（旧 API） |
| `pci_disable_msi()` | 禁用 MSI |

### 4.4 DMA 管理

| API | 功能 |
|-----|------|
| `pci_set_dma_mask()` | 设置 DMA 掩码 |
| `pci_set_consistent_dma_mask()` | 设置一致性 DMA 掩码 |
| `pci_dma_supported()` | 检查 DMA 支持 |

### 4.5 配置空间访问

| API | 功能 |
|-----|------|
| `pci_read_config_byte()` | 读取 8 位配置寄存器 |
| `pci_read_config_word()` | 读取 16 位配置寄存器 |
| `pci_read_config_dword()` | 读取 32 位配置寄存器 |
| `pci_write_config_byte()` | 写入 8 位配置寄存器 |
| `pci_write_config_word()` | 写入 16 位配置寄存器 |
| `pci_write_config_dword()` | 写入 32 位配置寄存器 |
| `pci_find_capability()` | 查找 PCIe 能力 |

### 4.6 电源管理

| API | 功能 |
|-----|------|
| `pci_set_power_state()` | 设置电源状态 |
| `pci_save_state()` | 保存设备状态 |
| `pci_restore_state()` | 恢复设备状态 |
| `pm_runtime_enable()` | 启用运行时电源管理 |
| `pm_runtime_get_sync()` | 请求电源 |
| `pm_runtime_put_sync()` | 释放电源 |

## 五、关键要点

### 5.1 GPU 驱动和 PCIe 驱动的关系

1. **分离设计**：
   - PCIe 驱动是内核核心子系统，负责 PCIe 总线管理
   - GPU 驱动是设备驱动，通过 PCIe API 访问硬件
   - GPU 驱动不直接操作 PCIe 硬件

2. **标准接口**：
   - GPU 驱动使用标准的 PCIe API
   - 不依赖特定的 PCIe 控制器实现
   - 可以在不同的 PCIe 控制器上工作

3. **资源管理**：
   - PCIe 子系统负责资源分配（BAR、IRQ）
   - GPU 驱动请求和使用这些资源
   - 资源释放由 PCIe 子系统或驱动管理

### 5.2 最佳实践

1. **使用 `pcim_*` 函数**：
   - `pcim_enable_device()` 而不是 `pci_enable_device()`
   - `pcim_iomap()` 而不是 `pci_ioremap_bar()`
   - 这些函数自动管理资源，减少错误

2. **错误处理**：
   - 每个 API 调用都要检查返回值
   - 使用 `goto` 标签进行清理
   - 按照相反顺序释放资源

3. **现代 API**：
   - 使用 `pci_alloc_irq_vectors()` 而不是 `pci_enable_msi()`
   - 使用 `pci_irq_vector()` 获取 IRQ 号
   - 支持 MSI-X 和 MSI

## 六、总结

**GPU 驱动使用 PCIe 的方式：**

1. **注册驱动**：定义 `pci_driver` 结构，注册到 PCIe 子系统
2. **设备发现**：PCIe 子系统调用 `probe()` 函数
3. **资源获取**：通过 PCIe API 获取 BAR、IRQ 等资源
4. **硬件访问**：映射 BAR 后，直接访问 GPU 寄存器
5. **中断处理**：通过 PCIe API 分配和注册中断
6. **电源管理**：使用 PCIe 和 PM Runtime API 管理电源

**关键点：**
- GPU 驱动**不直接操作 PCIe 硬件**
- GPU 驱动通过**标准 PCIe API** 访问设备
- PCIe 子系统负责**底层硬件管理**
- GPU 驱动专注于**GPU 特定功能**

