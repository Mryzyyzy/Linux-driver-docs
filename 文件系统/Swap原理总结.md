### Swap 原理总览（从内存页到块设备）

本文单独总结 Linux 下 Swap 的工作原理，重点放在「从内存页到块设备」的完整链路，以及和文件系统 / 块设备子系统的关系。

---

### 1. Swap 是什么？解决什么问题？

**目标**：在物理内存（RAM）不够用时，通过把暂时不用的内存页写到磁盘上的一块区域（Swap 设备），让系统“看起来”有更大的可用内存。

- **匿名内存（anonymous memory）**：
  - 进程堆（`malloc/new`）、栈、匿名 `mmap`。
  - 这些页**没有后备文件**，不能简单丢掉后再从文件加载回来。
  - 当内存紧张时，只能写到 Swap。
- **有后备文件的页**（文件页、页缓存）：
  - 可以通过回写 / 丢缓存来回收，用到时再从文件系统读回来。

可以理解为：

> **Swap = 匿名内存的“磁盘后备仓库”**。  
> 虚拟内存 = 物理内存 + 所有激活 Swap 设备能容纳的页数。

---

### 2. Swap 设备：从块设备到 `swap_info_struct`

Swap 可以基于两种载体：

- **Swap 分区**：例如 `/dev/sda2`，通过 `mkswap /dev/sda2` 格式化。
- **Swap 文件**：例如 `/swapfile`，在某个文件系统上的一个普通文件。

无论是哪种形式，**在内核内部都会被抽象成一个 `swap_info_struct`**：

```c
struct swap_info_struct {
    struct block_device *bdev;   // Swap 所在的块设备（分区/文件对应的 bdev）
    unsigned long *swap_map;     // 每个 Swap 槽（页大小）的使用状态
    unsigned long max;           // 该 Swap 设备最多能容纳多少页（总槽数）
    // ... 还包括优先级、标志、链表等
};
```

#### 2.1 `swapon` 激活流程（简化）

用户空间：

```bash
swapon /dev/sda2
```

内核系统调用（简化示意）：

```c
SYSCALL_DEFINE2(swapon, const char __user *, specialfile, int, swap_flags)
{
    struct block_device *bdev;
    struct swap_info_struct *p;

    // 1. 路径解析：/dev/sda2 → inode → i_rdev → block_device
    bdev = blkdev_get_by_path(specialfile, ...);

    // 2. 检查 / 格式化为 Swap 分区
    if (!is_swap_partition(bdev)) {
        format_swap_partition(bdev);  // 写入 SWAPSPACE2 头等信息
    }

    // 3. 分配 swap_info_struct
    p = alloc_swap_info();
    p->bdev = bdev;   // 记录块设备引用

    // 4. 读取 Swap 头部信息（签名、容量、坏块等）
    read_swap_header(p);

    // 5. 初始化 swap_map，加入全局 Swap 列表
    enable_swap(p);
}
```

从此开始，这个块设备就被视为一个**线性页槽数组**：

- 槽大小：`PAGE_SIZE`
- 槽编号：`0 .. p->max - 1`
- `swap_map[i]`：记录第 `i` 个槽的使用情况（是否被占用、引用计数等）

---

### 3. 关键抽象：swap entry（页到 Swap 位置的“指针”）

要把“某个虚拟页”映射到 “某个 Swap 设备的某个页槽”，内核定义了一个逻辑上的**swap entry**：

```c
// 概念上：
swap_entry = (type, offset)

// type   = 使用哪一个 swap_info_struct（哪一个 Swap 设备）
// offset = 该设备中的第几个页槽
```

在实现中，`swap entry` 会被编码成一个整数，塞进页表项（PTE）中，作为“这页在 Swap 上的位置描述”。

- 当页在内存里时：
  - PTE 里保存的是 **物理页帧号（PFN）**，`present bit = 1`。
- 当页被换出到 Swap 后：
  - PTE 不再保存 PFN，而是保存 **编码后的 swap entry**，并标记为“在 Swap 上”（某种意义的 `present = 0 + swap 标志`）。

后续再访问同一虚拟地址，内核就能根据这个 `swap entry` 找到对应的 Swap 槽，从磁盘把页读回来。

---

### 4. 匿名页被换出（swap-out）的完整流程

当系统内存紧张（`kswapd` 或直接回收路径触发）时，会尝试将部分匿名页换出到 Swap。

#### 4.1 选择要换出的页

内核的内存回收算法大致步骤（简化）：

1. 根据水位线检查：可用页（`free + file_cache` 等）是否太少；
2. 如果水位不足，启动回收：
   - 扫描 LRU 链表（active / inactive 匿名页和文件页列表）；
   - 找出**最近不常用**的匿名页作为回收候选；
3. 对文件页：
   - 可以回写 / 丢缓存；
4. 对匿名页：
   - 只能依靠 Swap 把内容保存在磁盘上。

#### 4.2 为匿名页分配 swap entry

对每一个要被换出的匿名页 `page`：

1. 在某个 `swap_info_struct` 中通过 `swap_map` 找一个空闲槽 `offset`；
2. 确定 `type`（对应这个 `swap_info_struct` 的编号）；
3. 构造 `swap_entry = (type, offset)`；
4. 在页表中，将原来的 PTE（指向物理页帧号）替换为一个 Swap PTE：
   - PTE 中保存编码后的 `swap_entry`；
   - `present` 相关标志表明：这是一个在 Swap 上的页；

#### 4.3 把页内容写入 Swap 设备

然后，内核通过通用块 I/O 层，把页写到 `swap_info_struct->bdev` 对应的块设备：

