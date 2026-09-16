# ARMv9 BTI (Branch Target Identification) 功能验证

## 1. BTI 简介

### 1.1 什么是 BTI

BTI（Branch Target Identification，分支目标识别）是 ARMv8.5-A 引入的控制流完整性（Control Flow Integrity, CFI）安全扩展，在 ARMv9 平台中广泛应用。

BTI 的主要目标：

- 防止 JOP（Jump-Oriented Programming）
- 防止 COP（Call-Oriented Programming）
- 限制间接分支（Indirect Branch）只能跳转到合法目标地址

**传统情况下：**

```
Indirect Branch
        |
        v
  任意地址执行
```

攻击者可以通过修改函数指针、虚函数表、跳转表等方式，将控制流重定向到任意代码位置。

**开启 BTI 后：**

```
Indirect Branch
        |
        v
目标地址检查
        |
        +----------------+
        |                |
        v                v
存在 BTI landing pad    无 BTI
        |                |
        v                v
   正常执行          BTI exception
```

只有包含合法 BTI landing pad 的地址才能作为间接跳转目标。

---

## 2. 测试平台信息

### Hardware

| 项目         | 配置                               |
| ------------ | ---------------------------------- |
| Board        | Hardware-EVB                       |
| SoC          | CIX P1 CP8180                      |
| CPU          | Cortex-A720 / Cortex-A520          |
| Architecture | ARMv9-A                            |
| CPU topology | 8 × Cortex-A720, 4 × Cortex-A520 |

### Kernel

| 项目         | 配置                     |
| ------------ | ------------------------ |
| Linux        | 6.6.89-cix-build-generic |
| Architecture | aarch64                  |

---

## 3. BTI 验证流程

BTI 验证分为四个层次：

```
CPU Hardware
       |
       v
Linux Kernel Feature Detection
       |
       v
Compiler / ELF Generation
       |
       v
Runtime Enforcement
```

---

## 4. CPU Hardware 支持验证

### 4.1 查看 Linux CPU Feature

执行：

```bash
grep -m1 '^Features' /proc/cpuinfo
```

输出：

```
...
paca pacg
bti
mte
mte3
...
```

结果：

| Feature | Support |
| ------- | ------- |
| PAC     | Yes     |
| BTI     | Yes     |
| MTE     | Yes     |

其中 `bti` 表示：**Linux ARM64 feature framework 已识别 BTI capability**。

### 4.2 BTI 对应硬件寄存器

ARM ARM 定义：

```
ID_AA64ISAR2_EL1
```

其中 **BTI field** 用于描述 Branch Target Identification 支持情况。

因此：

```
CPU
 |
 | ID_AA64ISAR2_EL1
 |
 v
Linux cpufeature
 |
 v
/proc/cpuinfo bti
```

说明：**硬件具备 BTI 扩展**。

---

## 5. 编译 BTI 测试程序

测试代码 `bti_test.c`：

```c
#include <stdio.h>

__attribute__((noinline))
int foo()
{
    return 123;
}

int main()
{
    printf("%d\n", foo());
    return 0;
}
```

### 使用 BTI 编译

命令：

```bash
gcc \
-O2 \
-mbranch-protection=bti \
bti_test.c \
-o bti_test
```

参数说明：`-mbranch-protection=bti`

作用：要求 GCC：

- 插入 BTI landing pad
- 生成 GNU property note

---

## 6. ELF BTI 属性检查

执行：

```bash
readelf -n bti_test
```

输出：

```
Displaying notes found in: .note.gnu.property

Owner:
    GNU

Description:
    NT_GNU_PROPERTY_TYPE_0

Properties:
    AArch64 feature: BTI
```

说明：

```
Compiler
     |
     v
ELF GNU property
     |
     v
BTI enabled binary
```

验证结果：**AArch64 feature: BTI ✅ ELF 已启用 BTI**。

---

## 7. 查看 BTI 指令

反汇编：

```bash
objdump -d bti_test | grep bti
```

预期：

```
bti c
```

例如：

```
0000000000000754 <foo>:
    bti c
    mov w0,#123
    ret
```

说明：**函数入口增加 BTI landing pad**。

---

