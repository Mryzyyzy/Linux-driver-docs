# ATF 中安全分区（SP）的限制和实际情况

## 重要澄清

您的理解是**完全正确的**！本文档澄清 ATF 中 SP 的实际限制和实现情况。

---

## 1. 关键要点

### 1.1 SP 不是嵌入在 ATF 框架中的

**正确理解**：
- **SP（安全分区）是独立的二进制镜像**，不是 ATF 代码的一部分
- SP 在运行时加载，不是编译时链接到 ATF
- SP 通过 `SP_LAYOUT_FILE` 指定，在系统启动时由 BL2 加载

**错误的误解**：
- ❌ SP 是 ATF 代码的一部分
- ❌ 可以在 ATF 源代码中创建 SP1、SP2 等代码

**实际情况**：
- ✅ SP 是独立的可执行镜像（.bin 文件）
- ✅ SP 通过清单文件（manifest）和布局文件（layout）配置
- ✅ SP 在运行时动态加载

### 1.2 ATF 的 EL3 SPMC 只支持单个 SP

**代码证据**：

```c
// services/std_svc/spm/el3_spmc/spmc.h

/*
 * This define identifies the only SP that will be initialised and participate
 * in FF-A communication. The implementation leaves the door open for more SPs
 * to be managed in future but for now it is reasonable to assume that either a
 * single S-EL0 or a single S-EL1 SP will be supported. This define will be used
 * to identify which SP descriptor to initialise and manage during SP runtime.
 */
#define ACTIVE_SP_DESC_INDEX	0
```

**关键注释说明**：
- "the **only** SP" - 唯一的 SP
- "a **single** S-EL0 or a **single** S-EL1 SP" - 单个 SP

**文档证据**：

```
EL3 SPMC based on the FF-A specification, managing a single S-EL1 partition
```

---

## 2. ATF 中的三种实现模式对比

### 2.1 三种模式

| 模式 | SPMC位置 | 支持的SP数量 | 多SP支持 |
|------|----------|-------------|---------|
| **S-EL2 SPMC** | S-EL2 | **多个** | ✅ 支持 |
| **EL3 SPMC** | EL3 | **单个** | ❌ 不支持 |
| **EL3 SPM (MM)** | EL3 | **单个** | ❌ 不支持 |

### 2.2 S-EL2 SPMC 模式（支持多个 SP）

**特点**：
- SPMC 运行在 **S-EL2**（需要 FEAT_SEL2 架构扩展）
- 支持**多个 S-EL1 或 S-EL0 分区**
- 使用 `SP_LAYOUT_FILE` 指定多个 SP
- **SPMC 通常是 Hafnium**，不是 ATF 的一部分

**实现**：
- Hafnium 是一个独立的项目，不是 ATF 代码的一部分
- ATF 的 SPMD 将请求转发给 Hafnium（作为 SPMC）
- Hafnium 管理多个安全分区

**构建配置**：
```bash
SPD=spmd
SPMD_SPM_AT_SEL2=1        # SPMC 在 S-EL2
BL32=<path-to-hafnium>    # Hafnium 二进制
SP_LAYOUT_FILE=sp_layout.json  # 多个 SP 的布局文件
```

### 2.3 EL3 SPMC 模式（只支持单个 SP）

**特点**：
- SPMC 运行在 **EL3**（ATF 的一部分）
- **只支持单个 S-EL1 分区**
- 可选的 SEL0 SP 支持（`SPMC_AT_EL3_SEL0_SP=1`）

**代码实现**：
```c
// spmc.h
#define ACTIVE_SP_DESC_INDEX	0  // 只有索引 0 被使用

// spmc_main.c
static struct secure_partition_desc sp_desc[SECURE_PARTITION_COUNT];
// 虽然定义数组，但实际上只使用 sp_desc[0]
```

**构建配置**：
```bash
SPD=spmd
SPMC_AT_EL3=1             # SPMC 在 EL3
BL32=<path-to-sp-binary>  # 单个 SP 的二进制
# 注意：不需要 SP_LAYOUT_FILE（或只包含一个 SP）
```

---

## 3. SP 的实际创建方式

### 3.1 SP 是独立的二进制

**SP 的本质**：
- SP 是**独立编译的可执行镜像**
- 不是 ATF 源代码的一部分
- 实现 FF-A 接口，提供安全服务

**SP 开发流程**：
```
1. 开发 SP 源代码（独立的项目）
   ├── sp1/
   │   ├── sp1.c          # SP1 的源代码
   │   └── Makefile
   ├── sp2/
   │   ├── sp2.c          # SP2 的源代码
   │   └── Makefile
   
2. 编译生成 SP 二进制镜像
   ├── sp1.bin            # SP1 的二进制
   └── sp2.bin            # SP2 的二进制

3. 创建 SP 清单文件（manifest）
   ├── sp1.dts            # SP1 的清单
   └── sp2.dts            # SP2 的清单

4. 创建布局文件（layout）
   └── sp_layout.json     # 列出所有 SP

5. 在 ATF 构建时指定
   SP_LAYOUT_FILE=sp_layout.json
```

### 3.2 在 ATF 中"创建"SP 的含义

**实际上不是"创建"代码，而是"配置"SP**：

1. **准备 SP 二进制**：独立开发并编译 SP
2. **创建清单文件**：定义 SP 的属性（内存、权限等）
3. **创建布局文件**：列出要加载的 SP
4. **构建 ATF**：ATF 构建系统会：
   - 将 SP 二进制打包到 FIP
   - 在运行时加载 SP

**关键点**：
- ❌ **不是在 ATF 源代码中写 SP 代码**
- ✅ **是准备独立的 SP 镜像，然后在 ATF 构建时包含它们**

