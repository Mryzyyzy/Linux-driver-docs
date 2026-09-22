# 设备树 #address-cells 和 #size-cells 的作用域

## 核心规则

**节点自己的 `reg` 属性遵循父节点的 `#address-cells` 和 `#size-cells`，而不是自己的！**

---

## 1. 作用域规则详解

### 1.1 基本规则

```dts
parent {
    #address-cells = <2>;
    #size-cells = <2>;
    
    child {
        #address-cells = <3>;  // ← 这是给子节点的子节点用的！
        #size-cells = <2>;     // ← 这是给子节点的子节点用的！
        
        reg = <...>;  // ← 这个 reg 遵循父节点（parent）的规则：2+2=4
    }
}
```

### 1.2 实际示例：Tegra194 PCIe

```dts
/ {  // 根节点
    #address-cells = <2>;  // ← 定义 1
    #size-cells = <2>;     // ← 定义 1
    
    pcie@14100000 {
        #address-cells = <3>;  // ← 定义 2（给子节点用）
        #size-cells = <2>;     // ← 定义 2（给子节点用）
        
        // 这个 reg 遵循根节点的定义 1（2+2=4）
        reg = <0x00 0x14100000 0x0 0x00020000>,  // 4 个值
              <0x00 0x30000000 0x0 0x00040000>,  // 4 个值
              <0x00 0x30040000 0x0 0x00040000>,  // 4 个值
              <0x00 0x30080000 0x0 0x00040000>;  // 4 个值
        
        // 如果有子节点，子节点的 reg 会遵循定义 2（3+2=5）
        // 例如：PCIe 设备的配置空间地址
    }
}
```

---

## 2. 为什么 PCIe 节点定义 #address-cells = <3>？

### 2.1 PCIe 配置空间地址格式

PCIe 设备的地址需要 3 个 cell 来表示：

```
<配置空间类型 总线/设备/功能号 寄存器偏移>
```

例如：
```dts
pcie@14100000 {
    #address-cells = <3>;  // ← 用于子节点
    #size-cells = <2>;
    
    // 假设有一个 PCIe 设备
    device@0 {
        reg = <0x00        // 配置空间类型（0x00 = 配置空间）
              0x00010000   // 总线1，设备0，功能0
              0x00000000   // 寄存器偏移 0
              0x0          // 大小高 32 位
              0x00001000>; // 大小低 32 位（4KB）
        // 总共 3+2=5 个值
    }
}
```

### 2.2 PCIe 地址的 3 个 cell 含义

```
Cell 1: 配置空间类型
  - 0x00: 配置空间
  - 0x01: I/O 空间
  - 0x02: 内存空间（32位）
  - 0x03: 内存空间（64位）

Cell 2: 总线/设备/功能号
  - 位 [31:24]: 保留
  - 位 [23:16]: 总线号（8位）
  - 位 [15:11]: 设备号（5位）
  - 位 [10:8]:  功能号（3位）
  - 位 [7:0]:   保留

Cell 3: 寄存器偏移
  - 在配置空间内的偏移
```

---

## 3. 完整的层次结构示例

```dts
/ {  // 根节点
    #address-cells = <2>;  // 规则 A
    #size-cells = <2>;     // 规则 A
    
    soc {
        #address-cells = <2>;  // 规则 B
        #size-cells = <2>;     // 规则 B
        
        pcie@14100000 {
            // 自己的 reg 遵循规则 A（父节点是根节点）
            reg = <0x00 0x14100000 0x0 0x00020000>;  // 2+2=4
            
            #address-cells = <3>;  // 规则 C（给子节点用）
            #size-cells = <2>;     // 规则 C（给子节点用）
            
            // 如果有子节点（PCIe 设备）
            device@0 {
                // 这个 reg 遵循规则 C（父节点是 pcie@14100000）
                reg = <0x00 0x00010000 0x00000000 0x0 0x00001000>;  // 3+2=5
            }
        }
    }
}
```

---

## 4. 验证方法

### 4.1 在驱动中验证

```c
// 解析 PCIe 节点的 reg 属性
struct resource *res;
int i = 0;

// 获取第一个区域（appl）
res = platform_get_resource(pdev, IORESOURCE_MEM, i++);
// res->start = 0x14100000
// res->end = 0x1411FFFF

// 获取第二个区域（config）
res = platform_get_resource(pdev, IORESOURCE_MEM, i++);
// res->start = 0x30000000
// res->end = 0x3003FFFF

// 每个资源都是通过 4 个值解析出来的（2+2）
```

### 4.2 使用 reg-names

```c
// 通过名称获取资源（更清晰）
res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "appl");
res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "config");
res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "atu_dma");
res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
```

---

## 5. 常见误解

### 误解 1：节点自己的 reg 遵循自己的定义

```dts
// ❌ 错误理解
node {
    #address-cells = <3>;
    reg = <...>;  // 认为应该是 3+2=5 个值
}

// ✅ 正确理解
node {
    #address-cells = <3>;  // 这是给子节点用的
    reg = <...>;  // 遵循父节点的定义（可能是 2+2=4）
}
```

### 误解 2：所有 reg 都遵循同一个规则

```dts
// ❌ 错误理解
// 认为所有 reg 都遵循同一个 #address-cells

// ✅ 正确理解
// 每个节点的 reg 遵循其父节点的规则
```

---

## 6. 总结

### 6.1 关键规则

1. **节点自己的 `reg`**：遵循**父节点**的 `#address-cells` 和 `#size-cells`
2. **子节点的 `reg`**：遵循**当前节点**的 `#address-cells` 和 `#size-cells`
3. **作用域是向下的**：定义影响子节点，不影响自己

### 6.2 Tegra194 PCIe 的情况

| 属性 | 值 | 作用对象 |
|------|-----|---------|
| 父节点的 `#address-cells` | 2 | PCIe 节点的 `reg` |
| 父节点的 `#size-cells` | 2 | PCIe 节点的 `reg` |
| PCIe 节点的 `#address-cells` | 3 | PCIe 子节点（设备）的 `reg` |
| PCIe 节点的 `#size-cells` | 2 | PCIe 子节点（设备）的 `reg` |

### 6.3 为什么这样设计？

- **灵活性**：不同层次的节点可以使用不同的地址表示方式
- **PCIe 特殊性**：PCIe 配置空间地址需要 3 个 cell 的特殊格式
- **向后兼容**：保持与标准设备树格式的兼容性

---

## 7. 参考资料

- [Device Tree Specification - reg property](https://www.devicetree.org/specifications/)
- [Linux Device Tree Documentation](https://www.kernel.org/doc/html/latest/devicetree/index.html)
- [PCIe Device Tree Binding](https://www.kernel.org/doc/html/latest/devicetree/bindings/pci/pci.txt)





