# Swap 和系统启动时内存管理分析

## 一、系统启动时的内存管理初始化流程

### 1.1 启动流程概览

系统启动时的内存管理初始化在 `start_kernel()` 中按以下顺序进行：

```964:964:init/main.c
	mm_core_init();
```

### 1.2 mm_core_init() - 内存管理核心初始化

```2636:2675:mm/mm_init.c
void __init mm_core_init(void)
{
	/* Initializations relying on SMP setup */
	BUILD_BUG_ON(MAX_ZONELISTS > 2);
	build_all_zonelists(NULL);
	page_alloc_init_cpuhp();

	/*
	 * page_ext requires contiguous pages,
	 * bigger than MAX_PAGE_ORDER unless SPARSEMEM.
	 */
	page_ext_init_flatmem();
	mem_debugging_and_hardening_init();
	kfence_alloc_pool_and_metadata();
	report_meminit();
	kmsan_init_shadow();
	stack_depot_early_init();
	mem_init();
	kmem_cache_init();
	/*
	 * page_owner must be initialized after buddy is ready, and also after
	 * slab is ready so that stack_depot_init() works properly
	 */
	page_ext_init_flatmem_late();
	kmemleak_init();
	ptlock_cache_init();
	pgtable_cache_init();
	debug_objects_mem_init();
	vmalloc_init();
	/* If no deferred init page_ext now, as vmap is fully initialized */
	if (!deferred_struct_pages)
		page_ext_init();
	/* Should be run before the first non-init thread is created */
	init_espfix_bsp();
	/* Should be run after espfix64 is set up. */
	pti_init();
	kmsan_init_runtime();
	mm_cache_init();
	execmem_init();
}
```

**关键步骤：**

1. **build_all_zonelists()** - 构建所有节点的 zone 列表
2. **mem_init()** - 架构相关的内存初始化（通常是调用 `memblock_free_all()`）
3. **kmem_cache_init()** - SLAB/SLUB 分配器初始化
4. **vmalloc_init()** - vmalloc 区域初始化

### 1.3 mem_init() 和 memblock_free_all()

`mem_init()` 通常是架构相关的函数，最终会调用 `memblock_free_all()`：

```2260:2271:mm/memblock.c
void __init memblock_free_all(void)
{
	unsigned long pages;

	free_unused_memmap();
	reset_all_zones_managed_pages();

	pages = free_low_memory_core_early();
	totalram_pages_add(pages);
}
```

**作用：**
- 将 memblock 分配器管理的空闲内存释放给 buddy 分配器
- 这是从早期内存分配器（memblock）到正式内存分配器（buddy）的转换点

### 1.4 page_alloc_init_late() - 延迟初始化

在 `start_kernel()` 的后期调用：

```1578:1578:init/main.c
	page_alloc_init_late();
```

```2273:2317:mm/mm_init.c
void __init page_alloc_init_late(void)
{
	struct zone *zone;
	int nid;

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT

	/* There will be num_node_state(N_MEMORY) threads */
	atomic_set(&pgdat_init_n_undone, num_node_state(N_MEMORY));
	for_each_node_state(nid, N_MEMORY) {
		kthread_run(deferred_init_memmap, NODE_DATA(nid), "pgdatinit%d", nid);
	}

	/* Block until all are initialised */
	wait_for_completion(&pgdat_init_all_done_comp);

	/*
	 * We initialized the rest of the deferred pages.  Permanently disable
	 * on-demand struct page initialization.
	 */
	static_branch_disable(&deferred_pages);

	/* Reinit limits that are based on free pages after the kernel is up */
	files_maxfiles_init();
#endif

	/* Accounting of total+free memory is stable at this point. */
	mem_init_print_info();
	buffer_init();

	/* Discard memblock private memory */
	memblock_discard();

	for_each_node_state(nid, N_MEMORY)
		shuffle_free_memory(NODE_DATA(nid));

	for_each_populated_zone(zone)
		set_zone_contiguous(zone);

	/* Initialize page ext after all struct pages are initialized. */
	if (deferred_struct_pages)
		page_ext_init();

	page_alloc_sysctl_init();
}
```

**关键操作：**
- 延迟初始化剩余的 struct page（如果启用）
- 打印内存信息
- 丢弃 memblock 的私有内存
- 初始化 buffer cache

## 二、Swap 系统初始化和管理

### 2.1 Swap 数据结构

#### swap_info_struct - Swap 设备信息

```291:350:include/linux/swap.h
struct swap_info_struct {
	struct percpu_ref users;	/* indicate and keep swap device valid. */
	unsigned long	flags;		/* SWP_USED etc: see above */
	signed short	prio;		/* swap priority of this type */
	struct plist_node list;		/* entry in swap_active_head */
	signed char	type;		/* strange name for an index */
	unsigned int	max;		/* extent of the swap_map */
	unsigned char *swap_map;	/* vmalloc'ed array of usage counts */
	unsigned long *zeromap;		/* kvmalloc'ed bitmap to track zero pages */
	struct swap_cluster_info *cluster_info; /* cluster info. Only for SSD */
	struct list_head free_clusters; /* free clusters list */
	struct list_head full_clusters; /* full clusters list */
	struct list_head nonfull_clusters[SWAP_NR_ORDERS];
					/* list of cluster that contains at least one free slot */
	struct list_head frag_clusters[SWAP_NR_ORDERS];
					/* list of cluster that are fragmented or contented */
	unsigned int frag_cluster_nr[SWAP_NR_ORDERS];
	unsigned int lowest_bit;	/* index of first free in swap_map */
	unsigned int highest_bit;	/* index of last free in swap_map */
	unsigned int pages;		/* total of usable pages of swap */
	unsigned int inuse_pages;	/* number of those currently in use */
	unsigned int cluster_next;	/* likely index for next allocation */
	unsigned int cluster_nr;	/* countdown to next cluster search */
	unsigned int __percpu *cluster_next_cpu; /*percpu index for next allocation */
	struct percpu_cluster __percpu *percpu_cluster; /* per cpu's swap location */
	struct rb_root swap_extent_root;/* root of the swap extent rbtree */
	struct block_device *bdev;	/* swap device or bdev of swap file */
	struct file *swap_file;		/* seldom referenced */
	struct completion comp;		/* seldom referenced */
	spinlock_t lock;		/*
					 * protect map scan related fields like
					 * swap_map, lowest_bit, highest_bit,
					 * inuse_pages, cluster_next,
					 * cluster_nr, lowest_alloc,
					 * highest_alloc, free/discard cluster
					 * list. other fields are only changed
					 * at swapon/swapoff, so are protected
					 * by swap_lock. changing flags need
					 * hold this lock and swap_lock. If
					 * both locks need hold, hold swap_lock
					 * first.
					 */
	spinlock_t cont_lock;		/*
					 * protect swap count continuation page
					 * list.
					 */
	struct work_struct discard_work; /* discard worker */
	struct work_struct reclaim_work; /* reclaim worker */
	struct list_head discard_clusters; /* discard clusters list */
	struct plist_node avail_lists[]; /*
					   * entries in swap_avail_heads, one
					   * entry per node.
					   * Must be last as the number of the
					   * array is nr_node_ids, which is not
					   * a fixed value so have to allocate
					   * dynamically.
					   * And it has to be an array so that
					   * plist_for_each_* can work.
					   */
};
```

**关键字段：**
- `swap_map` - 跟踪每个 swap slot 的使用计数
- `cluster_info` - SSD swap 的集群信息
- `pages` - swap 设备的总页数
- `inuse_pages` - 当前使用的页数

#### 全局变量

```66:78:mm/swapfile.c
static DEFINE_SPINLOCK(swap_lock);
static unsigned int nr_swapfiles;
atomic_long_t nr_swap_pages;
/*
 * Some modules use swappable objects and may try to swap them out under
 * memory pressure (via the shrinker). Before doing so, they may wish to
 * check to see if any swap space is available.
 */
EXPORT_SYMBOL_GPL(nr_swap_pages);
/* protected with swap_lock. reading in vm_swap_full() doesn't need lock */
long total_swap_pages;
static int least_priority = -1;
unsigned long swapfile_maximum_size;
```

- `nr_swap_pages` - 当前可用的 swap 页数（原子变量）
- `total_swap_pages` - 总 swap 页数
- `swap_lock` - 保护 swap 列表的锁

### 2.2 Swap 初始化流程

#### 2.2.1 Swap 地址空间初始化

```710:731:mm/swap_state.c
int init_swap_address_space(unsigned int type, unsigned long nr_pages)
{
	struct address_space *spaces, *space;
	unsigned int i, nr;

	nr = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_PAGES);
	spaces = kvcalloc(nr, sizeof(struct address_space), GFP_KERNEL);
	if (!spaces)
		return -ENOMEM;
	for (i = 0; i < nr; i++) {
		space = spaces + i;
		xa_init_flags(&space->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space->i_mmap_writable, 0);
		space->a_ops = &swap_aops;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space);
	}
	nr_swapper_spaces[type] = nr;
	swapper_spaces[type] = spaces;

	return 0;
}
```

**作用：**
- 为每个 swap 设备创建地址空间（address_space）
- 用于管理 swap cache 中的页面

#### 2.2.2 Swap 设备设置

```2644:2718:mm/swapfile.c
static void setup_swap_info(struct swap_info_struct *si, int prio,
			    unsigned char *swap_map,
			    struct swap_cluster_info *cluster_info,
			    unsigned long *zeromap)
{
	int i;

	if (prio >= 0)
		si->prio = prio;
	else
		si->prio = --least_priority;
	/*
	 * the plist prio is negated because plist ordering is
	 * low-to-high, while swap ordering is high-to-low
	 */
	si->list.prio = -si->prio;
	for_each_node(i) {
		if (si->prio >= 0)
			si->avail_lists[i].prio = -si->prio;
		else {
			if (swap_node(si) == i)
				si->avail_lists[i].prio = 1;
			else
				si->avail_lists[i].prio = -si->prio;
		}
	}
	si->swap_map = swap_map;
	si->cluster_info = cluster_info;
	si->zeromap = zeromap;
}

static void _enable_swap_info(struct swap_info_struct *si)
{
	si->flags |= SWP_WRITEOK;
	atomic_long_add(si->pages, &nr_swap_pages);
	total_swap_pages += si->pages;

	assert_spin_locked(&swap_lock);
	/*
	 * both lists are plist, and thus priority ordered.
	 * swap_active_head needs to be priority ordered for swapoff(),
	 * which on removal of any swap_info_struct with an auto-assigned
	 * (i.e. negative) priority increments the auto-assigned priority
	 * of any lower-priority swap_info_structs.
	 * swap_avail_head needs to be priority ordered for folio_alloc_swap(),
	 * which allocates swap pages from the highest available priority
	 * swap_info_struct.
	 */
	plist_add(&si->list, &swap_active_head);

	/* add to available list iff swap device is not full */
	if (si->highest_bit)
		add_to_avail_list(si);
}

static void enable_swap_info(struct swap_info_struct *si, int prio,
				unsigned char *swap_map,
				struct swap_cluster_info *cluster_info,
				unsigned long *zeromap)
{
	spin_lock(&swap_lock);
	spin_lock(&si->lock);
	setup_swap_info(si, prio, swap_map, cluster_info, zeromap);
	spin_unlock(&si->lock);
	spin_unlock(&swap_lock);
	/*
	 * Finished initializing swap device, now it's safe to reference it.
	 */
	percpu_ref_resurrect(&si->users);
	spin_lock(&swap_lock);
	spin_lock(&si->lock);
	_enable_swap_info(si);
	spin_unlock(&si->lock);
	spin_unlock(&swap_lock);
}
```

**关键操作：**
1. 设置 swap 优先级
2. 初始化 swap_map 和 cluster_info
3. 更新全局 swap 统计（`nr_swap_pages`, `total_swap_pages`）
4. 将 swap 设备添加到活动列表

### 2.3 Swap 操作

#### Swap 分配

- `folio_alloc_swap()` - 为 folio 分配 swap entry
- `get_swap_pages()` - 批量分配 swap pages

#### Swap 释放

- `folio_free_swap()` - 释放 folio 的 swap entry
- `swap_free_nr()` - 批量释放 swap pages

#### Swap 统计

```463:477:include/linux/swap.h
/* linux/mm/swapfile.c */
extern atomic_long_t nr_swap_pages;
extern long total_swap_pages;
extern atomic_t nr_rotate_swap;
extern bool has_usable_swap(void);

/* Swap 50% full? Release swapcache more aggressively.. */
static inline bool vm_swap_full(void)
{
	return atomic_long_read(&nr_swap_pages) * 2 < total_swap_pages;
}

static inline long get_nr_swap_pages(void)
{
	return atomic_long_read(&nr_swap_pages);
}
```

## 三、Swap 与内存管理的关系

### 3.1 内存压力时的 Swap 使用

当系统内存不足时：

1. **kswapd** 线程被唤醒
2. 扫描 LRU 列表，选择页面进行回收
3. 如果页面可交换，调用 `add_to_swap()` 将其写入 swap
4. 更新 `nr_swap_pages` 计数

### 3.2 Swap Cache

Swap cache 是内存中缓存的 swap 页面：

```33:39:mm/swap_state.c
static const struct address_space_operations swap_aops = {
	.writepage	= swap_writepage,
	.dirty_folio	= noop_dirty_folio,
#ifdef CONFIG_MIGRATION
	.migrate_folio	= migrate_folio,
#endif
};
```

- 使用 address_space 管理
- 与文件系统页面缓存类似
- 减少重复的 swap I/O

### 3.3 Swap 与 Buddy 分配器的关系

- Swap 不直接依赖 buddy 分配器初始化
- Swap 在系统运行时动态添加（通过 `swapon` 系统调用）
- Swap 使用 vmalloc 分配 `swap_map` 和 `cluster_info`

## 四、关键时间线

### 系统启动时：

1. **早期阶段**（`start_kernel()` 开始）
   - memblock 分配器管理内存
   - 架构相关的内存初始化

2. **mm_core_init()**（`start_kernel()` 中）
   - 构建 zone 列表
   - 调用 `mem_init()` → `memblock_free_all()`
   - 初始化 buddy 分配器
   - 初始化 SLAB/SLUB

3. **page_alloc_init_late()**（`start_kernel()` 后期）
   - 延迟初始化剩余的 struct page
   - 丢弃 memblock 私有内存
   - 打印内存信息

### Swap 初始化：

- **不是**在系统启动时自动初始化
- 通过用户空间的 `swapon` 命令或系统调用动态添加
- 每个 swap 设备独立初始化

## 五、总结

1. **内存管理初始化**：
   - 从 memblock → buddy 分配器
   - 建立 zone 和 node 结构
   - 初始化各种内存分配器

2. **Swap 系统**：
   - 独立于内存管理初始化
   - 运行时动态添加
   - 使用地址空间管理 swap cache

3. **关系**：
   - Swap 在内存压力时与内存回收协同工作
   - Swap cache 使用类似文件系统的页面缓存机制
   - Swap 统计信息影响内存回收策略

---

## 六、文件页（File Pages）的内存区域和申请方式

### 6.1 文件页是什么？

文件页（File Pages）是**页面缓存（Page Cache）**的一部分，用于缓存文件系统文件的内容。它们是对应磁盘信息的缓存，存储在内存中以提高文件访问性能。

### 6.2 文件页存放在哪个内存区域？

#### 6.2.1 数据结构层面

文件页存储在 `address_space` 结构的 `i_pages` 字段中：

```465:485:include/linux/fs.h
struct address_space {
	struct inode		*host;
	struct xarray		i_pages;
	struct rw_semaphore	invalidate_lock;
	gfp_t			gfp_mask;
	atomic_t		i_mmap_writable;
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	/* number of thp, only for non-shmem files */
	atomic_t		nr_thps;
#endif
	struct rb_root_cached	i_mmap;
	unsigned long		nrpages;
	pgoff_t			writeback_index;
	const struct address_space_operations *a_ops;
	unsigned long		flags;
	errseq_t		wb_err;
	spinlock_t		i_private_lock;
	struct list_head	i_private_list;
	struct rw_semaphore	i_mmap_rwsem;
	void *			i_private_data;
} __attribute__((aligned(sizeof(long)))) __randomize_layout;
```

- `i_pages` 是一个 **xarray**（扩展数组），用于存储文件页
- 每个 inode 都有一个 `address_space`（通过 `inode->i_mapping` 访问）

#### 6.2.2 物理内存区域（Zone）

文件页的物理内存从哪个 zone 分配，取决于 `address_space->gfp_mask`：

```338:341:include/linux/pagemap.h
static inline gfp_t mapping_gfp_mask(struct address_space * mapping)
{
	return mapping->gfp_mask;
}
```

**常见的 gfp_mask 值：**

1. **GFP_USER** - 用于用户空间映射的文件
   - 通常从 **ZONE_NORMAL** 分配
   - 定义：`GFP_USER = __GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL`

2. **GFP_HIGHUSER** - 用于可能映射到用户空间的文件（支持 HIGHMEM）
   - 可以从 **ZONE_HIGHMEM** 或 **ZONE_NORMAL** 分配
   - 定义：`GFP_HIGHUSER = GFP_USER | __GFP_HIGHMEM`

3. **GFP_HIGHUSER_MOVABLE** - 可移动的用户空间页面
   - 可以从 **ZONE_MOVABLE**、**ZONE_HIGHMEM** 或 **ZONE_NORMAL** 分配
   - 定义：`GFP_HIGHUSER_MOVABLE = GFP_HIGHUSER | __GFP_MOVABLE`

**Zone 选择逻辑：**

```132:141:include/linux/gfp.h
static inline enum zone_type gfp_zone(gfp_t flags)
{
	enum zone_type z;
	int bit = (__force int) (flags & GFP_ZONEMASK);

	z = (GFP_ZONE_TABLE >> (bit * GFP_ZONES_SHIFT)) &
					 ((1 << GFP_ZONES_SHIFT) - 1);
	VM_BUG_ON((GFP_ZONE_BAD >> bit) & 1);
	return z;
}
```

### 6.3 文件系统如何申请文件页？

#### 6.3.1 申请流程

1. **文件系统设置 gfp_mask**

   文件系统在创建 inode 时设置 `address_space->gfp_mask`：

```354:357:include/linux/pagemap.h
static inline void mapping_set_gfp_mask(struct address_space *m, gfp_t mask)
{
	m->gfp_mask = mask;
}
```

   例如，ext4 文件系统通常在 `ext4_alloc_inode()` 中设置。

2. **分配文件页**

   当需要读取文件内容时，通过 `filemap_alloc_folio()` 分配：

```664:670:include/linux/pagemap.h
#ifdef CONFIG_NUMA
struct folio *filemap_alloc_folio_noprof(gfp_t gfp, unsigned int order);
#else
static inline struct folio *filemap_alloc_folio_noprof(gfp_t gfp, unsigned int order)
{
	return folio_alloc_noprof(gfp, order);
}
#endif
```

```995:1011:mm/filemap.c
struct folio *filemap_alloc_folio_noprof(gfp_t gfp, unsigned int order)
{
	int n;
	struct folio *folio;

	if (cpuset_do_page_mem_spread()) {
		unsigned int cpuset_mems_cookie;
		do {
			cpuset_mems_cookie = read_mems_allowed_begin();
			n = cpuset_mem_spread_node();
			folio = __folio_alloc_node_noprof(gfp, order, n);
		} while (!folio && read_mems_allowed_retry(cpuset_mems_cookie));

		return folio;
	}
	return folio_alloc_noprof(gfp, order);
}
```

3. **添加到页面缓存**

   分配后，通过 `filemap_add_folio()` 添加到 `address_space->i_pages`：

```851:970:mm/filemap.c
noinline int __filemap_add_folio(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	XA_STATE(xas, &mapping->i_pages, index);
	void *alloced_shadow = NULL;
	int alloced_order = 0;
	bool huge;
	long nr;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapbacked(folio), folio);
	VM_BUG_ON_FOLIO(folio_order(folio) < mapping_min_folio_order(mapping),
			folio);
	mapping_set_update(&xas, mapping);

	VM_BUG_ON_FOLIO(index & (folio_nr_pages(folio) - 1), folio);
	xas_set_order(&xas, index, folio_order(folio));
	huge = folio_test_hugetlb(folio);
	nr = folio_nr_pages(folio);

	gfp &= GFP_RECLAIM_MASK;
	folio_ref_add(folio, nr);
	folio->mapping = mapping;
	folio->index = xas.xa_index;

	for (;;) {
		int order = -1, split_order = 0;
		void *entry, *old = NULL;

		xas_lock_irq(&xas);
		xas_for_each_conflict(&xas, entry) {
			old = entry;
			if (!xa_is_value(entry)) {
				xas_set_err(&xas, -EEXIST);
				goto unlock;
			}
			/*
			 * If a larger entry exists,
			 * it will be the first and only entry iterated.
			 */
			if (order == -1)
				order = xas_get_order(&xas);
		}

		/* entry may have changed before we re-acquire the lock */
		if (alloced_order && (old != alloced_shadow || order != alloced_order)) {
			xas_destroy(&xas);
			alloced_order = 0;
		}

		if (old) {
			if (order > 0 && order > folio_order(folio)) {
				/* How to handle large swap entries? */
				BUG_ON(shmem_mapping(mapping));
				if (!alloced_order) {
					split_order = order;
					goto unlock;
				}
				xas_split(&xas, old, order);
				xas_reset(&xas);
			}
			if (shadowp)
				*shadowp = old;
		}

		xas_store(&xas, folio);
		if (xas_error(&xas))
			goto unlock;

		mapping->nrpages += nr;

		/* hugetlb pages do not participate in page cache accounting */
		if (!huge) {
			__lruvec_stat_mod_folio(folio, NR_FILE_PAGES, nr);
			if (folio_test_pmd_mappable(folio))
				__lruvec_stat_mod_folio(folio,
						NR_FILE_THPS, nr);
		}

unlock:
		xas_unlock_irq(&xas);

		/* split needed, alloc here and retry. */
		if (split_order) {
			xas_split_alloc(&xas, old, split_order, gfp);
			if (xas_error(&xas))
				goto error;
			alloced_shadow = old;
			alloced_order = split_order;
			xas_reset(&xas);
			continue;
		}

		if (!xas_nomem(&xas, gfp))
			break;
	}

	if (xas_error(&xas))
		goto error;

	trace_mm_filemap_add_to_page_cache(folio);
	return 0;
error:
	folio->mapping = NULL;
	/* Leave page->index set: truncation relies upon it */
	folio_put_refs(folio, nr);
	return xas_error(&xas);
}
```

**关键点：**
- 文件页添加到 `mapping->i_pages`（xarray）
- 更新 `mapping->nrpages` 计数
- 更新 `NR_FILE_PAGES` 统计（LRU 统计）

#### 6.3.2 实际使用示例

**文件读取时的流程：**

```3313:3367:mm/filemap.c
vm_fault_t filemap_fault(struct vm_fault *vmf)
{
	int error;
	struct file *file = vmf->vma->vm_file;
	struct file *fpin = NULL;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t max_idx, index = vmf->pgoff;
	struct folio *folio;
	vm_fault_t ret = 0;
	bool mapping_locked = false;

	max_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	if (unlikely(index >= max_idx))
		return VM_FAULT_SIGBUS;

	trace_mm_filemap_fault(mapping, index);

	/*
	 * Do we have something in the page cache already?
	 */
	folio = filemap_get_folio(mapping, index);
	if (likely(!IS_ERR(folio))) {
		/*
		 * We found the page, so try async readahead before waiting for
		 * the lock.
		 */
		if (!(vmf->flags & FAULT_FLAG_TRIED))
			fpin = do_async_mmap_readahead(vmf, folio);
		if (unlikely(!folio_test_uptodate(folio))) {
			filemap_invalidate_lock_shared(mapping);
			mapping_locked = true;
		}
	} else {
		ret = filemap_fault_recheck_pte_none(vmf);
		if (unlikely(ret))
			return ret;

		/* No page in the page cache at all */
		count_vm_event(PGMAJFAULT);
		count_memcg_event_mm(vmf->vma->vm_mm, PGMAJFAULT);
		ret = VM_FAULT_MAJOR;
		fpin = do_sync_mmap_readahead(vmf);
retry_find:
		/*
		 * See comment in filemap_create_folio() why we need
		 * invalidate_lock
		 */
		if (!mapping_locked) {
			filemap_invalidate_lock_shared(mapping);
			mapping_locked = true;
		}
		folio = __filemap_get_folio(mapping, index,
					  FGP_CREAT|FGP_FOR_MMAP,
					  vmf->gfp_mask);
		if (IS_ERR(folio)) {
			if (fpin)
				goto out_retry;
			filemap_invalidate_unlock_shared(mapping);
			return VM_FAULT_OOM;
		}
	}
```

**创建新文件页：**

```2475:2521:mm/filemap.c
static int filemap_create_folio(struct file *file,
		struct address_space *mapping, loff_t pos,
		struct folio_batch *fbatch)
{
	struct folio *folio;
	int error;
	unsigned int min_order = mapping_min_folio_order(mapping);
	pgoff_t index;

	folio = filemap_alloc_folio(mapping_gfp_mask(mapping), min_order);
	if (!folio)
		return -ENOMEM;

	/*
	 * Protect against truncate / hole punch. Grabbing invalidate_lock
	 * here assures we cannot instantiate and bring uptodate new
	 * pagecache folios after evicting page cache during truncate
	 * and before actually freeing blocks.	Note that we could
	 * release invalidate_lock after inserting the folio into
	 * the page cache as the locked folio would then be enough to
	 * synchronize with hole punching. But there are code paths
	 * such as filemap_update_page() filling in partially uptodate
	 * pages or ->readahead() that need to hold invalidate_lock
	 * while mapping blocks for IO so let's hold the lock here as
	 * well to keep locking rules simple.
	 */
	filemap_invalidate_lock_shared(mapping);
	index = (pos >> (PAGE_SHIFT + min_order)) << min_order;
	error = filemap_add_folio(mapping, folio, index,
			mapping_gfp_constraint(mapping, GFP_KERNEL));
	if (error == -EEXIST)
		error = AOP_TRUNCATED_PAGE;
	if (error)
		goto error;

	error = filemap_read_folio(file, mapping->a_ops->read_folio, folio);
	if (error)
		goto error;

	filemap_invalidate_unlock_shared(mapping);
	folio_batch_add(fbatch, folio);
	return 0;
error:
	filemap_invalidate_unlock_shared(mapping);
	folio_put(folio);
	return error;
}
```

