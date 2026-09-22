# PCIe 驱动类型：芯片厂家通常写什么？

## 核心答案

**是的，芯片厂家（如 NVIDIA、Qualcomm、TI、Rockchip 等）通常写的是 PCIe Root Complex（根复合体）驱动。**

但 PCIe 驱动还有其他类型，取决于芯片的角色。

---

## 1. PCIe 设备的两种角色

### 1.1 Root Complex（根复合体）

```
┌─────────────────────────────────┐
│   SoC (如 Tegra)                │
│  ┌───────────────────────────┐  │
│  │  PCIe Root Complex        │  │ ← 芯片厂家写这个驱动
│  │  (Host Controller)        │  │
│  └───────────┬───────────────┘  │
└──────────────┼──────────────────┘
               │ PCIe 总线
               │
    ┌──────────┴──────────┐
    │                       │
┌───▼────┐            ┌────▼───┐
│ PCIe   │            │ PCIe   │
│ 设备   │            │ 设备   │
└────────┘            └────────┘
```

**特点**：
- 作为 PCIe 主机
- 管理 PCIe 总线
- 枚举和管理连接的设备
- **芯片厂家通常写这种驱动**

### 1.2 Endpoint（端点设备）

```
┌────────┐
│ PCIe   │ ← 如果芯片作为 PCIe 设备（不常见）
│ 设备   │
└────────┘
```

**特点**：
- 作为 PCIe 设备
- 连接到其他主机的 PCIe 总线
- **较少见，通常是专用芯片**

---

## 2. 芯片厂家写的驱动类型

### 2.1 大多数情况：Root Complex 驱动

**芯片厂家通常写的是 Root Complex 驱动**，因为：

1. **SoC 通常作为主机**：
   - 嵌入式 SoC（如 Tegra、Snapdragon）通常作为系统的主机
   - 需要连接各种 PCIe 设备（网卡、存储卡等）

2. **需要硬件特定支持**：
   - 每个 SoC 的 PCIe 控制器硬件不同
   - 需要芯片厂家提供驱动来初始化和管理硬件

3. **提供基础设施**：
   - 配置空间访问
   - 地址转换
   - 中断处理

### 2.2 实际例子

#### NVIDIA Tegra

```c
// drivers/pci/controller/pci-tegra.c
// Root Complex 驱动
static int tegra_pcie_probe(struct platform_device *pdev)
{
    // 初始化 Tegra PCIe Root Complex
    // 提供配置空间访问
    // 管理 PCIe 端口
}
```

#### Qualcomm Snapdragon

```c
// drivers/pci/controller/dwc/pcie-qcom.c
// Root Complex 驱动
static int qcom_pcie_probe(struct platform_device *pdev)
{
    // 初始化 Qualcomm PCIe Root Complex
}
```

#### Rockchip

```c
// drivers/pci/controller/dwc/pcie-rockchip-host.c
// Root Complex 驱动
static int rockchip_pcie_probe(struct platform_device *pdev)
{
    // 初始化 Rockchip PCIe Root Complex
}
```

#### TI Keystone

```c
// drivers/pci/controller/dwc/pci-keystone.c
// Root Complex 驱动
static int ks_pcie_probe(struct platform_device *pdev)
{
    // 初始化 TI PCIe Root Complex
}
```

---

## 3. Root Complex 驱动实现的功能

### 3.1 核心功能

所有 Root Complex 驱动都需要实现：

```c
// 1. 配置空间访问
static struct pci_ops pcie_ops = {
    .map_bus = pcie_map_bus,
    .read = pcie_config_read,
    .write = pcie_config_write,
};

// 2. 地址转换
static void pcie_setup_translations(struct pcie *pcie)
{
    // 设置 PCIe 地址到系统内存的映射
}

// 3. 中断处理
static irqreturn_t pcie_irq_handler(int irq, void *arg)
{
    // 处理 PCIe 中断
}
```

### 3.2 硬件特定功能

每个芯片厂家需要实现：

1. **硬件初始化**：
   - 时钟配置
   - 复位控制
   - PHY 初始化
   - 电源管理

2. **寄存器访问**：
   - 每个 SoC 的寄存器布局不同
   - 需要芯片特定的寄存器定义

3. **特殊功能**：
   - 多端口管理
   - 链路训练
   - 错误处理

---

## 4. 驱动架构模式

### 4.1 通用框架 + 芯片特定驱动

Linux 内核采用了分层架构：