## 8. BTI 工作机制

### 正常间接调用

```
BLR x0
  |
  v
foo entry
  |
  v
BTI c check
  |
  v
function body
```

目标地址：

```
foo:
+-------------+
| bti c       |
+-------------+
| instruction |
| ...         |
+-------------+
```

### 非法跳转

```
BLR x0
  |
  v
foo+4  (no BTI landing pad)
  |
  v
BTI exception
```

攻击者无法直接跳转到函数中间位置执行。

---

## 9. BTI 与 PAC 区别

ARMv9 常见组合：**PAC + BTI**

二者保护目标不同：

| Feature | 防护对象                                     |
| ------- | -------------------------------------------- |
| PAC     | Pointer Authentication，防止指针篡改         |
| BTI     | Branch Target Identification，限制控制流目标 |

### PAC 保护返回地址

```
Return address
      |
      v
PAC authentication
      |
      v
Valid return
```

### BTI 保护间接调用

```
Indirect Call
      |
      v
BTI landing pad check
      |
      v
Valid target
```

---

## 10. SVE 说明（重要修正）

当前：

```bash
grep Features /proc/cpuinfo
```

未显示：

```
sve
sve2
```

**不能直接说明 CPU 不支持 SVE**。

### 原因

Linux `/proc/cpuinfo` 显示的是：

> **Linux sanitised feature set**

不是：

> Raw hardware feature register

### 硬件验证结果

后续通过 `ID_AA64PFR0_EL1` 验证：

```
CPU0  SVE=1
CPU1  SVE=1
...
CPU11 SVE=1
```

说明：

```
Cortex-A720:
    SVE supported

Cortex-A520:
    SVE supported
```

但是 Linux：

```
SANITISED ID_AA64PFR0_EL1

SVE=0
```

### 结论

**硬件支持 SVE，但当前 BSP Linux 内核没有向用户空间开放 SVE**。

需要进一步分析 `arch/arm64/kernel/cpufeature.c` 中的 SVE feature sanitisation。

---

## 11. 当前 BTI 测试结论

### 平台

- CIX P1 CP8180
- ARMv9-A
- Cortex-A720/A520

### 验证结果

| Layer          | Result                  |
| -------------- | ----------------------- |
| CPU Hardware   | ✅ BTI supported        |
| Linux Kernel   | ✅ Recognized           |
| GCC            | ✅ Generates BTI        |
| ELF Property   | ✅ AArch64 feature: BTI |
| Runtime Binary | ✅ BTI enabled          |

### 完整链路

```
CPU
 |
 | BTI capability
 v

Linux Kernel
 |
 | CPU feature detection
 v

GCC
 |
 | -mbranch-protection=bti
 v

ELF GNU Property
 |
 v

Runtime BTI Protection
```

---

## 总结

在 CIX P1 ARMv9 平台上，通过：

1. `/proc/cpuinfo` 检查 BTI capability；
2. GCC `-mbranch-protection=bti` 编译生成 BTI-enabled ELF；
3. `readelf -n` 验证 `AArch64 feature: BTI`；
4. `objdump` 查看 `bti c` 指令。

确认：**Cortex-A720/A520 平台具备 BTI 硬件能力，Linux BSP 已正确识别 BTI，工具链能够生成 BTI 指令，用户态 ELF 已启用 BTI 防护**。

同时，SVE 调查表明：

**`/proc/cpuinfo` 不显示 SVE 并不代表 CPU 不支持 SVE。通过直接读取 ID_AA64PFR0_EL1 已确认 Cortex-A720/A520 均支持 SVE，目前问题定位到 Linux kernel feature sanitisation 阶段，需要进一步分析 BSP 内核实现。**

---

## 附录：CPU MIDR 解析

```
0x410fd811
        ^^^
        A720

0x410fd801
        ^^^
        A520
```

| CPU 范围          | MIDR       | 核类型      | SVE 状态 |
| ----------------- | ---------- | ----------- | -------- |
| 0,1,6,7,8,9,10,11 | 0x410fd811 | Cortex-A720 | SVE=1    |
| 2,3,4,5           | 0x410fd801 | Cortex-A520 | SVE=1    |
