# PCIe 相关文档目录

本目录包含所有与 PCIe 相关的技术文档，已全部使用中文命名。

## 文档列表

### 基础概念
- **PCIe概述.md** - PCIe 协议基础概述
- **PCIe根复合体详解.md** - Root Complex 详细说明
- **PCIe_NOC术语详解.md** - PCIe 与 NoC 相关术语

### 硬件机制
- **PCIe链路训练详解.md** - PCIe 链路训练过程
- **PCIe速度和宽度设置详解.md** - PCIe 速度和宽度配置
- **PCIe初始化完整流程.md** - PCIe 初始化完整流程

### 配置和资源
- **PCIe配置空间布局详解.md** - PCIe 配置空间详细布局
- **PCIe_BAR详解.md** - Base Address Register 详解
- **PCIe_BAR内存映射详解.md** - BAR 内存映射机制
- **PCIe总线设备功能号详解.md** - Bus/Device/Function 编号
- **PCIe总线号来源详解.md** - 总线号分配机制
- **PCIe地址与系统物理地址映射详解.md** - PCIe 地址与系统物理地址的关系
- **PCIe地址的本质是什么.md** - PCIe 地址的本质和用途（重要澄清）
- **PCIe_ranges属性格式详解.md** - ranges 属性的格式和含义

### 路由机制
- **PCIe根复合体路由表详解.md** - Root Complex 路由表
- **PCIe交换机路由详解.md** - PCIe Switch 路由机制

### 驱动框架
- **PCIe_Linux驱动框架详解.md** - Linux PCIe 驱动框架详解
- **ARM64平台PCIe驱动完整流程详解-Tegra.md** - ARM64 平台 PCIe 驱动流程（Tegra 示例）
- **GPU驱动使用PCIe_API详解.md** - GPU 驱动如何使用 PCIe API

### 中断机制
- **PCIe_MSI中断机制详解.md** - PCIe MSI 中断机制详解
- **MSI消息数据内容详解.md** - MSI 消息数据字段详解
- **GPU_MSI中断触发机制.md** - GPU 如何触发 MSI 中断

### DMA 和 IOMMU
- **PCIe_DMA和IOMMU流程详解.md** - PCIe DMA 和 IOMMU 完整流程

## 文档分类

### 按主题分类

**硬件层：**
- PCIe概述.md
- PCIe根复合体详解.md
- PCIe链路训练详解.md
- PCIe速度和宽度设置详解.md
- PCIe初始化完整流程.md

**配置层：**
- PCIe配置空间布局详解.md
- PCIe_BAR详解.md
- PCIe_BAR内存映射详解.md
- PCIe总线设备功能号详解.md
- PCIe总线号来源详解.md
- PCIe地址与系统物理地址映射详解.md
- PCIe地址的本质是什么.md
- PCIe_ranges属性格式详解.md

**路由层：**
- PCIe根复合体路由表详解.md
- PCIe交换机路由详解.md

**驱动层：**
- PCIe_Linux驱动框架详解.md
- ARM64平台PCIe驱动完整流程详解-Tegra.md
- GPU驱动使用PCIe_API详解.md

**中断层：**
- PCIe_MSI中断机制详解.md
- MSI消息数据内容详解.md
- GPU_MSI中断触发机制.md

**DMA层：**
- PCIe_DMA和IOMMU流程详解.md

**其他：**
- PCIe_NOC术语详解.md

## 阅读建议

### 初学者
1. PCIe概述.md
2. PCIe根复合体详解.md
3. PCIe配置空间布局详解.md
4. PCIe_BAR详解.md

### 驱动开发者
1. PCIe_Linux驱动框架详解.md
2. GPU驱动使用PCIe_API详解.md
3. PCIe_MSI中断机制详解.md
4. PCIe_DMA和IOMMU流程详解.md

### 硬件工程师
1. PCIe链路训练详解.md
2. PCIe初始化完整流程.md
3. PCIe根复合体路由表详解.md
4. PCIe交换机路由详解.md

## 更新日期

文档最后更新：2025年1月20日

## 重要说明

### PCIe 地址的本质

**关键理解：**
- PCIe 设备**直接使用系统物理地址**，没有独立的 PCIe 地址空间
- PCIe 设备在 BAR 里存储的就是系统物理地址
- `ranges` 属性主要用于**地址解码和路由**，不是地址转换

**推荐阅读：**
- **PCIe地址的本质是什么.md** - 直接回答 PCIe 地址的本质和用途
- **PCIe地址与系统物理地址映射详解.md** - 详细解释地址映射关系

