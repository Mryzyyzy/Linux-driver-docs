# PCIe 驱动程序

这是一个完整的Linux PCIe驱动程序示例，展示了PCIe设备的基本操作，包括设备检测、初始化、中断处理和清理。

## 功能特性

- PCIe设备探测和初始化
- 内存映射I/O (BAR0和BAR1)
- 中断处理
- 字符设备接口
- IOCTL接口用于寄存器读写
- 完整的错误处理

## 文件结构

```
pcie_driver/
├── pcie_driver.c              # 主驱动源文件
├── pcie_driver.h              # 头文件（IOCTL定义等）
├── Makefile                   # 编译Makefile
├── pcie_manager.sh            # 设备管理脚本（便捷工具）
├── README.md                      # 本文档（总索引）
├── RUNTIME_OPERATIONS.md          # 运行时操作详细指南
├── COMMAND_IMPLEMENTATION.md      # 命令实现原理详解
├── SYNC_INTERFACE_COMPARISON.md   # 系统同步vs源同步对比（详细版）
├── SYNC_QUICK_REFERENCE.md        # 系统同步vs源同步快速参考
├── CDR_MECHANISM.md               # CDR时钟数据恢复详细原理
├── CDR_VISUAL_GUIDE.md            # CDR工作原理可视化指南
├── PCIe_OVERVIEW.md               # PCIe/SerDes 总体入门概览
├── PCIe_LINK_TRAINING.md          # PCIe链路训练详解
├── LINK_TRAINING_QUICK_REF.md     # 链路训练快速参考
├── PCIe_INITIALIZATION_COMPLETE.md# PCIe完整初始化流程（训练+枚举+驱动）
├── PCIe_CONFIG_SPACE_LAYOUT.md    # PCIe配置空间完整布局 + 扩展/VSEC 说明
├── PCIe_BAR_DETAILED.md           # PCIe BAR详解和使用方法
├── PCIe_BAR_MEMORY_MAPPING.md     # BAR地址与系统内存映射关系 + 解码示例
├── PCIe_SPEED_WIDTH_SET.md        # 链路速率/宽度的含义与读写方式
├── PCIe_ROOT_COMPLEX.md           # Root Complex 结构与职责
├── PCIe_RC_ROUTING_TABLE.md       # RC 路由表创建与配置
├── PCIe_BUS_DEV_FUNC.md           # Bus/Device/Function 含义与使用
├── PCIe_SWITCH_ROUTING.md         # Switch 识别/路由与 RC 区别
├── PCIe_BUS_NUMBER_SOURCE.md      # Bus 号来源与“临时分配”详解
├── NOC_ARCHITECTURE_DIAGRAM.md    # 基于NoC的嵌入式系统架构图
├── PCIe_NOC_TERMINOLOGY.md        # PCIe 与 NoC 术语解释大全
├── PCIe_DMA_IOMMU_FLOW.md         # PCIe DMA + IOMMU 访存流程图
└── examples/                      # 示例程序
    ├── Makefile                   # 示例程序编译
    ├── read_link_status.c         # C程序：读取链路状态
    ├── read_pcie_info.sh          # Shell脚本：读取PCIe信息
    ├── set_aspm.c                 # Linux 内核中设置 ASPM 示例
    ├── set_aspm_simple.c          # 精简版 ASPM 设置示例（内核）
    ├── ASPM_SETTING_README.md     # ASPM 设置示例使用说明（内核环境）
    ├── set_aspm_atf.c             # ATF 环境下设置 ASPM 示例
    ├── set_aspm_atf_simple.c      # 精简版 ASPM 设置示例（ATF）
    ├── ASPM_ATF_README.md         # ASPM 设置示例使用说明（ATF）
    ├── set_aspm_after_bar.c       # BAR 配置完成后修改 ASPM 示例
    └── ASPM_AFTER_BAR_README.md   # ASPM-after-BAR 示例使用说明
```

## 编译要求

- Linux内核源码（或内核头文件）
- GCC编译器
- Make工具
- 内核开发包（kernel-devel或linux-headers）

## 编译步骤

1. **修改设备ID**
   
   编辑 `pcie_driver.c`，修改 `pcie_ids` 数组中的Vendor ID和Device ID以匹配你的硬件：
   ```c
   static const struct pci_device_id pcie_ids[] = {
       { PCI_DEVICE(0x1234, 0x5678), },  /* 修改为你的设备ID */
       { 0, }
   };
   ```

