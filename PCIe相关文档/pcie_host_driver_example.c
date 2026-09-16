// SPDX-License-Identifier: GPL-2.0
/*
 * Example PCIe Host Controller Driver
 * 
 * This is a template showing how to write a PCIe host controller driver
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/phy/phy.h>

// 1. 定义驱动私有数据结构
struct my_pcie {
	struct device *dev;
	void __iomem *base;           // PCIe 控制器寄存器基地址
	struct clk *clk;              // 时钟
	struct reset_control *rst;    // 复位控制
	struct phy *phy;              // PHY
	struct irq_domain *irq_domain;
	int irq;
	
	// 其他硬件特定数据
	u32 busnr;                    // 总线号
};

// 2. 实现 PCI 配置空间读写操作
// 这是 PCIe host controller 驱动的核心部分

// 读取 Root Complex 自己的配置空间
static int my_pcie_rd_own_conf(struct my_pcie *pcie, int where, int size, u32 *val)
{
	void __iomem *addr = pcie->base + 0x1000 + where; // 假设配置空间在 base+0x1000
	
	if (!IS_ALIGNED((uintptr_t)addr, size)) {
		*val = 0;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	
	if (size == 4) {
		*val = readl(addr);
	} else if (size == 2) {
		*val = readw(addr);
	} else if (size == 1) {
		*val = readb(addr);
	} else {
		*val = 0;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	
	return PCIBIOS_SUCCESSFUL;
}

// 写入 Root Complex 自己的配置空间
static int my_pcie_wr_own_conf(struct my_pcie *pcie, int where, int size, u32 val)
{
	void __iomem *addr = pcie->base + 0x1000 + where;
	u32 mask, tmp;
	
	if (size == 4) {
		writel(val, addr);
		return PCIBIOS_SUCCESSFUL;
	}
	
	// 对于非对齐的写入，需要读-修改-写
	mask = ~(((1 << (size * 8)) - 1) << ((where & 0x3) * 8));
	tmp = readl(addr) & mask;
	tmp |= val << ((where & 0x3) * 8);
	writel(tmp, addr);
	
	return PCIBIOS_SUCCESSFUL;
}

// 读取下游设备的配置空间
static int my_pcie_rd_other_conf(struct my_pcie *pcie, struct pci_bus *bus,
				  u32 devfn, int where, int size, u32 *val)
{
	// 实现通过 PCIe 配置事务访问下游设备
	// 这通常涉及：
	// 1. 设置配置地址（bus, dev, func, register）
	// 2. 触发配置读事务
	// 3. 等待完成并读取数据
	
	// 示例：通过 Type1 配置访问
	u32 addr = PCIE_ECAM_ADDR(bus->number, PCI_SLOT(devfn), 
				   PCI_FUNC(devfn), where);
	
	// 触发配置读
	writel(addr, pcie->base + PCIE_CFG_ADDR_OFFSET);
	
	// 等待完成
	udelay(1);
	
	// 读取数据
	if (size == 4) {
		*val = readl(pcie->base + PCIE_CFG_DATA_OFFSET);
	} else if (size == 2) {
		*val = readw(pcie->base + PCIE_CFG_DATA_OFFSET + (where & 0x2));
	} else if (size == 1) {
		*val = readb(pcie->base + PCIE_CFG_DATA_OFFSET + (where & 0x3));
	} else {
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	
	return PCIBIOS_SUCCESSFUL;
}

// 写入下游设备的配置空间
static int my_pcie_wr_other_conf(struct my_pcie *pcie, struct pci_bus *bus,
				  u32 devfn, int where, int size, u32 val)
{
	// 类似读取，但执行写操作
	u32 addr = PCIE_ECAM_ADDR(bus->number, PCI_SLOT(devfn),
				   PCI_FUNC(devfn), where);
	
	writel(addr, pcie->base + PCIE_CFG_ADDR_OFFSET);
	udelay(1);
	
	if (size == 4) {
		writel(val, pcie->base + PCIE_CFG_DATA_OFFSET);
	} else if (size == 2) {
		writew(val, pcie->base + PCIE_CFG_DATA_OFFSET + (where & 0x2));
	} else if (size == 1) {
		writeb(val, pcie->base + PCIE_CFG_DATA_OFFSET + (where & 0x3));
	} else {
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	
	return PCIBIOS_SUCCESSFUL;
}

// PCI 配置空间读写接口（由 PCI 核心调用）
static int my_pcie_rd_conf(struct pci_bus *bus, u32 devfn, int where,
			    int size, u32 *val)
{
	struct my_pcie *pcie = bus->sysdata;
	
	// 如果是 Root Complex 自己（bus 0, dev 0）
	if (pci_is_root_bus(bus) && PCI_SLOT(devfn) == 0) {
		return my_pcie_rd_own_conf(pcie, where, size, val);
	}
	
	// 否则读取下游设备
	return my_pcie_rd_other_conf(pcie, bus, devfn, where, size, val);
}

static int my_pcie_wr_conf(struct pci_bus *bus, u32 devfn, int where,
			   int size, u32 val)
{
	struct my_pcie *pcie = bus->sysdata;
	
	if (pci_is_root_bus(bus) && PCI_SLOT(devfn) == 0) {
		return my_pcie_wr_own_conf(pcie, where, size, val);
	}
	
	return my_pcie_wr_other_conf(pcie, bus, devfn, where, size, val);
}

// 定义 PCI 操作结构
static struct pci_ops my_pcie_ops = {
	.read = my_pcie_rd_conf,
	.write = my_pcie_wr_conf,
};

// 3. 硬件初始化函数
static int my_pcie_init_hw(struct my_pcie *pcie)
{
	int ret;
	
	// 使能时钟
	ret = clk_prepare_enable(pcie->clk);
	if (ret) {
		dev_err(pcie->dev, "Failed to enable clock\n");
		return ret;
	}
	
	// 释放复位
	ret = reset_control_deassert(pcie->rst);
	if (ret) {
		dev_err(pcie->dev, "Failed to deassert reset\n");
		goto err_clk;
	}
	
	// 初始化 PHY
	ret = phy_init(pcie->phy);
	if (ret) {
		dev_err(pcie->dev, "Failed to init PHY\n");
		goto err_rst;
	}
	
	ret = phy_power_on(pcie->phy);
	if (ret) {
		dev_err(pcie->dev, "Failed to power on PHY\n");
		goto err_phy_init;
	}
	
	// 等待链路稳定
	msleep(100);
	
	// 配置 PCIe 控制器寄存器
	// 例如：设置链路宽度、速度等
	// writel(0x12345678, pcie->base + SOME_REG_OFFSET);
	
	return 0;
	
err_phy_init:
	phy_exit(pcie->phy);
err_rst:
	reset_control_assert(pcie->rst);
err_clk:
	clk_disable_unprepare(pcie->clk);
	return ret;
}

// 4. 中断处理
static void my_pcie_irq_handler(struct irq_desc *desc)
{
	struct my_pcie *pcie = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 status;
	
	chained_irq_enter(chip, desc);
	
	// 读取中断状态
	status = readl(pcie->base + PCIE_INT_STATUS_OFFSET);
	
	// 处理各种中断
	if (status & PCIE_INT_LINK_UP) {
		dev_info(pcie->dev, "PCIe link up\n");
	}
	
	if (status & PCIE_INT_LINK_DOWN) {
		dev_info(pcie->dev, "PCIe link down\n");
	}
	
	// 清除中断
	writel(status, pcie->base + PCIE_INT_CLEAR_OFFSET);
	
	chained_irq_exit(chip, desc);
}

// 5. 从设备树解析资源
static int my_pcie_parse_dt(struct my_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct device_node *np = dev->of_node;
	int ret;
	
	// 获取寄存器基地址
	pcie->base = devm_platform_ioremap_resource(to_platform_device(dev), 0);
	if (IS_ERR(pcie->base)) {
		return PTR_ERR(pcie->base);
	}
	
	// 获取时钟
	pcie->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(pcie->clk)) {
		return PTR_ERR(pcie->clk);
	}
	
	// 获取复位控制
	pcie->rst = devm_reset_control_get(dev, NULL);
	if (IS_ERR(pcie->rst)) {
		return PTR_ERR(pcie->rst);
	}
	
	// 获取 PHY
	pcie->phy = devm_phy_get(dev, "pcie-phy");
	if (IS_ERR(pcie->phy)) {
		return PTR_ERR(pcie->phy);
	}
	
	// 获取中断
	pcie->irq = platform_get_irq(to_platform_device(dev), 0);
	if (pcie->irq < 0) {
		return pcie->irq;
	}
	
	// 解析总线号（可选，默认从设备树获取）
	ret = of_pci_get_bus_range(np, &pcie->busnr);
	if (ret) {
		pcie->busnr = 0; // 默认从 0 开始
	}
	
	return 0;
}

// 6. 平台驱动 probe 函数（核心入口）
static int my_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct my_pcie *pcie;
	int ret;
	
	// 分配 PCI host bridge（包含私有数据）
	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!bridge) {
		return -ENOMEM;
	}
	
	// 获取私有数据指针
	pcie = pci_host_bridge_priv(bridge);
	pcie->dev = dev;
	
	// 解析设备树
	ret = my_pcie_parse_dt(pcie);
	if (ret) {
		dev_err(dev, "Failed to parse device tree\n");
		return ret;
	}
	
	// 初始化硬件
	ret = my_pcie_init_hw(pcie);
	if (ret) {
		dev_err(dev, "Failed to init hardware\n");
		return ret;
	}
	
	// 设置中断
	ret = devm_request_irq(dev, pcie->irq, my_pcie_irq_handler,
				IRQF_SHARED, "my-pcie", pcie);
	if (ret) {
		dev_err(dev, "Failed to request IRQ\n");
		goto err_hw;
	}
	
	// 设置 host bridge 的 sysdata 和 ops
	bridge->sysdata = pcie;
	bridge->ops = &my_pcie_ops;
	bridge->busnr = pcie->busnr;
	
	// 设置资源窗口（IO、Memory、Prefetch Memory）
	// 这些通常从设备树解析
	ret = of_pci_parse_bus_range(dev->of_node, &bridge->busn);
	if (ret) {
		bridge->busn.start = pcie->busnr;
		bridge->busn.end = 255;
	}
	
	// 解析资源窗口（ranges）
	pci_parse_request_of_pci_ranges(dev, &bridge->windows, &bridge->dma_ranges, NULL);
	
	// 启动 PCIe 枚举和扫描
	ret = pci_host_probe(bridge);
	if (ret < 0) {
		dev_err(dev, "Failed to probe PCI host\n");
		goto err_hw;
	}
	
	platform_set_drvdata(pdev, pcie);
	
	dev_info(dev, "PCIe host controller initialized\n");
	return 0;
	
err_hw:
	phy_power_off(pcie->phy);
	phy_exit(pcie->phy);
	reset_control_assert(pcie->rst);
	clk_disable_unprepare(pcie->clk);
	return ret;
}

// 7. 平台驱动 remove 函数
static void my_pcie_remove(struct platform_device *pdev)
{
	struct my_pcie *pcie = platform_get_drvdata(pdev);
	
	// 停止 PCIe 操作
	// pci_stop_root_bus(...);
	
	// 关闭硬件
	phy_power_off(pcie->phy);
	phy_exit(pcie->phy);
	reset_control_assert(pcie->rst);
	clk_disable_unprepare(pcie->clk);
}

// 8. 设备树匹配表
static const struct of_device_id my_pcie_of_match[] = {
	{ .compatible = "my-company,my-pcie", },
	{ },
};
MODULE_DEVICE_TABLE(of, my_pcie_of_match);

// 9. 平台驱动结构
static struct platform_driver my_pcie_driver = {
	.driver = {
		.name = "my-pcie",
		.of_match_table = my_pcie_of_match,
	},
	.probe = my_pcie_probe,
	.remove_new = my_pcie_remove,
};

// 10. 模块初始化和退出
module_platform_driver(my_pcie_driver);

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Example PCIe Host Controller Driver");
MODULE_LICENSE("GPL v2");