### 6.4 总结

1. **存储位置**：
   - **逻辑上**：存储在 `address_space->i_pages`（xarray）中
   - **物理上**：从 **ZONE_NORMAL**、**ZONE_HIGHMEM** 或 **ZONE_MOVABLE** 分配（取决于 `gfp_mask`）

2. **申请方式**：
   - 文件系统在创建 inode 时设置 `address_space->gfp_mask`
   - 通过 `filemap_alloc_folio(mapping_gfp_mask(mapping), order)` 分配
   - 通过 `filemap_add_folio()` 添加到页面缓存

3. **统计信息**：
   - `NR_FILE_PAGES` - 文件页总数
   - `mapping->nrpages` - 该 address_space 的页数

4. **内存区域选择**：
   - 由 `mapping->gfp_mask` 决定
   - 通常使用 `GFP_USER` 或 `GFP_HIGHUSER_MOVABLE`
   - 通过 `gfp_zone()` 函数确定具体的 zone

---

## 七、Page Cache 详细解析

### 7.1 Page Cache 概述

Page Cache（页面缓存）是 Linux 内核中用于缓存文件系统数据页的核心机制。它将磁盘上的文件内容缓存在内存中，以显著提高文件读写性能。

**Page Cache 的作用：**
1. **减少磁盘 I/O**：频繁访问的文件数据可以直接从内存读取
2. **统一缓存接口**：为所有文件系统提供统一的缓存机制
3. **预读优化**：通过预读机制提高顺序访问性能
4. **写回优化**：合并写操作，减少磁盘写入次数

### 7.2 Page Cache 核心数据结构

#### 7.2.1 address_space - 地址空间

每个文件（inode）都有一个 `address_space`，用于管理该文件的所有缓存页：

```465:509:include/linux/fs.h
struct address_space {
	struct inode		*host;
	struct xarray		i_pages;
	struct rw_semaphore	invalidate_lock;
	gfp_t			gfp_mask;
	atomic_t		i_mmap_writable;
#ifdef CONFIG_READ_ONLY_THP_FOR_FS
	/* number of thp, only for non-shmem files */
	atomic_t		nr_thps;
#endif
	struct rb_root_cached	i_mmap;
	unsigned long		nrpages;
	pgoff_t			writeback_index;
	const struct address_space_operations *a_ops;
	unsigned long		flags;
	errseq_t		wb_err;
	spinlock_t		i_private_lock;
	struct list_head	i_private_list;
	struct rw_semaphore	i_mmap_rwsem;
	void *			i_private_data;
} __attribute__((aligned(sizeof(long)))) __randomize_layout;
```

**关键字段：**
- `i_pages` - **xarray**，存储文件的所有缓存页（以文件偏移量索引）
- `nrpages` - 缓存页总数
- `a_ops` - 地址空间操作函数（读写页面的方法）
- `host` - 指向所属的 inode

#### 7.2.2 xarray - 扩展数组

`xarray` 是内核中用于存储稀疏索引数据的结构，类似于可扩展的数组：

```276:290:include/linux/xarray.h
struct xarray {
	spinlock_t	xa_lock;
/* private: The rest of the data structure is not defined to the rest of the kernel. */
	gfp_t		xa_flags;
	void __rcu *	xa_head;
} __attribute__((aligned(sizeof(long)))); /* Allows 'xfp' to be the last member */
```

- Page Cache 使用 xarray 以文件偏移量（`pgoff_t`）为索引存储页面
- 支持大页面（THP）的存储（一个索引可以存储多个页面）
- 支持 RCU 保护，提高并发性能

#### 7.2.3 folio - 页面容器

现代内核使用 `folio` 而不是 `page` 作为页面容器：

```47:63:include/linux/mm_types.h
struct folio {
	/* private: don't document the anon or file, or swap members */
	union {
		struct {
	/* public: */
			unsigned long flags;
			struct list_head lru;
			struct address_space *mapping;
			pgoff_t index;
			void *private;
			atomic_t _mapcount;
			atomic_t _refcount;
```

**关键字段：**
- `mapping` - 指向所属的 `address_space`
- `index` - 在文件中的偏移量（页号）
- `flags` - 页面状态标志（dirty, uptodate 等）
- `_refcount` - 引用计数

### 7.3 Page Cache 的查找机制

#### 7.3.1 查找流程

当需要访问文件数据时，内核首先在 Page Cache 中查找：

```396:438:mm/filemap.c
struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index,
		fgf_t fgp_flags, gfp_t gfp)
{
	struct folio *folio;

repeat:
	folio = __filemap_get_folio_gfp(mapping, index, fgp_flags, gfp);
	if (IS_ERR(folio) && (fgp_flags & FGP_WRITEBEGIN)) {
		folio = __filemap_get_folio_gfp(mapping, index,
						fgp_flags & ~FGP_WRITEBEGIN,
						gfp);
		if (IS_ERR(folio)) {
			filemap_invalidate_unlock_shared(mapping);
			return folio;
		}
	}
	return folio;
}
EXPORT_SYMBOL(__filemap_get_folio);

struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index)
{
	return __filemap_get_folio(mapping, index, 0, 0);
}
EXPORT_SYMBOL(filemap_get_folio);
```

**查找过程：**
1. 使用 `mapping` 和 `index`（文件偏移量）作为键
2. 在 `address_space->i_pages`（xarray）中查找
3. 如果找到，增加引用计数并返回
4. 如果未找到，根据标志决定是否创建新页

#### 7.3.2 查找的具体实现

```255:383:mm/filemap.c
static struct folio *__filemap_get_folio_gfp(struct address_space *mapping,
		pgoff_t index, fgf_t fgp_flags, gfp_t gfp)
{
	struct folio *folio;
	XA_STATE(xas, &mapping->i_pages, index);

repeat:
	rcu_read_lock();
	folio = filemap_get_entry(&xas, mapping, index);
	if (folio) {
		if (fgp_flags & FGP_ACCESSED)
			folio_mark_accessed(folio);
		else if (fgp_flags & FGP_READAHEAD) {
			if (xa_is_value(folio) || !folio_test_uptodate(folio))
				return NULL;
			ra_submit(&file_ra_state(folio->mapping->host->i_fop),
					mapping, folio->file, folio->index, gfp);
		}
		if (fgp_flags & FGP_LOCK) {
			if (fgp_flags & FGP_NOWAIT) {
				if (!folio_trylock(folio)) {
					folio_put(folio);
					rcu_read_unlock();
					return NULL;
				}
			} else {
				folio_lock(folio);
			}
		}
		rcu_read_unlock();
		goto fallback;
	}
```

**关键点：**
- 使用 RCU 读锁保护查找过程（无锁读）
- 通过 xarray 的 `xas_load()` 查找
- 如果找到，根据标志执行额外操作（标记访问、预读等）

### 7.4 Page Cache 的添加流程

#### 7.4.1 添加新页面到缓存

当文件数据不在缓存中时，需要创建新页并添加到缓存：

```851:970:mm/filemap.c
noinline int __filemap_add_folio(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	XA_STATE(xas, &mapping->i_pages, index);
	void *alloced_shadow = NULL;
	int alloced_order = 0;
	bool huge;
	long nr;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapbacked(folio), folio);
	VM_BUG_ON_FOLIO(folio_order(folio) < mapping_min_folio_order(mapping),
			folio);
	mapping_set_update(&xas, mapping);

	VM_BUG_ON_FOLIO(index & (folio_nr_pages(folio) - 1), folio);
	xas_set_order(&xas, index, folio_order(folio));
	huge = folio_test_hugetlb(folio);
	nr = folio_nr_pages(folio);

	gfp &= GFP_RECLAIM_MASK;
	folio_ref_add(folio, nr);
	folio->mapping = mapping;
	folio->index = xas.xa_index;

	for (;;) {
		int order = -1, split_order = 0;
		void *entry, *old = NULL;

		xas_lock_irq(&xas);
		xas_for_each_conflict(&xas, entry) {
			old = entry;
			if (!xa_is_value(entry)) {
				xas_set_err(&xas, -EEXIST);
				goto unlock;
			}
			/*
			 * If a larger entry exists,
			 * it will be the first and only entry iterated.
			 */
			if (order == -1)
				order = xas_get_order(&xas);
		}

		/* entry may have changed before we re-acquire the lock */
		if (alloced_order && (old != alloced_shadow || order != alloced_order)) {
			xas_destroy(&xas);
			alloced_order = 0;
		}

		if (old) {
			if (order > 0 && order > folio_order(folio)) {
				/* How to handle large swap entries? */
				BUG_ON(shmem_mapping(mapping));
				if (!alloced_order) {
					split_order = order;
					goto unlock;
				}
				xas_split(&xas, old, order);
				xas_reset(&xas);
			}
			if (shadowp)
				*shadowp = old;
		}

		xas_store(&xas, folio);
		if (xas_error(&xas))
			goto unlock;

		mapping->nrpages += nr;

		/* hugetlb pages do not participate in page cache accounting */
		if (!huge) {
			__lruvec_stat_mod_folio(folio, NR_FILE_PAGES, nr);
			if (folio_test_pmd_mappable(folio))
				__lruvec_stat_mod_folio(folio,
						NR_FILE_THPS, nr);
		}

unlock:
		xas_unlock_irq(&xas);

		/* split needed, alloc here and retry. */
		if (split_order) {
			xas_split_alloc(&xas, old, split_order, gfp);
			if (xas_error(&xas))
				goto error;
			alloced_shadow = old;
			alloced_order = split_order;
			xas_reset(&xas);
			continue;
		}

		if (!xas_nomem(&xas, gfp))
			break;
	}

	if (xas_error(&xas))
		goto error;

	trace_mm_filemap_add_to_page_cache(folio);
	return 0;
error:
	folio->mapping = NULL;
	/* Leave page->index set: truncation relies upon it */
	folio_put_refs(folio, nr);
	return xas_error(&xas);
}
```

**添加流程：**
1. **锁定页面**：页面必须已锁定才能添加
2. **设置映射关系**：`folio->mapping = mapping`, `folio->index = index`
3. **处理冲突**：如果该索引已有条目，处理替换或分割
4. **存储到 xarray**：使用 `xas_store()` 将页面存储到 `i_pages`
5. **更新统计**：
   - `mapping->nrpages` - 地址空间的页数
   - `NR_FILE_PAGES` - 全局文件页统计（用于 LRU）

#### 7.4.2 完整的数据读取流程

当读取文件时，完整的缓存流程：

```3313:3367:mm/filemap.c
vm_fault_t filemap_fault(struct vm_fault *vmf)
{
	int error;
	struct file *file = vmf->vma->vm_file;
	struct file *fpin = NULL;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t max_idx, index = vmf->pgoff;
	struct folio *folio;
	vm_fault_t ret = 0;
	bool mapping_locked = false;

	max_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	if (unlikely(index >= max_idx))
		return VM_FAULT_SIGBUS;

	trace_mm_filemap_fault(mapping, index);

	/*
	 * Do we have something in the page cache already?
	 */
	folio = filemap_get_folio(mapping, index);
	if (likely(!IS_ERR(folio))) {
		/*
		 * We found the page, so try async readahead before waiting for
		 * the lock.
		 */
		if (!(vmf->flags & FAULT_FLAG_TRIED))
			fpin = do_async_mmap_readahead(vmf, folio);
		if (unlikely(!folio_test_uptodate(folio))) {
			filemap_invalidate_lock_shared(mapping);
			mapping_locked = true;
		}
	} else {
		ret = filemap_fault_recheck_pte_none(vmf);
		if (unlikely(ret))
			return ret;

		/* No page in the page cache at all */
		count_vm_event(PGMAJFAULT);
		count_memcg_event_mm(vmf->vma->vm_mm, PGMAJFAULT);
		ret = VM_FAULT_MAJOR;
		fpin = do_sync_mmap_readahead(vmf);
retry_find:
		/*
		 * See comment in filemap_create_folio() why we need
		 * invalidate_lock
		 */
		if (!mapping_locked) {
			filemap_invalidate_lock_shared(mapping);
			mapping_locked = true;
		}
		folio = __filemap_get_folio(mapping, index,
					  FGP_CREAT|FGP_FOR_MMAP,
					  vmf->gfp_mask);
		if (IS_ERR(folio)) {
			if (fpin)
				goto out_retry;
			filemap_invalidate_unlock_shared(mapping);
			return VM_FAULT_OOM;
		}
	}
```

**流程说明：**
1. **查找缓存**：首先尝试在 Page Cache 中查找
2. **缓存命中**：
   - 如果找到且数据最新（uptodate），直接使用
   - 触发异步预读
3. **缓存未命中**：
   - 记录 major fault（需要磁盘 I/O）
   - 执行同步预读
   - 创建新页面并添加到缓存
   - 从磁盘读取数据

### 7.5 Page Cache 的回收机制

#### 7.5.1 LRU 列表管理

Page Cache 的页面被组织在 LRU（Least Recently Used）列表中：

```28:44:include/linux/mmzone.h
enum lru_list {
	LRU_INACTIVE_ANON = LRU_BASE,
	LRU_ACTIVE_ANON = LRU_BASE + LRU_ACTIVE,
	LRU_INACTIVE_FILE = LRU_BASE + LRU_FILE,
	LRU_ACTIVE_FILE = LRU_BASE + LRU_FILE + LRU_ACTIVE,
	LRU_UNEVICTABLE,
	NR_LRU_LISTS
};
```

**LRU 分类：**
- `LRU_INACTIVE_FILE` - 非活动的文件页（可优先回收）
- `LRU_ACTIVE_FILE` - 活动的文件页（最近被访问）
- `LRU_INACTIVE_ANON` - 非活动的匿名页
- `LRU_ACTIVE_ANON` - 活动的匿名页

#### 7.5.2 页面标记为活跃/非活跃

```1612:1651:mm/swap.c
void folio_mark_accessed(struct folio *folio)
{
	if (!folio_test_referenced(folio)) {
		folio_set_referenced(folio);
	} else if (folio_test_unevictable(folio)) {
		/*
		 * Unevictable folios are on the "LRU_UNEVICTABLE" list. But
		 * this list is never rotated or maintained, so marking an
		 * unevictable folio accessed has no effect.
		 */
	} else if (!folio_test_active(folio)) {
		/*
		 * If the folio is on the LRU, queue it for activation via
		 * lru_cache_add() or folio_activate(). The "lru" variable
		 * is determined by folio_is_file_lru().
		 * Otherwise, leave it on the unevictable list.
		 */
		if (folio_lru(folio)) {
			int lru = folio_is_file_lru(folio);
			folio_activate(folio);
			folio_set_referenced(folio);
			folio_clear_referenced(folio);
		}
	}
}
```

**访问标记逻辑：**
1. 第一次访问：设置 `referenced` 标志
2. 第二次访问：如果页面在非活跃列表，将其移到活跃列表

#### 7.5.3 页面回收流程

当内存压力时，内核通过 `shrink_page_list()` 回收页面：

```1368:1469:mm/vmscan.c
static unsigned int shrink_page_list(struct list_head *page_list,
		struct pglist_data *pgdat,
		struct scan_control *sc,
		enum tt_classification reclaim_stat,
		struct reclaim_stat *stat,
		bool ignore_references)
{
	LIST_HEAD(ret_pages);
	LIST_HEAD(free_pages);
	unsigned int nr_reclaimed = 0;
	unsigned int pgactivate = 0;
	bool do_demote = false;
	cond_resched();

	memset(stat, 0, sizeof(*stat));
	cond_resched();

	while (!list_empty(page_list)) {
		struct folio *folio;
		enum page_references references = PAGEREF_RECLAIM;
		bool dirty, writeback;
		unsigned int nr_pages;

		cond_resched();

		folio = lru_to_folio(page_list);
		list_del(&folio->lru);

		if (!folio_trylock(folio))
			goto keep;

		if (unlikely(!folio_evictable(folio))) {
			folio_putback_lru(folio);
			folio_unlock(folio);
			continue;
		}

		/* Double the slab pressure for mapped and swapcache folios */
		if (folio_mapped(folio) || folio_test_swapcache(folio))
			sc->nr_scanned -= folio_nr_pages(folio);

		references = folio_check_references(folio, sc);
		switch (references) {
		case PAGEREF_ACTIVATE:
			goto activate_locked;
		case PAGEREF_KEEP:
			stat->nr_ref_keep += nr_pages;
			goto keep_locked;
		case PAGEREF_RECLAIM:
		case PAGEREF_RECLAIM_CLEAN:
			; /* try to reclaim the folio below */
		}

		/*
		 * Anonymous process memory has backing store?
		 * Try to allocate it some swap space here.
		 * Lazyfree folios could be freed directly
		 */
		if (folio_test_anon(folio) && folio_test_swapbacked(folio)) {
			if (!folio_test_swapcache(folio)) {
				if (!(sc->gfp_mask & __GFP_IO))
					goto keep_locked;
				if (folio_may_be_demoted(folio))
					goto keep_locked;
				if (folio_try_swapcache_add(folio) == 0) {
					folio_set_swapcache(folio);
					folio_set_dirty(folio);
				} else {
					goto keep_locked;
				}
			}
		} else if (folio_test_swapbacked(folio) &&
			   !folio_test_swapcache(folio)) {
			folio_clear_swapbacked(folio);
		}

		/*
		 * The page is mapped into the page tables of one or more
		 * processes. Try to unmap it here.
		 */
		if (folio_mapped(folio)) {
			enum ttu_flags flags = (TTU_BATCH_FLUSH | TTU_RMAP_LOCKED);
			bool was_swapbacked = folio_test_swapbacked(folio);

			if (folio_test_pmd_mappable(folio))
				flags |= TTU_SPLIT_HUGE_PMD;

			try_to_unmap(folio, flags);
			if (folio_mapped(folio)) {
				stat->nr_unmap_fail += nr_pages;
				if (!was_swapbacked && folio_test_swapbacked(folio))
					stat->nr_lazyfree_fail += nr_pages;
				goto activate_locked;
			}
		}
```

**回收决策：**
1. **检查引用**：`folio_check_references()` 决定是否回收
2. **解除映射**：如果有进程映射，先尝试取消映射
3. **处理脏页**：如果是脏页，需要先写回
4. **释放页面**：如果页面未映射且干净，可以释放

#### 7.5.4 文件页的回收特点

文件页的回收相对简单，因为：
- 数据在磁盘上有备份，可以直接丢弃
- 下次访问时可以从磁盘重新读取
- 但脏页需要先写回磁盘

```1470:1520:mm/vmscan.c
		/*
		 * If the folio is dirty, only perform writeback if that write
		 * will be non-blocking.  To prevent this allocation from being
		 * stalled by pagecache activity.  But note that there may be
		 * stalls if we need to get new memory for the folio to be written.
		 */
		if (folio_test_dirty(folio)) {
			/*
			 * Only kswapd can writeback filesystem folios
			 * to avoid risk of stack overflow.  But avoid
			 * injecting inefficient single-folio I/O into
			 * flusher for large read requests.
			 */
			if (folio_is_file_lru(folio) &&
			    (!current_is_kswapd() || sc->priority >= DEF_PRIORITY - 2)) {
				/*
				 * Immediately reclaim when written back.
				 * Similar in principal to folio_deactivate()
				 * except we already have the folio isolated
				 * and know it's dirty
				 */
				inc_lruvec_state(lruvec, PGCLEAN);
				stat->nr_congested += nr_pages;

				folio_clear_active(folio);
				folio_set_workingset(folio);
				folio_account_cleaned(folio, mapping, nr_pages,
						     &wbc);
				folio_unlock(folio);
				folio_redirty_for_writepage(&wbc, folio);
				folio_putback_lru(folio);
				continue;
			}

			stat->nr_writeback += nr_pages;

			/*
			 * We use a folio test to identify non-zero-order
			 * folios so that we don't have to unnecessarily
			 * update statistics for order-0 folios.
			 */
			if (folio_order(folio) > 0) {
				inc_lruvec_state(lruvec, PGCLEAN);
				folio_account_cleaned(folio, mapping, nr_pages,
						     &wbc);
			}

			if (!folio_clear_dirty_for_io(folio))
				BUG();
```

### 7.6 写回（Writeback）机制

#### 7.6.1 脏页标记

当页面被修改时，标记为脏（dirty）：

```522:547:mm/page-writeback.c
void __folio_mark_dirty(struct folio *folio, struct address_space *mapping,
			     int warn)
{
	unsigned long flags;

	xa_lock_irqsave(&mapping->i_pages, flags);
	if (folio_test_dirty(folio)) {
		/* Out of order writes are handled by metadata IO threads. */
		if (warn && !folio_test_ordered(folio))
			warn_page_dirty_ratelimited(folio);
	} else {
		folio_set_dirty(folio);
		folio_account_dirty(mapping->host, folio);
	}
	xa_unlock_irqrestore(&mapping->i_pages, flags);
}
```

**脏页管理：**
- 脏页不能直接回收，必须先写回磁盘
- 通过 `writeback_index` 跟踪写回进度

#### 7.6.2 写回流程

```1815:1900:mm/page-writeback.c
int do_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	int ret;

	if (wbc->nr_to_write <= 0)
		return 0;
	while (1) {
		if (mapping->a_ops->writepages)
			ret = mapping->a_ops->writepages(mapping, wbc);
		else
			ret = generic_writepages(mapping, wbc);
		if ((ret != -ENOMEM) || (wbc->sync_mode != WB_SYNC_ALL))
			break;
		cond_resched();
		congestion_wait(BLK_RW_ASYNC, HZ/50);
	}
	return ret;
}
```

**写回触发条件：**
1. **定期写回**：`pdflush` 或 `writeback` 线程定期扫描脏页
2. **内存压力**：回收时遇到脏页
3. **同步请求**：`fsync()`、`sync()` 等系统调用
4. **时间阈值**：脏页存在时间超过 `dirty_expire_centisecs`

#### 7.6.3 写回操作

```33:39:mm/swap_state.c
static const struct address_space_operations swap_aops = {
	.writepage	= swap_writepage,
	.dirty_folio	= noop_dirty_folio,
#ifdef CONFIG_MIGRATION
	.migrate_folio	= migrate_folio,
#endif
};
```

每个文件系统实现自己的 `writepages` 操作，例如 ext4：

```3011:3069:fs/ext4/inode.c
static int ext4_writepages(struct address_space *mapping,
			   struct writeback_control *wbc)
{
	pgoff_t	writeback_index = 0;
	long nr_to_write = wbc->nr_to_write;
	int range_whole = 0;
	int cycled = 1;
	handle_t *handle = NULL;
	struct inode *inode = mapping->host;
	int needed_blocks, rsv_blocks = 0, ret = 0;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	bool done;
	struct folio_batch fbatch;
	unsigned int nr_folios;
	pgoff_t index;
	pgoff_t end;
	pgoff_t done_index;
	int range_cycled = 0;
	int tag;
	int nblocks = 0;
	int max_pages;
	int tag_pages = 0;
	int i;

	if (wbc->no_cgroup_owner)
		wbc->no_cgroup_owner = (sbi->s_journal && 
					ext4_should_dioread_nolock(inode));

	if (ext4_should_journal_data(inode)) {
		ret = generic_writepages(mapping, wbc);
		goto out_writepages;
	}

	if (ext4_should_dioread_nolock(inode)) {
		/*
		 * We may need to convert up to one extent per block in
		 * the page and we may dirty the inode.
		 */
		rsv_blocks = 1 + ext4_chunk_trans_blocks(inode,
					PAGE_SIZE >> inode->i_blkbits);
	}

	if (wbc->range_cyclic) {
		writeback_index = mapping->writeback_index;
		if (writeback_index)
			cycled = 0;
		end = -1;
	} else {
		index = wbc->range_start >> PAGE_SHIFT;
		end = wbc->range_end >> PAGE_SHIFT;
		if (wbc->range_start == 0 && wbc->range_end == LLONG_MAX)
			range_whole = 1;
		cycled = 1; /* ignore range_cyclic tests */
	}
```

### 7.7 预读（Readahead）机制

#### 7.7.1 预读目的

预读机制预测文件访问模式，提前将可能访问的页面读入缓存：

```118:145:mm/readahead.c
void page_cache_async_readahead(struct address_space *mapping,
		struct file_ra_state *ra, struct file *filp,
		struct folio *folio, pgoff_t index, unsigned long req_size)
{
	/* no read-ahead */
	if (!ra->ra_pages)
		return;

	/*
	 * Same bit is used for PG_readahead and PG_reclaim.
	 */
	if (folio_test_writeback(folio) ||
	    folio_test_readahead(folio) ||
	    folio_test_reclaim(folio))
		return;

	folio_clear_readahead(folio);

	if (blk_cgroup_congested())
		return;

	/* do read-ahead */
	ondemand_readahead(mapping, ra, filp, true, index, req_size);
}
```

**预读策略：**
- **顺序预读**：检测到顺序访问模式时，预读后续页面
- **随机预读**：在随机访问模式下，减少预读窗口

#### 7.7.2 预读窗口计算

