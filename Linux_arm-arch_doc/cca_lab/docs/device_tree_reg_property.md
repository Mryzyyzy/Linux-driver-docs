# 设备树 reg 属性详解：为什么有多个元素？

## 问题：Tegra194 PCIe 节点的 reg 属性

```dts
pcie@14100000 {
    compatible = "nvidia,tegra194-pcie";
    reg = <0x00 0x14100000 0x0 0x00020000>, /* appl registers (128K)      */
          <0x00 0x30000000 0x0 0x00040000>, /* configuration space (256K) */
          <0x00 0x30040000 0x0 0x00040000>, /* iATU_DMA reg space (256K)  */
          <0x00 0x30080000 0x0 0x00040000>; /* DBI reg space (256K)       */
    reg-names = "appl", "config", "atu_dma", "dbi";
    
    #address-cells = <3>;
    #size-cells = <2>;
}
```

---

## 1. reg 属性的基本格式

### 1.1 标准格式

设备树中 `reg` 属性的标准格式是：

```
reg = <地址1 大小1 [地址2 大小2] ...>;
```

每个 `<地址 大小>` 对表示一个**内存区域**。

### 1.2 地址和大小由多个 cell 组成

- **地址的 cell 数**：由父节点的 `#address-cells` 决定
- **大小的 cell 数**：由父节点的 `#size-cells` 决定

---

## 2. Tegra194 PCIe 节点的分析

### 2.1 ⚠️ 关键理解：作用域问题

**重要规则**：节点自己的 `reg` 属性遵循**父节点**的 `#address-cells` 和 `#size-cells`，而不是自己的！

```dts
/ {
    #address-cells = <2>;  // ← 父节点定义（根节点）
    #size-cells = <2>;    // ← 父节点定义
    
    pcie@14100000 {
        #address-cells = <3>;  // ← 这是给子节点用的！
        #size-cells = <2>;     // ← 这是给子节点用的！
        
        reg = <...>;  // ← 这个 reg 遵循父节点的规则（2+2=4）
    }
}
```

### 2.2 节点自己的 reg 属性

PCIe 节点的 `reg` 属性遵循**父节点**（根节点）的规则：
- 父节点：`#address-cells = <2>`, `#size-cells = <2>`
- 所以 PCIe 节点的 `reg` = **2 + 2 = 4 个值** ✓

```dts
reg = <
    // 第一个区域：appl registers
    0x00           // 地址高 32 位（遵循父节点的 #address-cells = <2>）
    0x14100000     // 地址低 32 位
    0x0            // 大小高 32 位（遵循父节点的 #size-cells = <2>）
    0x00020000     // 大小低 32 位（128KB）
    
    // 第二个区域：configuration space
    0x00           // 地址高 32 位
    0x30000000     // 地址低 32 位
    0x0            // 大小高 32 位
    0x00040000     // 大小低 32 位（256KB）
    
    // ... 依此类推
>;
```

### 2.3 节点内部的 #address-cells 和 #size-cells 的作用

```dts
#address-cells = <3>;  // 这是给子节点（PCIe 设备）用的！
#size-cells = <2>;     // 这是给子节点（PCIe 设备）用的！
```

这些定义用于**子节点**（PCIe 设备）的地址表示：

```dts
pcie@14100000 {
    #address-cells = <3>;  // ← 子节点遵循这个
    #size-cells = <2>;     // ← 子节点遵循这个
    
    // PCIe 设备的 reg 属性（如果存在）
    // 会使用 3+2=5 个值来表示 PCIe 配置空间地址
}
```

**PCIe 配置空间地址格式**（3 个 cell）：
- Cell 1: 配置空间类型/标志
- Cell 2: 总线号（8 位）+ 设备号（5 位）+ 功能号（3 位）
- Cell 3: 寄存器偏移

---

## 3. 为什么需要 4 个内存区域？

### 3.1 PCIe 控制器的内存映射

PCIe 控制器需要多个不同的寄存器空间：

```
1. appl registers (0x14100000)
   └─ 应用层寄存器：PCIe 协议处理、链路管理

2. configuration space (0x30000000)
   └─ PCIe 配置空间：设备枚举、配置寄存器

3. iATU_DMA reg space (0x30040000)
   └─ iATU (Internal Address Translation Unit) DMA 寄存器
   └─ 用于地址转换和 DMA 操作

4. DBI reg space (0x30080000)
   └─ DBI (DesignWare Bridge Interface) 寄存器
   └─ 设计相关的控制寄存器
```

### 3.2 物理地址映射

```
物理地址空间：
├─ 0x14100000 - 0x1411FFFF: appl registers (128KB)
├─ 0x30000000 - 0x3003FFFF: configuration space (256KB)
├─ 0x30040000 - 0x3007FFFF: iATU_DMA reg space (256KB)
└─ 0x30080000 - 0x300BFFFF: DBI reg space (256KB)
```