---

## 4. 多 SP 支持的实际限制

### 4.1 ATF 的 EL3 SPMC 限制

**实际情况**：
- ATF 的 EL3 SPMC 实现**只支持单个 SP**
- 代码中虽然定义了数组 `sp_desc[SECURE_PARTITION_COUNT]`
- 但实际只使用 `sp_desc[ACTIVE_SP_DESC_INDEX]`，即 `sp_desc[0]`

**代码证据**：
```c
// spmc_main.c
struct secure_partition_desc *spmc_get_current_sp_ctx(void)
{
    // 总是返回索引 0 的 SP
    return &(sp_desc[ACTIVE_SP_DESC_INDEX]);  // ACTIVE_SP_DESC_INDEX = 0
}
```

**未来可能的扩展**：
- 代码注释说："leaves the door open for more SPs in future"
- 但目前实现只支持单个 SP

### 4.2 真正的多 SP 支持

**需要 S-EL2 SPMC（通常是 Hafnium）**：

1. **Hafnium 不是 ATF 的一部分**
   - Hafnium 是独立的项目
   - 实现完整的 FF-A 规范，支持多个 SP

2. **ATF + Hafnium 的组合**
   ```
   ATF (BL31)
   └── SPMD (EL3) ──→ Hafnium (S-EL2, 作为 SPMC)
                        └── 管理多个 SP
                            ├── SP1
                            ├── SP2
                            └── SP3
   ```

3. **构建流程**：
   - 独立编译 Hafnium
   - 独立编译各个 SP
   - ATF 构建时包含 Hafnium 和 SP
   - 运行时：ATF 加载 Hafnium，Hafnium 加载多个 SP

---

## 5. 修正之前的理解

### 5.1 之前文档中的不准确之处

在之前的文档中，我可能给人错误的印象：
- ❌ 好像在 ATF 中可以直接创建多个 SP
- ❌ 好像 SP 是 ATF 代码的一部分

### 5.2 正确的理解

**SP 的本质**：
1. **SP 是独立的二进制镜像**，不是 ATF 代码
2. **ATF 的 EL3 SPMC 只支持单个 SP**
3. **多个 SP 需要 S-EL2 SPMC（如 Hafnium）**

**"创建多个 SP"的实际含义**：
- 不是写代码创建
- 而是：
  1. 独立开发多个 SP 项目
  2. 编译成多个二进制镜像
  3. 通过布局文件配置
  4. 在支持多 SP 的 SPMC（如 Hafnium）中运行

---

## 6. 实际开发场景

### 6.1 场景 1：使用 ATF EL3 SPMC（单个 SP）

```bash
# 1. 开发单个 SP
sp_project/
  ├── sp.c
  └── Makefile

# 2. 编译 SP
make → sp.bin

# 3. 创建清单文件
sp.dts (定义 SP 属性)

# 4. 构建 ATF（单个 SP）
make SPD=spmd SPMC_AT_EL3=1 BL32=sp.bin
```

**限制**：只能有一个 SP

### 6.2 场景 2：使用 S-EL2 SPMC/Hafnium（多个 SP）

```bash
# 1. 开发多个 SP（独立项目）
sp1_project/ → sp1.bin
sp2_project/ → sp2.bin
sp3_project/ → sp3.bin

# 2. 创建清单文件
sp1.dts
sp2.dts
sp3.dts

# 3. 创建布局文件
sp_layout.json:
{
    "SP1": {"image": "sp1.bin", "pm": "sp1.dts"},
    "SP2": {"image": "sp2.bin", "pm": "sp2.dts"},
    "SP3": {"image": "sp3.bin", "pm": "sp3.dts"}
}

# 4. 编译 Hafnium（独立的 SPMC）
hafnium_project/ → hafnium.bin

# 5. 构建 ATF（包含 Hafnium 和多个 SP）
make SPD=spmd SPMD_SPM_AT_SEL2=1 \
     BL32=hafnium.bin \
     SP_LAYOUT_FILE=sp_layout.json
```

**支持**：可以有多个 SP

---

## 7. 总结

### 7.1 关键要点

1. **SP 不是 ATF 代码的一部分**
   - SP 是独立的二进制镜像
   - 在运行时加载

2. **ATF 的 EL3 SPMC 只支持单个 SP**
   - 代码中明确注释："single SP"
   - `ACTIVE_SP_DESC_INDEX = 0` 只使用索引 0

3. **多个 SP 需要 S-EL2 SPMC（如 Hafnium）**
   - Hafnium 是独立的项目
   - 不是 ATF 的一部分
   - 实现完整的多 SP 支持

4. **"创建多个 SP"的正确理解**
   - 不是写代码创建
   - 而是开发独立的 SP 项目
   - 通过布局文件配置
   - 在支持多 SP 的 SPMC 中运行

### 7.2 架构对比

```
ATF EL3 SPMC 模式：
ATF (EL3 SPMC)
└── 单个 SP

ATF + Hafnium 模式：
ATF (SPMD)
└── Hafnium (S-EL2 SPMC)
    ├── SP1
    ├── SP2
    └── SP3
```

---

## 参考资料

1. ATF 源代码注释：
   - `services/std_svc/spm/el3_spmc/spmc.h:195-202`
   - `ACTIVE_SP_DESC_INDEX` 的定义和注释

2. ATF 文档：
   - `docs/components/secure-partition-manager.rst`
   - "EL3 SPMC ... managing a single S-EL1 partition"

3. Hafnium 项目：
   - 独立的 S-EL2 SPMC 实现
   - 支持多个安全分区

---

*文档创建日期：2025年*
*感谢您的纠正！*