```518:602:mm/readahead.c
static void ondemand_readahead(struct address_space *mapping,
		struct file_ra_state *ra, struct file *filp,
		bool hit_readahead_marker, pgoff_t index,
		unsigned long req_size)
{
	struct backing_dev_info *bdi = inode_to_bdi(mapping->host);
	unsigned long max_pages = ra->ra_pages;
	unsigned long add_pages;
	pgoff_t prev_index;
	unsigned long expected;

	/*
	 * If the request exceeds the readahead window, allow the read to
	 * be up to the optimal hardware I/O size
	 */
	if (req_size > max_pages)
		max_pages = req_size;

	/*
	 * start of file
	 */
	if (!index)
		goto initial_readahead;

	/*
	 * It's the expected callback index, assume sequential access.
	 * Ramp up sizes, and push forward the readahead window.
	 */
	expected = round_up(ra->async_size, 1UL << compound_order(folio));
	if (index == expected || index == (ra->start + ra->async_size)) {
		ra->async_size += req_size;
		ra->async_size = get_next_ra_size(ra, max_pages);
		ra->start += ra->async_size;
		ra->size = ra->async_size;
		goto readit;
	}

	/*
	 * Hit a marked folio, and assume sequential access.
	 * Ramp up sizes, and push forward the readahead window.
	 */
	if (hit_readahead_marker) {
		pgoff_t start;

		rcu_read_lock();
		start = page_cache_next_miss(mapping, index + 1, max_pages);
		rcu_read_unlock();

		if (!start || start - index > max_pages)
			return;

		ra->start = start;
		ra->size = start - index;	/* old async_size */
		ra->size += req_size;
		ra->size = get_next_ra_size(ra, max_pages);
		ra->async_size = ra->size;
		goto readit;
	}

	/*
	 * oversize read
	 */
	if (req_size > max_pages)
		goto initial_readahead;

	/*
	 * sequential cache miss
	 */
	prev_index = (unsigned long)(ra->prev_index);
	if (index - prev_index <= 1UL || req_size > 1)
		goto initial_readahead;

	/*
	 * Query the page cache and ask for the expected page.
	 */
	rcu_read_lock();
	expected = page_cache_next_miss(mapping, index + 1, req_size);
	rcu_read_unlock();

	/*
	 * We found the cache is not coherent and need to be careful.
	 * We will try to read ahead, but not too much.
	 */
	if (expected - index <= max_pages)
		goto initial_readahead;

	/*
	 * trivial case: (index - prev_index == 1)
	 * We have a strict sequential pattern. Start sequential readahead.
	 */
initial_readahead:
	ra->start = index;
	ra->size = get_init_ra_size(req_size, max_pages);
	ra->async_size = ra->size > req_size ? ra->size - req_size : ra->size;
	ra->prev_index = index;

readit:
	/*
	 * Will this read hit the readahead marker made by itself?
	 * If so, trigger the readahead marker hit now, and merge
	 * the resulted next readahead window into the current one.
	 * Take care of maximum IO pages as well.
	 */
	if (index == ra->start && ra->size == ra->async_size) {
		ra->async_size = get_next_ra_size(ra, max_pages);
		ra->size = ra->async_size;
	}

	return ra_submit(ra, mapping, filp);
}
```

**预读窗口调整：**
- 初始窗口：`get_init_ra_size()` 计算初始预读大小
- 窗口增长：检测到顺序访问时，逐渐增大预读窗口
- 窗口收缩：检测到随机访问时，减小预读窗口

### 7.8 Page Cache 统计和监控

#### 7.8.1 内核统计信息

```338:353:include/linux/mmzone.h
enum node_stat_item {
	NR_FILE_PAGES,
	NR_SLAB_RECLAIMABLE_B,
	NR_SLAB_UNRECLAIMABLE_B,
	NR_KERNEL_STACK_KB,
	NR_PAGETABLE,
	NR_SECONDARY_PAGETABLE,
	NR_BOUNCE,
	NR_UNSTABLE_NFS,
	NR_VMSCAN_WRITE,
	NR_VMSCAN_IMMEDIATE,	/* Prioritise for reclaim when writeback ends */
	NR_WRITEBACK,
	NR_WRITEBACK_TEMP,	/* Writeback using temporary buffers */
	NR_ISOLATED_ANON,	/* Temporary isolated pages from anon LRU */
	NR_ISOLATED_FILE,	/* Temporary isolated pages from file LRU */
	NR_SHMEM,		/* shmem pages (included in NR_FILE_PAGES) */
```

**关键统计：**
- `NR_FILE_PAGES` - 文件页总数（Page Cache 的主要组成部分）
- `NR_SLAB_RECLAIMABLE_B` - 可回收的 slab
- `NR_WRITEBACK` - 正在写回的页数

#### 7.8.2 /proc/meminfo 中的信息

```bash
$ cat /proc/meminfo
Cached:         12345678 kB    # Page Cache 大小
Buffers:          123456 kB    # 块设备缓冲区
Dirty:             12345 kB    # 脏页大小
Writeback:           123 kB    # 正在写回的页数
```

#### 7.8.3 查看特定文件的 Page Cache

可以通过 `/proc/<pid>/clear_refs` 和 `/proc/<pid>/pagemap` 查看进程映射的页面状态。

### 7.9 Page Cache 的实际使用示例

#### 7.9.1 读取文件的完整流程

```c
// 用户空间：read() 系统调用
// ↓
// 内核空间：vfs_read()
// ↓
// 文件系统：ext4_file_read_iter() / generic_file_read_iter()
// ↓
// 通用文件层：filemap_read()
// ↓
// Page Cache 查找：filemap_get_folio()
//   - 缓存命中：直接从缓存返回
//   - 缓存未命中：filemap_create_folio()
//     - 分配页面：filemap_alloc_folio()
//     - 添加到缓存：filemap_add_folio()
//     - 从磁盘读取：filemap_read_folio()
//     - 触发预读：ondemand_readahead()
// ↓
// 将数据拷贝到用户空间
```

#### 7.9.2 写入文件的完整流程

```c
// 用户空间：write() 系统调用
// ↓
// 内核空间：vfs_write()
// ↓
// 文件系统：ext4_file_write_iter() / generic_file_write_iter()
// ↓
// Page Cache 操作：
//   - 查找或创建页面：filemap_get_folio() / filemap_create_folio()
//   - 标记为脏：__folio_mark_dirty()
//   - 写入数据
// ↓
// 写回（延迟或同步）：
//   - 延迟写回：标记脏页，稍后由 writeback 线程写回
//   - 同步写回：fsync() / sync() 立即触发 do_writepages()
```

### 7.10 Page Cache 性能优化要点

1. **预读调优**：
   - 调整 `/sys/block/<device>/queue/read_ahead_kb` 控制预读大小
   - 内核根据访问模式自动调整

2. **写回调优**：
   - `dirty_ratio` / `dirty_background_ratio` - 控制脏页比例
   - `dirty_expire_centisecs` - 脏页过期时间

3. **回收策略**：
   - `swappiness` - 控制匿名页和文件页的回收比例
   - 文件页通常更容易回收（有磁盘备份）

4. **NUMA 优化**：
   - Page Cache 页面尽量分配在访问它的 CPU 所在的 NUMA 节点

---

## 八、内核查找文件内容的详细路径

当用户程序要读取文件内容时（例如 `read()` 系统调用），内核需要经历多个层次才能找到或加载数据。下面详细解析这个完整的查找路径。

### 8.1 完整的查找路径概览

```
用户空间 read() 系统调用
    ↓
内核入口 (sys_read / ksys_read)
    ↓
VFS 层 (vfs_read)
    ↓
文件系统层 (ext4_file_read_iter / generic_file_read_iter)
    ↓
通用文件层 (filemap_read)
    ↓
Page Cache 查找层 (filemap_get_folio)
    ↓
    ├─→ 缓存命中：直接返回数据
    └─→ 缓存未命中：
            ↓
        页面分配 (filemap_alloc_folio)
            ↓
        添加到缓存 (filemap_add_folio)
            ↓
        磁盘 I/O (filemap_read_folio / read_folio)
            ↓
        触发预读 (ondemand_readahead)
            ↓
        返回数据
```

### 8.2 第一层：用户空间到内核入口

#### 8.2.1 read() 系统调用

用户程序调用 `read(fd, buf, count)` 时：

```c
// glibc 封装
ssize_t read(int fd, void *buf, size_t count)
{
    return SYSCALL_CANCEL(read, fd, buf, count);
}
```

系统调用进入内核，通过 `syscall` 指令切换到内核态。

#### 8.2.2 内核入口 - ksys_read()

```666:683:fs/read_write.c
ssize_t ksys_read(unsigned int fd, char __user *buf, size_t count)
{
	struct fd f = fdget_pos(fd);
	ssize_t ret = -EBADF;

	if (f.file) {
		loff_t pos, *ppos = file_ppos(f.file);
		if (ppos) {
			pos = *ppos;
			ppos = &pos;
		}
		ret = vfs_read(f.file, buf, count, ppos);
		if (ret >= 0 && ppos)
			f.file->f_pos = pos;
		fdput_pos(f);
	}
	return ret;
}
```

**关键操作：**
1. `fdget_pos(fd)` - 通过文件描述符获取 `file` 结构
2. `file_ppos(f.file)` - 获取当前文件偏移量
3. `vfs_read()` - 调用 VFS 层读取函数

### 8.3 第二层：VFS（Virtual File System）层

#### 8.3.1 vfs_read()

```497:523:fs/read_write.c
ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_READ))
		return -EINVAL;
	if (unlikely(!access_ok(buf, count)))
		return -EFAULT;

	ret = rw_verify_area(READ, file, pos, count);
	if (ret)
		return ret;
	if (count > 0) {
		ret = __vfs_read(file, buf, count, pos);
		if (ret > 0) {
			fsnotify_access(file);
			add_rchar(current, ret);
		}
		inc_syscr(current);
	} else {
		ret = 0;
	}

	return ret;
}
```

**功能：**
- 权限检查（`FMODE_READ`、`FMODE_CAN_READ`）
- 用户缓冲区访问权限检查（`access_ok`）
- 文件区域锁定检查（`rw_verify_area`）
- 调用实际读取函数 `__vfs_read()`

#### 8.3.2 __vfs_read()

```474:495:fs/read_write.c
ssize_t __vfs_read(struct file *file, char __user *buf, size_t count,
		   loff_t *pos)
{
	if (file->f_op->read)
		return file->f_op->read(file, buf, count, pos);
	else if (file->f_op->read_iter)
		return new_sync_read(file, buf, count, pos);
	else
		return -EINVAL;
}
```

**关键点：**
- 检查文件操作结构 `file->f_op`
- 如果有 `read_iter`（现代接口），使用 `new_sync_read()`
- 如果有旧的 `read` 接口，直接调用

#### 8.3.3 new_sync_read() - 同步读取包装

```406:426:fs/read_write.c
static ssize_t new_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct kiocb kiocb;
	struct iov_iter iter;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	iov_iter_init(&iter, ITER_DEST, &iov, 1, len);
	kiocb.ki_pos = (ppos ? *ppos : 0);
	ret = call_read_iter(filp, &kiocb, &iter);
	BUG_ON(ret == -EIOCBQUEUED);
	if (ppos)
		*ppos = kiocb.ki_pos;
	return ret;
}
```

**作用：**
- 将用户缓冲区封装为 `iov_iter`（迭代器接口）
- 创建 `kiocb`（kernel I/O control block）
- 调用 `call_read_iter()` → `filp->f_op->read_iter()`

### 8.4 第三层：文件系统层

#### 8.4.1 ext4_file_read_iter() - ext4 文件系统

对于 ext4 文件系统：

```462:490:fs/ext4/file.c
static ssize_t ext4_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret = 0;

	if (iocb->ki_flags & IOCB_DIRECT) {
		ret = ext4_dio_read_iter(iocb, to);
		goto out;
	}

	if (unlikely(ext4_forced_shutdown(EXT4_SB(inode->i_sb))))
		return -EIO;

	if (!iov_iter_count(to))
		return 0; /* skip atime */

	ret = generic_file_read_iter(iocb, to);

out:
	return ret;
}
```

**关键决策：**
- **直接 I/O**（`IOCB_DIRECT`）：绕过 Page Cache，直接读写磁盘 → `ext4_dio_read_iter()`
- **缓冲 I/O**（默认）：使用 Page Cache → `generic_file_read_iter()`

大多数情况使用缓冲 I/O，走 `generic_file_read_iter()`。

#### 8.4.2 generic_file_read_iter() - 通用文件读取

```2495:2544:mm/filemap.c
ssize_t generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	size_t count = iov_iter_count(iter);
	ssize_t retval = 0;

	if (!count)
		goto out; /* skip atime */

	if (iocb->ki_flags & IOCB_DIRECT) {
		struct file *file = iocb->ki_filp;
		struct address_space *mapping = file->f_mapping;
		struct inode *inode = mapping->host;
		loff_t size = i_size_read(inode);
		size_t written = 0;
		ssize_t status;

		if (unlikely(*iocb->ki_pos >= size))
			goto out;

		status = filemap_write_and_wait_range(mapping,
					*iocb->ki_pos,
					*iocb->ki_pos + count - 1);
		if (status < 0) {
			retval = status;
			goto out;
		}

		file_accessed(file);

		retval = mapping->a_ops->direct_IO(iocb, iter);
		if (retval > 0)
			written = retval;
		if (mapping->nrpages) {
			invalidate_mapping_pages(mapping,
					(*iocb->ki_pos) >> PAGE_SHIFT,
					((*iocb->ki_pos + written - 1) >> PAGE_SHIFT));
		}
		if (retval > 0)
			*iocb->ki_pos += written;
		goto out;
	}

	retval = filemap_read(iocb, iter, 0);
out:
	return retval;
}
```

**路径选择：**
- **直接 I/O**：调用 `mapping->a_ops->direct_IO()`，绕过 Page Cache
- **缓冲 I/O**：调用 `filemap_read()`，使用 Page Cache

### 8.5 第四层：通用文件映射层 - filemap_read()

这是 Page Cache 查找的核心函数：

```2667:2777:mm/filemap.c
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *iter,
		ssize_t already_read)
{
	struct file *filp = iocb->ki_filp;
	struct file_ra_state *ra = filp->f_ra;
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct folio_batch fbatch;
	int i, error = 0;
	bool writably_mapped;
	loff_t isize, end_offset;

	if (unlikely(iocb->ki_pos >= inode->i_size))
		return 0;

	end_offset = min_t(loff_t, isize, iocb->ki_pos + iter->count);

	folio_batch_init(&fbatch);
	do {
		cond_resched();

		/*
		 * If we've already successfully copied some data, then we
		 * can no longer safely return -EIOCBQUEUED. Hence mark
		 * an async read NOWAIT at that point.
		 */
		if ((iocb->ki_flags & IOCB_NOWAIT) && already_read)
			iocb->ki_flags &= ~IOCB_NOWAIT;

		error = filemap_get_pages(iocb, iter, &fbatch);
		if (error < 0)
			break;

		if (iocb->ki_flags & IOCB_WAITQ) {
			iocb->ki_flags &= ~IOCB_WAITQ;
			iocb->ki_flags |= IOCB_NOWAIT;
			if (iocb->ki_waitq)
				wake_up(iocb->ki_waitq);
		}

		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *folio = fbatch.folios[i];
			size_t fsize = folio_size(folio);
			size_t offset = iocb->ki_pos & (fsize - 1);
			size_t bytes = min(fsize - offset, iov_iter_count(iter));
			size_t copied;

			if (end_offset < iocb->ki_pos + bytes)
				bytes = end_offset - iocb->ki_pos;

			if (folio_test_waiters(folio)) {
				folio_wait_bit_clear(folio, PG_writeback);
				folio_clear_error(folio);
			}

			/*
			 * If userspace can write to this folio using arbitrary
			 * virtual addresses, take care of potential aliasing
			 * issues.
			 */
			if (mapping_writably_mapped(mapping))
				folio_flush_dirty(folio);

			copied = copy_folio_to_iter(folio, offset, bytes, iter);
			already_read += copied;
			iocb->ki_pos += copied;
			all_really_read += copied;

			if (copied < bytes) {
				error = -EFAULT;
				break;
			}
		}

		folio_batch_release(&fbatch);
	} while (iov_iter_count(iter) && iocb->ki_pos < isize);

	return already_read ? already_read : error;
}
```

**核心循环：**
1. `filemap_get_pages()` - 获取页面（查找或创建）
2. `copy_folio_to_iter()` - 将页面数据拷贝到用户缓冲区
3. 更新文件位置指针 `iocb->ki_pos`

#### 8.5.1 filemap_get_pages() - 获取页面

```2537:2645:mm/filemap.c
static int filemap_get_pages(struct kiocb *iocb, size_t count,
		struct folio_batch *fbatch, unsigned int fgp_flags)
{
	struct file *filp = iocb->ki_filp;
	struct address_space *mapping = filp->f_mapping;
	struct file_ra_state *ra = filp->f_ra;
	pgoff_t index = iocb->ki_pos >> PAGE_SHIFT;
	pgoff_t last_index;
	struct folio *folio;
	int err = 0;

	/* "last_index" is the index of the page beyond the end of the read */
	last_index = DIV_ROUND_UP(iocb->ki_pos + count, PAGE_SIZE);

	do {
		if (iocb->ki_pos >= i_size_read(mapping->host))
			break;

		cond_resched();
retry:
		folio = __filemap_get_folio(mapping, index, fgp_flags, 0);
		if (IS_ERR(folio)) {
			err = PTR_ERR(folio);
			break;
		}
		if (folio) {
			err = filemap_read_folio(iocb, filp, ra, folio);
			if (err == AOP_TRUNCATED_PAGE) {
				folio_put(folio);
				folio = NULL;
				goto retry;
			}
			if (err)
				goto err;
		} else {
			err = filemap_create_folio(filp, mapping,
					iocb->ki_pos, fbatch);
			if (err == AOP_TRUNCATED_PAGE)
				goto retry;
			if (err)
				goto err;

			continue;
		}

		folio_batch_add(fbatch, folio);
		index++;
	} while (index < last_index);

	return fbatch->nr ? 0 : (err ? err : -EFAULT);

err:
	folio_batch_release(fbatch);
	return err;
}
```

**关键操作：**
1. 计算页面索引：`index = iocb->ki_pos >> PAGE_SHIFT`
2. 尝试查找页面：`__filemap_get_folio(mapping, index, ...)`
   - **找到**：调用 `filemap_read_folio()` 确保数据最新
   - **未找到**：调用 `filemap_create_folio()` 创建新页

### 8.6 第五层：Page Cache 查找 - __filemap_get_folio()

这是实际的 Page Cache 查找函数：

```396:438:mm/filemap.c
struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index,
		fgf_t fgp_flags, gfp_t gfp)
{
	struct folio *folio;

repeat:
	folio = __filemap_get_folio_gfp(mapping, index, fgp_flags, gfp);
	if (IS_ERR(folio) && (fgp_flags & FGP_WRITEBEGIN)) {
		folio = __filemap_get_folio_gfp(mapping, index,
						fgp_flags & ~FGP_WRITEBEGIN,
						gfp);
		if (IS_ERR(folio)) {
			filemap_invalidate_unlock_shared(mapping);
			return folio;
		}
	}
	return folio;
}
EXPORT_SYMBOL(__filemap_get_folio);
```

#### 8.6.1 __filemap_get_folio_gfp() - 实际查找

```255:383:mm/filemap.c
static struct folio *__filemap_get_folio_gfp(struct address_space *mapping,
		pgoff_t index, fgf_t fgp_flags, gfp_t gfp)
{
	struct folio *folio;
	XA_STATE(xas, &mapping->i_pages, index);

repeat:
	rcu_read_lock();
	folio = filemap_get_entry(&xas, mapping, index);
	if (folio) {
		if (fgp_flags & FGP_ACCESSED)
			folio_mark_accessed(folio);
		else if (fgp_flags & FGP_READAHEAD) {
			if (xa_is_value(folio) || !folio_test_uptodate(folio))
				return NULL;
			ra_submit(&file_ra_state(folio->mapping->host->i_fop),
					mapping, folio->file, folio->index, gfp);
		}
		if (fgp_flags & FGP_LOCK) {
			if (fgp_flags & FGP_NOWAIT) {
				if (!folio_trylock(folio)) {
					folio_put(folio);
					rcu_read_unlock();
					return NULL;
				}
			} else {
				folio_lock(folio);
			}
		}
		rcu_read_unlock();
		goto fallback;
	}
```

**查找过程：**
1. 创建 XA_STATE：指向 `mapping->i_pages` 和指定索引
2. RCU 读锁：保护并发查找
3. `filemap_get_entry()`：从 xarray 中查找条目
4. 找到后的处理：
   - 标记访问（更新 LRU）
   - 触发预读
   - 锁定页面（如果需要）

#### 8.6.2 filemap_get_entry() - xarray 查找

```182:208:mm/filemap.c
static void *filemap_get_entry(struct xa_state *xas, struct address_space *mapping,
		pgoff_t index)
{
	void *entry;

retry:
	xas_reset(xas);
	xas_set(xas, index);
	entry = xas_load(xas);
	if (xa_is_value(entry)) {
		entry = NULL;
	} else if (xas_get_mark(xas, PAGECACHE_TAG_TOWRITE)) {
		xas_clear_mark(xas, PAGECACHE_TAG_TOWRITE);
		entry = NULL;
	} else if (xas_get_mark(xas, PAGECACHE_TAG_TOWRITE)) {
		xas_clear_mark(xas, PAGECACHE_TAG_TOWRITE);
		entry = NULL;
	}

	if (unlikely(entry != xas_reload(xas)))
		goto retry;

	return entry;
}
```

**xarray 查找：**
- `xas_load(xas)` - 从 xarray 中加载指定索引的值
- 处理特殊标记（`xa_is_value`、`PAGECACHE_TAG_TOWRITE`）
- 检查条目是否有效（`xas_reload`）

### 8.7 缓存未命中：创建新页面

如果 `__filemap_get_folio()` 返回 NULL，需要创建新页面：

#### 8.7.1 filemap_create_folio()

```2475:2521:mm/filemap.c
static int filemap_create_folio(struct file *file,
		struct address_space *mapping, loff_t pos,
		struct folio_batch *fbatch)
{
	struct folio *folio;
	int error;
	unsigned int min_order = mapping_min_folio_order(mapping);
	pgoff_t index;

	folio = filemap_alloc_folio(mapping_gfp_mask(mapping), min_order);
	if (!folio)
		return -ENOMEM;

	/*
	 * Protect against truncate / hole punch. Grabbing invalidate_lock
	 * here assures we cannot instantiate and bring uptodate new
	 * pagecache folios after evicting page cache during truncate
	 * and before actually freeing blocks.	Note that we could
	 * release invalidate_lock after inserting the folio into
	 * the page cache as the locked folio would then be enough to
	 * synchronize with hole punching. But there are code paths
	 * such as filemap_update_page() filling in partially uptodate
	 * pages or ->readahead() that need to hold invalidate_lock
	 * while mapping blocks for IO so let's hold the lock here as
	 * well to keep locking rules simple.
	 */
	filemap_invalidate_lock_shared(mapping);
	index = (pos >> (PAGE_SHIFT + min_order)) << min_order;
	error = filemap_add_folio(mapping, folio, index,
			mapping_gfp_constraint(mapping, GFP_KERNEL));
	if (error == -EEXIST)
		error = AOP_TRUNCATED_PAGE;
	if (error)
		goto error;

	error = filemap_read_folio(file, mapping->a_ops->read_folio, folio);
	if (error)
		goto error;

	filemap_invalidate_unlock_shared(mapping);
	folio_batch_add(fbatch, folio);
	return 0;
error:
	filemap_invalidate_unlock_shared(mapping);
	folio_put(folio);
	return error;
}
```

**创建流程：**
1. **分配页面**：`filemap_alloc_folio()` - 从 buddy 分配器分配页面
2. **加锁保护**：`filemap_invalidate_lock_shared()` - 防止文件截断
3. **添加到缓存**：`filemap_add_folio()` - 将页面添加到 `i_pages` xarray
4. **读取数据**：`filemap_read_folio()` - 从磁盘读取数据到页面

#### 8.7.2 filemap_read_folio() - 从磁盘读取

```2152:2178:mm/filemap.c
int filemap_read_folio(struct kiocb *iocb, struct file *filp,
		struct file_ra_state *ra, struct folio *folio)
{
	struct address_space *mapping = filp->f_mapping;
	int error;

	/*
	 * A previous I/O error may have been due to temporary
	 * failures, eg. multipath errors.
	 * PG_error will be set again if read_folio fails.
	 */
	folio_clear_error(folio);
	error = mapping->a_ops->read_folio(filp, folio);
	if (error)
		return error;

	folio_mark_uptodate(folio);
	filemap_readahead(iocb, filp, ra, folio, mapping);

	return 0;
}
```

**读取过程：**
1. 调用文件系统的 `read_folio` 操作（例如 `ext4_read_folio()`）
2. 标记页面为 uptodate（数据最新）
3. 触发预读：`filemap_readahead()`

#### 8.7.3 ext4_read_folio() - ext4 文件系统读取

```3767:3792:fs/ext4/inode.c
static int ext4_read_folio(struct file *file, struct folio *folio)
{
	int ret = -EAGAIN;
	struct inode *inode = folio->mapping->host;

	trace_ext4_read_folio(inode, folio);

	if (ext4_has_inline_data(inode)) {
		ret = ext4_readpage_inline(inode, folio);
		if (ret != -EAGAIN)
			return ret;
	}

	if (trace_ext4_da_write_pages_enabled()) {
		ret = ext4_da_read_folio(file, folio);
		if (ret != -EAGAIN)
			return ret;
	}

	ret = mpage_read_folio(folio, ext4_get_block);
	if (ret)
		return ret;

	folio_unlock(folio);
	return 0;
}
```

**ext4 读取：**
- 检查内联数据（小文件优化）
- 调用 `mpage_read_folio()` - 多页面读取
- `ext4_get_block()` - 将文件偏移映射到磁盘块号

### 8.8 路径总结和关键数据结构

#### 8.8.1 完整调用链

