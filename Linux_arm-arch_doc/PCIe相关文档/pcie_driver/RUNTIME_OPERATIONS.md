# PCIe设备在Linux系统运行过程中的操作指南

本文档详细说明PCIe设备在Linux系统运行过程中需要进行的各种操作。

## 目录

1. [系统启动时的自动操作](#系统启动时的自动操作)
2. [驱动加载和初始化](#驱动加载和初始化)
3. [运行时监控和管理](#运行时监控和管理)
4. [设备配置和调优](#设备配置和调优)
5. [故障排查](#故障排查)
6. [性能优化](#性能优化)

---

## 系统启动时的自动操作

### 1. BIOS/UEFI阶段

系统启动时，BIOS/UEFI会自动执行以下操作：

- **PCIe枚举**：扫描PCIe总线，发现所有连接的设备
- **资源分配**：为每个设备分配内存空间（BAR）和I/O空间
- **中断路由**：配置中断路由表
- **设备初始化**：执行基本的设备初始化

### 2. Linux内核启动阶段

内核启动时会自动：

```bash
# 查看内核启动日志中的PCIe信息
dmesg | grep -i pci
dmesg | grep -i "pcie\|pci express"
```

**自动执行的操作：**
- 扫描PCIe总线树
- 识别所有PCIe设备
- 读取设备配置空间
- 分配设备资源（内存、中断等）
- 尝试加载匹配的驱动程序

---

## 驱动加载和初始化

### 1. 检查设备是否被系统识别

```bash
# 查看所有PCIe设备
lspci

# 查看特定设备（根据Vendor ID和Device ID）
lspci | grep -i "1234:5678"  # 替换为你的设备ID

# 查看设备详细信息
lspci -v -s <bus:device.function>

# 查看设备配置空间
lspci -vvv -s <bus:device.function>

# 查看PCIe链路信息
lspci -vvv | grep -A 5 "LnkSta"
```

### 2. 检查当前驱动状态

```bash
# 查看设备使用的驱动
lspci -k -s <bus:device.function>

# 查看已加载的驱动模块
lsmod | grep pcie_driver

# 查看驱动信息
modinfo pcie_driver
```

### 3. 加载驱动

```bash
# 方法1：手动加载
sudo insmod pcie_driver.ko

# 方法2：使用modprobe（推荐，会自动处理依赖）
sudo modprobe pcie_driver

# 方法3：使用Makefile
sudo make load
```

### 4. 验证驱动加载成功

```bash
# 查看内核日志
dmesg | tail -30
# 或实时监控
sudo dmesg -w

# 检查设备节点是否创建
ls -l /dev/pcie_dev

# 检查设备是否被驱动绑定
cat /sys/bus/pci/drivers/pcie_driver/bind
```

---

## 运行时监控和管理

### 1. 设备状态监控

```bash
# 查看设备基本信息
lspci -s <bus:device.function>

# 查看设备资源分配
cat /proc/bus/pci/devices | grep <设备信息>

# 查看设备内存映射
cat /proc/iomem | grep -i pci

# 查看设备I/O端口
cat /proc/ioports | grep -i pci
```

### 2. 中断监控

```bash
# 查看中断统计
cat /proc/interrupts | grep pcie

# 查看中断亲和性
cat /proc/irq/<irq_number>/smp_affinity

# 设置中断亲和性（绑定到特定CPU）
echo 1 > /proc/irq/<irq_number>/smp_affinity
```

### 3. DMA和内存操作监控

```bash
# 查看DMA映射
dmesg | grep -i dma

# 查看内存使用情况
cat /proc/meminfo

# 查看设备内存映射
sudo cat /sys/kernel/debug/pci/<bus:device.function>/resource
```

### 4. 性能监控

```bash
# 查看PCIe链路速度
lspci -vvv | grep -A 5 "LnkSta"
# 关注：
# - Speed: 2.5GT/s, 5.0GT/s, 8.0GT/s, 16.0GT/s
# - Width: x1, x2, x4, x8, x16
#
# 想了解这个命令是如何实现的？请查看 COMMAND_IMPLEMENTATION.md

# 查看设备错误统计
lspci -vvv | grep -i error

# 使用perf监控（如果支持）
sudo perf stat -e pcie_uncore_imc_0/read_write/
```

---

## 设备配置和调优

### 1. PCIe链路参数调整

```bash
# 查看当前链路状态
lspci -vvv -s <bus:device.function> | grep -A 10 "LnkSta"

# 强制链路速度（需要内核支持）
# 在grub中添加：pcie_aspm=off
# 或使用setpci命令（需要root权限）
sudo setpci -s <bus:device.function> CAP_EXP+0x10.l=0x42
```

### 2. MSI/MSI-X配置

```bash
# 查看中断类型
lspci -vvv -s <bus:device.function> | grep -i "msi\|msix"

# 检查MSI支持
cat /proc/interrupts | grep -i msi

# 禁用MSI（如果需要，在驱动中设置）
# 在pci_alloc_irq_vectors时只使用PCI_IRQ_LEGACY
```

### 3. 电源管理

```bash
# 查看电源管理状态
lspci -vvv | grep -i "pm\|power"

# 禁用ASPM（Active State Power Management）
# 在grub中添加：pcie_aspm=off

# 查看设备电源状态
cat /sys/bus/pci/devices/<bus:device.function>/power_state
```

### 4. 热插拔支持

```bash
# 检查热插拔支持
ls /sys/bus/pci/slots/

# 查看插槽信息
cat /sys/bus/pci/slots/<slot_number>/address
cat /sys/bus/pci/slots/<slot_number>/power

# 手动移除设备（用于测试）
echo 1 > /sys/bus/pci/devices/<bus:device.function>/remove

# 重新扫描总线
echo 1 > /sys/bus/pci/rescan
```

---

## 故障排查

### 1. 设备未被识别

```bash
# 检查设备是否在总线上
lspci | grep <vendor:device>

# 检查PCIe插槽是否正常
lspci -vvv | grep -A 5 "LnkSta"
# 检查：
# - LnkSta: Speed, Width, Clock
# - 如果有错误，会显示错误计数

# 检查内核日志
dmesg | grep -i "pci\|pcie" | tail -50
```

### 2. 驱动加载失败

```bash
# 查看详细错误信息
dmesg | tail -50

# 检查设备ID是否匹配
lspci -n | grep <vendor:device>
# 与驱动中的pcie_ids数组对比

# 检查资源冲突
lspci -vvv -s <bus:device.function>
# 查看BAR是否正常分配

# 检查内核版本兼容性
uname -r
modinfo pcie_driver | grep vermagic
```

### 3. 中断问题

```bash
# 检查中断是否注册
cat /proc/interrupts | grep pcie

# 查看中断统计（是否有大量中断）
watch -n 1 'cat /proc/interrupts | grep pcie'

# 检查中断共享冲突
cat /proc/interrupts

# 查看中断处理函数
cat /proc/kallsyms | grep pcie_interrupt_handler
```

### 4. 内存映射问题

```bash
# 检查BAR映射
lspci -vvv -s <bus:device.function>
# 查看Base Address Register的值

# 检查内存映射是否成功
dmesg | grep -i "bar\|iomap"

# 检查资源冲突
cat /proc/iomem | grep -i pci
cat /proc/ioports | grep -i pci
```

### 5. 性能问题

```bash
# 检查链路速度是否降级
lspci -vvv | grep -A 5 "LnkSta"
# 如果Speed低于预期，可能是：
# - 硬件问题
# - 电源管理导致降速
# - 链路训练失败

# 检查错误计数
lspci -vvv | grep -i "error\|uncorrectable\|correctable"

# 使用perf分析性能瓶颈
sudo perf record -a -g
sudo perf report
```

---

## 性能优化

### 1. 中断优化

```bash
# 绑定中断到特定CPU核心
echo <cpu_mask> > /proc/irq/<irq_number>/smp_affinity
# 例如：绑定到CPU 0
echo 1 > /proc/irq/123/smp_affinity

# 设置中断类型（在驱动中）
# 优先使用MSI-X，然后是MSI，最后是Legacy
```

### 2. DMA优化

```bash
# 使用64位DMA（如果设备支持）
# 在驱动中设置：pci_set_dma_mask(pdev, DMA_BIT_MASK(64))

# 启用DMA合并
# 在驱动中设置：pci_set_master(pdev)
```

### 3. 内存对齐优化

```c
// 在驱动代码中，确保DMA缓冲区对齐
// 使用dma_alloc_coherent而不是kmalloc
dma_addr_t dma_addr;
void *virt_addr = dma_alloc_coherent(&pdev->dev, size, &dma_addr, GFP_KERNEL);
```

### 4. 批处理操作

```c
// 减少PCIe事务数量，使用批处理
// 例如：一次读取多个寄存器，而不是多次单独读取
```

---

## 常用操作脚本

创建一个管理脚本 `pcie_manager.sh`：

```bash
#!/bin/bash

# PCIe设备管理脚本

DEVICE_ID="1234:5678"  # 修改为你的设备ID

case "$1" in
    status)
        echo "=== PCIe Device Status ==="
        lspci | grep "$DEVICE_ID"
        echo ""
        echo "=== Driver Status ==="
        lsmod | grep pcie_driver
        echo ""
        echo "=== Device Node ==="
        ls -l /dev/pcie_dev 2>/dev/null || echo "Device node not found"
        ;;
    load)
        echo "Loading driver..."
        sudo insmod pcie_driver.ko
        dmesg | tail -10
        ;;
    unload)
        echo "Unloading driver..."
        sudo rmmod pcie_driver
        dmesg | tail -10
        ;;
    info)
        BUS=$(lspci -n | grep "$DEVICE_ID" | cut -d' ' -f1)
        if [ -n "$BUS" ]; then
            lspci -vvv -s "$BUS"
        else
            echo "Device not found"
        fi
        ;;
    logs)
        dmesg | grep -i pcie | tail -20
        ;;
    *)
        echo "Usage: $0 {status|load|unload|info|logs}"
        exit 1
        ;;
esac
```

使用方法：
```bash
chmod +x pcie_manager.sh
./pcie_manager.sh status
./pcie_manager.sh load
./pcie_manager.sh info
```

---

## 系统服务配置（可选）

如果需要驱动在系统启动时自动加载，可以创建systemd服务：

创建 `/etc/systemd/system/pcie-driver.service`：

```ini
[Unit]
Description=PCIe Driver Service
After=sysinit.target

[Service]
Type=oneshot
ExecStart=/sbin/insmod /path/to/pcie_driver.ko
ExecStop=/sbin/rmmod pcie_driver
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

启用服务：
```bash
sudo systemctl enable pcie-driver.service
sudo systemctl start pcie-driver.service
```

---

## 总结

PCIe设备在Linux系统运行过程中主要需要：

1. **系统启动时**：自动枚举和资源分配（无需手动操作）
2. **驱动加载**：手动或自动加载驱动模块
3. **运行时监控**：定期检查设备状态、中断、性能
4. **故障排查**：使用各种工具诊断问题
5. **性能优化**：根据需求调整参数

大多数操作都可以通过标准Linux工具完成，驱动本身已经处理了大部分底层细节。

