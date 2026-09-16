# PCIe MSI/MSI-X 中断与 GIC ITS

> PCIe 中断从设备写到中断到达 CPU 的全链路。代码摘自本地内核树 6.12.58（`drivers/pci/msi/`、`drivers/irqchip/irq-gic-v3-its.c`）。

## 1. 三种中断机制对比

| 机制 | 原理 | 向量数 | 现状 |
|---|---|---|---|
| INTx | 共享 4 条 legacy 线（A/B/C/D），电平 | 4 | 兼容用，性能差 |
| MSI | 设备向特定地址写特定数据（MemWr TLP）触发 | 1~32 | 向量少 |
| **MSI-X** | 同 MSI，但表结构独立（Table + PBA） | 数百~数千 | **现代标配** |

**MSI 本质 = 一个 Memory Write TLP**：地址是中断控制器的 doorbell，数据携带向量号。没有线、没有电平、没有 ACK--天生适合虚拟化路由。

## 2. MSI-X 的表结构

```
MSI-X Capability
  ├─ Table BAR offset + BIR -> 指向某 BAR 内的 Vector Table
  │     每项 { Message Address(高/低), Message Upper Address, Message Data, Vector Control }
  └─ PBA（Pending Bit Array） -> mask 中的向量挂起位
```

- 每个 Table 项 = 一个独立向量：驱动给每项写 ITS 分配的 doorbell 地址+eventid
- **mask 一个向量不影响其他**（MSI 是整体 enable，这是 MSI-X 的核心优势）
- PBA 用于 masked 向量的 pending 查询

## 3. 内核分配 API

```c
/* drivers/pci/msi/api.c */
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
			  unsigned int max_vecs, unsigned int flags)
{
	return pci_alloc_irq_vectors_affinity(dev, min_vecs, max_vecs,
					      flags, NULL);
}
```

```c
/* 驱动标准用法：MSI-X 优先，回退 MSI，再回退 legacy */
nvecs = pci_alloc_irq_vectors(pdev, 1, 64,
                               PCI_IRQ_MSIX | PCI_IRQ_MSI | PCI_IRQ_LEGACY);
irq = pci_irq_vector(pdev, i);          /* 第 i 个向量 -> virq */
request_irq(irq, handler, 0, "dev-rx", dev);
pci_free_irq_vectors(pdev);             /* 配对释放（泄漏=向量耗尽，全设备失败）*/
```

flags 可加 `PCI_IRQ_AFFINITY` 自动按 NUMA 分散亲和性（NVMe/网卡多队列刚需）。

## 4. ARM64 上的落地：GICv3 ITS

```
设备 MSI 写 {LPI doorbell addr, eventid}
  -> GIC ITS
       eventid -> Device Table(按 RequesterID/BDF 索引) -> ITT -> LPI 号
  -> GICR redistributor -> 注入 CPU
  -> Linux virq（irq domain 层级：ITS msi_domain -> 上层）
```

- **Requester ID = BDF** 直接作为 ITS 的 device id：设备身份天然绑定
- 内核侧：`its_msi_domain` 是层级 irq_domain 的 parent，`msi_domain_alloc` 时为每个向量分配 LPI + 配 doorbell 地址/data 写回设备 MSI-X Table
- DT 描述：RC 节点 `msi-map = <0x0 &its 0x0 0x10000>`（BDF 段 -> ITS device id 段）
- **LPI 配置表（LPI tables）在内存里**，ITS 靠 DMA 访问--`GITS_BASER` 指向的表必须分配自 ITS 可访问的内存（non-cacheable 直通区域），这是 ITS 驱动初始化的核心工作

## 5. 调试

| 手段 | 用法 |
|---|---|
| `/proc/interrupts` | `IR-<n>-...` 前缀即 ITS 路由的 MSI；看每向量计数是否均衡 |
| `lspci -vv` 的 `MSI-X: Enable+ Count=N Masked-` | 使能状态、向量数 |
| `/sys/kernel/irq/<n>/` | domain 层级、亲和性 |
| dmesg `ITS: ...` | LPI 表分配/doorbell 配置失败会报 |
| 常见症状 | 收不到 MSI：设备侧 Table 没写成功（BAR 映射错）/ Bus Master 没开 / msi-map 不匹配；只收到第一个向量：驱动没用 `pci_irq_vector(i)` 而是复用 irq 0 |

## 6. 面试问答

**Q: MSI 和 MSI-X 区别？**
向量组织：MSI 一个使能位+连续向量编码进 data；MSI-X 每向量独立 Table 项（可 mask、可不同地址、数量大）。MSI 还要求向量号 2 的幂对齐。

**Q: MSI 写的到底是什么？**
一个普通 MemWr TLP：地址 = 中断控制器 doorbell（ARM 上是 ITS 的 GITS_TRANSLATER 地址），data 含 eventid。所以 MSI 失败常表现为"设备的寄存器写不进去"而不是中断问题。

**Q: ARM64 MSI 为什么离不开 ITS？**
LPI 是基于内存表（LPI tables）的消息中断，ITS 负责按 BDF 索引翻译 eventid->LPI；没有 ITS 就没有 per-device 的 MSI 路由（GICv2 时代用 GICD 的 software generated interrupt 模拟，向量极少）。

**Q: pci_alloc_irq_vectors 泄漏什么后果？**
每向量占一个 ITS LPI + irq_desc；不 free 则后续分配逐步失败，表现为全系统 MSI 设备 probe 报 `-ENOSPC`。