```
用户空间：
read(fd, buf, count)
    ↓
内核空间：
ksys_read(fd, buf, count)
    ↓
vfs_read(file, buf, count, pos)
    ↓
__vfs_read(file, buf, count, pos)
    ↓
new_sync_read(filp, buf, len, ppos)
    ↓
call_read_iter(filp, kiocb, iter)
    ↓
ext4_file_read_iter(kiocb, iter)  [文件系统层]
    ↓
generic_file_read_iter(kiocb, iter)
    ↓
filemap_read(kiocb, iter, already_read)  [Page Cache 层]
    ↓
filemap_get_pages(kiocb, count, fbatch, fgp_flags)
    ↓
__filemap_get_folio(mapping, index, fgp_flags, gfp)  [查找/创建]
    ↓
    ├─→ 缓存命中：
    │       __filemap_get_folio_gfp()
    │           ↓
    │       filemap_get_entry()  [xarray 查找]
    │           ↓
    │       返回 folio
    │
    └─→ 缓存未命中：
            filemap_create_folio()
                ↓
            分配页面：filemap_alloc_folio()
                ↓
            添加到缓存：filemap_add_folio()
                ↓
            从磁盘读取：filemap_read_folio()
                ↓
                ext4_read_folio()
                    ↓
                mpage_read_folio()
                    ↓
                ext4_get_block() [块映射]
                    ↓
                提交 I/O 请求到块设备层
```

#### 8.8.2 关键数据结构

**1. struct file** - 打开的文件
```c
struct file {
    struct path f_path;
    struct inode *f_inode;
    const struct file_operations *f_op;
    loff_t f_pos;  // 文件偏移量
    struct address_space *f_mapping;  // 指向 address_space
    struct file_ra_state f_ra;  // 预读状态
    // ...
};
```

**2. struct address_space** - 地址空间（Page Cache 的容器）
```c
struct address_space {
    struct inode *host;
    struct xarray i_pages;  // Page Cache 的核心存储
    unsigned long nrpages;  // 缓存的页数
    const struct address_space_operations *a_ops;  // 操作函数
    // ...
};
```

**3. struct folio** - 页面容器
```c
struct folio {
    unsigned long flags;
    struct address_space *mapping;  // 所属的地址空间
    pgoff_t index;  // 文件中的页号
    atomic_t _refcount;  // 引用计数
    // ...
};
```

**4. struct xarray** - 扩展数组（存储页面）
```c
struct xarray {
    spinlock_t xa_lock;
    gfp_t xa_flags;
    void __rcu *xa_head;  // 树根节点
};
```

#### 8.8.3 查找性能考虑

1. **RCU 保护**：使用 RCU 读锁，支持无锁并发查找
2. **xarray 性能**：
   - 稀疏索引：只存储实际存在的页面
   - 树结构：O(log n) 查找复杂度
   - 支持大页面（THP）：一个索引可存储多个页面
3. **预读优化**：
   - 顺序访问时预读后续页面
   - 减少磁盘 I/O 次数
4. **缓存命中率**：
   - 频繁访问的文件保持在 Page Cache 中
   - LRU 算法管理页面淘汰

### 8.9 特殊情况处理

#### 8.9.1 内存映射（mmap）

如果文件是内存映射的，查找路径不同：

```c
// 用户空间：访问 mmap 的内存区域
*(char *)mmap_addr
    ↓
// 触发页错误（Page Fault）
do_page_fault()
    ↓
handle_mm_fault()
    ↓
filemap_fault()  // 文件映射的页错误处理
    ↓
filemap_get_folio()  // 同样在 Page Cache 中查找
```

#### 8.9.2 直接 I/O（O_DIRECT）

如果文件打开时指定了 `O_DIRECT` 标志：
- 绕过 Page Cache
- 直接调用 `mapping->a_ops->direct_IO()`
- 数据直接从磁盘到用户缓冲区

#### 8.9.3 同步读取

某些情况下需要同步读取（例如 `O_SYNC`）：
- 等待 I/O 完成
- 确保数据已写回磁盘
- 使用 `filemap_write_and_wait_range()`

### 8.10 调试和监控

#### 8.10.1 跟踪点（Tracepoints）

内核提供了多个跟踪点来监控文件查找：

```bash
# 启用跟踪点
echo 1 > /sys/kernel/debug/tracing/events/filemap/enable

# 查看跟踪输出
cat /sys/kernel/debug/tracing/trace

# 相关跟踪点：
# - mm_filemap_add_to_page_cache
# - mm_filemap_delete_from_page_cache
# - mm_filemap_fault
```

#### 8.10.2 /proc 接口

```bash
# 查看 Page Cache 统计
cat /proc/meminfo | grep -E "Cached|Buffers|Dirty"

# 查看特定进程的文件映射
cat /proc/<pid>/maps
cat /proc/<pid>/smaps
```

#### 8.10.3 性能分析工具

- **ftrace**：跟踪内核函数调用
- **perf**：性能分析
- **bpftrace**：动态跟踪

---

## 九、内核如何识别文件页和匿名页

内核需要准确区分文件页（File Page）和匿名页（Anonymous Page），因为它们在内存管理、回收策略、swap 处理等方面有根本性的不同。本节详细解析内核的识别机制。

### 9.1 页面类型的基本概念

**文件页（File Page）：**
- 对应文件系统中的文件内容
- 有后备存储（backing store）：磁盘上的文件
- 属于某个 `address_space`（通过 `folio->mapping` 指向）
- 存储在 Page Cache 中
- 回收时可以直接丢弃（数据在磁盘上）

**匿名页（Anonymous Page）：**
- 不对应任何文件
- 通常是进程的堆、栈等内存区域
- 没有后备存储（除非 swap 出去）
- `folio->mapping` 为 NULL 或指向特殊的 anon_vma
- 必须 swap 才能回收

### 9.2 识别机制：mapping 字段

#### 9.2.1 folio 结构中的 mapping 字段

`struct folio` 中有一个关键的 `mapping` 字段：

```47:63:include/linux/mm_types.h
struct folio {
	/* private: don't document the anon or file, or swap members */
	union {
		struct {
	/* public: */
			unsigned long flags;
			struct list_head lru;
			struct address_space *mapping;
			pgoff_t index;
			void *private;
			atomic_t _mapcount;
			atomic_t _refcount;
```

**mapping 字段的三种状态：**

1. **指向 address_space**：文件页
   - `mapping` 指向一个有效的 `struct address_space`
   - 该页面属于某个文件的 Page Cache

2. **指向 anon_vma**：匿名页
   - `mapping` 指向 `struct anon_vma`（低 2 位为标记）
   - 使用指针的低位作为标志位

3. **NULL**：特殊情况
   - 未初始化的页面
   - 某些特殊用途的页面

#### 9.2.2 使用 mapping 字段区分页面类型

内核使用指针的低位作为标志位来区分：

```136:146:include/linux/mm.h
/*
 * On an anonymous page mapped into a user virtual memory page,
 * page->mapping points to its anon_vma, not to a struct address_space;
 * with the PAGE_MAPPING_ANON bit set to distinguish it.  See rmap.h.
 *
 * On an anonymous page in a VM_MERGEABLE area, if CONFIG_KSM is enabled,
 * the PAGE_MAPPING_MOVABLE bit may be set along with the PAGE_MAPPING_ANON
 * bit; and then page->mapping points, not to an anon_vma, but to a private
 * struct ksm_master_page.
 *
 * PAGE_MAPPING_KSM without PAGE_MAPPING_ANON is currently never used.
 *
 * Please note that, confusingly, "page_mapping" refers to the inode
 * address_space while "page_mapped" refers to actual userspace mappings of
 * the page.
 */
```

**标志位定义：**

```165:169:include/linux/mm.h
#define PAGE_MAPPING_ANON	0x1
#define PAGE_MAPPING_MOVABLE	0x2
#define PAGE_MAPPING_KSM	(PAGE_MAPPING_ANON | PAGE_MAPPING_MOVABLE)
#define PAGE_MAPPING_FLAGS	(PAGE_MAPPING_ANON | PAGE_MAPPING_MOVABLE)
```

#### 9.2.3 核心判断函数：folio_mapping()

```212:241:include/linux/mm.h
static inline struct address_space *folio_mapping(struct folio *folio)
{
	struct address_space *mapping;

	/* This happens if someone calls flush_dcache_page on slab page */
	if (unlikely(folio_test_slab(folio)))
		return NULL;

	if (unlikely(folio_test_swapcache(folio)))
		return swap_address_space(folio_swap_entry(folio));

	mapping = folio->mapping;
	if ((unsigned long)mapping & PAGE_MAPPING_ANON)
		return NULL;

	return (struct address_space *)((unsigned long)mapping & ~PAGE_MAPPING_FLAGS);
}
```

**函数逻辑：**
1. 检查是否为 slab 页面：如果是，返回 NULL
2. 检查是否为 swap cache 页面：如果是，返回 swap 的 address_space
3. 检查 `mapping` 的低位：
   - 如果设置了 `PAGE_MAPPING_ANON`，说明是匿名页，返回 NULL
   - 否则，清除标志位，返回 `address_space` 指针

### 9.3 核心判断函数

#### 9.3.1 folio_test_anon() - 判断是否为匿名页

```191:194:include/linux/mm.h
static inline bool folio_test_anon(struct folio *folio)
{
	return ((unsigned long)folio->mapping & PAGE_MAPPING_ANON) != 0;
}
```

**判断逻辑：**
- 检查 `mapping` 字段的最低一位（`PAGE_MAPPING_ANON`）
- 如果设置了，说明是匿名页

#### 9.3.2 folio_is_file_lru() - 判断是否属于文件 LRU

```361:364:mm/swap.c
static inline bool folio_is_file_lru(struct folio *folio)
{
	return !folio_test_anon(folio);
}
```

**判断逻辑：**
- 简单反义：不是匿名页的就是文件页
- 用于确定页面应该在哪个 LRU 列表中

#### 9.3.3 folio_mapping() - 获取 address_space

上面的 `folio_mapping()` 函数用于获取文件页的 `address_space`：
- 对于文件页：返回对应的 `address_space`
- 对于匿名页：返回 NULL

### 9.4 匿名页的 mapping 设置

#### 9.4.1 创建匿名页时设置

当创建匿名页（例如通过 `do_anonymous_page()`）时：

```4087:4122:mm/memory.c
static vm_fault_t do_anonymous_page(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct folio *folio;
	vm_fault_t ret = 0;
	pgoff_t idx = vmf->pgoff;

	/*
	 * Use __pte_alloc() instead of pte_alloc(). We can't run pte_offset_map()
	 * on pmds, where a huge pmd might be created in a parallel thread. But
	 * doing it in mmap_lock context could be contended. See vma_alloc_folio().
	 */
	if (pte_alloc(vma->vm_mm, vmf->pmd))
		return VM_FAULT_OOM;

	/* File mapping without ->vm_ops ? */
	if (vma->vm_ops)
		return VM_FAULT_SIGBUS;

	/* Check if we need to add a guard page to the stack. */
	if (check_stack_guard_page(vma, vmf->address) < 0)
		return VM_FAULT_SIGBUS;

	/*
	 * Use pte_alloc() instead of pte_alloc_map().  We can't run
	 * pte_offset_map() on pmds where a full pmd might be created
	 * from a different thread.
	 *
	 * pte_alloc_map() is unsafe to use under mmap_read_lock() or
	 * outside of locked context since it might allocate memory.
	 */
	if (pte_alloc(vma->vm_mm, vmf->pmd))
		return VM_FAULT_OOM;

	/* See comment in pte_alloc_one_map() */
	if (unlikely(pmd_trans_unstable(vmf->pmd)))
		return 0;

	if (unlikely(anon_vma_prepare(vma)))
		return VM_FAULT_OOM;

	folio = vma_alloc_folio(GFP_HIGHUSER_MOVABLE, 0, vma, vmf->address,
				false);
	if (!folio)
		return VM_FAULT_OOM;
```

匿名页在分配后，`mapping` 字段会通过 `anon_vma` 相关函数设置。

#### 9.4.2 anon_vma 的设置

```335:377:mm/rmap.c
static inline void page_set_anon_rmap(struct page *page,
	struct vm_area_struct *vma, unsigned long address, bool exclusive)
{
	struct anon_vma *anon_vma = vma->anon_vma;

	page->index = linear_page_index(vma, address);

	BUG_ON(!anon_vma);
	anon_vma = (void *) anon_vma + PAGE_MAPPING_ANON;
	page->mapping = (struct address_space *) anon_vma;
}
```

**关键操作：**
1. 获取 `vma->anon_vma`
2. **设置标志位**：`anon_vma = (void *)anon_vma + PAGE_MAPPING_ANON`
   - 通过指针算术，在地址上加 1（`PAGE_MAPPING_ANON = 0x1`）
   - 这样指针的最低位置 1，同时保留了 `anon_vma` 的地址信息
3. 将修改后的指针赋给 `page->mapping`

**为什么这样可以工作？**
- `anon_vma` 结构通常是 4/8 字节对齐的
- 最低位（bit 0）本来就应该为 0
- 加 1 后，最低位置 1，同时不影响其他位的地址信息

### 9.5 文件页的 mapping 设置

#### 9.5.1 添加到 Page Cache 时设置

文件页在添加到 Page Cache 时设置 `mapping`：

```851:970:mm/filemap.c
noinline int __filemap_add_folio(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	XA_STATE(xas, &mapping->i_pages, index);
	// ...
	folio->mapping = mapping;
	folio->index = xas.xa_index;
	// ...
}
```

**关键操作：**
- `folio->mapping = mapping;` - 直接指向 `address_space`
- 不需要设置任何标志位
- `address_space` 指针通常按 8 字节对齐，最低位自然为 0

### 9.6 完整的识别流程

#### 9.6.1 判断页面类型的完整流程

```c
// 伪代码：判断页面类型
bool is_anonymous_page(struct folio *folio) {
    // 方法1：使用专门的测试函数
    if (folio_test_anon(folio))
        return true;
    
    // 方法2：检查 mapping 字段
    if ((unsigned long)folio->mapping & PAGE_MAPPING_ANON)
        return true;
    
    // 方法3：使用 folio_mapping()
    if (folio_mapping(folio) == NULL)
        return true;  // 注意：需要排除其他 NULL 情况
    
    return false;
}

bool is_file_page(struct folio *folio) {
    // 不是匿名页的就是文件页
    return !folio_test_anon(folio) && folio_mapping(folio) != NULL;
}
```

#### 9.6.2 实际使用示例

在内存回收中的判断：

```1368:1383:mm/vmscan.c
		/*
		 * Anonymous process memory has backing store?
		 * Try to allocate it some swap space here.
		 * Lazyfree folios could be freed directly
		 */
		if (folio_test_anon(folio) && folio_test_swapbacked(folio)) {
			if (!folio_test_swapcache(folio)) {
				if (!(sc->gfp_mask & __GFP_IO))
					goto keep_locked;
				if (folio_may_be_demoted(folio))
					goto keep_locked;
				if (folio_try_swapcache_add(folio) == 0) {
					folio_set_swapcache(folio);
					folio_set_dirty(folio);
				} else {
					goto keep_locked;
				}
			}
		} else if (folio_test_swapbacked(folio) &&
			   !folio_test_swapcache(folio)) {
			folio_clear_swapbacked(folio);
		}
```

**逻辑：**
- 如果是匿名页且支持 swap（`folio_test_swapbacked`），尝试分配 swap 空间
- 如果是文件页，不会进入这个分支

在 LRU 列表管理中的判断：

```1288:1293:mm/swap.c
		if (folio_lru(folio)) {
			int lru = folio_is_file_lru(folio);
			folio_activate(folio);
			folio_set_referenced(folio);
			folio_clear_referenced(folio);
		}
```

**逻辑：**
- `folio_is_file_lru()` 判断页面应该属于文件 LRU 还是匿名 LRU
- 根据结果将页面添加到对应的 LRU 列表

### 9.7 特殊情况：swap cache 页面

#### 9.7.1 swap cache 的特殊性

swap cache 页面比较特殊，它们：
- 原本是匿名页（被 swap 出去）
- 但从 swap 读回时，临时存在于 swap cache 中
- `folio->mapping` 指向 swap 的 `address_space`

```710:731:mm/swap_state.c
int init_swap_address_space(unsigned int type, unsigned long nr_pages)
{
	struct address_space *spaces, *space;
	unsigned int i, nr;

	nr = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_PAGES);
	spaces = kvcalloc(nr, sizeof(struct address_space), GFP_KERNEL);
	if (!spaces)
		return -ENOMEM;
	for (i = 0; i < nr; i++) {
		space = spaces + i;
		xa_init_flags(&space->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space->i_mmap_writable, 0);
		space->a_ops = &swap_aops;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space);
	}
	nr_swapper_spaces[type] = nr;
	swapper_spaces[type] = spaces;

	return 0;
}
```

#### 9.7.2 判断 swap cache 页面

```154:158:include/linux/mm.h
static inline bool folio_test_swapcache(struct folio *folio)
{
	return folio_test_swapbacked(folio) &&
		folio_mapping(folio) == swap_address_space(
				folio_swap_entry(folio));
}
```

**判断逻辑：**
- 检查 `mapping` 是否指向 swap 的 `address_space`
- 检查是否有 swap entry

### 9.8 内存统计中的分类

#### 9.8.1 LRU 列表分类

```28:44:include/linux/mmzone.h
enum lru_list {
	LRU_INACTIVE_ANON = LRU_BASE,
	LRU_ACTIVE_ANON = LRU_BASE + LRU_ACTIVE,
	LRU_INACTIVE_FILE = LRU_BASE + LRU_FILE,
	LRU_ACTIVE_FILE = LRU_BASE + LRU_FILE + LRU_ACTIVE,
	LRU_UNEVICTABLE,
	NR_LRU_LISTS
};
```

页面根据类型被放入不同的 LRU 列表：
- **匿名页**：`LRU_INACTIVE_ANON` 或 `LRU_ACTIVE_ANON`
- **文件页**：`LRU_INACTIVE_FILE` 或 `LRU_ACTIVE_FILE`

#### 9.8.2 统计信息更新

当页面添加到缓存时：

```684:689:mm/filemap.c
		/* hugetlb pages do not participate in page cache accounting */
		if (!huge) {
			__lruvec_stat_mod_folio(folio, NR_FILE_PAGES, nr);
			if (folio_test_pmd_mappable(folio))
				__lruvec_stat_mod_folio(folio,
						NR_FILE_THPS, nr);
		}
```

文件页更新 `NR_FILE_PAGES` 统计。

匿名页在分配时：

```c
// 匿名页更新 NR_ANON_MAPPED 或相关统计
__lruvec_stat_mod_folio(folio, NR_ANON_MAPPED, nr);
```

### 9.9 识别机制的优势

#### 9.9.1 高效性

- **O(1) 复杂度**：只需要检查指针的最低几位
- **无需额外存储**：利用指针的对齐特性，不占用额外空间
- **位操作**：使用高效的位运算

#### 9.9.2 正确性

- **明确区分**：通过标志位明确区分页面类型
- **类型安全**：编译时和运行时都有检查
- **特殊情况处理**：swap cache 等特殊情况有专门处理

### 9.10 实际调试和验证

#### 9.10.1 查看页面类型

```bash
# 通过 /proc/kpageflags 查看页面标志
# (需要内核编译时启用 CONFIG_PROC_PAGE_MONITOR)

# 或者使用 crash 工具
crash> page -p <page_address> flags
```

#### 9.10.2 内核调试输出

在内核代码中可以添加调试输出：

```c
if (folio_test_anon(folio))
    pr_debug("Page is anonymous\n");
else
    pr_debug("Page is file page, mapping=%px\n", folio_mapping(folio));
```

### 9.11 总结

内核通过以下机制识别文件页和匿名页：

1. **mapping 字段**：
   - 文件页：指向 `address_space`（指针最低位为 0）
   - 匿名页：指向 `anon_vma`（指针最低位为 1）

2. **标志位**：
   - `PAGE_MAPPING_ANON`：标识匿名页
   - 利用指针对齐特性，用最低位存储标志

3. **判断函数**：
   - `folio_test_anon()`：判断是否为匿名页
   - `folio_mapping()`：获取 address_space（匿名页返回 NULL）
   - `folio_is_file_lru()`：判断是否属于文件 LRU

4. **应用场景**：
   - LRU 列表管理
   - 内存回收策略
   - Swap 管理
   - 统计信息更新

这种设计巧妙利用了指针的对齐特性，在不增加存储开销的情况下实现了高效的页面类型识别。

---

## 十、struct folio 详解：为什么需要它以及它的作用

`struct folio` 是 Linux 内核内存管理中的一个重要数据结构，用于表示一个或多个物理页面。理解 folio 的设计目的和用途对于理解现代内核的内存管理至关重要。

### 10.1 folio 的诞生背景：解决 struct page 的问题

#### 10.1.1 传统 struct page 的问题

在引入 folio 之前，内核主要使用 `struct page` 来表示物理页面。但 `struct page` 存在以下问题：

1. **只表示单个页面**：
   - `struct page` 总是表示一个 4KB 的页面
   - 当需要处理大页面（THP - Transparent Huge Pages，2MB 或更大）时，需要遍历多个 `struct page`
   - 代码中大量出现 `page += i` 这样的指针运算，容易出错

2. **类型混淆**：
   - `struct page` 同时用于表示：
     - 单个物理页面
     - 大页面的一部分
     - 复合页面（compound page）的头页
   - 很难从类型上区分这几种情况

3. **API 设计问题**：
   - 很多函数接受 `struct page *`，但实际可能处理的是多个页面
   - 调用者需要记住这个函数是否支持复合页面
   - 容易产生 bug，例如只更新了头页而忽略了尾页

#### 10.1.2 引入 folio 的动机

**folio**（拉丁语，意思是"叶子"或"页面"）的设计目标是：

1. **统一抽象**：无论是一个页面还是多个页面，都用 `struct folio` 表示
2. **类型安全**：从类型就能知道这是一个页面容器，可能包含多个页面
3. **简化代码**：API 更清晰，减少错误

### 10.2 struct folio 的定义和结构

#### 10.2.1 folio 的完整定义

```47:82:include/linux/mm_types.h
struct folio {
	/* private: don't document the anon or file, or swap members */
	union {
		struct {
	/* public: */
			unsigned long flags;
			struct list_head lru;
			struct address_space *mapping;
			pgoff_t index;
			void *private;
			atomic_t _mapcount;
			atomic_t _refcount;
#ifdef CONFIG_MEMCG
			unsigned long memcg_data;
#endif
	/* public: */
		};
		struct {
			dma_addr_t dma_addr;
		};
		struct {
			union {
				struct list_head lru;
				struct {
					void *__filler;
					unsigned int mnt_id;
				};
			};
			struct address_space *mapping;
			pgoff_t index;
		};
		struct {
			union {
				struct list_head lru;
				struct {
					void *__filler;
					unsigned int mnt_id;
				};
			};
			struct address_space *mapping;
			pgoff_t index;
			void *private;
			atomic_t _mapcount;
			atomic_t _refcount;
#ifdef CONFIG_MEMCG
			unsigned long memcg_data;
#endif
		};
	};
	unsigned long _flags_1;
	unsigned long _flags_2;
};
```

**关键特点：**
- `struct folio` 的第一个字段就是 `struct page`（通过 union 实现）
- 实际上，`struct folio` 就是对 `struct page` 的包装
- 提供了更高级的抽象和操作接口

#### 10.2.2 folio 与 page 的关系

```c
// folio 实际上就是 page 的包装
#define folio_page(folio, n)	nth_page(&(folio)->page, n)
```

**关系说明：**
- `struct folio` 的第一个成员是 `struct page`
- 对于单页面：`folio->page` 就是这个页面
- 对于大页面：`folio->page` 是头页，可以通过 `folio_page(folio, i)` 访问其他页面

### 10.3 folio 的主要用途

#### 10.3.1 统一表示单个或多个页面

**单页面情况：**
```c
// 分配单个页面
struct folio *folio = folio_alloc(gfp_mask, 0);  // order=0，单页面
// folio->page 就是这个页面
```

**大页面情况：**
```c
// 分配大页面（例如 THP，2MB）
struct folio *folio = folio_alloc(gfp_mask, 9);  // order=9，512 个页面
// folio->page 是头页
// folio_nr_pages(folio) == 512
```

**关键优势：**
- 无论是一个还是多个页面，都使用相同的 `struct folio` 类型
- API 统一，调用者不需要关心页面数量

#### 10.3.2 页面操作的统一接口

**访问页面内容：**
```c
// 获取页面的虚拟地址
void *kaddr = folio_address(folio);

// 获取页面的物理地址（第一个页面）
phys_addr_t paddr = folio_pfn(folio) << PAGE_SHIFT;

// 获取页面数量
unsigned int nr_pages = folio_nr_pages(folio);

// 获取页面大小
size_t size = folio_size(folio);  // 自动处理单页和大页
```

**访问特定页面：**
```c
// 获取第 i 个页面
struct page *page = folio_page(folio, i);

// 遍历所有页面
for (int i = 0; i < folio_nr_pages(folio); i++) {
    struct page *page = folio_page(folio, i);
    // 处理页面
}
```

#### 10.3.3 页面状态管理

**标志位操作：**
```c
// 检查标志
if (folio_test_dirty(folio))
    // 页面是脏的

if (folio_test_uptodate(folio))
    // 页面数据是最新的

// 设置标志
folio_set_dirty(folio);
folio_clear_dirty(folio);
```

**引用计数：**
```c
// 获取引用（增加计数）
folio_get(folio);

// 释放引用（减少计数）
folio_put(folio);

// 检查计数
if (folio_ref_count(folio) > 1)
    // 有多个引用
```

#### 10.3.4 在 Page Cache 中的使用

**添加到 Page Cache：**
```851:970:mm/filemap.c
noinline int __filemap_add_folio(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	XA_STATE(xas, &mapping->i_pages, index);
	// ...
	folio->mapping = mapping;
	folio->index = xas.xa_index;
	// ...
}
```

**从 Page Cache 查找：**
```396:438:mm/filemap.c
struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index,
		fgf_t fgp_flags, gfp_t gfp)
{
	struct folio *folio;
	// ...
	folio = __filemap_get_folio_gfp(mapping, index, fgp_flags, gfp);
	// ...
	return folio;
}
```

**关键优势：**
- 无论单页还是大页，都使用相同的 API
- xarray 可以直接存储大页面（一个索引对应多个页面）
- 减少了代码复杂度和错误

### 10.4 folio vs page：主要区别

#### 10.4.1 API 层面的区别

