# `__aarch64__` 宏定义说明

## 1. 概述

`__aarch64__` 是一个**编译器预定义的宏**，当使用 AArch64 架构的编译器（如 GCC、Clang）编译代码时，编译器会自动定义这个宏。

**重要**：这个宏**不需要手动定义**，编译器会根据目标架构自动设置。

---

## 2. 如何定义

### 2.1 编译器自动定义

当你使用 AArch64 交叉编译器（如 `aarch64-linux-gnu-gcc`）编译代码时，编译器会自动定义 `__aarch64__` 宏。

**验证方法**：

```bash
# 使用 AArch64 编译器查看预定义的宏
aarch64-linux-gnu-gcc -dM -E -x c /dev/null | grep aarch64
```

**输出示例**：
```
#define __AARCH64_CMODEL_SMALL__ 1
#define __aarch64__ 1
#define __AARCH64EL__ 1
#define __ARM_ARCH_8A 1
#define __ARM_ARCH_ISA_A64 1
```

### 2.2 在代码中使用

```c
#ifdef __aarch64__
    // AArch64 (64-bit) 代码
    val = read_sctlr_el3();
#else
    // AArch32 (32-bit) 代码
    val = read_sctlr();
#endif
```

---

## 3. 在 TFTF 测试中的使用

### 3.1 编译配置

**文件**: `Makefile`

```makefile
CC := ${CROSS_COMPILE}gcc
# 如果 CROSS_COMPILE=aarch64-linux-gnu-，则使用 AArch64 编译器
```

**关键点**：
- 如果 `CROSS_COMPILE=aarch64-linux-gnu-`，则编译器是 `aarch64-linux-gnu-gcc`
- 编译器会自动定义 `__aarch64__` 宏
- **不需要在 Makefile 或代码中手动定义**

### 3.2 代码示例

**文件**: `plat/arm/fvp/include/platform_def.h`

```c
#ifdef __aarch64__
#define PLATFORM_LINKER_FORMAT		"elf64-littleaarch64"
#define PLATFORM_LINKER_ARCH		aarch64
#else
#define PLATFORM_LINKER_FORMAT		"elf32-littlearm"
#define PLATFORM_LINKER_ARCH		arm
#endif
```

**文件**: `tftf/tests/misc_tests/test_normal_serror.c`

```c
#ifdef __aarch64__

extern void inject_normal_serror(void);
extern void inject_uncontainable_ras_error(void);

static volatile uint64_t serror_triggered_in_tftf;
// ... AArch64 特定代码

#endif /* __aarch64__ */
```

---

## 4. 编译选项设置

### 4.1 使用 AArch64 编译器

**方法1：设置 CROSS_COMPILE 环境变量**

```bash
export CROSS_COMPILE=aarch64-linux-gnu-
make PLAT=fvp
```

**方法2：在 Makefile 中设置**

```makefile
CROSS_COMPILE ?= aarch64-linux-gnu-
```

### 4.2 编译器自动定义的宏

使用 AArch64 编译器时，以下宏会被自动定义：

| 宏 | 说明 |
|---|------|
| `__aarch64__` | AArch64 架构 |
| `__AARCH64EL__` | AArch64 小端（Little Endian） |
| `__ARM_ARCH_8A` | ARMv8-A 架构 |
| `__ARM_ARCH_ISA_A64` | AArch64 指令集 |

---

## 5. 常见问题

### 5.1 问题：`__aarch64__` 未定义

**原因**：
- 使用了错误的编译器（如使用 `arm-linux-gnueabihf-gcc` 而不是 `aarch64-linux-gnu-gcc`）
- 编译器配置错误

**解决方法**：
```bash
# 检查编译器
which aarch64-linux-gnu-gcc

# 设置正确的交叉编译器
export CROSS_COMPILE=aarch64-linux-gnu-

# 验证宏定义
aarch64-linux-gnu-gcc -dM -E -x c /dev/null | grep __aarch64__
```

### 5.2 问题：如何在代码中检查架构

**方法1：使用 `__aarch64__`（推荐）**

```c
#ifdef __aarch64__
    // AArch64 代码
#else
    // AArch32 代码
#endif
```

**方法2：使用 `__ARM_ARCH` 和 `__ARM_ARCH_ISA_A64`**

```c
#if defined(__ARM_ARCH_ISA_A64) && __ARM_ARCH_ISA_A64
    // AArch64 代码
#else
    // AArch32 代码
#endif
```

---

## 6. 实际使用示例

### 6.1 ATF 代码中的使用

**文件**: `bl1/bl1_main.c`

```c
#ifdef __aarch64__
	val = read_sctlr_el3();
#else
	val = read_sctlr();
#endif
```

### 6.2 TFTF 测试代码中的使用

**文件**: `tftf/tests/misc_tests/test_normal_serror.c`

```c
#ifdef __aarch64__

extern void inject_normal_serror(void);
extern void inject_uncontainable_ras_error(void);

static volatile uint64_t serror_triggered_in_tftf;

test_result_t test_normal_serror(void)
{
    // AArch64 特定的测试代码
    // ...
}

#endif /* __aarch64__ */
```

---

## 7. 编译流程

### 7.1 完整的编译流程

```
1. 设置编译器
   export CROSS_COMPILE=aarch64-linux-gnu-

2. 编译代码
   make PLAT=fvp

3. 编译器自动定义宏
   aarch64-linux-gnu-gcc 自动定义 __aarch64__ 宏

4. 预处理阶段
   #ifdef __aarch64__ 条件编译生效

5. 生成 AArch64 二进制文件
```

### 7.2 Makefile 中的配置

```makefile
# Makefile
CC := ${CROSS_COMPILE}gcc

# 如果 CROSS_COMPILE=aarch64-linux-gnu-
# 则 CC = aarch64-linux-gnu-gcc
# 编译器自动定义 __aarch64__ 宏
```

---

## 8. 总结

### 8.1 关键点

1. ✅ **`__aarch64__` 是编译器自动定义的宏**
2. ✅ **不需要手动定义**
3. ✅ **使用 AArch64 编译器（如 `aarch64-linux-gnu-gcc`）时会自动定义**
4. ✅ **在代码中使用 `#ifdef __aarch64__` 进行条件编译**

### 8.2 使用步骤

1. **设置正确的交叉编译器**：
   ```bash
   export CROSS_COMPILE=aarch64-linux-gnu-
   ```

2. **编译代码**：
   ```bash
   make PLAT=<your_platform>
   ```

3. **编译器自动定义宏**：
   - 编译器自动定义 `__aarch64__`
   - 代码中的 `#ifdef __aarch64__` 条件编译生效

4. **无需额外操作**：
   - 不需要在 Makefile 中添加 `-D__aarch64__`
   - 不需要在代码中手动定义

### 8.3 验证方法

```bash
# 1. 检查编译器是否支持 AArch64
aarch64-linux-gnu-gcc --version

# 2. 查看预定义的宏
aarch64-linux-gnu-gcc -dM -E -x c /dev/null | grep __aarch64__

# 3. 编译代码并检查
make PLAT=fvp VERBOSE=1
```

---

## 参考资料

1. GCC 预定义宏文档
2. ARM Architecture Reference Manual
3. TFTF 测试框架文档

---

*文档创建日期：2025年*








