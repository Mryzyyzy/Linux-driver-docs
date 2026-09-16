# lspci 读取的 sysfs 属性详解

## 概述

`lspci` 命令通过读取 `/sys/bus/pci/devices/` 目录下的属性文件来显示 PCI 设备信息。不同的 `lspci` 实现（busybox vs pciutils）读取的属性略有不同。

## 目录结构

```
/sys/bus/pci/devices/
├── 0000:00:00.0/          # PCI 设备目录（格式：domain:bus:device.function）
│   ├── uevent             # 设备事件信息（主要被 busybox lspci 使用）
│   ├── config             # PCI 配置空间的二进制镜像（pciutils 使用）
│   ├── vendor             # 厂商 ID
│   ├── device             # 设备 ID
│   ├── class              # 设备类别
│   ├── subsystem_vendor   # 子系统厂商 ID
│   ├── subsystem_device   # 子系统设备 ID
│   ├── revision           # 修订版本
│   ├── irq                # 中断号
│   ├── resource           # 资源信息（BAR、IRQ 等）
│   ├── driver             # 指向驱动目录的符号链接
│   └── ...                # 其他属性
```

## 主要属性文件说明

### 1. **uevent** - 设备事件信息（busybox lspci 主要使用）

**路径**: `/sys/bus/pci/devices/0000:00:00.0/uevent`

**内容示例**:
```
DRIVER=agpgart-intel
PCI_CLASS=60000
PCI_ID=8086:7190
PCI_SUBSYS_ID=15AD:1976
PCI_SLOT_NAME=0000:00:00.0
MODALIAS=pci:v00008086d00007190sv000015ADsd00001976bc06sc00i00
```

**用途**:
- busybox 版本的 `lspci` 主要读取此文件
- 包含设备的基本标识信息
- 由内核自动生成和维护

### 2. **config** - PCI 配置空间镜像（pciutils lspci 使用）

**路径**: `/sys/bus/pci/devices/0000:00:00.0/config`

**内容**: 二进制格式，256 字节（或 4096 字节，如果支持扩展配置空间）

**用途**:
- 标准 `pciutils` 的 `lspci` 读取此文件
- 包含完整的 PCI 配置空间数据
- 可以解析出所有 PCI 配置寄存器

**示例查看**:
```bash
# 查看配置空间（十六进制）
hexdump -C /sys/bus/pci/devices/0000:00:00.0/config | head -10

# 使用 setpci 读取
setpci -s 00:00.0 CAP_EXP+0x10.L
```

### 3. **基础标识属性**

#### vendor
**路径**: `/sys/bus/pci/devices/0000:00:00.0/vendor`
**内容**: `0x8086`（十六进制厂商 ID）
**对应**: PCI 配置空间偏移 0x00

#### device
**路径**: `/sys/bus/pci/devices/0000:00:00.0/device`
**内容**: `0x7190`（十六进制设备 ID）
**对应**: PCI 配置空间偏移 0x02

#### class
**路径**: `/sys/bus/pci/devices/0000:00:00.0/class`
**内容**: `0x060000`（设备类别，24 位）
**对应**: PCI 配置空间偏移 0x08-0x0B

#### subsystem_vendor
**路径**: `/sys/bus/pci/devices/0000:00:00.0/subsystem_vendor`
**内容**: `0x15AD`（子系统厂商 ID）
**对应**: PCI 配置空间偏移 0x2C

#### subsystem_device
**路径**: `/sys/bus/pci/devices/0000:00:00.0/subsystem_device`
**内容**: `0x1976`（子系统设备 ID）
**对应**: PCI 配置空间偏移 0x2E

#### revision
**路径**: `/sys/bus/pci/devices/0000:00:00.0/revision`
**内容**: `0x01`（修订版本号）
**对应**: PCI 配置空间偏移 0x08（低 8 位）

### 4. **资源信息**

#### resource
**路径**: `/sys/bus/pci/devices/0000:00:00.0/resource`
**内容**: 文本格式，列出所有资源（BAR、IRQ 等）

**示例**:
```
0x00000000f0000000 0x00000000f7ffffff 0x0000000000040200
0x0000000000000000 0x0000000000000000 0x0000000000000000
...
IRQ 9.
```

**格式**: 每行一个资源
- 前两列：起始地址和结束地址
- 第三列：资源标志（IORESOURCE_*）
- IRQ 行：中断号