**旧代码（使用 page）：**
```c
struct page *page = alloc_pages(gfp_mask, order);
if (page) {
    // 需要手动处理多个页面
    for (int i = 0; i < (1 << order); i++) {
        struct page *p = page + i;
        // 处理每个页面
        // 容易忘记处理尾页，导致 bug
    }
}
```

**新代码（使用 folio）：**
```c
struct folio *folio = folio_alloc(gfp_mask, order);
if (folio) {
    // 统一处理，无论单页还是多页
    folio_mark_dirty(folio);  // 自动处理所有页面
    // 或者显式遍历
    for (int i = 0; i < folio_nr_pages(folio); i++) {
        struct page *p = folio_page(folio, i);
        // 处理页面
    }
}
```

#### 10.4.2 类型语义的区别

| 特性 | struct page | struct folio |
|------|------------|--------------|
| 表示范围 | 总是单个页面 | 可能是一个或多个页面 |
| 类型含义 | 不明确（可能是复合页的一部分） | 明确（一个完整的页面容器） |
| API 清晰度 | 需要知道是否支持复合页 | 自动处理复合页 |
| 大页面支持 | 需要手动处理 | 原生支持 |

#### 10.4.3 实际使用示例对比

**示例 1：标记页面为脏**

```c
// 旧方式（使用 page）
void mark_page_dirty(struct page *page) {
    if (PageCompound(page)) {
        // 复合页面，需要标记所有页面
        unsigned int nr_pages = 1 << compound_order(page);
        for (unsigned int i = 0; i < nr_pages; i++) {
            SetPageDirty(page + i);
        }
    } else {
        // 单页面
        SetPageDirty(page);
    }
}

// 新方式（使用 folio）
void mark_folio_dirty(struct folio *folio) {
    folio_set_dirty(folio);  // 自动处理所有情况
}
```

**示例 2：获取页面大小**

```c
// 旧方式
size_t get_page_size(struct page *page) {
    if (PageCompound(page))
        return PAGE_SIZE << compound_order(page);
    return PAGE_SIZE;
}

// 新方式
size_t get_folio_size(struct folio *folio) {
    return folio_size(folio);  // 一行搞定
}
```

### 10.5 folio 的关键操作函数

#### 10.5.1 分配和释放

```c
// 分配 folio
struct folio *folio_alloc(gfp_t gfp, unsigned int order);

// 释放 folio
void folio_free(struct folio *folio);

// 从 page 创建 folio（不需要分配）
static inline struct folio *page_folio(struct page *page);
```

#### 10.5.2 页面数量和大小的获取

```c
// 获取页面数量
static inline unsigned int folio_nr_pages(const struct folio *folio);

// 获取页面大小（字节）
static inline size_t folio_size(const struct folio *folio);

// 获取页面序号（在文件中的位置）
static inline pgoff_t folio_index(const struct folio *folio);
```

#### 10.5.3 标志位操作

```c
// 检查标志
bool folio_test_dirty(const struct folio *folio);
bool folio_test_uptodate(const struct folio *folio);
bool folio_test_locked(const struct folio *folio);
bool folio_test_swapbacked(const struct folio *folio);

// 设置标志
void folio_set_dirty(struct folio *folio);
void folio_clear_dirty(struct folio *folio);
void folio_mark_uptodate(struct folio *folio);
```

#### 10.5.4 引用计数

```c
// 获取引用
void folio_get(struct folio *folio);

// 释放引用
void folio_put(struct folio *folio);

// 获取引用计数
int folio_ref_count(const struct folio *folio);
```

#### 10.5.5 地址转换

```c
// 获取虚拟地址（kmap）
void *folio_address(const struct folio *folio);

// 获取 PFN（Page Frame Number）
static inline unsigned long folio_pfn(const struct folio *folio);

// 获取特定页面
struct page *folio_page(const struct folio *folio, size_t n);
```

### 10.6 folio 在内存管理中的实际应用

#### 10.6.1 Page Cache 中的使用

**添加文件页到缓存：**
```851:970:mm/filemap.c
noinline int __filemap_add_folio(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	// ...
	folio->mapping = mapping;
	folio->index = index;
	// 无论单页还是大页，都统一处理
	mapping->nrpages += folio_nr_pages(folio);
	// ...
}
```

**查找页面：**
```396:438:mm/filemap.c
struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index)
{
	return __filemap_get_folio(mapping, index, 0, 0);
}
```

#### 10.6.2 内存回收中的使用

**回收决策：**
```c
// 在 shrink_page_list() 中
if (folio_test_anon(folio)) {
    // 处理匿名页
} else {
    // 处理文件页
}

// 无论单页还是大页，都使用相同的判断
```

#### 10.6.3 THP（Transparent Huge Pages）支持

**大页面的优势：**
- 减少页表项数量
- 提高 TLB 命中率
- 减少页面管理开销

**folio 使 THP 更容易使用：**
```c
// 分配 THP（如果可能）
struct folio *folio = vma_alloc_folio(GFP_HIGHUSER_MOVABLE, 
                                       HPAGE_PMD_ORDER, vma, addr, false);
if (folio) {
    // 自动是一个大页面（2MB）
    // 不需要特殊处理，API 统一
}
```

### 10.7 folio 的设计优势总结

1. **类型安全**：
   - `struct folio` 明确表示一个页面容器
   - 减少类型混淆和错误

2. **API 统一**：
   - 单页和大页使用相同的 API
   - 不需要判断页面数量

3. **代码简化**：
   - 减少 `if (PageCompound(page))` 这样的判断
   - 减少循环处理多个页面的代码

4. **性能优化**：
   - 更好地支持大页面
   - 减少函数调用开销

5. **可维护性**：
   - 代码更清晰
   - 更容易理解和调试

### 10.8 迁移策略：从 page 到 folio

内核采用了渐进式迁移策略：

1. **新代码优先使用 folio**：
   - 新的内存管理代码使用 folio
   - Page Cache 相关代码逐步迁移

2. **保持兼容性**：
   - 提供 `page_folio()` 从 page 转换到 folio
   - 旧的 page API 仍然可用

3. **逐步迁移**：
   - 核心路径先迁移（如 filemap）
   - 其他代码逐步跟进

### 10.9 总结

`struct folio` 是现代 Linux 内核中用于表示物理页面的统一抽象：

1. **目的**：解决 `struct page` 在处理多页面时的局限性和复杂性

2. **本质**：`struct folio` 是对 `struct page` 的高级封装，提供了更清晰的语义和更统一的 API

3. **优势**：
   - 统一表示单页和多页
   - 类型安全，语义清晰
   - API 简化，减少错误
   - 更好地支持 THP

4. **应用**：
   - Page Cache 管理
   - 内存回收
   - 文件系统操作
   - 大页面支持

5. **未来**：内核正在从 `struct page` 向 `struct folio` 迁移，新代码应该优先使用 folio API

理解 folio 对于理解现代内核的内存管理机制至关重要，它是内核向更高效、更安全的页面管理演进的重要一步。

---

## 十一、总结

Page Cache 是 Linux 内核中至关重要的性能优化机制：

1. **统一缓存**：为所有文件系统提供统一的页面缓存
2. **智能管理**：通过 LRU 算法管理缓存页面
3. **预读优化**：预测访问模式，提前加载数据
4. **延迟写回**：合并写操作，减少磁盘 I/O
5. **内存回收**：与内存管理系统紧密集成，支持高效回收

理解 Page Cache 的工作原理对于优化系统性能和调试相关问题非常重要。

---

## 十二、内核如何记录页面的使用状态

内核需要准确跟踪每个物理页面的状态：是空闲的还是已被分配、是否被使用、谁在使用它等。这个机制涉及到多个层面的信息记录。

### 12.1 PFN（Page Frame Number）：页面的唯一标识

#### 12.1.1 PFN 是什么

**PFN（Page Frame Number）**是物理页面的编号，它是页面的唯一标识符：

```c
// PFN 的定义
typedef unsigned long pfn_t;

// PFN 和物理地址的关系
#define __pfn_to_page(pfn)	(mem_map + ((pfn) - ARCH_PFN_OFFSET))
#define __page_to_pfn(page)	((unsigned long)((page) - mem_map) + \
				 ARCH_PFN_OFFSET)

// PFN 和物理地址的转换
#define PFN_PHYS(pfn)		((phys_addr_t)(pfn) << PAGE_SHIFT)
#define PHYS_PFN(phys)		((phys_addr_t)(phys) >> PAGE_SHIFT)
```

**关键概念：**
- PFN 从 0 开始编号，每个物理页面有唯一的 PFN
- PFN 和物理地址的关系：`物理地址 = PFN × PAGE_SIZE`
- `mem_map` 是一个数组，`mem_map[pfn]` 就是对应的 `struct page`

#### 12.1.2 PFN 的作用

PFN 主要用于：
1. **唯一标识页面**：通过 PFN 可以找到对应的 `struct page`
2. **页面查找**：在 `struct page` 和物理地址之间转换
3. **内存统计**：跟踪哪些 PFN 范围内的页面被使用

但 **PFN 本身不记录页面是否被使用**，它只是页面的编号。

### 12.2 struct page：页面的核心状态记录

#### 12.2.1 struct page 的关键字段

每个物理页面都有一个对应的 `struct page`，它记录了页面的所有状态信息：

```47:82:include/linux/mm_types.h
struct folio {
	/* private: don't document the anon or file, or swap members */
	union {
		struct {
	/* public: */
			unsigned long flags;      // 页面状态标志
			struct list_head lru;      // LRU 列表节点
			struct address_space *mapping;  // 所属的地址空间
			pgoff_t index;            // 页面索引
			void *private;            // 私有数据
			atomic_t _mapcount;       // 映射计数
			atomic_t _refcount;       // 引用计数
#ifdef CONFIG_MEMCG
			unsigned long memcg_data;
#endif
```

**关键字段说明：**

1. **flags**：页面状态标志位（最重要）
2. **_refcount**：引用计数（谁在使用这个页面）
3. **_mapcount**：映射计数（有多少个页表项映射这个页面）
4. **lru**：LRU 列表节点（用于页面回收）
5. **mapping**：所属的地址空间（文件页或匿名页标识）

#### 12.2.2 flags 字段：页面的状态标志

`flags` 字段使用位图记录页面的各种状态：

```c
// include/linux/page-flags.h

// 页面状态标志位定义
enum pageflags {
	PG_locked,		/* 页面被锁定 */
	PG_error,		/* 页面 I/O 错误 */
	PG_referenced,		/* 页面被引用（用于 LRU） */
	PG_uptodate,		/* 页面数据是最新的 */
	PG_dirty,		/* 页面是脏的（需要写回） */
	PG_lru,			/* 页面在 LRU 列表中 */
	PG_active,		/* 页面是活跃的 */
	PG_workingset,		/* 页面在工作集中 */
	PG_waiters,		/* 有进程在等待这个页面 */
	PG_slab,		/* 页面属于 slab */
	PG_owner_priv_1,	/* 所有者私有标志 1 */
	PG_arch_1,		/* 架构相关标志 1 */
	PG_reserved,		/* 页面被保留（不可分配） */
	PG_private,		/* 页面有私有数据 */
	PG_private_2,		/* 页面私有标志 2 */
	PG_writeback,		/* 页面正在写回 */
	PG_head,		/* 复合页面的头页 */
	PG_mappedtodisk,	/* 页面已映射到磁盘 */
	PG_reclaim,		/* 页面正在被回收 */
	PG_swapbacked,		/* 页面支持 swap */
	PG_unevictable,		/* 页面不可回收 */
	PG_mlocked,		/* 页面被 mlock */
	// ... 更多标志位
};
```

**使用示例：**

```c
// 检查标志
if (PageDirty(page))
    // 页面是脏的

if (PageUptodate(page))
    // 页面数据最新

if (PageLRU(page))
    // 页面在 LRU 列表中

// 设置标志
SetPageDirty(page);
ClearPageDirty(page);

// 测试并设置标志
if (TestSetPageLocked(page))
    // 页面已经被锁定
```

#### 12.2.3 _refcount：引用计数

`_refcount` 记录有多少个地方在使用这个页面：

```c
// include/linux/mm_types.h
atomic_t _refcount;  // 原子变量，线程安全

// 引用计数操作
static inline int page_ref_count(const struct page *page)
{
	return atomic_read(&page->_refcount);
}

static inline void page_ref_inc(struct page *page)
{
	atomic_inc(&page->_refcount);
}

static inline int page_ref_dec_and_test(struct page *page)
{
	return atomic_dec_and_test(&page->_refcount);
}
```

**引用计数的含义：**
- `_refcount = 0`：页面没有被使用，可以被释放
- `_refcount > 0`：页面正在被使用，不能被释放
- 当 `_refcount` 减到 0 时，页面会被释放回 buddy 分配器

**谁会增加引用计数：**
- 页面被添加到 Page Cache 时
- 页面被映射到进程地址空间时
- 页面被锁定时（`lock_page()`）
- I/O 操作进行中时

#### 12.2.4 _mapcount：映射计数

`_mapcount` 记录有多少个页表项（PTE）映射这个页面：

```c
// include/linux/mm_types.h
atomic_t _mapcount;  // 映射计数

// 映射计数操作
static inline int page_mapcount(const struct page *page)
{
	return atomic_read(&page->_mapcount) + 1;
}
```

**映射计数的含义：**
- `_mapcount = -1`：页面没有被映射到任何页表
- `_mapcount = 0`：页面被映射到 1 个页表（page_mapcount = 1）
- `_mapcount > 0`：页面被映射到多个页表（共享页面）

### 12.3 Buddy 分配器：跟踪空闲页面

#### 12.3.1 free_area 结构

Buddy 分配器通过 `free_area` 结构跟踪空闲页面：

```c
// include/linux/mmzone.h

#define MAX_ORDER 11  // 最大 order（2^11 = 2048 个页面，8MB）

struct free_area {
	struct list_head	free_list[MIGRATE_TYPES];  // 空闲页面链表
	unsigned long		nr_free;  // 空闲页面数量
};

struct zone {
	// ...
	struct free_area	free_area[MAX_ORDER];  // 按 order 组织的空闲区域
	// ...
	unsigned long		managed_pages;  // 管理的总页面数
	unsigned long		spanned_pages;  // zone 覆盖的总页面数
	unsigned long		present_pages;  // 实际存在的页面数
	// ...
};
```

**关键概念：**
- `free_area[order]`：存储 order 为 `order` 的空闲页面块
- `free_list[migrate_type]`：按迁移类型组织的空闲页面链表
- `nr_free`：该 order 下的空闲页面数量

#### 12.3.2 页面分配状态判断

**页面是否被分配：**

```c
// 如果页面在 free_area 的链表中，说明是空闲的
// 如果页面不在链表中，说明已被分配

// 检查页面是否空闲（简化逻辑）
static inline bool page_is_free(struct page *page)
{
	// 如果页面在 LRU 列表中，说明已被使用
	if (PageLRU(page))
		return false;
	
	// 如果页面有引用，说明已被使用
	if (page_ref_count(page) > 0)
		return false;
	
	// 如果页面属于 buddy 系统，检查是否在空闲链表
	// 实际实现更复杂，需要考虑各种情况
	return true;
}
```

#### 12.3.3 Buddy 分配器的页面管理

**分配页面时：**

```c
// mm/page_alloc.c
struct page *__alloc_pages(gfp_t gfp_mask, unsigned int order,
			   struct zonelist *zonelist, nodemask_t *nodemask)
{
	// 1. 在 free_area 中查找合适的空闲块
	// 2. 从空闲链表移除页面
	// 3. 清除页面的 PG_buddy 标志（如果设置了）
	// 4. 更新 free_area->nr_free
	// 5. 返回页面指针
}
```

**释放页面时：**

```c
// mm/page_alloc.c
void __free_pages(struct page *page, unsigned int order)
{
	// 1. 检查页面是否可以合并（buddy 合并）
	// 2. 将页面添加到 free_area 的空闲链表
	// 3. 设置页面的 PG_buddy 标志
	// 4. 更新 free_area->nr_free
}
```

### 12.4 zone 结构：区域级别的统计

#### 12.4.1 zone 中的统计信息

```c
// include/linux/mmzone.h
struct zone {
	// ...
	
	/* 页面统计 */
	unsigned long		managed_pages;  // 管理的页面数
	unsigned long		spanned_pages;  // 覆盖的页面数
	unsigned long		present_pages;  // 实际存在的页面数
	
	/* 空闲页面统计 */
	struct free_area	free_area[MAX_ORDER];
	
	/* 各种页面计数 */
	atomic_long_t		vm_stat[NR_VM_ZONE_STAT_ITEMS];
	// NR_FREE_PAGES - 空闲页面数
	// NR_INACTIVE_ANON - 非活跃匿名页数
	// NR_ACTIVE_ANON - 活跃匿名页数
	// NR_INACTIVE_FILE - 非活跃文件页数
	// NR_ACTIVE_FILE - 活跃文件页数
	// ...
};
```

#### 12.4.2 页面类型统计

```c
// include/linux/mmzone.h
enum zone_stat_item {
	NR_FREE_PAGES,           // 空闲页面数
	NR_ZONE_LRU_BASE,        // LRU 基础
	NR_ZONE_INACTIVE_ANON = NR_ZONE_LRU_BASE,  // 非活跃匿名页
	NR_ZONE_ACTIVE_ANON,     // 活跃匿名页
	NR_ZONE_INACTIVE_FILE,   // 非活跃文件页
	NR_ZONE_ACTIVE_FILE,     // 活跃文件页
	NR_ZONE_UNEVICTABLE,     // 不可回收页
	NR_ZONE_WRITE_PENDING,   // 等待写回的页
	// ... 更多统计项
};
```

### 12.5 如何判断页面是否被使用

#### 12.5.1 综合判断方法

内核通过多个维度综合判断页面是否被使用：

```c
// 伪代码：判断页面是否被使用
bool is_page_in_use(struct page *page)
{
	// 1. 检查引用计数
	if (page_ref_count(page) > 0)
		return true;  // 有引用，正在使用
	
	// 2. 检查是否在 LRU 列表中
	if (PageLRU(page))
		return true;  // 在 LRU 列表中，已被使用
	
	// 3. 检查是否有映射
	if (page_mapcount(page) > 0)
		return true;  // 被映射，正在使用
	
	// 4. 检查是否在空闲链表（buddy 系统）
	// 如果页面在 free_area 的链表中，说明是空闲的
	if (PageBuddy(page)) {
		// 检查是否真的在空闲链表
		// 实际实现需要检查链表
		return false;  // 在空闲链表，未被使用
	}
	
	// 5. 检查其他标志
	if (PageSlab(page) || PageReserved(page))
		return true;  // 特殊用途，视为使用中
	
	return false;  // 未被使用
}
```

#### 12.5.2 实际使用场景

**内存回收时的判断：**

```c
// mm/vmscan.c
static unsigned int shrink_page_list(struct list_head *page_list,
				     struct scan_control *sc, ...)
{
	while (!list_empty(page_list)) {
		struct folio *folio = lru_to_folio(page_list);
		
		// 页面在 LRU 列表中，说明正在使用
		// 但可能可以回收
		
		// 检查引用计数
		references = folio_check_references(folio, sc);
		switch (references) {
		case PAGEREF_ACTIVATE:
			goto activate_locked;
		case PAGEREF_KEEP:
			goto keep_locked;
		case PAGEREF_RECLAIM:
		case PAGEREF_RECLAIM_CLEAN:
			; /* try to reclaim */
		}
		
		// 检查是否有映射
		if (folio_mapped(folio)) {
			try_to_unmap(folio, flags);
			// 如果无法取消映射，说明正在使用
		}
		// ...
	}
}
```

**页面释放时的检查：**

```c
// mm/page_alloc.c
static inline void free_the_page(struct page *page, unsigned int order)
{
	if (order == 0)
		free_unref_page(page);  // 单页释放
	else
		__free_pages_ok(page, order);  // 多页释放
}

static void free_unref_page(struct page *page)
{
	// 检查引用计数应该为 0
	// 检查页面不在 LRU 列表中
	// 将页面添加到 buddy 分配器的空闲链表
}
```

### 12.6 页面状态的可视化

#### 12.6.1 /proc/meminfo

```bash
$ cat /proc/meminfo
MemTotal:        16384000 kB
MemFree:          8192000 kB    # 空闲内存
MemAvailable:     9216000 kB    # 可用内存
Buffers:           512000 kB    # 缓冲区
Cached:          4096000 kB     # Page Cache
Active:          6144000 kB     # 活跃页面
Inactive:        2048000 kB     # 非活跃页面
Active(anon):    3072000 kB     # 活跃匿名页
Inactive(anon):   512000 kB     # 非活跃匿名页
Active(file):    3072000 kB     # 活跃文件页
Inactive(file):  1536000 kB     # 非活跃文件页
```

这些统计信息来自各个 zone 的 `vm_stat`。

#### 12.6.2 /proc/pagetypeinfo

显示每个 zone 的页面类型统计：

```bash
$ cat /proc/pagetypeinfo
Page block order: 9
Pages per block:  512

Free pages count per migrate type at order       0      1      2      3      4      5      6      7      8      9     10
Node    0, zone   Normal, type    Unmovable      0      0      0      0      0      0      0      0      0      0      0
Node    0, zone   Normal, type  Movable      12345   6789   3456   1234    567    234     89     34     12      5      2
Node    0, zone   Normal, type  Reclaimable      0      0      0      0      0      0      0      0      0      0      0
```

### 12.7 页面状态的关键点总结

#### 12.7.1 页面使用状态的层次

1. **Buddy 分配器层**：
   - 页面在 `free_area` 的链表中 → 空闲
   - 页面不在链表中 → 已分配

2. **struct page 层**：
   - `_refcount = 0` → 没有引用，可能空闲
   - `_refcount > 0` → 正在使用
   - `PageLRU(page)` → 在 LRU 列表中，已被使用

3. **映射层**：
   - `_mapcount = -1` → 没有被映射
   - `_mapcount >= 0` → 被映射到页表

4. **统计层**：
   - zone 的 `vm_stat` 记录各种类型页面的数量

#### 12.7.2 判断页面是否被使用的步骤

```c
// 完整的判断流程
bool is_page_used(struct page *page)
{
	// 1. 最直接：检查引用计数
	if (page_ref_count(page) > 0)
		return true;
	
	// 2. 检查是否在 LRU 列表（文件页/匿名页）
	if (PageLRU(page))
		return true;
	
	// 3. 检查是否有页表映射
	if (page_mapcount(page) > 0)
		return true;
	
	// 4. 检查特殊标志
	if (PageSlab(page) || PageReserved(page))
		return true;
	
	// 5. 如果页面在 buddy 的空闲链表，说明空闲
	if (PageBuddy(page))
		return false;
	
	// 默认认为在使用（安全策略）
	return true;
}
```

### 12.8 总结

内核通过多层机制记录页面的使用状态：

1. **PFN（Page Frame Number）**：
   - 页面的唯一标识符
   - 不直接记录使用状态，但用于定位 `struct page`

2. **struct page**：
   - `flags`：记录页面状态标志（dirty, uptodate, locked 等）
   - `_refcount`：引用计数，表示有多少地方在使用
   - `_mapcount`：映射计数，表示有多少页表项映射
   - `lru`：LRU 列表节点，表示页面在回收系统中

3. **Buddy 分配器**：
   - `free_area`：按 order 组织的空闲页面链表
   - 页面在链表中 → 空闲
   - 页面不在链表中 → 已分配

4. **zone 统计**：
   - `vm_stat`：记录各种类型页面的数量
   - `managed_pages`、`spanned_pages` 等：区域级别的统计

5. **综合判断**：
   - 内核综合 `_refcount`、`PageLRU`、`_mapcount`、`PageBuddy` 等多个因素判断页面状态
   - 没有单一的"使用/未使用"标志，而是通过多个字段综合判断

这种多层次的记录机制确保了内核能够准确跟踪每个页面的状态，支持高效的内存分配、回收和管理。

---

## 十三、内核如何管理所有的 page：完整架构解析

内核需要管理系统中所有的物理页面，这是一个庞大的系统。本节详细解析内核如何组织、索引和管理所有页面的完整机制。

### 13.1 全局 mem_map 数组：所有 page 的存储

#### 13.1.1 mem_map 的概念

内核为系统中的每个物理页面创建一个 `struct page` 结构，所有这些结构存储在全局数组 `mem_map` 中：

```c
// mm/memory.c
struct page *mem_map;  // 全局 page 数组

// mem_map 的初始化（简化）
void __init mem_init(void)
{
	unsigned long max_mapnr;
	
	// 计算需要的 page 数量
	max_mapnr = max_pfn - ARCH_PFN_OFFSET;
	
	// 分配 mem_map 数组
	mem_map = alloc_large_system_hash("mem_map",
					   sizeof(struct page),
					   max_mapnr,
					   0,
					   HASH_EARLY | HASH_ZERO,
					   NULL,
					   NULL,
					   0,
					   0);
}
```

**关键概念：**
- `mem_map` 是一个 `struct page` 数组
- 数组大小 = 系统中的物理页面总数
- 每个物理页面都有一个唯一的 `struct page`

#### 13.1.2 struct page 数组的存储位置：vmemmap

**重要说明：`struct page` 数组实际上存储在 vmemmap 区域，而不是 fixmap 区域。**

**1. vmemmap 机制（现代内核）：**

现代内核使用 **vmemmap**（Virtual Memory Map）机制来存储所有 `struct page`：

```c
// arch/x86/include/asm/pgtable_64.h

/*
 * vmemmap 的虚拟地址范围
 * 这是一个固定的虚拟地址区域，用于映射所有 struct page
 */
#define __VMEMMAP_BASE_L4	0xffffea0000000000UL
#define VMEMMAP_START		__VMEMMAP_BASE_L4
#define VMEMMAP_END		(VMEMMAP_START + VMEMMAP_SIZE)

// vmemmap 区域的大小
#define VMEMMAP_SIZE ((_PAGE_END(MAX_PHYSMEM_BITS) - PAGE_OFFSET) >> PAGE_SHIFT)
```