```c
static int swap_writepage(struct page *page, ...)
{
    struct swap_info_struct *sis = page_swap_info(page);

    // 构造 bio，绑定到对应 Swap 设备
    bio = bio_alloc(sis->bdev, ...);
    //              ^^^^^^^^^
    //              来自 swapon 时保存的 bdev 引用

    // 设定扇区号 = swap_entry.offset * (PAGE_SIZE / 512)
    // 将 page 作为 I/O 缓冲区加入 bio

    submit_bio(bio);   // 提交给块层 → 对应块设备驱动
}
```

写入成功后：

- `swap_map[offset]` 标记为“被使用”；
- 物理页帧可以被回收（归还到 buddy allocator）；
- 页表 PTE 中只保留 Swap entry，不再持有物理页帧。

---

### 5. 再次访问时的换入（swap-in）流程

当进程再次访问一个已经换出的匿名页时：

#### 5.1 产生缺页异常

1. 进程访问一个虚拟地址；
2. TLB miss → 硬件查页表，发现 PTE 中没有 Present 的物理页帧，而是一个 Swap PTE；
3. CPU 触发缺页异常（page fault），进入内核的 `do_page_fault`。

#### 5.2 根据 swap entry 找回 Swap 槽

缺页处理过程中：

1. 内核解析对应的 PTE，识别这是一个 Swap PTE；
2. 从 PTE 解码出 `swap_entry = (type, offset)`：
   - `type` → 找到对应的 `swap_info_struct *sis`；
   - `offset` → 表示此页在该 Swap 设备中的第几个页槽；
3. 通过 `sis->bdev` 找到对应的块设备；
4. 分配一个新的物理页帧；

#### 5.3 从 Swap 设备读回页内容

1. 构造一个 `bio`：
   - `bio->bi_bdev = sis->bdev`；
   - 起始扇区 = `offset * (PAGE_SIZE / 512)`；
   - I/O 缓冲区 = 新分配的物理页；
2. `submit_bio(bio)` → 通用块层 → 对应块设备驱动；
3. 驱动从磁盘读取数据到物理页；

数据读回内存后：

1. 页表 PTE 更新为“指向这个物理页帧号”，标记为 Present；
2. `swap_map[offset]` 清除或减少引用计数（该 Swap 槽可以被再次使用）；
3. 返回到用户空间，重新执行引发缺页的指令。

**对用户进程而言，整个 Swap 出入过程是透明的，只是某些访问会变慢。**

---

### 6. Swap 与文件系统的关系（设备视角 + 内存视角）

#### 6.1 设备视角：谁在用块设备？

从“块设备”的眼睛看出去：

- **文件系统使用块设备**：
  - 通过挂载时的 `super_block->s_bdev`；
  - 每个挂载的文件系统有自己的 `super_block`，里面记录了“我依赖哪个块设备”；
  - 所有文件读写：`VFS → 文件系统（ext4 等）→ sb->s_bdev → 通用块层 → 驱动`。

- **Swap 使用块设备**：
  - 通过 `swap_info_struct->bdev`；
  - 每个激活的 Swap 设备有一个 `swap_info_struct`，里面记录“我是哪个块设备的哪一段”；
  - 所有 Swap I/O：`MM 子系统（swap_writepage / swap_readpage）→ swap_info->bdev → 通用块层 → 驱动`。

两者**共享同一个块设备子系统和驱动栈**，只是上层抽象完全不同：

- 文件系统：按“文件/目录”组织磁盘。
- Swap：按“页槽（Page Slot）”组织磁盘。

#### 6.2 内存视角：谁为谁提供后备存储？

```
┌─────────────────────────────────────────────────────────┐
│ 虚拟内存页的后备存储                                     │
├─────────────────────────────────────────────────────────┤
│                                                          │
│ 1. 有后备文件的页：                                       │
│    - 例如：通过 mmap 映射的文件页、页缓存中的文件数据     │
│    - 后备存储：磁盘上的某个文件（通过文件系统访问）       │
│    - 回收方式：                                           │
│        └─ 如果脏：回写文件；如果干净：直接丢缓存           │
│    - 再访问：从文件系统重新读取                            │
│                                                          │
│ 2. 匿名页：                                               │
│    - 例如：堆、栈、匿名 mmap                               │
│    - 默认没有任何持久化后备存储                            │
│    - 启用 Swap 后：                                        │
│        └─ Swap 设备成为它们的“磁盘后备仓库”               │
│        └─ 页表中保存 Swap entry 指向 Swap 上的位置         │
│    - 回收方式：写入 Swap，释放物理页帧                     │
│    - 再访问：通过 Swap entry 从 Swap 设备读回              │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

---

### 7. 总结：一句话重新审视 Swap

从你已经掌握的“文件系统 + 块设备 + VFS”知识回头看：

1. **同一块磁盘设备** 可以同时：
   - 被某个文件系统用来存储“文件/目录”（通过 `super_block->s_bdev`）；
   - 被 Swap 子系统用来存储“匿名页的镜像”（通过 `swap_info_struct->bdev`）。

2. **文件系统** 负责：  
   - “如何把磁盘空间解释成目录树 + 文件 + inode + 数据块”。

3. **Swap 子系统** 负责：  
   - “如何把磁盘空间解释成 N 个固定大小的页槽，每个槽绑定到某个匿名页”。

4. **虚拟内存管理（MM）** 通过：
   - LRU / 回收算法选出要换出的匿名页；
   - 用 Swap entry 作为“页 → Swap 页槽”的索引；
   - 在缺页异常时，根据 Swap entry 把页从磁盘读回。

所以，可以把 Swap 看成是：

> **建立在块设备之上的、专门为匿名内存设计的“页级文件系统”，  
> 每一个页槽就像这个“文件系统”中的一个固定大小的文件块，  
> 只是它不通过 VFS 提供路径访问，而是只被 MM 子系统和页表使用。**