2. **编译驱动**
   ```bash
   make
   ```

3. **加载驱动**
   ```bash
   sudo make load
   # 或
   sudo insmod pcie_driver.ko
   ```

4. **查看驱动信息**
   ```bash
   make info
   # 或
   modinfo pcie_driver.ko
   ```

5. **查看内核日志**
   ```bash
   make logs
   # 或
   dmesg | tail -20
   ```

6. **卸载驱动**
   ```bash
   sudo make unload
   # 或
   sudo rmmod pcie_driver
   ```

7. **使用管理脚本（推荐）**
   ```bash
   # 查看设备状态
   ./pcie_manager.sh status
   
   # 加载驱动
   ./pcie_manager.sh load
   
   # 卸载驱动
   ./pcie_manager.sh unload
   
   # 查看详细信息
   ./pcie_manager.sh info
   
   # 实时监控
   ./pcie_manager.sh monitor
   ```

## 使用方法

### 1. 检查设备是否被识别

```bash
# 查看PCIe设备
lspci | grep -i "你的设备名称"

# 查看设备详细信息
lspci -v -s <bus:device.function>

# 查看驱动是否加载
lsmod | grep pcie_driver
```

### 2. 使用字符设备接口

驱动会创建一个字符设备节点 `/dev/pcie_dev`，可以通过标准文件操作访问：

```bash
# 读取设备
cat /dev/pcie_dev

# 写入设备
echo "data" > /dev/pcie_dev
```

### 3. 使用IOCTL接口

可以编写用户空间程序使用IOCTL命令读写寄存器：

```c
#include <fcntl.h>
#include <sys/ioctl.h>
#include "pcie_driver.h"

int fd = open("/dev/pcie_dev", O_RDWR);

// 读取寄存器
struct pcie_reg_data reg;
reg.offset = PCIE_STATUS_REG;
ioctl(fd, PCIE_IOCTL_READ_REG, &reg);
printf("Status: 0x%x\n", reg.value);

// 写入寄存器
reg.offset = PCIE_CONTROL_REG;
reg.value = PCIE_CTRL_ENABLE;
ioctl(fd, PCIE_IOCTL_WRITE_REG, &reg);

close(fd);
```

## 自定义配置

### 修改寄存器偏移

编辑 `pcie_driver.h` 中的寄存器定义：
```c
#define PCIE_STATUS_REG   0x00
#define PCIE_CONTROL_REG  0x04
// ... 根据你的设备手册修改
```

### 修改中断处理逻辑

在 `pcie_interrupt_handler` 函数中添加你的中断处理代码。

### 添加更多BAR映射

如果需要映射更多BAR，可以在 `pcie_probe` 函数中添加：
```c
dev->bar2 = pci_iomap(pdev, 2, 0);
```

## 调试

### 查看内核日志
```bash
dmesg | grep pcie
```

### 检查设备状态
```bash
# 查看PCIe设备信息
cat /proc/bus/pci/devices

# 查看中断信息
cat /proc/interrupts | grep pcie
```

### ·1

驱动中已经包含了多个 `pr_info` 和 `pr_err` 语句，可以通过 `dmesg` 查看输出。

## 注意事项

1. **权限要求**：加载/卸载驱动和访问设备节点需要root权限
2. **内核版本**：确保内核版本与编译时使用的内核头文件匹配
3. **设备ID**：必须修改为实际硬件的Vendor ID和Device ID
4. **中断共享**：如果设备支持MSI/MSI-X，驱动会自动使用
5. **内存映射**：确保BAR空间大小足够，避免越界访问

## 常见问题

### Q: 编译时找不到内核头文件
A: 安装内核开发包：
```bash
# Ubuntu/Debian
sudo apt-get install linux-headers-$(uname -r)

# CentOS/RHEL
sudo yum install kernel-devel
```

### Q: 驱动加载失败
A: 检查：
- 设备ID是否正确
- 设备是否在系统中
- 查看 `dmesg` 错误信息

### Q: 无法访问设备节点
A: 检查设备节点权限：
```bash
ls -l /dev/pcie_dev
# 如果需要，修改权限
sudo chmod 666 /dev/pcie_dev
```

## 许可证

GPL v2

## 运行时操作