**vmemmap 的特点：**
- **固定的虚拟地址范围**：vmemmap 在内核虚拟地址空间中占据一个固定的区域
- **直接映射**：通过页表直接将物理内存中的 `struct page` 映射到这个虚拟地址
- **线性映射**：PFN 到虚拟地址的映射是线性的：`vmemmap + pfn * sizeof(struct page)`

**2. vmemmap 与 fixmap 的区别：**

| 特性 | vmemmap | fixmap |
|------|---------|--------|
| **用途** | 存储所有 `struct page` | 固定映射特殊用途页面 |
| **虚拟地址** | 固定的连续区域 | 固定的离散地址 |
| **映射内容** | `struct page` 数组 | 特殊设备、寄存器等 |
| **地址范围** | 连续的大块区域 | 少量固定地址 |
| **页表设置** | 线性映射 | 独立设置每个页表项 |

**3. vmemmap 的实际使用：**

```c
// include/asm-generic/memory_model.h

#ifdef CONFIG_SPARSEMEM_VMEMMAP
/*
 * vmemmap 模式下，struct page 数组存储在虚拟地址空间的固定区域
 */
#define __pfn_to_page(pfn)	(vmemmap + (pfn))
#define __page_to_pfn(page)	(unsigned long)((page) - vmemmap)

#else
/*
 * FLATMEM 模式（旧方式），mem_map 是一个普通数组
 */
#define __pfn_to_page(pfn)	(mem_map + ((pfn) - ARCH_PFN_OFFSET))
#define __page_to_pfn(page)	((unsigned long)((page) - mem_map) + \
				 ARCH_PFN_OFFSET)
#endif
```

**4. vmemmap 的初始化：**

```c
// mm/sparse-vmemmap.c
void __init sparse_vmemmap_init(void)
{
	unsigned long start = VMEMMAP_START;
	unsigned long end = VMEMMAP_END;
	
	/*
	 * 在虚拟地址空间中预留 vmemmap 区域
	 * 但不立即分配物理页面
	 * 采用延迟映射：访问时才分配物理页面并建立映射
	 */
	vmemmap_populate(start, end, node, NULL);
}
```

**5. 物理内存布局示例：**

以 x86_64 为例，内核虚拟地址空间布局：

```
0xffff800000000000 - 0xffff87ffffffffff  (128TB)  Direct mapping of all physical memory
0xffff880000000000 - 0xffffc7ffffffffff  (64TB)   vmalloc/ioremap space
0xffffc90000000000 - 0xffffe8ffffffffff  (32TB)   vmemmap (struct page array)
0xffffe90000000000 - 0xffffe9ffffffffff  (64GB)   Fixmap
0xffffea0000000000 - 0xffffea0000000000  (1TB)    vmemmap (旧的地址范围)
0xffffffff80000000 - 0xffffffffa0000000  (512MB)  Kernel code and data
```

**6. 为什么使用 vmemmap？**

**优势：**
- **线性映射**：PFN 到 `struct page` 的转换非常简单，不需要查找
- **节省内存**：采用稀疏映射，只为实际存在的物理内存分配 `struct page`
- **固定地址**：虚拟地址固定，简化页表管理
- **延迟分配**：物理页面延迟分配，节省启动时的内存

**7. 稀疏内存模型（SPARSEMEM_VMEMMAP）：**

现代内核默认使用稀疏内存模型：

```c
// include/linux/mmzone.h

#ifdef CONFIG_SPARSEMEM_VMEMMAP
/*
 * vmemmap 模式：struct page 存储在固定的虚拟地址区域
 * 支持非连续物理内存（NUMA、内存热插拔等）
 */
#define vmemmap ((struct page *)VMEMMAP_START)
#else
/*
 * FLATMEM 模式：传统的连续数组
 */
extern struct page *mem_map;
#endif
```

**8. 查看 vmemmap 信息：**

```bash
# 查看内核虚拟地址空间布局（需要内核支持）
$ cat /proc/iomem | grep -i vmemmap

# 通过 /sys/kernel/debug 查看（如果启用）
$ cat /sys/kernel/debug/kernel_page_tables

# 在代码中查看
$ dmesg | grep -i vmemmap
```

**总结：**
- `struct page` 数组存储在 **vmemmap 区域**，而不是 fixmap 区域
- vmemmap 是内核虚拟地址空间中固定的连续区域
- 现代内核使用稀疏内存模型（SPARSEMEM_VMEMMAP）
- PFN 到 `struct page` 的转换非常简单：`vmemmap + pfn`

#### 13.1.2.1 fixmap 区域的用途（补充说明）

虽然 `struct page` 数组不存储在 fixmap 区域，但了解 fixmap 有助于理解内核虚拟地址空间布局：

**fixmap 的定义：**

```c
// arch/x86/include/asm/fixmap.h

#define __end_of_permanent_fixed_addresses	\
	(__START_KERNEL_map + FIXADDR_SIZE)
	
/*
 * fixmap 是一个固定的虚拟地址区域，用于映射特殊用途的页面
 * 比如设备寄存器、ACPI 表、early IO remap 等
 */
#define FIXADDR_START		(FIXADDR_TOP - FIXADDR_SIZE)
#define FIXADDR_TOP		((unsigned long)__end_of_permanent_fixed_addresses)
```

**fixmap 的用途：**
- **早期 IO 重映射**：系统启动早期，用于映射设备寄存器
- **ACPI 表映射**：映射 ACPI 相关的数据结构
- **特殊设备**：映射需要在固定地址访问的设备
- **临时映射**：某些需要固定虚拟地址的临时映射

**fixmap 的特点：**
- **固定地址**：每个 fixmap 槽位有固定的虚拟地址
- **离散映射**：不是连续的区域，而是少量固定地址
- **特殊用途**：用于需要固定虚拟地址的特殊页面

**fixmap 与 vmemmap 的对比：**

| 特性 | vmemmap | fixmap |
|------|---------|--------|
| **大小** | 非常大的连续区域（TB 级别） | 较小的离散区域（几 MB） |
| **用途** | 存储所有 `struct page` | 映射特殊设备/寄存器 |
| **映射关系** | PFN 线性映射到虚拟地址 | 每个槽位独立映射 |
| **地址范围** | `0xffffc90000000000` 附近 | `0xffffe90000000000` 附近 |
| **数量** | 一个大的连续区域 | 多个固定地址槽位 |

**fixmap 使用示例：**

```c
// arch/x86/include/asm/fixmap.h

enum fixed_addresses {
	FIX_HOLE,
	FIX_DBGP_BASE,
	FIX_EARLYCON_MEM_BASE,
	FIX_ACPI_BEGIN,
	FIX_ACPI_END = FIX_ACPI_BEGIN + FIX_ACPI_PAGES - 1,
	// ... 更多固定地址
	__end_of_fixed_addresses
};

// 使用 fixmap 映射页面
void *fix_to_virt(const unsigned int idx)
{
	BUILD_BUG_ON(idx >= __end_of_fixed_addresses);
	return (void *)(__fix_to_virt(idx));
}
```

**关键区别记忆点：**
- **vmemmap**：用于存储 `struct page` 数组，大块连续区域
- **fixmap**：用于映射特殊设备，少量固定地址
- `struct page` 数组在 **vmemmap**，不在 fixmap

#### 13.1.3 PFN 到 page 的映射

通过 PFN（Page Frame Number）可以快速找到对应的 `struct page`：

```c
// include/linux/mm.h

// PFN 到 page 的转换
#define __pfn_to_page(pfn)	(mem_map + ((pfn) - ARCH_PFN_OFFSET))
#define __page_to_pfn(page)	((unsigned long)((page) - mem_map) + \
				 ARCH_PFN_OFFSET)

// 使用示例
struct page *pfn_to_page(unsigned long pfn)
{
	return __pfn_to_page(pfn);
}

unsigned long page_to_pfn(const struct page *page)
{
	return __page_to_pfn(page);
}
```

**工作原理：**
- `mem_map[pfn - ARCH_PFN_OFFSET]` 就是 PFN 为 `pfn` 的页面
- `ARCH_PFN_OFFSET` 是架构相关的偏移量（通常是 0 或内存起始地址）
- 这是 O(1) 的查找，非常高效

#### 13.1.3 物理地址到 page 的转换

```c
// 物理地址到 PFN
#define PHYS_PFN(phys)		((phys_addr_t)(phys) >> PAGE_SHIFT)

// 物理地址到 page
#define phys_to_page(phys)	pfn_to_page(PHYS_PFN(phys))

// page 到物理地址
#define page_to_phys(page)	PFN_PHYS(page_to_pfn(page))

// 使用示例
struct page *phys_addr_to_page(phys_addr_t addr)
{
	return phys_to_page(addr);
}
```

### 13.2 内存的组织结构：Node -> Zone -> Page

#### 13.2.1 三级组织结构

内核采用三级结构组织内存：

```
Node (NUMA 节点)
  └── Zone (内存区域: ZONE_DMA, ZONE_NORMAL, ZONE_HIGHMEM, ...)
       └── Page (物理页面)
```

**1. Node（节点）**：
- 在 NUMA 系统中，每个 CPU 有本地内存（节点）
- 即使是非 NUMA 系统，也有一个节点（node 0）

**2. Zone（区域）**：
- 每个节点被划分为多个 zone
- 常见的 zone：`ZONE_DMA`、`ZONE_NORMAL`、`ZONE_HIGHMEM`、`ZONE_MOVABLE`

**3. Page（页面）**：
- 每个 zone 包含多个物理页面
- 每个页面都有对应的 `struct page`

#### 13.2.2 struct pglist_data（节点结构）

```c
// include/linux/mmzone.h
typedef struct pglist_data {
	// ...
	struct zone node_zones[MAX_NR_ZONES];  // 该节点的所有 zone
	struct zonelist node_zonelists[MAX_ZONELISTS];  // zone 列表（用于分配）
	int nr_zones;  // zone 数量
	
	unsigned long node_start_pfn;  // 节点起始 PFN
	unsigned long node_spanned_pages;  // 节点覆盖的页面数
	unsigned long node_present_pages;  // 节点实际存在的页面数
	
	struct page *node_mem_map;  // 该节点的 mem_map（可能是全局 mem_map 的一部分）
	// ...
} pg_data_t;
```

#### 13.2.3 struct zone（区域结构）

```c
// include/linux/mmzone.h
struct zone {
	// ...
	
	/* 页面范围 */
	unsigned long zone_start_pfn;  // zone 起始 PFN
	unsigned long spanned_pages;   // zone 覆盖的页面数
	unsigned long present_pages;   // zone 实际存在的页面数
	unsigned long managed_pages;   // zone 管理的页面数
	
	/* Buddy 分配器：空闲页面管理 */
	struct free_area free_area[MAX_ORDER];  // 按 order 组织的空闲区域
	
	/* 页面统计 */
	atomic_long_t vm_stat[NR_VM_ZONE_STAT_ITEMS];
	
	// ...
};
```

**关键字段：**
- `zone_start_pfn`：这个 zone 的起始 PFN
- `free_area[MAX_ORDER]`：Buddy 分配器的空闲链表
- `vm_stat`：页面类型统计（文件页、匿名页、空闲页等）

### 13.3 Buddy 分配器：如何管理空闲页面

#### 13.3.1 free_area 结构详解

Buddy 分配器是内核管理空闲页面的核心机制：

```c
// include/linux/mmzone.h

#define MAX_ORDER 11  // 最大 order = 11，即 2^11 = 2048 个页面（8MB）

enum migratetype {
	MIGRATE_UNMOVABLE,   // 不可移动
	MIGRATE_MOVABLE,     // 可移动
	MIGRATE_RECLAIMABLE, // 可回收
	MIGRATE_PCPTYPES,    // per-cpu 页面类型
	MIGRATE_HIGHATOMIC,  // 高优先级原子分配
	MIGRATE_CMA,         // CMA（连续内存分配器）
	MIGRATE_ISOLATE,     // 隔离页面
	MIGRATE_TYPES
};

struct free_area {
	struct list_head	free_list[MIGRATE_TYPES];  // 空闲页面链表数组
	unsigned long		nr_free;  // 该 order 下的空闲页面总数
};

struct zone {
	// ...
	struct free_area	free_area[MAX_ORDER];  // [0..MAX_ORDER-1]
	// ...
};
```

**数据结构说明：**
- `free_area[order]`：存储 order 为 `order` 的空闲页面块
  - `order = 0`：单个页面（4KB）
  - `order = 1`：2 个连续页面（8KB）
  - `order = 2`：4 个连续页面（16KB）
  - ...
  - `order = 11`：2048 个连续页面（8MB）

- `free_list[migrate_type]`：按迁移类型组织的链表
  - 不同迁移类型的页面不能合并（防止碎片）

- `nr_free`：该 order 下的空闲页面总数

#### 13.3.2 空闲页面的存储方式

**空闲页面如何存储在链表中：**

```c
// 空闲页面的第一个 page 的 private 字段存储 order
// 空闲页面的第一个 page 的 lru 字段作为链表节点

// 设置页面为 buddy 页面
static inline void set_page_order(struct page *page, int order)
{
	page->private = order;
	__SetPageBuddy(page);
}

// 从 buddy 链表移除页面
static inline void rmv_page_order(struct page *page)
{
	__ClearPageBuddy(page);
	page->private = 0;
}
```

**关键机制：**
- 空闲页面的第一个 `struct page` 的 `private` 字段存储 order
- 空闲页面的第一个 `struct page` 的 `lru` 字段作为链表节点
- 通过 `PageBuddy` 标志标识这是一个 buddy 页面

#### 13.3.3 页面分配：从空闲链表移除

```c
// mm/page_alloc.c
static inline struct page *
__rmqueue(struct zone *zone, unsigned int order, int migratetype)
{
	struct free_area *area;
	struct page *page;

	// 从指定 order 开始，向上查找
	for (current_order = order; current_order < MAX_ORDER; ++current_order) {
		area = &(zone->free_area[current_order]);
		page = list_first_entry_or_null(&area->free_list[migratetype],
						struct page, lru);
		if (!page)
			continue;
		
		// 从链表移除
		list_del(&page->lru);
		rmv_page_order(page);
		area->nr_free--;
		
		// 如果需要分割（current_order > order）
		expand(zone, page, order, current_order, area, migratetype);
		
		return page;
	}
	return NULL;
}
```

**分配流程：**
1. 从请求的 `order` 开始，向上查找空闲块
2. 找到后从 `free_list` 移除
3. 如果找到的块比请求的大，进行分割（`expand`）
4. 返回分配的页面块

#### 13.3.4 页面释放：添加到空闲链表

```c
// mm/page_alloc.c
static inline void __free_one_page(struct page *page,
				    unsigned long pfn,
				    struct zone *zone, unsigned int order,
				    int migratetype)
{
	unsigned long combined_pfn;
	unsigned long uninitialized_var(buddy_pfn);
	struct page *buddy;

	// 尝试与 buddy 合并
	while (order < MAX_ORDER - 1) {
		buddy_pfn = __find_buddy_pfn(pfn, order);
		buddy = page + (buddy_pfn - pfn);
		
		if (!pfn_valid_within(buddy_pfn))
			goto done_merging;
		if (!page_is_buddy(page, buddy, order))
			goto done_merging;
		
		// 找到 buddy，可以合并
		list_del(&buddy->lru);
		rmv_page_order(buddy);
		combined_pfn = buddy_pfn & pfn;
		page = page + (combined_pfn - pfn);
		pfn = combined_pfn;
		order++;
	}

done_merging:
	// 添加到空闲链表
	set_page_order(page, order);
	list_add(&page->lru, &zone->free_area[order].free_list[migratetype]);
	zone->free_area[order].nr_free++;
}
```

**释放流程：**
1. 尝试与 buddy 页面合并（如果 buddy 也在空闲且 order 相同）
2. 合并后的页面添加到更高 order 的链表
3. 重复合并直到无法继续
4. 最终添加到对应 order 的 `free_list`

### 13.4 如何判断页面是空闲还是被使用

#### 13.4.1 判断流程

内核通过以下方式判断页面的状态：

```c
// 综合判断页面是否空闲
bool is_page_free(struct page *page)
{
	struct zone *zone = page_zone(page);
	
	// 方法 1：检查是否在 buddy 的空闲链表
	if (PageBuddy(page)) {
		unsigned int order = page_order(page);
		// 检查是否真的在 free_area[order] 的链表中
		// 实际实现需要遍历链表或使用其他机制验证
		struct free_area *area = &zone->free_area[order];
		// ... 验证逻辑
		return true;  // 在空闲链表，说明空闲
	}
	
	// 方法 2：检查引用计数
	if (page_ref_count(page) > 0)
		return false;  // 有引用，正在使用
	
	// 方法 3：检查是否在 LRU 列表
	if (PageLRU(page))
		return false;  // 在 LRU 列表，正在使用
	
	// 方法 4：检查是否有映射
	if (page_mapcount(page) > 0)
		return false;  // 被映射，正在使用
	
	// 方法 5：检查特殊标志
	if (PageSlab(page) || PageReserved(page))
		return false;  // 特殊用途，正在使用
	
	return true;  // 可能是空闲的
}
```

#### 13.4.2 页面状态的多种表示

**1. Buddy 系统中的状态：**
- 在 `free_area[order].free_list` 中 → 空闲
- 不在链表中 → 已分配

**2. struct page 中的状态：**
- `PageBuddy(page)` → 是 buddy 页面（空闲）
- `PageLRU(page)` → 在 LRU 列表（使用中）
- `page_ref_count(page) > 0` → 有引用（使用中）
- `page_mapcount(page) > 0` → 被映射（使用中）

**3. Zone 统计中的状态：**
- `zone->vm_stat[NR_FREE_PAGES]` → 空闲页面总数
- `zone->vm_stat[NR_FILE_PAGES]` → 文件页总数
- `zone->vm_stat[NR_ANON_PAGES]` → 匿名页总数

### 13.5 内存管理的完整流程

#### 13.5.1 系统启动时的初始化

```c
// 1. 初始化 mem_map
mem_init()
  └── 分配 mem_map 数组
      └── 为每个物理页面创建 struct page

// 2. 初始化 zone
build_all_zonelists()
  └── 为每个 zone 设置范围（zone_start_pfn, spanned_pages）
      └── 初始化 free_area 数组

// 3. 释放内存到 buddy 分配器
memblock_free_all()
  └── 将早期内存释放给 buddy 分配器
      └── 初始化 free_area 链表
```

#### 13.5.2 页面分配的完整路径

```c
// 用户空间：malloc() 或 mmap()
    ↓
// 内核空间：分配页面
alloc_pages(gfp_mask, order)
    ↓
// 选择 zone
gfp_zone(gfp_mask)  →  确定从哪个 zone 分配
    ↓
// 在 zone 中查找
__alloc_pages_nodemask()
    ↓
// Buddy 分配器查找空闲页面
__rmqueue()
    ↓
// 从 free_area[order].free_list 移除页面
    ↓
// 返回页面指针
```

#### 13.5.3 页面释放的完整路径

```c
// 释放页面
free_pages(page, order)
    ↓
// 找到 zone
page_zone(page)
    ↓
// Buddy 分配器释放
__free_pages_ok()
    ↓
// 尝试与 buddy 合并
__free_one_page()
    ↓
// 添加到 free_area[order].free_list
```

### 13.6 通过 zone 查看内存状态

#### 13.6.1 遍历所有页面

```c
// 伪代码：遍历 zone 中的所有页面
void traverse_zone_pages(struct zone *zone)
{
	unsigned long pfn;
	unsigned long zone_start = zone->zone_start_pfn;
	unsigned long zone_end = zone_start + zone->spanned_pages;
	
	for (pfn = zone_start; pfn < zone_end; pfn++) {
		struct page *page = pfn_to_page(pfn);
		
		if (!pfn_valid(pfn))
			continue;  // 页面不存在
		
		if (PageBuddy(page)) {
			// 空闲页面
			unsigned int order = page_order(page);
			pr_info("PFN %lu: FREE (order=%u)\n", pfn, order);
		} else if (PageLRU(page)) {
			// 在 LRU 列表（文件页或匿名页）
			pr_info("PFN %lu: IN USE (LRU)\n", pfn);
		} else if (page_ref_count(page) > 0) {
			// 有引用
			pr_info("PFN %lu: IN USE (refcount=%d)\n", 
				pfn, page_ref_count(page));
		} else {
			// 其他状态
			pr_info("PFN %lu: UNKNOWN STATE\n", pfn);
		}
	}
}
```

#### 13.6.2 统计空闲页面

```c
// 统计 zone 中的空闲页面
unsigned long count_free_pages(struct zone *zone)
{
	unsigned long free_count = 0;
	int order;
	
	// 方法 1：从 free_area 统计
	for (order = 0; order < MAX_ORDER; order++) {
		struct free_area *area = &zone->free_area[order];
		free_count += area->nr_free * (1UL << order);
	}
	
	// 方法 2：从 vm_stat 读取
	// free_count = zone->vm_stat[NR_FREE_PAGES];
	
	return free_count;
}
```

### 13.7 实际查看系统内存状态

#### 13.7.1 /proc/meminfo

```bash
$ cat /proc/meminfo
MemTotal:        16384000 kB    # 总内存
MemFree:          8192000 kB    # 空闲内存（在 buddy 系统中）
MemAvailable:     9216000 kB    # 可用内存（包括可回收的）
Buffers:           512000 kB    # 缓冲区
Cached:          4096000 kB     # Page Cache
Active:          6144000 kB     # 活跃页面
Inactive:        2048000 kB     # 非活跃页面
```

这些数据来自各个 zone 的统计。

#### 13.7.2 /proc/pagetypeinfo

显示每个 zone 的页面类型和空闲页面：

```bash
$ cat /proc/pagetypeinfo
Page block order: 9
Pages per block:  512

Free pages count per migrate type at order       0      1      2      3      4      5      6      7      8      9     10
Node    0, zone   Normal, type  Movable   12345   6789   3456   1234    567    234     89     34     12      5      2
```

显示每个 order 下各迁移类型的空闲页面数量。

#### 13.7.3 /proc/buddyinfo

显示 Buddy 分配器的状态：

```bash
$ cat /proc/buddyinfo
Node 0, zone   Normal     12345   6789   3456   1234    567    234     89     34     12      5      2
Node 0, zone   Movable    12345   6789   3456   1234    567    234     89     34     12      5      2
```

显示每个 zone 在不同 order 下的空闲页面数量。

### 13.8 内存管理的关键机制总结

#### 13.8.1 页面存储和组织

1. **全局 mem_map 数组**：
   - 系统中所有 `struct page` 的存储
   - 通过 PFN 索引：`mem_map[pfn - ARCH_PFN_OFFSET]`

2. **三级组织结构**：
   - Node（节点）→ Zone（区域）→ Page（页面）
   - 每个页面属于一个 zone

#### 13.8.2 空闲页面管理

1. **Buddy 分配器**：
   - `free_area[MAX_ORDER]`：按 order 组织空闲块
   - `free_list[MIGRATE_TYPES]`：按迁移类型组织链表
   - 空闲页面的第一个 page 的 `private` 存储 order
   - 空闲页面的第一个 page 的 `lru` 作为链表节点

2. **判断页面是否空闲**：
   - 在 `free_area[order].free_list` 中 → 空闲
   - `PageBuddy(page)` → 是 buddy 页面（空闲）
   - `page_ref_count(page) = 0` 且不在 LRU → 可能空闲

#### 13.8.3 使用中页面的管理

1. **引用计数**：
   - `page_ref_count(page) > 0` → 正在使用

2. **LRU 列表**：
   - `PageLRU(page)` → 在 LRU 列表（文件页或匿名页）

3. **页表映射**：
   - `page_mapcount(page) > 0` → 被映射到页表

4. **特殊用途**：
   - `PageSlab(page)` → slab 页面
   - `PageReserved(page)` → 保留页面

#### 13.8.4 统计和监控

1. **Zone 统计**：
   - `zone->vm_stat[NR_FREE_PAGES]`：空闲页面数
   - `zone->vm_stat[NR_FILE_PAGES]`：文件页数
   - `zone->vm_stat[NR_ANON_PAGES]`：匿名页数

2. **系统接口**：
   - `/proc/meminfo`：总体内存统计
   - `/proc/buddyinfo`：Buddy 分配器状态
   - `/proc/pagetypeinfo`：页面类型统计

### 13.9 总结

内核通过以下机制管理所有页面：

1. **全局存储**：
   - `mem_map` 数组存储所有 `struct page`
   - 通过 PFN 实现 O(1) 查找

2. **组织结构**：
   - Node → Zone → Page 三级结构
   - 每个页面属于一个 zone

3. **空闲页面管理**：
   - Buddy 分配器通过 `free_area[order].free_list` 管理空闲页面
   - 空闲页面的 `lru` 字段作为链表节点
   - 空闲页面的 `private` 字段存储 order

4. **使用状态判断**：
   - 在空闲链表 → 空闲
   - 有引用计数 → 使用中
   - 在 LRU 列表 → 使用中
   - 被映射 → 使用中

5. **统计和监控**：
   - Zone 级别的统计（`vm_stat`）
   - 系统接口（`/proc/meminfo`、`/proc/buddyinfo` 等）

这种设计确保了内核能够高效地管理所有物理页面，支持快速分配、释放和状态查询。

---

## 十四、内存回收机制详解：水位线、页面选择和回收过程

当系统内存不足时，内核需要回收页面以释放内存。本节详细解析内核的内存回收机制，包括三个水位线的设置、页面选择算法和完整的回收过程。

### 14.1 三个水位线的具体数值

#### 14.1.1 水位线的定义

内核为每个 zone 定义了三个水位线：

```c
// include/linux/mmzone.h
enum zone_watermarks {
	WMARK_MIN,    // 最低水位线（紧急情况）
	WMARK_LOW,    // 低水位线（触发 kswapd）
	WMARK_HIGH,   // 高水位线（kswapd 停止）
	NR_WMARK
};

struct zone {
	// ...
	unsigned long _watermark[NR_WMARK];  // 三个水位线的值
	unsigned long watermark_boost;       // 动态提升值
	// ...
};
```

#### 14.1.2 水位线的计算