#### irq
**路径**: `/sys/bus/pci/devices/0000:00:00.0/irq`
**内容**: `9`（中断号）

### 5. **驱动信息**

#### driver
**路径**: `/sys/bus/pci/devices/0000:00:00.0/driver`
**内容**: 符号链接，指向 `/sys/bus/pci/drivers/<driver_name>`

**示例**:
```bash
ls -l /sys/bus/pci/devices/0000:00:00.0/driver
# -> ../../../bus/pci/drivers/agpgart-intel
```

### 6. **PCIe 特定属性**（仅 PCIe 设备）

#### current_link_speed
**路径**: `/sys/bus/pci/devices/0000:00:01.0/current_link_speed`
**内容**: `2.5 GT/s` 或 `5 GT/s` 或 `8 GT/s` 等
**来源**: PCIe Link Status 寄存器

#### current_link_width
**路径**: `/sys/bus/pci/devices/0000:00:01.0/current_link_width`
**内容**: `x1`, `x2`, `x4`, `x8`, `x16` 等
**来源**: PCIe Link Status 寄存器

#### max_link_speed
**路径**: `/sys/bus/pci/devices/0000:00:01.0/max_link_speed`
**内容**: 设备支持的最大链路速度
**来源**: PCIe Link Capability 寄存器

#### max_link_width
**路径**: `/sys/bus/pci/devices/0000:00:01.0/max_link_width`
**内容**: 设备支持的最大链路宽度
**来源**: PCIe Link Capability 寄存器

### 7. **桥接器属性**（仅 PCI 桥）

#### secondary_bus_number
**路径**: `/sys/bus/pci/devices/0000:00:01.0/secondary_bus_number`
**内容**: `1`（下游总线号）
**对应**: PCI 配置空间偏移 0x19

#### subordinate_bus_number
**路径**: `/sys/bus/pci/devices/0000:00:01.0/subordinate_bus_number`
**内容**: `1`（下游最大总线号）
**对应**: PCI 配置空间偏移 0x1A

### 8. **其他属性**

#### modalias
**路径**: `/sys/bus/pci/devices/0000:00:00.0/modalias`
**内容**: `pci:v00008086d00007190sv000015ADsd00001976bc06sc00i00`
**格式**: `pci:v<vendor>d<device>sv<subsys_vendor>sd<subsys_device>bc<class>sc<subclass>i<interface>`
**用途**: 用于模块自动加载

#### enable
**路径**: `/sys/bus/pci/devices/0000:00:00.0/enable`
**内容**: `1`（设备使能计数）
**可写**: 是（需要 root 权限）
**用途**: 启用/禁用设备

#### power_state
**路径**: `/sys/bus/pci/devices/0000:00:00.0/power_state`
**内容**: `D0`, `D1`, `D2`, `D3hot`, `D3cold` 等
**用途**: 显示当前电源状态

#### broken_parity_status
**路径**: `/sys/bus/pci/devices/0000:00:00.0/broken_parity_status`
**内容**: `0` 或 `1`
**可写**: 是
**用途**: 标记设备是否有奇偶校验问题

#### msi_bus
**路径**: `/sys/bus/pci/devices/0000:00:00.0/msi_bus`
**内容**: `1`（允许 MSI）或 `0`（禁止 MSI）
**可写**: 是（需要 root 权限）
**用途**: 控制 MSI/MSI-X 支持

#### numa_node
**路径**: `/sys/bus/pci/devices/0000:00:00.0/numa_node`
**内容**: `-1`（无 NUMA）或节点号
**用途**: 显示设备所属的 NUMA 节点

#### dma_mask_bits
**路径**: `/sys/bus/pci/devices/0000:00:00.0/dma_mask_bits`
**内容**: `32` 或 `64` 等
**用途**: DMA 地址掩码位数

#### consistent_dma_mask_bits
**路径**: `/sys/bus/pci/devices/0000:00:00.0/consistent_dma_mask_bits`
**内容**: 一致性 DMA 掩码位数
**用途**: 一致性 DMA 地址掩码

#### ari_enabled
**路径**: `/sys/bus/pci/devices/0000:00:00.0/ari_enabled`
**内容**: `0` 或 `1`
**用途**: ARI（Alternative Routing-ID Interpretation）是否启用