关于PCIe设备在Linux系统运行过程中需要进行的详细操作，请参考 [RUNTIME_OPERATIONS.md](RUNTIME_OPERATIONS.md) 文档。

该文档包含：
- 系统启动时的自动操作
- 驱动加载和初始化步骤
- 运行时监控和管理方法
- 设备配置和调优
- 故障排查指南
- 性能优化建议

## 命令实现原理

想了解 `lspci` 等命令是如何实现的？请查看 [COMMAND_IMPLEMENTATION.md](COMMAND_IMPLEMENTATION.md) 文档。

该文档详细解释：
- lspci命令的底层实现原理
- PCIe配置空间访问机制
- 内核接口和系统调用
- 用户空间工具的实现
- 实际代码示例

### 示例程序

`examples/` 目录包含演示如何直接读取PCIe信息的示例：

```bash
# 编译C示例程序
cd examples
make

# 使用C程序读取链路状态
./read_link_status 0000:00:01.0

# 使用Shell脚本读取信息
./read_pcie_info.sh 0000:00:01.0
```

## 接口同步机制

想了解系统同步和源同步并行接口的区别？

- **快速参考**：[SYNC_QUICK_REFERENCE.md](SYNC_QUICK_REFERENCE.md) - 核心区别和快速对比
- **详细文档**：[SYNC_INTERFACE_COMPARISON.md](SYNC_INTERFACE_COMPARISON.md) - 完整技术分析

### 核心区别

- **系统同步**：发送端和接收端共享一个系统时钟
- **源同步**：数据和时钟一起从发送端传输

### 关键差异

| 特性 | 系统同步 | 源同步 |
|------|---------|--------|
| 时钟路径 | 独立分配 | 与数据一起 |
| 时钟偏斜 | 较大 | 很小 |
| 最大频率 | ~100MHz | >500MHz |
| 典型应用 | 低速总线 | DDR, PCIe |

详细文档包含：
- 工作原理和时序分析
- 性能对比和计算公式
- 应用场景和设计要点
- PCIe中的同步机制

## CDR时钟数据恢复

想了解SerDes中CDR如何从数据边沿抽取时钟并找到最优采样位置？

- **可视化指南**：[CDR_VISUAL_GUIDE.md](CDR_VISUAL_GUIDE.md) - 直观的图表和流程
- **技术详解**：[CDR_MECHANISM.md](CDR_MECHANISM.md) - 完整的技术原理

### CDR核心功能

1. **时钟抽取**：从数据边沿中提取时钟频率和相位
2. **采样优化**：找到眼图中心的最优采样位置
3. **相位跟踪**：持续调整以补偿相位变化

### 关键过程

```
数据边沿 ──> 相位检测 ──> VCO调整 ──> 相位插值 ──> 数据采样
            (检测跳变)   (恢复频率)  (对齐相位)  (优化位置)  (采样数据)
```

## PCIe链路训练

想了解PCIe链路训练是如何工作的？请查看 [PCIe_LINK_TRAINING.md](PCIe_LINK_TRAINING.md) 文档。

### 链路训练概述

链路训练是PCIe设备建立通信前的自动协商过程：

```
设备上电 ──> Detect ──> Polling ──> Configuration ──> L0 (正常工作)
          (检测链路)  (CDR锁定)   (参数协商)      (数据传输)
```

### 关键阶段

1. **Detect阶段**：检测链路是否存在
2. **Polling阶段**：发送TS1训练序列，CDR锁定频率
3. **Configuration阶段**：协商速度和宽度，对齐通道
4. **L0状态**：链路训练完成，正常传输数据

### 训练序列（TS1/TS2）

- **TS1**：用于初始训练和CDR锁定
- **TS2**：用于参数确认
- 包含链路参数（速度、宽度等）
- 提供足够的跳变供CDR锁定

## PCIe 与 NoC 术语解释

遇到不熟悉的术语？请查看 [PCIe_NOC_TERMINOLOGY.md](PCIe_NOC_TERMINOLOGY.md) 文档。

### 文档内容

该文档汇总了 PCIe 和 NoC 相关的重要术语和概念，包括：