水位线通常基于 zone 的 `managed_pages`（管理的页面数）计算：

```c
// mm/page_alloc.c

/*
 * 计算水位线的函数
 * managed_pages: zone 管理的总页面数
 * min_free_kbytes: 系统级别的保留内存（通过 /proc/sys/vm/min_free_kbytes 设置）
 */
static void __setup_per_zone_wmarks(void)
{
	unsigned long pages_min = min_free_kbytes >> (PAGE_SHIFT - 10);
	unsigned long lowmem_pages = 0;
	struct zone *zone;
	unsigned long flags;

	// 计算所有 lowmem zones 的总页面数
	for_each_zone(zone) {
		if (!is_highmem(zone))
			lowmem_pages += zone->managed_pages;
	}

	// 为每个 zone 设置水位线
	for_each_zone(zone) {
		unsigned long min, low, high;
		u64 tmp;

		// 如果是 highmem zone，使用较小的比例
		if (is_highmem(zone)) {
			tmp = (u64)pages_min * zone->managed_pages;
			do_div(tmp, lowmem_pages ? lowmem_pages : 1);
			min = tmp;
		} else {
			min = (u64)pages_min * zone->managed_pages;
			do_div(tmp, lowmem_pages);
		}

		// LOW 水位线 = MIN 水位线的 2 倍
		low = min * 2;
		
		// HIGH 水位线 = MIN 水位线的 3 倍
		high = min * 3;
		
		// 设置水位线
		zone->_watermark[WMARK_MIN] = min;
		zone->_watermark[WMARK_LOW] = low;
		zone->_watermark[WMARK_HIGH] = high;
	}
}
```

**典型数值示例：**

假设系统有 16GB 内存（4,194,304 个 4KB 页面），`min_free_kbytes` 默认值通常为：

- 对于 16GB 系统：`min_free_kbytes ≈ 65536 KB`（约 64MB）

对于 ZONE_NORMAL（假设管理 4GB = 1,048,576 个页面）：

```
WMARK_MIN = min_free_kbytes * zone_pages / lowmem_pages
          ≈ 65536 * 1048576 / 4194304
          ≈ 16,384 个页面 (64MB)

WMARK_LOW = WMARK_MIN * 2
          = 32,768 个页面 (128MB)

WMARK_HIGH = WMARK_MIN * 3
           = 49,152 个页面 (192MB)
```

**实际比例：**
- **WMARK_MIN**：约占总内存的 **0.39%**（min_free_kbytes / total_memory）
- **WMARK_LOW**：约占总内存的 **0.78%**（2 × WMARK_MIN）
- **WMARK_HIGH**：约占总内存的 **1.17%**（3 × WMARK_MIN）

#### 14.1.3 查看实际水位线

```bash
# 查看水位线值（需要内核支持）
$ cat /proc/zoneinfo

Node 0, zone   Normal
  pages free     123456
        min      16384        # WMARK_MIN
        low      32768        # WMARK_LOW
        high     49152        # WMARK_HIGH
        spanned  1048576
        present  1048576
        managed  1024000
```

#### 14.1.4 水位线的动态调整

```c
// mm/page_alloc.c

/*
 * watermark_boost: 动态提升水位线
 * 当有突发的大量分配时，临时提升水位线以增加回收压力
 */
static inline unsigned long wmark_pages(struct zone *z, enum zone_watermarks w)
{
	return z->_watermark[w] + z->watermark_boost;
}
```

**boost 机制：**
- 在短时间内有大量分配时，`watermark_boost` 会增加
- 提升后的水位线 = 基础水位线 + boost
- 分配成功后，boost 会逐渐降低

### 14.2 页面选择机制：如何选择要回收的页面

#### 14.2.1 LRU 列表结构

内核使用 LRU（Least Recently Used）列表来组织可回收的页面：

```c
// include/linux/mmzone.h
enum lru_list {
	LRU_INACTIVE_ANON = LRU_BASE,        // 非活跃匿名页
	LRU_ACTIVE_ANON = LRU_BASE + LRU_ACTIVE,   // 活跃匿名页
	LRU_INACTIVE_FILE = LRU_BASE + LRU_FILE,   // 非活跃文件页
	LRU_ACTIVE_FILE = LRU_BASE + LRU_FILE + LRU_ACTIVE,  // 活跃文件页
	LRU_UNEVICTABLE,                      // 不可回收页
	NR_LRU_LISTS
};

struct lruvec {
	struct list_head		lists[NR_LRU_LISTS];  // 四个 LRU 列表
	struct zone_reclaim_stat	reclaim_stat;  // 回收统计
	// ...
};
```

**LRU 列表优先级（从高到低，优先回收）：**
1. **LRU_INACTIVE_ANON** - 非活跃匿名页（最优先回收）
2. **LRU_INACTIVE_FILE** - 非活跃文件页
3. **LRU_ACTIVE_ANON** - 活跃匿名页
4. **LRU_ACTIVE_FILE** - 活跃文件页（最后回收）

#### 14.2.1.1 LRU 列表的存储位置和组织方式

**1. lru_list 枚举的作用：**

`enum lru_list` 只是一个**枚举类型**，用于标识 LRU 列表的类型（索引），而不是实际的列表本身。它定义了 5 种类型的 LRU 列表。

**2. 实际的 LRU 列表存储在哪里：**

LRU 列表实际存储在 `struct lruvec` 的 `lists` 数组中：

```c
// include/linux/mmzone.h

struct lruvec {
	struct list_head	lists[NR_LRU_LISTS];  // 实际的 LRU 列表数组
	/* ... */
};

// 每个 zone 都有一个 lruvec
struct zone {
	/* ... */
	struct lruvec		__lruvec;  // zone 的 LRU 向量
	/* ... */
};
```

**3. lruvec 与 zone 的关系：**

每个 **zone** 都有一个 `lruvec`（LRU vector），它包含了该 zone 的所有 LRU 列表：

```c
// include/linux/mmzone.h

struct zone {
	// ...
	struct lruvec __lruvec;  // 该 zone 的 LRU 向量
	// ...
};

// 访问 zone 的 lruvec
#define zone_lruvec(zone)	(&(zone)->__lruvec)

// 访问特定 LRU 列表
#define lruvec_page_lruvec(lruvec, page) (lruvec)
```

**4. 页面如何链接到 LRU 列表：**

每个 `struct folio`（或 `struct page`）都有一个 `lru` 字段，用于链接到 LRU 列表：

```c
// include/linux/mm_types.h

struct folio {
	union {
		struct {
			unsigned long flags;
			struct list_head lru;  // LRU 列表节点
			struct address_space *mapping;
			// ...
		};
	};
};
```

**5. 页面添加到 LRU 列表的过程和判断逻辑：**

内核通过以下三个关键判断来确定页面应该添加到哪个 LRU 列表：

#### 判断流程：

```c
// mm/swap.c

/*
 * folio_add_lru: 将页面添加到 LRU 列表
 */
void folio_add_lru(struct folio *folio)
{
	struct lruvec *lruvec;
	int lru;

	// 1. 确定页面应该加入哪个 LRU 列表
	if (folio_test_unevictable(folio))
		lru = LRU_UNEVICTABLE;
	else if (folio_test_active(folio))
		lru = folio_is_file_lru(folio) ? LRU_ACTIVE_FILE : LRU_ACTIVE_ANON;
	else
		lru = folio_is_file_lru(folio) ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;

	// 2. 获取页面所属的 lruvec
	lruvec = folio_lruvec(folio);  // 通过 zone 获取 lruvec

	// 3. 添加到对应的 LRU 列表
	list_add(&folio->lru, &lruvec->lists[lru]);

	// 4. 设置 LRU 标志
	folio_set_lru(folio);
}
```

#### 判断逻辑详解：

**判断 1：是否不可回收（unevictable）**

```c
// include/linux/page-flags.h

/*
 * folio_test_unevictable: 检查页面是否不可回收
 */
static inline int folio_test_unevictable(const struct folio *folio)
{
	return test_bit(PG_unevictable, folio_flags(folio, 0));
}
```

**不可回收页面的情况：**
- 页面被 `mlock()` 锁定（进程主动锁定，避免 swap）
- 页面标记为 `PG_unevictable`
- 这些页面应该添加到 `LRU_UNEVICTABLE` 列表

**判断 2：是否活跃（active）**

```c
// include/linux/page-flags.h

/*
 * folio_test_active: 检查页面是否是活跃的
 */
static inline int folio_test_active(const struct folio *folio)
{
	return test_bit(PG_active, folio_flags(folio, 0));
}
```

**活跃页面的含义：**
- 页面最近被访问过（通过第二次机会算法标记）
- 活跃页面应该在活跃 LRU 列表（`LRU_ACTIVE_ANON` 或 `LRU_ACTIVE_FILE`）
- 非活跃页面应该在非活跃 LRU 列表（`LRU_INACTIVE_ANON` 或 `LRU_INACTIVE_FILE`）

**活跃/非活跃的转换：**

```c
// mm/swap.c

/*
 * folio_mark_accessed: 标记页面被访问
 */
void folio_mark_accessed(struct folio *folio)
{
	if (!folio_test_referenced(folio)) {
		// 第一次访问：设置 referenced 标志
		folio_set_referenced(folio);
	} else if (!folio_test_active(folio)) {
		// 第二次访问且不在活跃列表：移到活跃列表
		if (folio_lru(folio)) {
			int lru = folio_is_file_lru(folio);
			folio_activate(folio);  // 移到活跃列表
			folio_set_referenced(folio);
			folio_clear_referenced(folio);
		}
	}
}
```

**判断 3：是文件页还是匿名页（file_lru vs anon_lru）**

```c
// mm/swap.c

/*
 * folio_is_file_lru: 判断是否是文件页
 */
static inline bool folio_is_file_lru(struct folio *folio)
{
	return !folio_test_anon(folio);
}

/*
 * folio_test_anon: 判断是否是匿名页
 */
static inline bool folio_test_anon(const struct folio *folio)
{
	return ((unsigned long)folio->mapping & PAGE_MAPPING_ANON) != 0;
}
```

**判断依据：**
- **匿名页**：`folio->mapping` 的低位设置了 `PAGE_MAPPING_ANON` 标志
- **文件页**：`folio->mapping` 指向 `address_space`（低位为 0）

#### 完整的判断决策树：

```
folio_add_lru(folio)
    │
    ├─→ folio_test_unevictable(folio) ?
    │   ├─→ YES → LRU_UNEVICTABLE
    │   └─→ NO  ↓
    │
    ├─→ folio_test_active(folio) ?
    │   ├─→ YES ↓
    │   │       ├─→ folio_is_file_lru(folio) ?
    │   │       │   ├─→ YES → LRU_ACTIVE_FILE
    │   │       │   └─→ NO  → LRU_ACTIVE_ANON
    │   │
    │   └─→ NO  ↓
    │           ├─→ folio_is_file_lru(folio) ?
    │           │   ├─→ YES → LRU_INACTIVE_FILE
    │           │   └─→ NO  → LRU_INACTIVE_ANON
```

#### 判断逻辑的代码实现（简化版）：

```c
// 伪代码：完整的判断逻辑

int determine_lru_list(struct folio *folio)
{
	// 优先级 1：检查是否不可回收
	if (folio_test_unevictable(folio))
		return LRU_UNEVICTABLE;

	// 优先级 2：检查是否是文件页
	bool is_file = folio_is_file_lru(folio);
	// folio_is_file_lru() 内部调用：
	//   return !folio_test_anon(folio);
	// folio_test_anon() 检查：
	//   return (folio->mapping & PAGE_MAPPING_ANON) != 0;

	// 优先级 3：检查是否活跃
	bool is_active = folio_test_active(folio);
	// folio_test_active() 检查：
	//   return test_bit(PG_active, folio->flags);

	// 组合判断
	if (is_active) {
		return is_file ? LRU_ACTIVE_FILE : LRU_ACTIVE_ANON;
	} else {
		return is_file ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	}
}
```

#### 实际的判断示例：

**示例 1：文件页（Page Cache）**

```c
// 文件页的特征：
struct folio *file_folio = ...;
// file_folio->mapping = address_space* (低位为 0)

// 判断过程：
folio_test_unevictable(file_folio) → false
folio_test_anon(file_folio) → false  (mapping 不是 anon_vma)
folio_is_file_lru(file_folio) → true
folio_test_active(file_folio) → false  (假设是非活跃的)

// 结果：添加到 LRU_INACTIVE_FILE
```

**示例 2：匿名页（进程堆/栈）**

```c
// 匿名页的特征：
struct folio *anon_folio = ...;
// anon_folio->mapping = anon_vma + PAGE_MAPPING_ANON (低位为 1)

// 判断过程：
folio_test_unevictable(anon_folio) → false
folio_test_anon(anon_folio) → true  (mapping 低位设置了 PAGE_MAPPING_ANON)
folio_is_file_lru(anon_folio) → false
folio_test_active(anon_folio) → true  (假设是活跃的)

// 结果：添加到 LRU_ACTIVE_ANON
```

**示例 3：mlock 的页面**

```c
// mlock 页面的特征：
struct folio *locked_folio = ...;
// 设置了 PG_unevictable 标志

// 判断过程：
folio_test_unevictable(locked_folio) → true

// 结果：直接添加到 LRU_UNEVICTABLE（不再检查其他条件）
```

#### 页面状态变化时的 LRU 列表切换：

**1. 非活跃 → 活跃（页面被访问）**

```c
// mm/swap.c

/*
 * folio_activate: 将页面移到活跃列表
 */
static void folio_activate(struct folio *folio)
{
	if (!folio_test_active(folio) && !folio_test_unevictable(folio)) {
		struct lruvec *lruvec;
		int lru = folio_is_file_lru(folio);

		// 从非活跃列表移除
		folio_del_lru(folio);

		// 设置活跃标志
		folio_set_active(folio);

		// 添加到活跃列表
		lruvec = folio_lruvec(folio);
		list_add_tail(&folio->lru, &lruvec->lists[lru + LRU_ACTIVE]);
		folio_set_lru(folio);
	}
}
```

**2. 活跃 → 非活跃（页面老化）**

```c
// mm/vmscan.c

/*
 * folio_deactivate: 将页面移到非活跃列表
 */
void folio_deactivate(struct folio *folio)
{
	if (folio_test_active(folio) && !folio_test_unevictable(folio)) {
		struct lruvec *lruvec;
		int lru = folio_is_file_lru(folio);

		// 从活跃列表移除
		folio_del_lru(folio);

		// 清除活跃标志
		folio_clear_active(folio);

		// 添加到非活跃列表
		lruvec = folio_lruvec(folio);
		list_add(&folio->lru, &lruvec->lists[lru]);
		folio_set_lru(folio);
	}
}
```

#### 总结：判断的三要素

1. **是否不可回收**（`folio_test_unevictable`）
   - 如果是 → `LRU_UNEVICTABLE`
   - 如果否 → 继续判断

2. **是否活跃**（`folio_test_active`）
   - 活跃 → 添加到 `LRU_ACTIVE_*`
   - 非活跃 → 添加到 `LRU_INACTIVE_*`

3. **是文件页还是匿名页**（`folio_is_file_lru`）
   - 文件页 → `*_FILE`
   - 匿名页 → `*_ANON`

最终组合：
- `LRU_UNEVICTABLE`（不可回收）
- `LRU_ACTIVE_FILE`（活跃文件页）
- `LRU_ACTIVE_ANON`（活跃匿名页）
- `LRU_INACTIVE_FILE`（非活跃文件页）
- `LRU_INACTIVE_ANON`（非活跃匿名页）

/*
 * folio_lruvec: 获取页面所属的 lruvec
 */
static inline struct lruvec *folio_lruvec(struct folio *folio)
{
	struct pglist_data *pgdat = folio_pgdat(folio);
	return &pgdat->node_zones[folio_zonenum(folio)].__lruvec;
}
```

**6. 完整的组织结构：**

```
Node (pg_data_t)
  └── Zone[0..N] (struct zone)
       └── __lruvec (struct lruvec)
            └── lists[NR_LRU_LISTS] (struct list_head[])
                 ├── lists[LRU_INACTIVE_ANON]  ← 匿名页通过 lru 字段链接到这里
                 ├── lists[LRU_ACTIVE_ANON]
                 ├── lists[LRU_INACTIVE_FILE]  ← 文件页通过 lru 字段链接到这里
                 ├── lists[LRU_ACTIVE_FILE]
                 └── lists[LRU_UNEVICTABLE]
```

**7. 内存布局示例：**

```c
// 假设有一个 zone，其 lruvec 的内存布局：

struct zone zone_normal = {
	// ...
	.__lruvec = {
		.lists = {
			[LRU_INACTIVE_ANON] = {  // 非活跃匿名页链表
				.next = &page1->lru,
				.prev = &page100->lru
			},
			[LRU_ACTIVE_ANON] = {    // 活跃匿名页链表
				.next = &page101->lru,
				.prev = &page200->lru
			},
			[LRU_INACTIVE_FILE] = {  // 非活跃文件页链表
				.next = &page201->lru,
				.prev = &page300->lru
			},
			[LRU_ACTIVE_FILE] = {    // 活跃文件页链表
				.next = &page301->lru,
				.prev = &page400->lru
			},
			[LRU_UNEVICTABLE] = {    // 不可回收页链表
				.next = &list_head,
				.prev = &list_head
			}
		}
	}
};
```

**8. 遍历 LRU 列表：**

```c
// mm/vmscan.c

/*
 * 遍历特定 LRU 列表中的所有页面
 */
static void scan_lru_list(struct lruvec *lruvec, enum lru_list lru,
			  struct scan_control *sc)
{
	struct list_head *head = &lruvec->lists[lru];
	struct folio *folio;
	struct folio *next;

	// 遍历链表
	list_for_each_entry_safe(folio, next, head, lru) {
		// 处理页面
		if (should_reclaim(folio, sc)) {
			list_del(&folio->lru);
			// 回收页面
		}
	}
}
```

**9. 查看 LRU 列表大小：**

```c
// include/linux/mmzone.h

/*
 * lruvec_lru_size: 获取特定 LRU 列表的页面数量
 */
static inline unsigned long
lruvec_lru_size(struct lruvec *lruvec, enum lru_list lru, int zone_idx)
{
	return node_page_state(lruvec_pgdat(lruvec),
			       NR_LRU_BASE + lru);
}
```

**10. 从 LRU 列表移除页面：**

```c
// mm/swap.c

/*
 * folio_del_lru: 从 LRU 列表移除页面
 */
void folio_del_lru(struct folio *folio)
{
	struct lruvec *lruvec = folio_lruvec(folio);

	// 从链表移除
	list_del(&folio->lru);

	// 清除 LRU 标志
	folio_clear_lru(folio);

	// 更新统计
	__lruvec_stat_mod_folio(folio, NR_LRU_BASE + folio_lru(folio), -nr);
}
```

**总结：**

1. **`enum lru_list`**：只是枚举类型，用于标识 LRU 列表的类型（作为数组索引）
2. **实际的 LRU 列表**：存储在 `struct lruvec` 的 `lists[NR_LRU_LISTS]` 数组中
3. **lruvec 的位置**：每个 `struct zone` 都有一个 `__lruvec` 字段
4. **页面链接**：每个 `struct folio` 的 `lru` 字段作为链表节点，链接到对应的 LRU 列表
5. **组织结构**：Node → Zone → lruvec → lists[] → 通过 page->lru 链接的页面

#### 14.2.2 页面扫描策略

**关键问题：`shrink_lruvec` 没有指定 zone，它怎么知道扫描哪个 zone？**

答案：**`lruvec` 本身就关联到特定的 zone**。每个 `lruvec` 都唯一对应一个 zone，通过 `lruvec` 可以反向找到对应的 zone。

**1. lruvec 与 zone 的关联关系：**

```c
// include/linux/mmzone.h

/*
 * 每个 zone 都内嵌一个 lruvec
 */
struct zone {
	// ...
	struct lruvec __lruvec;  // zone 内嵌的 lruvec
	// ...
};

/*
 * 通过 zone 获取 lruvec
 */
#define zone_lruvec(zone)	(&(zone)->__lruvec)

/*
 * 通过 lruvec 反向获取 zone（使用 container_of）
 */
static inline struct zone *lruvec_zone(struct lruvec *lruvec)
{
	return container_of(lruvec, struct zone, __lruvec);
}
```

**2. shrink_lruvec 的调用链：**

```c
// mm/vmscan.c

/*
 * balance_pgdat: 节点级别的回收
 * 这个函数遍历所有 zone，为每个 zone 获取 lruvec 并调用 shrink_lruvec
 */
static unsigned long balance_pgdat(pg_data_t *pgdat, int order,
				    int highest_zoneidx)
{
	int i;
	struct lruvec *target_lruvec;

	// 从最高 zone 到最低 zone 扫描
	for (i = highest_zoneidx; i >= 0; i--) {
		struct zone *zone = pgdat->node_zones + i;

		if (!managed_zone(zone))
			continue;

		// 关键：从 zone 获取 lruvec
		target_lruvec = &zone->__lruvec;
		// 或者使用：target_lruvec = zone_lruvec(zone);

		// 传入的 lruvec 已经确定了对应的 zone
		nr_reclaimed += shrink_lruvec(target_lruvec, &sc);

		// 如果需要，可以从 lruvec 反向获取 zone
		// struct zone *zone = lruvec_zone(target_lruvec);
	}

	return nr_reclaimed;
}
```

**3. shrink_lruvec 的实现：**

```c
// mm/vmscan.c

/*
 * shrink_lruvec: 扫描 LRU 列表，选择页面回收
 * 
 * 注意：虽然函数签名没有 zone 参数，但 lruvec 本身就属于特定 zone
 */
static unsigned long shrink_lruvec(struct lruvec *lruvec,
				    struct scan_control *sc)
{
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_scan;
	struct lru_gen_mm_walk *walk = NULL;
	struct mm_struct *mm = NULL;

	// 如果需要访问 zone，可以从 lruvec 获取：
	// struct zone *zone = lruvec_zone(lruvec);

	// 1. 计算要扫描的页面数
	nr_to_scan = min(sc->nr_to_scan, 1024L);

	// 2. 按优先级扫描各个 LRU 列表
	// 直接使用 lruvec->lists[lru] 访问对应的 LRU 列表
	while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_FILE] ||
	       nr[LRU_INACTIVE_FILE]) {
		
		// 优先扫描非活跃匿名页
		// shrink_list 使用 lruvec->lists[LRU_INACTIVE_ANON]
		nr_to_scan = shrink_list(lruvec, LRU_INACTIVE_ANON, sc);
		if (nr_to_scan)
			nr_reclaimed += nr_to_scan;
		
		// 然后扫描非活跃文件页
		// shrink_list 使用 lruvec->lists[LRU_INACTIVE_FILE]
		nr_to_scan = shrink_list(lruvec, LRU_INACTIVE_FILE, sc);
		if (nr_to_scan)
			nr_reclaimed += nr_to_scan;
		
		// 如果还需要，扫描活跃列表
		if (sc->nr_to_reclaim <= nr_reclaimed)
			break;
	}

	return nr_reclaimed;
}
```

**4. shrink_list 如何使用 lruvec：**

```c
// mm/vmscan.c

/*
 * shrink_list: 扫描特定的 LRU 列表
 */
static unsigned long shrink_list(struct lruvec *lruvec,
				  enum lru_list lru,
				  struct scan_control *sc)
{
	struct list_head *head = &lruvec->lists[lru];  // 直接访问对应的 LRU 列表
	struct folio *folio;
	unsigned long nr_reclaimed = 0;

	// 遍历该 LRU 列表中的所有页面
	list_for_each_entry(folio, head, lru) {
		// 处理页面...
		// 页面的 zone 信息已经通过 lruvec 确定了
	}

	return nr_reclaimed;
}
```

**5. 为什么这样设计？**

**设计优势：**

1. **抽象层**：`lruvec` 作为抽象层，隐藏了 zone 的实现细节
2. **简洁接口**：`shrink_lruvec` 只需要关注 LRU 列表，不需要直接操作 zone
3. **1:1 关系**：每个 `lruvec` 唯一对应一个 zone，通过内存布局保证
4. **容器结构**：使用 `container_of` 可以反向获取 zone

**内存布局关系：**

```
struct zone {
	// ...
	struct lruvec __lruvec;  // lruvec 内嵌在 zone 中
	// ...
};

// lruvec 在 zone 中的偏移是固定的
// 可以通过 container_of 从 lruvec 指针反向获取 zone 指针
```

**6. 完整的调用流程：**

```
balance_pgdat(pgdat, order, highest_zoneidx)
    ↓
for each zone in pgdat->node_zones[]:
    ↓
    target_lruvec = &zone->__lruvec;  // 从 zone 获取 lruvec
    ↓
    shrink_lruvec(target_lruvec, sc)  // 传入 lruvec（隐含了 zone 信息）
        ↓
        shrink_list(lruvec, LRU_INACTIVE_ANON, sc)
            ↓
            访问 lruvec->lists[LRU_INACTIVE_ANON]  // 直接访问对应 zone 的 LRU 列表
                ↓
                扫描该列表中的页面（这些页面都属于该 zone）
```

**7. 如果需要从 lruvec 获取 zone：**

```c
// mm/vmscan.c

/*
 * 在某些情况下，shrink_lruvec 内部可能需要访问 zone 信息
 * 可以通过 container_of 从 lruvec 反向获取 zone
 */
static unsigned long shrink_lruvec(struct lruvec *lruvec,
				    struct scan_control *sc)
{
	// 如果需要访问 zone：
	struct zone *zone = lruvec_zone(lruvec);
	
	// 现在可以使用 zone 的信息：
	// - zone->watermark_boost
	// - zone->free_area
	// - zone->vm_stat
	// 等等
	
	// 但实际上，大多数情况下不需要直接访问 zone
	// 因为 lruvec 已经包含了需要的信息
}
```

**8. lruvec_zone 的实现：**

```c
// include/linux/mmzone.h

/*
 * 从 lruvec 反向获取 zone
 */