#### d3cold_allowed
**路径**: `/sys/bus/pci/devices/0000:00:00.0/d3cold_allowed`
**内容**: `0` 或 `1`
**可写**: 是
**用途**: 是否允许进入 D3cold 状态

## lspci 命令如何读取

### busybox 版本的 lspci

主要读取 `/sys/bus/pci/devices/*/uevent` 文件：

```c
// busybox lspci.c
char *uevent_filename = concat_path_file(fileName, "/uevent");
parser = config_open2(uevent_filename, fopen_for_read);

// 解析 uevent 文件中的：
// - PCI_CLASS
// - PCI_ID (vendor:device)
// - PCI_SUBSYS_ID
// - PCI_SLOT_NAME
// - DRIVER
```

### 标准 pciutils 版本的 lspci

主要读取 `/sys/bus/pci/devices/*/config` 文件：

```c
// pciutils lspci
fd = open("/sys/bus/pci/devices/0000:00:00.0/config", O_RDONLY);
read(fd, config, 256);  // 读取配置空间

// 然后解析配置空间：
// - 偏移 0x00: vendor ID
// - 偏移 0x02: device ID
// - 偏移 0x08: class code
// - 偏移 0x0A: status register
// - 偏移 0x0C: command register
// - 等等...
```

### lspci -vvv 显示的详细信息来源

`lspci -vvv` 会读取更多属性：

1. **基础信息**: `vendor`, `device`, `class`, `revision`
2. **配置空间**: `config` 文件（完整解析）
3. **资源信息**: `resource` 文件
4. **驱动信息**: `driver` 符号链接
5. **PCIe 信息**: `current_link_speed`, `current_link_width` 等
6. **桥接信息**: `secondary_bus_number`, `subordinate_bus_number`

## 属性文件创建位置

这些属性文件由内核在以下位置创建：

**文件**: `drivers/pci/pci-sysfs.c`

```c
// 基础属性
static struct attribute *pci_dev_attrs[] = {
    &dev_attr_vendor.attr,
    &dev_attr_device.attr,
    &dev_attr_class.attr,
    &dev_attr_subsystem_vendor.attr,
    &dev_attr_subsystem_device.attr,
    &dev_attr_revision.attr,
    &dev_attr_irq.attr,
    &dev_attr_resource.attr,
    &dev_attr_modalias.attr,
    // ...
};

// PCIe 特定属性
static struct attribute *pcie_dev_attrs[] = {
    &dev_attr_current_link_speed.attr,
    &dev_attr_current_link_width.attr,
    &dev_attr_max_link_width.attr,
    &dev_attr_max_link_speed.attr,
    NULL,
};

// 桥接器属性
static struct attribute *pci_bridge_attrs[] = {
    &dev_attr_subordinate_bus_number.attr,
    &dev_attr_secondary_bus_number.attr,
    NULL,
};
```

## 实际查看示例

```bash
# 查看所有属性
ls -1 /sys/bus/pci/devices/0000:00:00.0/

# 查看基础信息
cat /sys/bus/pci/devices/0000:00:00.0/vendor
cat /sys/bus/pci/devices/0000:00:00.0/device
cat /sys/bus/pci/devices/0000:00:00.0/class

# 查看 uevent（busybox lspci 使用）
cat /sys/bus/pci/devices/0000:00:00.0/uevent

# 查看配置空间（pciutils lspci 使用）
hexdump -C /sys/bus/pci/devices/0000:00:00.0/config | head -10

# 查看资源
cat /sys/bus/pci/devices/0000:00:00.0/resource

# 查看 PCIe 链路信息（如果是 PCIe 设备）
cat /sys/bus/pci/devices/0000:00:01.0/current_link_speed
cat /sys/bus/pci/devices/0000:00:01.0/current_link_width
```

## 总结

| 属性类型 | 主要用途 | 被谁使用 |
|---------|---------|---------|
| `uevent` | 设备基本信息 | busybox lspci |
| `config` | 完整配置空间 | pciutils lspci |
| `vendor`, `device`, `class` | 设备标识 | 所有 lspci |
| `resource` | 资源信息 | lspci -v |
| `driver` | 驱动信息 | lspci -k |
| `current_link_speed/width` | PCIe 链路信息 | lspci -vvv |
| `secondary_bus_number` | 桥接信息 | lspci -vvv |

这些属性文件提供了用户空间访问 PCI 设备信息的标准接口，`lspci` 只是其中一个使用者。