- **PCIe 基础术语**：RC/EP/DM 模式、BDF、BAR、配置空间
- **PCIe 配置与枚举**：枚举时机、设备驱动作用、训练 vs 配置
- **PCIe 物理层与链路**：Gen1/2/3/4、SerDes、PIPE、bif mode
- **NoC 基础术语**：NoC、AXI/CHI、同步/异步
- **NoC 一致性协议**：snoop、CHA、SNF、DVM
- **NoC 地址映射**：SAM、RN-SAM/CHA-SAM/ISB-SAM、hash range、LDID
- **系统架构相关**：SLC、SCG region、NUMA、PLDA 寄存器等

### 快速查找

文档按类别组织，每个术语都有：
- **含义**：清晰的定义
- **作用/用途**：实际应用场景
- **注意事项**：容易混淆的地方

适合作为快速参考手册，遇到不熟悉的术语时随时查阅。

## PCIe DMA + IOMMU 访存流程

想了解 PCIe 设备在有 DMA 和 IOMMU 的情况下如何访问系统内存？请查看 [PCIe_DMA_IOMMU_FLOW.md](PCIe_DMA_IOMMU_FLOW.md) 文档。

### 文档内容

该文档详细展示了完整的访存流程，包括：

- **CPU 配置阶段**：分配 DMA buffer、IOMMU 建立映射、配置设备
- **设备 DMA 阶段**：设备发起 Memory Read/Write TLP
- **IOMMU 翻译阶段**：IOVA 到物理地址的翻译和权限检查
- **NoC 路由阶段**：地址映射表查询、hash 路由、多通道打散
- **DDR 访问阶段**：DDR 控制器访问内存芯片
- **数据返回阶段**：数据沿原路返回、设备接收、MSI 中断

### 流程图特点

- **7 个阶段**：从 CPU 配置到数据返回的完整流程
- **地址空间示意**：IOVA、物理地址、虚拟地址的关系
- **代码示例**：实际的驱动代码示例
- **时序图**：各组件之间的交互时序

适合理解 PCIe 设备 DMA 的完整工作机制，特别是 IOMMU 的作用和地址翻译过程。

## NoC 软件工程师知识要点

想了解 NoC 软件工程师需要掌握哪些知识？请查看 [NOC_SOFTWARE_ENGINEER_GUIDE.md](NOC_SOFTWARE_ENGINEER_GUIDE.md) 文档。

### 文档内容

该文档总结了 NoC 软件工程师的核心知识领域，包括：

- **NoC 基础架构**：拓扑结构、协议层次、Bridge 类型
- **地址映射系统（SAM）**：RN-SAM、CHA-SAM、ISB-SAM、hash range
- **缓存一致性协议（CHI）**：CHA、SNF、snoop、DVM
- **协议转换与桥接**：AXI ↔ NoC、PCIe ↔ AXI
- **内存管理**：SRAM、DDR、IOMMU、DMA
- **系统初始化与配置**：固件、内核、驱动

### 学习路径

文档提供了分阶段的学习建议：
- **阶段 1**：基础概念（1-2 周）
- **阶段 2**：深入理解（2-4 周）
- **阶段 3**：实践应用（持续）

### 实用指南

- **常见工作场景**：系统初始化、设备驱动开发、性能优化、问题调试
- **技能要求总结**：硬件理解、软件能力、系统理解
- **关键文档和资源**：必须阅读的文档清单

适合 NoC 软件工程师了解工作范围和重点，以及制定学习计划。

## ATF 安全启动实现

想了解 ARM Trusted Firmware (ATF) 中安全启动是如何实现的？请查看 [ATF_SECURE_BOOT_GUIDE.md](ATF_SECURE_BOOT_GUIDE.md) 文档。

### 文档内容

该文档详细介绍了 ATF 安全启动的实现，包括：

- **安全启动基本原理**：启动链、密钥体系
- **ATF 安全启动实现**：密钥管理、签名验证流程、密钥吊销检查
- **实际代码示例**：基于 ATF 的真实代码示例
- **配置和工具**：编译选项、密钥生成、镜像签名
- **完整流程总结**：从 Boot ROM 到 Linux Kernel 的完整验证流程

### 关键特性

- **启动链验证**：每个阶段都验证下一阶段的签名
- **证书链验证**：从内容证书到根证书的完整验证
- **密钥吊销**：支持密钥吊销列表检查
- **代码示例**：提供实际的 C 代码示例

适合理解嵌入式系统安全启动的实现机制，特别是 ATF 中的安全启动流程。

## 作者

根据你的需求修改作者信息