static inline struct zone *lruvec_zone(struct lruvec *lruvec)
{
	// 使用 container_of 从成员指针获取包含它的结构体指针
	return container_of(lruvec, struct zone, __lruvec);
}
```

**总结：**

1. **`lruvec` 本身就关联到特定的 zone**：每个 zone 内嵌一个 `lruvec`
2. **调用链保证 zone 信息**：`balance_pgdat` 从 zone 获取 lruvec 后传入 `shrink_lruvec`
3. **内存布局保证关系**：`lruvec` 内嵌在 `zone` 中，可以通过 `container_of` 反向获取
4. **抽象设计**：`shrink_lruvec` 不需要直接处理 zone，只需要处理 LRU 列表
5. **直接访问**：`shrink_lruvec` 直接访问 `lruvec->lists[lru]`，这些列表就属于对应的 zone

#### 14.2.3 扫描数量和比例

```c
// mm/vmscan.c

/*
 * 计算每个 LRU 列表要扫描的页面数
 */
static void get_scan_count(struct lruvec *lruvec, struct scan_control *sc,
			   unsigned long *nr)
{
	unsigned long anon_cost, file_cost, total_cost;
	int swappiness = sc->may_swap ? get_swappiness() : 0;

	// 计算总成本
	anon_cost = lruvec_lru_size(lruvec, LRU_ACTIVE_ANON, MAX_NR_ZONES) +
		    lruvec_lru_size(lruvec, LRU_INACTIVE_ANON, MAX_NR_ZONES);
	file_cost = lruvec_lru_size(lruvec, LRU_ACTIVE_FILE, MAX_NR_ZONES) +
		    lruvec_lru_size(lruvec, LRU_INACTIVE_FILE, MAX_NR_ZONES);

	total_cost = anon_cost + file_cost;

	// 根据 swappiness 计算扫描比例
	if (swappiness) {
		// 有 swappiness 时，优先扫描匿名页
		nr[LRU_INACTIVE_ANON] = get_nr_to_scan(lruvec, sc, swappiness);
		nr[LRU_INACTIVE_FILE] = get_nr_to_scan(lruvec, sc, 200 - swappiness);
	} else {
		// 没有 swappiness，只扫描文件页
		nr[LRU_INACTIVE_FILE] = get_nr_to_scan(lruvec, sc, 200);
		nr[LRU_INACTIVE_ANON] = 0;
	}
}
```

**swappiness 参数：**
- 默认值：`60`（可以通过 `/proc/sys/vm/swappiness` 调整）
- `swappiness = 0`：几乎不回收匿名页，只回收文件页
- `swappiness = 100`：匿名页和文件页同等对待
- `swappiness = 60`：稍微偏向回收匿名页

#### 14.2.4 页面访问检测：第二次机会算法

```c
// mm/swap.c

/*
 * folio_check_references: 检查页面是否被访问
 * 实现第二次机会算法
 */
enum page_references {
	PAGEREF_RECLAIM,      // 可以回收
	PAGEREF_RECLAIM_CLEAN, // 可以回收（干净页）
	PAGEREF_KEEP,         // 保留（最近访问）
	PAGEREF_ACTIVATE,     // 激活（移到活跃列表）
};

static enum page_references folio_check_references(struct folio *folio,
						    struct scan_control *sc)
{
	int referenced_ptes, referenced_page;
	unsigned long vm_flags;

	referenced_ptes = folio_referenced(folio, 1, sc->target_mem_cgroup,
					   &vm_flags);
	referenced_page = folio_test_referenced(folio);

	// 如果最近被访问过
	if (referenced_ptes) {
		// 清除 referenced 标志，给第二次机会
		folio_clear_referenced(folio);
		
		// 如果之前就在活跃列表，保留
		if (folio_test_active(folio))
			return PAGEREF_ACTIVATE;
		
		// 移到活跃列表
		folio_set_active(folio);
		return PAGEREF_ACTIVATE;
	}

	// 如果页面有引用但不是最近访问
	if (referenced_page && !folio_test_active(folio))
		return PAGEREF_RECLAIM_CLEAN;

	return PAGEREF_RECLAIM;
}
```

**第二次机会算法的逻辑：**
1. 页面第一次被扫描到：如果最近访问过，清除 `referenced` 标志，移到活跃列表
2. 页面第二次被扫描到：如果 `referenced` 标志仍然被清除（说明这段时间没被访问），可以回收
3. 如果 `referenced` 标志被设置（说明又被访问了），再次给机会

### 14.3 完整的回收过程

#### 14.3.1 回收流程概览

```
内存分配请求
    ↓
水位线检查（低于 WMARK_LOW）
    ↓
唤醒 kswapd 或直接回收
    ↓
balance_pgdat() / shrink_node()
    ↓
get_scan_count() - 计算扫描数量
    ↓
shrink_lruvec() - 扫描 LRU 列表
    ↓
shrink_list() - 扫描特定 LRU 列表
    ↓
shrink_page_list() - 回收页面列表
    ↓
try_to_unmap() - 取消页表映射
    ↓
pageout() - 写回脏页
    ↓
__remove_mapping() - 从 LRU 移除
    ↓
free_the_page() - 释放到 buddy 分配器
```

#### 14.3.2 balance_pgdat() - 节点级别的回收

```c
// mm/vmscan.c

static unsigned long balance_pgdat(pg_data_t *pgdat, int order,
				    int highest_zoneidx)
{
	int i;
	unsigned long nr_kept = 0;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.order = order,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = 1,
	};
	struct lruvec *target_lruvec;

	// 从最高 zone 到最低 zone 扫描
	for (i = highest_zoneidx; i >= 0; i--) {
		struct zone *zone = pgdat->node_zones + i;
		unsigned long zone_balance;

		if (!managed_zone(zone))
			continue;

		target_lruvec = &pgdat->node_zones[i].__lruvec;

		// 扫描 LRU 列表
		nr_reclaimed += shrink_lruvec(target_lruvec, &sc);

		// 检查是否达到 HIGH 水位线
		if (zone_watermark_ok(zone, order, high_wmark_pages(zone),
				      highest_zoneidx, 0))
			break;
	}

	return nr_reclaimed;
}
```

#### 14.3.3 shrink_page_list() - 页面回收核心函数

```c
// mm/vmscan.c

static unsigned int shrink_page_list(struct list_head *page_list,
				      struct pglist_data *pgdat,
				      struct scan_control *sc,
				      enum tt_classification reclaim_stat,
				      struct reclaim_stat *stat,
				      bool ignore_references)
{
	LIST_HEAD(ret_pages);
	LIST_HEAD(free_pages);
	unsigned int nr_reclaimed = 0;
	unsigned int pgactivate = 0;

	while (!list_empty(page_list)) {
		struct folio *folio;
		enum page_references references = PAGEREF_RECLAIM;
		bool dirty, writeback;
		unsigned int nr_pages;

		cond_resched();

		folio = lru_to_folio(page_list);
		list_del(&folio->lru);

		// 1. 锁定页面
		if (!folio_trylock(folio))
			goto keep;

		// 2. 检查是否可以回收
		if (unlikely(!folio_evictable(folio))) {
			folio_putback_lru(folio);
			folio_unlock(folio);
			continue;
		}

		// 3. 检查页面引用
		references = folio_check_references(folio, sc);
		switch (references) {
		case PAGEREF_ACTIVATE:
			goto activate_locked;
		case PAGEREF_KEEP:
			goto keep_locked;
		case PAGEREF_RECLAIM:
		case PAGEREF_RECLAIM_CLEAN:
			; /* try to reclaim */
		}

		// 4. 处理匿名页的 swap
		if (folio_test_anon(folio) && folio_test_swapbacked(folio)) {
			if (!folio_test_swapcache(folio)) {
				if (!(sc->gfp_mask & __GFP_IO))
					goto keep_locked;
				if (folio_try_swapcache_add(folio) == 0) {
					folio_set_swapcache(folio);
					folio_set_dirty(folio);
				} else {
					goto keep_locked;
				}
			}
		}

		// 5. 取消页表映射
		if (folio_mapped(folio)) {
			enum ttu_flags flags = TTU_BATCH_FLUSH | TTU_RMAP_LOCKED;
			bool was_swapbacked = folio_test_swapbacked(folio);

			try_to_unmap(folio, flags);
			if (folio_mapped(folio)) {
				stat->nr_unmap_fail += nr_pages;
				if (!was_swapbacked && folio_test_swapbacked(folio))
					stat->nr_lazyfree_fail += nr_pages;
				goto activate_locked;
			}
		}

		// 6. 处理脏页（需要先写回）
		if (folio_test_dirty(folio)) {
			if (folio_is_file_lru(folio) &&
			    (!current_is_kswapd() || sc->priority >= DEF_PRIORITY - 2)) {
				folio_clear_active(folio);
				folio_set_workingset(folio);
				folio_unlock(folio);
				folio_redirty_for_writepage(&wbc, folio);
				folio_putback_lru(folio);
				continue;
			}

			// 写回脏页
			stat->nr_writeback += nr_pages;
			if (!folio_clear_dirty_for_io(folio))
				BUG();
			error = folio_write_one(folio);
			if (error) {
				folio_putback_lru(folio);
				continue;
			}
		}

		// 7. 从 LRU 和 Page Cache 移除
		__remove_mapping(folio, true);

		// 8. 释放页面到 buddy 分配器
		folio_unlock(folio);
		list_add(&folio->lru, &free_pages);
		nr_reclaimed += nr_pages;
		continue;

activate_locked:
		// 激活页面（移到活跃列表）
		folio_unlock(folio);
		folio_activate(folio);
		pgactivate += nr_pages;

keep_locked:
		folio_unlock(folio);
keep:
		list_add(&folio->lru, &ret_pages);
	}

	// 释放页面
	free_unref_page_list(&free_pages);
	// 将保留的页面放回 LRU
	list_splice(&ret_pages, page_list);

	return nr_reclaimed;
}
```

#### 14.3.4 关键步骤详解

**步骤 1：取消页表映射（try_to_unmap）**

```c
// mm/rmap.c

/*
 * try_to_unmap: 尝试取消页面的所有页表映射
 */
bool try_to_unmap(struct folio *folio, enum ttu_flags flags)
{
	struct rmap_walk_control rwc = {
		.rmap_one = try_to_unmap_one,
		.arg = (void *)flags,
		.done = folio_not_mapped,
		.anon_lock = folio_lock_anon_vma_read,
	};

	// 遍历所有映射该页面的进程
	return rmap_walk(folio, &rwc) == SWAP_SUCCESS;
}
```

**步骤 2：写回脏页（folio_write_one）**

```c
// mm/filemap.c

/*
 * folio_write_one: 写回一个脏页
 */
static int folio_write_one(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;
	int ret = 0;

	if (mapping && mapping->a_ops->writepage) {
		ret = mapping->a_ops->writepage(&folio->page, NULL);
	}

	return ret;
}
```

**步骤 3：从映射移除（__remove_mapping）**

```c
// mm/filemap.c

/*
 * __remove_mapping: 从 address_space 和 LRU 移除页面
 */
static int __remove_mapping(struct address_space *mapping, struct folio *folio,
			    bool reclaimed)
{
	XA_STATE(xas, &mapping->i_pages, folio->index);

	// 从 xarray 移除
	xas_lock_irq(&xas);
	xas_store(&xas, NULL);
	xas_unlock_irq(&xas);

	// 从 LRU 移除
	__folio_clear_lru_flags(folio);
	list_del(&folio->lru);

	// 更新统计
	__lruvec_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	mapping->nrpages -= nr;

	return 1;
}
```

### 14.4 回收优先级和扫描深度

#### 14.4.1 回收优先级

```c
// mm/vmscan.c

#define DEF_PRIORITY 12  // 默认优先级

/*
 * 优先级范围：0-12
 * 0 = 最高优先级（最激进）
 * 12 = 最低优先级（最温和）
 */
struct scan_control {
	int priority;           // 回收优先级
	unsigned long nr_to_scan;  // 要扫描的页面数
	unsigned long nr_to_reclaim;  // 要回收的页面数
	// ...
};
```

**优先级的影响：**
- 高优先级（小数值）：扫描更多页面，更激进
- 低优先级（大数值）：扫描较少页面，较温和
- 如果回收无进展，优先级会降低（数值减小）

#### 14.4.2 扫描深度计算

```c
// mm/vmscan.c

/*
 * 根据优先级计算扫描深度
 */
static unsigned long calculate_lruvec_size(struct lruvec *lruvec,
					    enum lru_list lru,
					    int priority)
{
	unsigned long size = lruvec_lru_size(lruvec, lru, MAX_NR_ZONES);
	
	// 优先级越高（数值越小），扫描比例越大
	// priority=0: 扫描 100%
	// priority=12: 扫描约 1/16
	size >>= min(priority, 12);
	
	return size;
}
```

### 14.5 页面回收后再次访问的处理流程

这是一个非常重要的问题：**如果页面在回收时被取消了映射，进程再次访问时会发生什么？**

#### 14.5.1 回收时的映射取消

**1. try_to_unmap() - 取消页表映射**

```c
// mm/rmap.c

/*
 * try_to_unmap: 尝试取消页面的所有页表映射
 * 返回 SWAP_SUCCESS 表示成功取消所有映射
 */
bool try_to_unmap(struct folio *folio, enum ttu_flags flags)
{
	struct rmap_walk_control rwc = {
		.rmap_one = try_to_unmap_one,
		.arg = (void *)flags,
	};

	// 遍历所有映射该页面的进程，取消映射
	return rmap_walk(folio, &rwc) == SWAP_SUCCESS;
}

/*
 * try_to_unmap_one: 取消一个进程的页表映射
 */
static bool try_to_unmap_one(struct folio *folio,
			     struct vm_area_struct *vma,
			     unsigned long address, void *arg)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte;
	pte_t pteval;
	swp_entry_t entry;

	// 1. 获取页表项
	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	pteval = ptep_get(pte);

	// 2. 如果是匿名页，需要设置 swap entry
	if (folio_test_anon(folio)) {
		// 分配或获取 swap entry
		entry = folio_alloc_swap(folio);
		// 将 PTE 设置为 swap entry（而不是物理地址）
		set_pte_at(mm, address, pte, swp_entry_to_pte(entry));
	} else {
		// 文件页：直接将 PTE 设置为空（无映射）
		pteval = pteval_clear_accessed(pteval);
		pteval = pteval_clear_dirty(pteval);
		set_pte_at(mm, address, pte, pteval);
	}

	// 3. 刷新 TLB
	flush_tlb_page(vma, address);

	return true;
}
```

**关键操作：**
- **匿名页**：PTE 被设置为 swap entry（包含 swap 设备号和偏移）
- **文件页**：PTE 被清除（标记为不存在）
- **TLB 刷新**：确保 TLB 中的旧映射被清除

#### 14.5.2 进程再次访问时的页错误处理

**1. 触发页错误**

当进程再次访问已取消映射的页面时：

```c
// 用户空间代码访问内存
*(char *)addr  // 访问被回收的页面

    ↓
// CPU 硬件检测到页错误（PTE 无效或指向 swap）
    ↓
// 触发页错误异常（Page Fault）
    ↓
// 进入内核页错误处理程序
```

**2. do_page_fault() - 页错误入口**

```c
// arch/x86/mm/fault.c (x86_64 示例)

/*
 * do_page_fault: 页错误处理入口
 */
dotraplinkage void do_page_fault(struct pt_regs *regs, unsigned long error_code)
{
	unsigned long address;
	struct vm_area_struct *vma;
	struct mm_struct *mm;

	address = read_cr2();  // 获取访问的虚拟地址

	mm = current->mm;
	vma = find_vma(mm, address);

	// 调用通用页错误处理
	__do_page_fault(regs, error_code, address);
}
```

**3. handle_mm_fault() - 通用页错误处理**

```c
// mm/memory.c

/*
 * handle_mm_fault: 处理页错误的通用函数
 */
vm_fault_t handle_mm_fault(struct vm_area_struct *vma,
			   unsigned long address,
			   unsigned int flags,
			   struct pt_regs *regs)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	// 1. 获取页表项
	pgd = pgd_offset(mm, address);
	p4d = p4d_offset(pgd, address);
	pud = pud_offset(p4d, address);
	pmd = pmd_offset(pud, address);
	pte = pte_offset_map_lock(mm, pmd, address, &ptl);

	// 2. 检查 PTE 状态
	pteval = ptep_get(pte);

	if (pte_none(pteval)) {
		// PTE 为空（可能是文件页或未映射的匿名页）
		ret = do_anonymous_page(vma, address, pte, pmd, flags);
	} else if (!pte_present(pteval)) {
		// PTE 存在但不在内存中（可能是 swap 或文件页）
		if (pte_swp_exclusive(pteval)) {
			// 是 swap entry
			ret = do_swap_page(vma, address, pte, pmd, flags, pteval);
		} else if (pte_file(pteval)) {
			// 是文件页的 swap entry（很少见）
			ret = do_file_page(vma, address, pte, pmd, flags, pteval);
		}
	}

	return ret;
}
```

#### 14.5.3 匿名页的 swap 读回（do_swap_page）

**这是匿名页被 swap 出去后再次访问的处理：**

```c
// mm/memory.c

/*
 * do_swap_page: 处理 swap 页面的页错误
 */
vm_fault_t do_swap_page(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	swp_entry_t entry;
	struct folio *folio;
	vm_fault_t ret = 0;

	// 1. 从 PTE 中提取 swap entry
	entry = pte_to_swp_entry(vmf->orig_pte);

	// 2. 查找 swap entry 对应的页面（可能在 swap cache 中）
	folio = lookup_swap_cache(entry, vma, vmf->address);
	if (!folio) {
		// 不在 swap cache 中，需要从 swap 设备读取
		folio = swapin_readahead(entry, GFP_HIGHUSER_MOVABLE, vma, vmf->address);
	}

	// 3. 等待页面读取完成
	if (!folio || folio_test_locked(folio))
		folio_lock_or_retry(folio, vma->vm_mm, vmf->flags);

	// 4. 重新建立页表映射
	vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd, vmf->address, &vmf->ptl);

	// 5. 检查是否有其他进程已经将页面读回
	if (likely(!pte_same(*vmf->pte, vmf->orig_pte))) {
		// 其他进程已经处理了，直接使用
		folio_unlock(folio);
		folio_put(folio);
		return 0;
	}

	// 6. 将 swap entry 替换为物理地址
	inc_mm_counter(vma->vm_mm, MM_ANONPAGES);
	dec_mm_counter(vma->vm_mm, MM_SWAPENTS);
	set_pte_at(vma->vm_mm, vmf->address, vmf->pte, pteval);

	// 7. 刷新 TLB
	update_mmu_cache(vma, vmf->address, vmf->pte);

	return 0;
}
```

**关键步骤：**
1. **从 PTE 提取 swap entry**：PTE 中存储了 swap 设备号和偏移
2. **查找 swap cache**：先检查页面是否在 swap cache 中
3. **从 swap 设备读取**：如果不在 cache 中，从 swap 设备读取
4. **重新建立映射**：将 PTE 设置为物理地址
5. **处理竞争**：可能有多个进程同时访问，需要处理竞争

#### 14.5.4 文件页的重新读取（filemap_fault）

**文件页被回收后再次访问的处理：**

```c
// mm/filemap.c

/*
 * filemap_fault: 文件页的页错误处理
 */
vm_fault_t filemap_fault(struct vm_fault *vmf)
{
	struct file *file = vmf->vma->vm_file;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t index = vmf->pgoff;
	struct folio *folio;

	// 1. 在 Page Cache 中查找
	folio = filemap_get_folio(mapping, index);
	
	if (likely(!IS_ERR(folio))) {
		// 找到了，直接使用
		if (folio_test_uptodate(folio))
			goto out;
	} else {
		// 2. 未找到，需要从磁盘读取
		folio = filemap_create_folio(file, mapping, pos, &fbatch);
		
		// 3. 从磁盘读取数据
		error = filemap_read_folio(file, mapping->a_ops->read_folio, folio);
		if (error)
			goto error;
	}

	// 4. 建立页表映射
	vmf->page = &folio->page;
	ret = finish_fault(vmf);
	
	return ret;
}
```

**关键步骤：**
1. **在 Page Cache 中查找**：检查页面是否仍在缓存中
2. **创建新页面**：如果不在缓存中，创建新页面
3. **从磁盘读取**：调用文件系统的 `read_folio` 操作
4. **建立映射**：将页面映射到进程地址空间

#### 14.5.5 完整的访问流程对比

**场景 1：匿名页被 swap 出去后访问**

```
进程访问 *(char *)addr
    ↓
CPU 检测到页错误（PTE 指向 swap entry）
    ↓
do_page_fault() → handle_mm_fault()
    ↓
检查 PTE，发现是 swap entry
    ↓
do_swap_page()
    ↓
查找 swap cache（lookup_swap_cache）
    ├─→ 找到：直接使用
    └─→ 未找到：
            ↓
        从 swap 设备读取（swapin_readahead）
            ↓
        读取到新页面
            ↓
        添加到 swap cache（可选）
            ↓
        建立页表映射（PTE = 物理地址）
            ↓
        刷新 TLB
            ↓
        返回，进程继续执行
```

**场景 2：文件页被回收后访问**

```
进程访问 *(char *)addr（文件映射）
    ↓
CPU 检测到页错误（PTE 无效）
    ↓
do_page_fault() → handle_mm_fault()
    ↓
检查 PTE，发现为空
    ↓
filemap_fault()
    ↓
在 Page Cache 中查找（filemap_get_folio）
    ├─→ 找到：直接使用
    └─→ 未找到：
            ↓
        创建新页面（filemap_create_folio）
            ↓
        从磁盘读取（filemap_read_folio）
            ↓
        添加到 Page Cache
            ↓
        建立页表映射
            ↓
        返回，进程继续执行
```

#### 14.5.6 swap cache 的作用

**为什么需要 swap cache？**

```c
// mm/swap_state.c

/*
 * swap cache: 存储刚从 swap 读回的页面
 * 避免多个进程同时访问同一个 swap 页面时重复读取
 */

/*
 * lookup_swap_cache: 在 swap cache 中查找页面
 */
struct folio *lookup_swap_cache(swp_entry_t entry,
				struct vm_area_struct *vma,
				unsigned long addr)
{
	struct folio *folio;

	// 通过 swap entry 查找对应的 address_space
	swap_address_space(entry);

	// 在 swap cache 的 xarray 中查找
	folio = filemap_get_folio(swap_address_space(entry), swp_offset(entry));

	return folio;
}
```

**swap cache 的优势：**
- **避免重复读取**：多个进程访问同一个 swap 页面时，只需读取一次
- **一致性**：保证多个进程看到相同的页面内容
- **减少 I/O**：显著减少 swap I/O 操作

#### 14.5.7 页错误处理的优化

**1. Swapin Readahead**

```c
// mm/swap_state.c

/*
 * swapin_readahead: 预读 swap 页面
 */
struct folio *swapin_readahead(swp_entry_t entry, gfp_t gfp_flags,
			       struct vm_area_struct *vma, unsigned long addr)
{
	struct folio *folio;
	unsigned int i, nr;
	swp_entry_t entries[SWAP_BATCH];

	// 预读多个连续的 swap 页面
	for (i = 0; i < nr; i++) {
		entries[i] = entry + i;
	}

	for (i = 0; i < nr; i++) {
		// 从 swap 设备读取页面
		folio = swap_read_folio(entries[i], gfp_flags, vma, addr);
		if (folio)
			folio_put(folio);  // 已添加到 swap cache
	}

	return lookup_swap_cache(entry, vma, addr);
}
```

**2. 预读（Readahead）**

```c
// mm/filemap.c

/*
 * filemap_readahead: 文件页的预读
 */
void filemap_readahead(struct address_space *mapping,
		       struct file_ra_state *ra,
		       struct file *file,
		       pgoff_t index)
{
	// 触发预读，提前读取后续页面
	ondemand_readahead(mapping, ra, file, true, index, ra->async_size);
}
```

#### 14.5.8 总结：回收和再次访问的完整流程

**回收阶段：**
1. **取消页表映射**（`try_to_unmap`）
   - 匿名页：PTE → swap entry
   - 文件页：PTE → 无效
2. **写回数据**（如果是脏页）
   - 匿名页：写入 swap 设备
   - 文件页：写入磁盘文件
3. **释放页面**：页面返回到 buddy 分配器

**再次访问阶段：**
1. **触发页错误**：CPU 检测到 PTE 无效
2. **页错误处理**：
   - 匿名页：`do_swap_page()` → 从 swap 读取
   - 文件页：`filemap_fault()` → 从磁盘读取
3. **重新建立映射**：PTE → 物理地址
4. **进程继续执行**：页面重新可用

**关键设计点：**
- **透明性**：对进程透明，进程不需要知道页面被回收
- **延迟加载**：按需加载，只在访问时才读取
- **缓存优化**：swap cache 和 Page Cache 避免重复读取
- **预读优化**：提前读取后续页面，提高性能

### 14.6 总结

1. **三个水位线**：
   - **WMARK_MIN**：约 0.39% 总内存（紧急情况）
   - **WMARK_LOW**：约 0.78% 总内存（触发 kswapd）
   - **WMARK_HIGH**：约 1.17% 总内存（kswapd 停止）

2. **页面选择**：
   - 优先回收非活跃匿名页
   - 其次是非活跃文件页
   - 使用第二次机会算法避免误回收
   - 根据 swappiness 调整匿名页和文件页的比例

3. **回收过程**：
   - 扫描 LRU 列表
   - 检查页面引用
   - 取消页表映射（匿名页→swap entry，文件页→无效）
   - 写回脏页
   - 从 Page Cache 移除
   - 释放到 buddy 分配器

4. **再次访问处理**：
   - 触发页错误
   - 匿名页：从 swap 设备读取（`do_swap_page`）
   - 文件页：从磁盘读取（`filemap_fault`）
   - 重新建立页表映射
   - 透明恢复，进程继续执行

---

## 十五、总结

Page Cache 是 Linux 内核中至关重要的性能优化机制：

Page Cache 是 Linux 内核中至关重要的性能优化机制：

Page Cache 是 Linux 内核中至关重要的性能优化机制：