```
┌─────────────────────────────────┐
│   PCI 核心子系统                │
│   (drivers/pci/)                │
│   - 设备枚举                    │
│   - 总线管理                    │
│   - 通用接口                    │
└────────────┬────────────────────┘
             │
             │ 调用
             │
┌────────────▼────────────────────┐
│   PCIe Host Controller 框架     │
│   (drivers/pci/controller/)     │
│   - 通用 Host Controller 接口   │
└────────────┬────────────────────┘
             │
             │ 实现
             │
┌────────────▼────────────────────┐
│   芯片特定驱动                  │
│   - pci-tegra.c (NVIDIA)        │
│   - pcie-qcom.c (Qualcomm)      │
│   - pcie-rockchip.c (Rockchip)  │
│   - pci-keystone.c (TI)         │
└─────────────────────────────────┘
```

### 4.2 使用通用 IP 的情况

有些芯片使用标准的 PCIe IP（如 Synopsys DesignWare），可以使用通用驱动：

```c
// drivers/pci/controller/dwc/pcie-designware.c
// 通用 DesignWare PCIe 驱动

// 芯片只需要提供平台特定配置
static const struct dw_pcie_ops dw_pcie_ops = {
    .link_up = qcom_pcie_link_up,
    .start_link = qcom_pcie_start_link,
};
```

---

## 5. 其他类型的 PCIe 驱动

### 5.1 Endpoint 驱动（较少见）

如果芯片作为 PCIe 设备（Endpoint），需要写 Endpoint 驱动：

```c
// drivers/pci/endpoint/
// PCIe Endpoint 驱动
// 用于芯片作为 PCIe 设备的情况
```

**使用场景**：
- 专用加速卡
- 协处理器
- 特殊用途芯片

### 5.2 Switch 驱动（桥接器）

```c
// drivers/pci/switch/
// PCIe Switch 驱动
// 用于 PCIe 交换芯片
```

---

## 6. 芯片厂家驱动的特点

### 6.1 必须实现的功能

所有 Root Complex 驱动都必须：

1. ✅ **实现 `pci_ops`**：提供配置空间访问
2. ✅ **注册 `pci_host_bridge`**：让内核能够枚举设备
3. ✅ **处理地址转换**：将 PCIe 地址映射到系统内存
4. ✅ **支持中断**：Legacy 和/或 MSI/MSI-X

### 6.2 芯片特定的部分

每个芯片厂家需要：

1. **硬件初始化序列**：
   ```c
   // Tegra 特定的初始化
   tegra_pcie_power_on()
   tegra_pcie_phy_enable()
   tegra_pcie_enable_controller()
   ```

2. **寄存器定义**：
   ```c
   // 每个芯片的寄存器不同
   #define AFI_PCIE_CONFIG    0x0f8  // Tegra
   #define PCIE_CFG_ADDR      0x1000 // 其他芯片
   ```

3. **设备树绑定**：
   ```dts
   // 每个芯片的设备树属性不同
   compatible = "nvidia,tegra194-pcie";
   compatible = "qcom,pcie-apq8084";
   ```

---

## 7. 实际例子对比

### 7.1 NVIDIA Tegra

```c
// drivers/pci/controller/pci-tegra.c
// 特点：
// - 多端口支持
// - 自定义寄存器布局
// - Tegra 特定的 PHY 控制
```

### 7.2 Qualcomm Snapdragon

```c
// drivers/pci/controller/dwc/pcie-qcom.c
// 特点：
// - 使用 DesignWare IP
// - Qualcomm 特定的配置
// - 电源管理集成
```

### 7.3 Rockchip

```c
// drivers/pci/controller/dwc/pcie-rockchip-host.c
// 特点：
// - 使用 DesignWare IP
// - Rockchip 特定的时钟和复位
```

---

## 8. 总结

### 8.1 芯片厂家通常写什么？

| 驱动类型 | 是否常见 | 说明 |
|---------|---------|------|
| **Root Complex 驱动** | ✅ **非常常见** | 芯片作为 PCIe 主机 |
| **Endpoint 驱动** | ❌ 较少见 | 芯片作为 PCIe 设备 |
| **Switch 驱动** | ❌ 较少见 | PCIe 交换芯片 |

### 8.2 为什么写 Root Complex 驱动？

1. **SoC 通常作为主机**：需要连接各种 PCIe 设备
2. **硬件特定**：每个 SoC 的 PCIe 控制器不同
3. **基础设施**：提供 PCIe 总线管理功能

### 8.3 关键点

- ✅ **芯片厂家写的是 Root Complex 驱动**
- ✅ **实现 PCIe Root Complex 的功能**
- ✅ **提供配置空间访问、地址转换、中断处理**
- ✅ **每个芯片的硬件实现不同，需要特定驱动**

---

## 9. 参考资料

- [Linux PCI 子系统](https://www.kernel.org/doc/html/latest/PCI/index.html)
- [PCIe Root Complex 规范](https://pcisig.com/)
- [各芯片厂家的 PCIe 驱动源码](https://elixir.bootlin.com/linux/latest/source/drivers/pci/controller/)





