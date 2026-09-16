# 预备知识
refer：
https://blog.csdn.net/hexiaolong2009/article/details/107592704

在 kernel  驱动中，实现 mmap 系统调用离不开两个关键步骤：
（1）内存分配 
（2）建立映射关系。
这刚好也对应了 DRM 中的 dumb_create 和 mmap 操作。
```
建立映射的方式（怎么映射）        内存分配的时机（什么时候分配）
─────────────────────            ──────────────────────────
① 一次性映射                      ③ mmap 之前分配
② Page Fault 缺页映射             ④ mmap 之中分配
                                 ⑤ fault 中分配
```
## 映射关系
先说映射关系，在 linux 驱动中建立映射关系的方法主要有如下两种：
一次性映射 —— 在 mmap 回调函数中，一次性建立好整块内存的映射关系，通常以 remap_pfn_range() 为代表 。
Page Fault —— mmap 先不建立映射关系，等上层触发缺页异常时，在 fault 中断处理函数中建立映射关系，缺哪块补哪块，通常以 vm_insert_page() 为代表。
## 内存分配
而内存分配的时机也会影响驱动程序的设计，大致分为如下三种：
1. 在 mmap 系统调用之前分配
2. 在 mmap 系统调用过程中分配
3. 在 fault 中断处理函数中分配
因此不同的分配时机 + 不同的映射机制，就会得到不同的 mmap 的实现策略。这也是为什么在 DRM 驱动中，各家的 _dumb_create_ 和 _mmap_ 实现代码差异很大的原因。
![[Pasted image 20260615140437.png|475]]


# 五个概念速览

先把它们归类——**两种是"建立映射的方式"，三种是"内存在什么时机分配"**，它们经常组合出现。


---

## 一、两种"建立映射的方式"

### ① 一次性映射（remap_pfn_range 为代表）

在 `mmap` 回调里**一口气把整块内存的页表全部建好**，函数返回时映射已完整。

```c
static int my_mmap(struct file *file, struct vm_area_struct *vma)
{
    return remap_pfn_range(vma, vma->vm_start,
                           pfn, size, vma->vm_page_prot);  // 整块一次建表
}
```

- 用户拿到地址后**第一次访问就不缺页**
- **要求物理连续**（remap_pfn_range 按连续 PFN 区间填表）
- 适合：显示 framebuffer、寄存器 MMIO、物理连续 DMA 缓冲

### ② Page Fault 缺页映射（vm_insert_page 为代表）

`mmap` 回调里**不建映射**，只登记一个 `.fault` 钩子。用户**访问到哪一页**，才触发缺页异常，在 fault 里**补那一页**。

```c
static vm_fault_t my_fault(struct vm_fault *vmf)
{
    struct page *page = ...;                  // 找到缺的那一页
    return vmf_insert_pfn(vmf->vma, vmf->address, page_to_pfn(page)); // 只补一页
}
```

- 用户访问新页 → 缺页 → fault → 补页，**缺哪补哪**
- **允许物理不连续**（每页单独插）
- 适合：大 buffer、可能用不满、物理散乱的内存（如 shmem、GPU 渲染 buffer）

**一句话对比**：
> 一次性 = 入住前把整栋楼装修好；Page Fault = 住到哪间才装修哪间。

---

## 二、三种"内存分配的时机"

### ③ 在 mmap 系统调用之前分配

驱动**初始化时（probe/init）就把内存分配好**，mmap 只负责映射。

```c
static int __init my_init(void)
{
    kaddr = kzalloc(PAGE_SIZE * 3, GFP_KERNEL);  // 加载时就分配
    return misc_register(&mdev);
}
```

- 你那个 30 行示例、mmapper 都是这种
- 特点：内存**常驻**，开机分配用到卸载；mmap 时直接拿来映射
- 适合：固定大小、必然要用的缓冲

### ④ 在 mmap 系统调用过程中分配

**等用户真的调 `mmap` 了，才在 mmap 回调里分配**内存，然后立即映射。

```c
static int my_mmap(struct file *file, struct vm_area_struct *vma)
{
    kaddr = kzalloc(size, GFP_KERNEL);   // 这时才分配
    return remap_pfn_range(vma, vma->vm_start,
                           virt_to_phys(kaddr) >> PAGE_SHIFT, size, prot);
}
```

- 不 mmap 就不占内存，按需分配
- DRM dumb buffer 接近这种思路：`CREATE_DUMB` 时分配，`mmap` 时映射（分配早于 mmap 一点，但都是"用到才分配"）

### ⑤ 在 fault 中断处理函数中分配

**最懒**：连分配都推迟到缺页时。用户访问某页 → 缺页 → 在 fault 里**现场分配这一页**再映射。

```c
static vm_fault_t my_fault(struct vm_fault *vmf)
{
    struct page *page = alloc_page(GFP_KERNEL);  // 缺页时才分配这一页
    vmf->page = page;
    return 0;
}
```

- 分配和映射**都**推迟到真正访问时
- 最省内存（碰过的页才分配），但每次首次访问有开销
- 适合：超大稀疏映射、不确定用户会访问多少

---

## 三、组合关系（关键理解）

"映射方式"和"分配时机"会**搭配组合**：

| 典型组合 | 映射方式 | 分配时机 | 代表 |
|----------|----------|----------|------|
| 预分配 + 一次性映射 | ① | ③ | 你的示例 / mmapper / DRM CMA dumb buffer |
| mmap 时分配 + 一次性映射 | ① | ④ | 按需分配的连续缓冲 |
| 缺页分配 + 缺页映射 | ② | ⑤ | shmem、匿名内存、稀疏大 buffer |

最常见两端：

```text
最"勤快"：③ 之前分配 + ① 一次性映射
   mmap 前就备好内存，mmap 时整块建表，访问零缺页
   → DRM 显示 framebuffer 走这条（要喂硬件 DMA）

最"懒惰"：⑤ fault 分配 + ② 缺页映射
   啥都不提前做，碰一页才分配+映射一页
   → 普通用户内存、GPU 渲染 buffer 常走这条
```

---

## 四、对应到 DRM

```text
DRM dumb buffer (linlon-dp, 显示用):
   分配时机 = CREATE_DUMB 时 (≈ ③/④，用到才分配但早于 mmap)
   映射方式 = ① 一次性 (drm_gem_dma_mmap → remap_pfn_range)
   原因：显示控制器 DMA 要物理连续 + 访问不能缺页

DRM shmem buffer (GPU 渲染用):
   分配时机 = ⑤ 接近缺页分配 (get_pages 懒分配)
   映射方式 = ② Page Fault (drm_gem_shmem_fault → vmf_insert_pfn)
   原因：buffer 大、可不连续、用不满
```

---

## 五、一句话总结

- **① 一次性映射**：mmap 时整块建表，要连续，访问不缺页（`remap_pfn_range`）
- **② Page Fault 映射**：mmap 不建表，访问时缺哪补哪，可不连续（`vm_insert_page`）
- **③ mmap 前分配**：驱动加载时就备好内存
- **④ mmap 中分配**：用户调 mmap 时才分配
- **⑤ fault 中分配**：访问到才现场分配那一页

**前两个回答"怎么映射"，后三个回答"何时分配"，实际驱动里两两组合**：显示 framebuffer 用"③/④ + ①"（勤快、要连续喂硬件），普通/渲染内存用"⑤ + ②"（懒惰、省内存）。