---

## 4. 设备树解析过程

### 4.1 内核如何解析

```c
// drivers/pci/controller/pcie-tegra194.c

static int tegra194_pcie_probe(struct platform_device *pdev)
{
    struct tegra194_pcie *pcie;
    struct resource *res;
    
    // 解析第一个区域：appl
    res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "appl");
    pcie->appl_base = devm_ioremap_resource(&pdev->dev, res);
    
    // 解析第二个区域：config
    res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "config");
    pcie->config_base = devm_ioremap_resource(&pdev->dev, res);
    
    // 解析第三个区域：atu_dma
    res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "atu_dma");
    pcie->atu_dma_base = devm_ioremap_resource(&pdev->dev, res);
    
    // 解析第四个区域：dbi
    res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
    pcie->dbi_base = devm_ioremap_resource(&pdev->dev, res);
}
```

### 4.2 reg-names 的作用

```dts
reg-names = "appl", "config", "atu_dma", "dbi";
```

`reg-names` 为每个 reg 区域提供名称，方便通过名称访问：

```c
// 通过名称获取资源
res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "appl");
// 而不是通过索引
res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
```

---

## 5. 为什么不能合并成一个区域？

### 5.1 物理地址不连续

```
0x14100000 (appl)      ← 在 SoC 内部地址空间
0x30000000 (config)    ← 在 PCIe 地址空间
0x30040000 (atu_dma)  ← 在 PCIe 地址空间
0x30080000 (dbi)      ← 在 PCIe 地址空间
```

这些地址**物理上不连续**，无法合并成一个区域。

### 5.2 功能分离

不同的寄存器空间有不同的功能：
- **appl**: 应用层控制
- **config**: PCIe 配置空间
- **atu_dma**: 地址转换和 DMA
- **dbi**: 设计相关

分离管理更清晰。

---

## 6. 其他 PCIe 控制器的对比

### 6.1 简单的 PCIe 控制器

```dts
pcie@0 {
    reg = <0x0 0x10000000 0x0 0x1000000>;  // 只有一个区域
    // 所有寄存器都在一个连续地址空间
};
```

### 6.2 复杂的 PCIe 控制器（如 Tegra194）

```dts
pcie@14100000 {
    reg = <...>, <...>, <...>, <...>;  // 多个区域
    // 寄存器分散在多个地址空间
};
```

---

## 7. 地址 cell 数的含义

### 7.1 #address-cells = <3> 的含义

在 PCIe 设备树中，`#address-cells = <3>` 通常表示：

```
<配置空间地址(8位) 总线号(8位) 设备/功能号(16位)>
或
<高32位 中32位 低32位>
```

对于 Tegra194，可能是：
- Cell 1: 地址空间标识（0x00 = 内存空间）
- Cell 2: 地址高 32 位（通常为 0）
- Cell 3: 地址低 32 位

### 7.2 实际解析

```c
// 内核解析 reg 属性时
of_read_number(reg, 3);  // 读取 3 个 cell 作为地址
address = (cell[0] << 64) | (cell[1] << 32) | cell[2];

of_read_number(reg + 3, 2);  // 读取 2 个 cell 作为大小
size = (cell[3] << 32) | cell[4];
```

---

## 8. 总结

### 8.1 为什么有 4 个 reg 条目？

1. **物理地址不连续**：4 个不同的物理地址区域
2. **功能分离**：不同的寄存器空间有不同的功能
3. **硬件设计**：PCIe 控制器需要多个寄存器空间

### 8.2 每个 reg 条目的格式

```
<地址高32位 地址低32位 大小高32位 大小低32位>
```

在 Tegra194 中，地址和大小的高 32 位通常为 0，所以看起来是 4 个值。

### 8.3 reg-names 的作用

为每个区域提供有意义的名称，方便驱动通过名称访问，而不是通过索引。

---

## 9. 实际使用示例

```c
// 在驱动中访问不同的寄存器空间
void __iomem *appl_base;    // appl registers
void __iomem *config_base;   // configuration space
void __iomem *atu_dma_base; // iATU_DMA registers
void __iomem *dbi_base;     // DBI registers

// 读取 appl 寄存器
val = readl(appl_base + APPL_REG_OFFSET);

// 写入 config 空间
writel(val, config_base + CONFIG_REG_OFFSET);

// 配置 iATU
writel(src_addr, atu_dma_base + IATU_SRC_ADDR);
writel(dst_addr, atu_dma_base + IATU_DST_ADDR);
```

---

## 10. 参考资料

- [Device Tree Specification](https://www.devicetree.org/specifications/)
- [Linux Device Tree Documentation](https://www.kernel.org/doc/html/latest/devicetree/index.html)
- [Tegra PCIe Driver](https://elixir.bootlin.com/linux/latest/source/drivers/pci/controller/pcie-tegra194.c)

