# ATF 中的 FFA 框架总结

## 概述

**FF-A（Firmware Framework for Arm）** 是 ARM 定义的固件框架规范，用于在安全世界（Secure World）中实现安全分区（Secure Partition）之间的标准化通信和管理机制。在 **ARM Trusted Firmware (ATF/TF-A)** 中，FF-A 框架通过 **SPMD（Secure Partition Manager Dispatcher）** 和 **SPMC（Secure Partition Manager Core）** 来实现。

---

## 1. 架构组件

### 1.1 核心组件关系

```
┌─────────────────────────────────────────┐
│      Normal World (Hypervisor/OS)       │
│            FF-A 接口调用                 │
└─────────────────────────────────────────┘
                    │
                    ↓ SMC调用 (EL3)
┌─────────────────────────────────────────┐
│            SPMD (EL3)                    │
│  - 接收 FF-A 调用                        │
│  - 路由到 SPMC                           │
│  - 管理 SPMC 上下文切换                  │
└─────────────────────────────────────────┘
                    │
                    ↓
┌─────────────────────────────────────────┐
│    SPMC (S-EL1/S-EL2/EL3)               │
│  - 管理安全分区 (Secure Partitions)      │
│  - 实现 FF-A 规范                        │
│  - 处理分区间通信                        │
└─────────────────────────────────────────┘
                    │
                    ↓
┌──────────┐ ┌──────────┐ ┌──────────┐
│   SP1    │ │   SP2    │ │   SP3    │
│ (安全分区)│ │ (安全分区)│ │ (安全分区)│
└──────────┘ └──────────┘ └──────────┘
```

### 1.2 组件说明

#### SPMD (Secure Partition Manager Dispatcher)
- **位置**：运行在 **EL3** 特权级别
- **作用**：
  - 作为 FF-A 协议的中转站，接收来自正常世界的 FF-A 调用
  - 将请求转发到 SPMC 进行处理
  - 管理正常世界与安全世界之间的上下文切换
  - 处理 SPMC 的初始化和生命周期管理

#### SPMC (Secure Partition Manager Core)
- **位置**：可在 **S-EL1**、**S-EL2** 或 **EL3** 运行（取决于平台支持）
- **作用**：
  - 实现 FF-A 规范的核心功能
  - 管理安全分区的生命周期（创建、初始化、运行、销毁）
  - 处理安全分区之间的通信
  - 管理内存共享和安全隔离
  - 调度安全分区的执行

#### Secure Partition (SP)
- **位置**：运行在 **S-EL1** 或 **S-EL0**（当 SPMC 在 S-EL2 时）
- **作用**：
  - 提供安全服务（加密、认证、密钥管理等）
  - 通过 FF-A 接口与其他分区或正常世界通信

---

## 2. ATF 中的实现方式

### 2.1 三种实现模式

ATF 支持三种不同的 SPM 实现模式：

#### 2.1.1 S-EL2 SPMC（推荐）
- **特点**：基于 FF-A 规范，支持安全世界虚拟化
- **位置**：SPMC 运行在 S-EL2，管理多个 S-EL1 或 S-EL0 分区
- **要求**：需要 **FEAT_SEL2** 架构扩展支持
- **用途**：现代平台，支持多租户安全环境

#### 2.1.2 EL3 SPMC
- **特点**：基于 FF-A 规范，管理单个 S-EL1 分区
- **位置**：SPMC 运行在 EL3
- **用途**：不支持 FEAT_SEL2 的旧平台，或需要简单安全服务的场景

#### 2.1.3 EL3 SPM（传统）
- **特点**：基于 MM 规范的传统实现
- **位置**：运行在 EL3，管理单个 S-EL0 分区
- **用途**：向后兼容，遗留系统

### 2.2 构建选项

#### 关键构建参数

| 构建选项 | 说明 | 默认值 |
|---------|------|--------|
| `SPD=spmd` | 选择 SPMD 作为 Secure Payload Dispatcher | 必选 |
| `SPMD_SPM_AT_SEL2` | SPMC 运行在 S-EL2 | 当 SPD=spmd 时默认启用 |
| `SPMC_AT_EL3` | SPMC 运行在 EL3 | 0 |
| `SPMC_AT_EL3_SEL0_SP` | 启用 SEL0 SP 支持（当 SPMC 在 EL3 时） | 0 |
| `SP_LAYOUT_FILE` | 安全分区布局文件路径 | 必需（S-EL2 SPMC） |
| `BL32` | SPMC 镜像路径（如 Hafnium 或 TEE） | - |

#### 配置组合

| SPMC位置 | SPMD_SPM_AT_SEL2 | SPMC_AT_EL3 | CTX_INCLUDE_EL2_REGS |
|----------|------------------|-------------|---------------------|
| S-EL1 | 0 | 0 | 0 |
| S-EL2 | 1（默认） | 0 | 1 |
| EL3 | 0 | 1 | 0 |

---

## 3. FF-A 接口

### 3.1 主要接口函数

ATF 实现的 FF-A 接口定义在 `include/services/ffa_svc.h` 中，主要包括：

#### 3.1.1 基础接口
- **FFA_VERSION** (0x63)：获取 FF-A 版本信息
- **FFA_FEATURES** (0x64)：查询功能支持情况
- **FFA_ID_GET** (0x69)：获取调用者的端点 ID
- **FFA_SPM_ID_GET** (0x85)：获取 SPM 标识符

#### 3.1.2 分区管理接口
- **FFA_PARTITION_INFO_GET** (0x68)：获取分区信息
- **FFA_PARTITION_INFO_GET_REGS** (0x8B)：获取分区信息（扩展版本，v1.2）

#### 3.1.3 消息传递接口
- **FFA_MSG_SEND_DIRECT_REQ** (0x6F)：发送直接消息请求
- **FFA_MSG_SEND_DIRECT_RESP** (0x70)：发送直接消息响应
- **FFA_MSG_SEND_DIRECT_REQ2** (0x8D)：发送直接消息请求（v1.2）
- **FFA_MSG_SEND_DIRECT_RESP2** (0x8E)：发送直接消息响应（v1.2）
- **FFA_MSG_SEND2** (0x86)：间接消息发送（v1.1）
- **FFA_MSG_WAIT** (0x6B)：等待消息
- **FFA_MSG_YIELD** (0x6C)：让出执行
- **FFA_MSG_RUN** (0x6D)：运行安全分区

#### 3.1.4 内存管理接口
- **FFA_MEM_SHARE** (0x73)：共享内存
- **FFA_MEM_LEND** (0x72)：出借内存
- **FFA_MEM_DONATE** (0x71)：捐赠内存
- **FFA_MEM_RETRIEVE_REQ** (0x74)：检索共享内存请求
- **FFA_MEM_RETRIEVE_RESP** (0x75)：检索共享内存响应
- **FFA_MEM_RELINQUISH** (0x76)：释放内存
- **FFA_MEM_RECLAIM** (0x77)：回收内存
- **FFA_MEM_FRAG_TX** (0x7B)：内存片段发送
- **FFA_MEM_FRAG_RX** (0x7A)：内存片段接收
- **FFA_MEM_PERM_GET** (0x88)：获取内存权限（v1.1）
- **FFA_MEM_PERM_SET** (0x89)：设置内存权限（v1.1）

#### 3.1.5 邮箱接口
- **FFA_RXTX_MAP** (0x66)：映射 RX/TX 缓冲区
- **FFA_RXTX_UNMAP** (0x67)：取消映射 RX/TX 缓冲区
- **FFA_RX_RELEASE** (0x65)：释放 RX 缓冲区（v1.0 遗留）
- **FFA_RX_ACQUIRE** (0x84)：获取 RX 缓冲区（v1.1）

#### 3.1.6 通知接口（v1.1）
- **FFA_NOTIFICATION_BITMAP_CREATE** (0x7D)：创建通知位图
- **FFA_NOTIFICATION_BITMAP_DESTROY** (0x7E)：销毁通知位图
- **FFA_NOTIFICATION_BIND** (0x7F)：绑定通知
- **FFA_NOTIFICATION_UNBIND** (0x80)：解绑通知
- **FFA_NOTIFICATION_SET** (0x81)：设置通知
- **FFA_NOTIFICATION_GET** (0x82)：获取通知
- **FFA_NOTIFICATION_INFO_GET** (0x83)：获取通知信息

#### 3.1.7 其他接口
- **FFA_NORMAL_WORLD_RESUME** (0x7C)：恢复正常世界执行
- **FFA_SECONDARY_EP_REGISTER** (0x87)：注册辅助端点（v1.1）
- **FFA_EL3_INTR_HANDLE** (0x8C)：EL3 中断处理（v1.2）
- **FFA_NS_RES_INFO_GET** (0x8F)：获取非安全资源信息（v1.2）
- **FFA_ABORT** (0x90)：中止操作（v1.3 ALP2）

### 3.2 FF-A 版本支持

- **当前版本**：FF-A v1.2（部分 v1.3 功能）
- **版本格式**：主版本.次版本 (Major.Minor)
- **编译版本**：`FFA_VERSION_MAJOR=1`, `FFA_VERSION_MINOR=2`
- **版本号**：`MAKE_FFA_VERSION(1, 2) = 0x00010002`

### 3.3 端点 ID 分配

| ID 范围 | 用途 | 说明 |
|---------|------|------|
| 0x0000 - 0x7FFF | Normal World | 正常世界端点（Hypervisor/OS） |
| 0x8000 | SPMC ID | 安全分区管理器核心 |
| 0x8001 - 0xFFFE | Secure Partitions | 安全分区 ID |
| 0xFFFF | SPMD 端点 | SPMD 直接消息端点 |

---

## 4. 关键代码结构

### 4.1 目录结构

```
arm-trusted-firmware/
├── include/services/
│   └── ffa_svc.h                    # FF-A 接口定义和宏
├── services/
│   ├── std_svc/
│   │   ├── spmd/                    # SPMD 实现
│   │   │   ├── spmd_main.c          # SPMD 主逻辑
│   │   │   ├── spmd_svc.c           # SPMD 服务处理
│   │   │   └── spmd_private.h       # SPMD 私有定义
│   │   └── spm/
│   │       └── el3_spmc/            # EL3 SPMC 实现
│   │           ├── spmc_main.c      # SPMC 主逻辑
│   │           ├── spmc.h           # SPMC 数据结构
│   │           └── spmc_shared_mem.h # 共享内存管理
│   └── el3_spmc_ffa_memory.h        # EL3 SPMC 内存管理
└── bl32/tsp/
    └── ffa_helpers.h                # FF-A 辅助函数（TSP 使用）
```

### 4.2 核心数据结构

#### SPMD 上下文
```c
// spmd_private.h
typedef struct spmd_spm_core_context {
    cpu_context_t cpu_ctx;           // CPU 上下文
    void *c_rt_ctx;                  // C 运行时上下文
    uint32_t state;                  // SPMC 状态
} spmd_spm_core_context_t;
```

#### 安全分区描述符
```c
// spmc.h
struct secure_partition_desc {
    struct sp_exec_ctx *ec[PLATFORM_CORE_COUNT];  // 执行上下文
    struct mailbox mailbox;                       // 邮箱
    uint16_t sp_id;                               // 分区 ID
    uint32_t uuid[4];                             // UUID
    enum sp_runtime_el el;                        // 异常级别
    enum sp_execution_state exec_state;           // 执行状态
    // ... 其他字段
};
```

#### 执行上下文
```c
// spmc.h
struct sp_exec_ctx {
    uint64_t c_rt_ctx;                // C 运行时上下文
    cpu_context_t cpu_ctx;            // CPU 架构上下文
    enum sp_runtime_states rt_state;  // 运行时状态
    enum sp_runtime_model rt_model;   // 运行时模型
    uint16_t dir_req_origin_id;       // 直接请求源 ID
    uint16_t dir_req_funcid;          // 直接请求函数 ID
};
```

---

## 5. 工作流程

### 5.1 系统启动流程

```
1. BL1 启动
   ↓
2. BL2 启动
   │   ├─ 加载 SPMC 镜像（BL32）
   │   ├─ 加载安全分区镜像
   │   └─ 加载配置（TOS_FW_CONFIG）
   ↓
3. BL31 启动
   │   ├─ SPMD 初始化
   │   │   ├─ 初始化 SPMC 上下文
   │   │   ├─ 设置异常级别
   │   │   └─ 准备启动参数
   │   └─ 启动 SPMC
   │       ├─ 传递配置地址
   │       └─ 进入 SPMC
   ↓
4. SPMC 初始化
   │   ├─ 解析清单文件
   │   ├─ 初始化安全分区
   │   └─ 准备运行环境
   ↓
5. 系统就绪
```

### 5.2 FF-A 调用流程

```
Normal World (调用者)
    │
    │ 1. 发起 SMC 调用 (FF-A 函数 ID)
    ↓
SPMD (EL3)
    │
    │ 2. 解析 FF-A 函数 ID
    │ 3. 验证调用者权限
    │ 4. 判断目标：
    │    ├─ SPMD 自身处理？
    │    └─ 转发到 SPMC？
    ↓
SPMC (S-EL1/S-EL2/EL3)
    │
    │ 5. 处理 FF-A 调用
    │ 6. 根据调用类型：
    │    ├─ 分区管理
    │    ├─ 消息路由
    │    ├─ 内存操作
    │    └─ 其他服务
    │
    │ 7. 如需要，切换到目标安全分区
    ↓
Secure Partition (如需要)
    │
    │ 8. 处理服务请求
    │ 9. 返回结果
    ↓
SPMC
    │
    │ 10. 收集结果
    │ 11. 切换回调用者上下文
    ↓
SPMD
    │
    │ 12. 返回结果给 Normal World
    ↓
Normal World (收到响应)
```

### 5.3 消息传递流程（直接消息）

```
发送方 (Sender)
    │
    │ 调用 FFA_MSG_SEND_DIRECT_REQ
    ↓
SPMD
    │
    │ 转发到 SPMC
    ↓
SPMC
    │
    │ 1. 验证目标分区 ID
    │ 2. 检查目标分区状态
    │ 3. 切换上下文到目标分区
    │ 4. 传递消息参数（x0-x7）
    ↓
接收方 (Receiver)
    │
    │ 5. 处理消息
    │ 6. 准备响应
    │ 7. 调用 FFA_MSG_SEND_DIRECT_RESP
    ↓
SPMC
    │
    │ 8. 切换上下文回发送方
    ↓
SPMD
    │
    │ 返回响应到 Normal World
    ↓
发送方 (收到响应)
```

### 5.4 内存共享流程

```
源分区 (Source Partition)
    │
    │ 1. 调用 FFA_MEM_SHARE
    │    ├─ 指定内存区域
    │    ├─ 指定接收者
    │    └─ 设置权限
    ↓
SPMC
    │
    │ 2. 验证内存区域
    │ 3. 创建内存描述符 (MTD)
    │ 4. 设置内存属性
    │ 5. 通知接收者（通过消息或通知机制）
    ↓
目标分区 (Target Partition)
    │
    │ 6. 调用 FFA_MEM_RETRIEVE_REQ
    │    ├─ 提供内存句柄
    │    └─ 指定映射属性
    ↓
SPMC
    │
    │ 7. 验证权限
    │ 8. 建立内存映射
    │ 9. 返回映射信息 (FFA_MEM_RETRIEVE_RESP)
    ↓
目标分区
    │
    │ 10. 可以访问共享内存
    │ 11. 使用完后调用 FFA_MEM_RELINQUISH
    ↓
源分区
    │
    │ 12. 调用 FFA_MEM_RECLAIM 回收内存
```

---

## 6. 关键特性

### 6.1 安全隔离

多个独立安全分区通过以下机制实现隔离：

#### 6.1.1 内存隔离（Memory Isolation）

**独立的页表上下文（Translation Table Context）**

每个安全分区拥有完全独立的页表上下文（`xlat_ctx_handle`），这是内存隔离的核心机制：

```c
// 每个安全分区描述符中包含独立的页表上下文
struct secure_partition_desc {
    xlat_ctx_t *xlat_ctx_handle;  // 独立的页表上下文
    // ... 其他字段
};
```

**工作原理**：
1. **独立的页表基地址（TTBR）**：每个分区在初始化时创建自己的页表，设置独立的 `TTBR0_EL1` 寄存器值
2. **独立的地址空间映射**：每个分区只能访问自己的内存区域，无法访问其他分区的内存
3. **硬件强制隔离**：通过 ARM MMU（Memory Management Unit）硬件强制实施，确保分区间内存完全隔离

**内存区域配置**：
- 每个分区的清单文件（Manifest）中定义了其可访问的内存区域
- 包括代码区、数据区、堆、栈等
- 只有明确配置的内存区域才会被映射到分区的页表中

#### 6.1.2 执行上下文隔离（Execution Context Isolation）

**独立的 CPU 上下文**

每个安全分区拥有独立的执行上下文（`sp_exec_ctx`），包含完整的 CPU 状态：

```c
struct sp_exec_ctx {
    uint64_t c_rt_ctx;              // C 运行时上下文（栈指针等）
    cpu_context_t cpu_ctx;          // CPU 架构状态（寄存器、系统寄存器等）
    enum sp_runtime_states rt_state; // 运行时状态
    enum sp_runtime_model rt_model;  // 运行时模型
    uint16_t dir_req_origin_id;     // 直接请求源 ID（用于验证）
    uint16_t dir_req_funcid;        // 直接请求函数 ID
};
```

**隔离特性**：
- **寄存器隔离**：每个分区运行时，其通用寄存器、系统寄存器（如 SCTLR、TCR、MAIR 等）都是独立的
- **栈隔离**：每个分区有独立的栈空间，不会相互干扰
- **状态隔离**：每个分区的运行时状态（运行中、等待、阻塞等）独立管理

#### 6.1.3 ID 隔离（Identity Isolation）

**唯一的分区标识符**

每个安全分区拥有唯一的分区 ID（`sp_id`）：

```c
struct secure_partition_desc {
    uint16_t sp_id;        // 唯一的分区 ID（0x8001 - 0xFFFE）
    uint32_t uuid[4];      // UUID（全局唯一标识符）
    // ...
};
```

**ID 分配规则**：
- 正常世界端点：0x0000 - 0x7FFF
- SPMC ID：0x8000
- 安全分区 ID：0x8001 - 0xFFFE（每个分区分配一个）
- SPMD 端点：0xFFFF

**作用**：
- 消息路由：通过 ID 唯一标识消息的发送者和接收者
- 权限验证：确保分区只能访问授权的资源
- 资源管理：通过 ID 关联分区的所有资源

#### 6.1.4 邮箱隔离（Mailbox Isolation）

**独立的通信缓冲区**

每个安全分区拥有独立的邮箱（Mailbox）用于消息传递：

```c
struct mailbox {
    enum mailbox_state state;     // 邮箱状态（空/满）
    void *rx_buffer;              // 接收缓冲区（用于接收消息）
    const void *tx_buffer;        // 发送缓冲区（用于发送消息）
    uint32_t rxtx_page_count;     // 缓冲区页数
    spinlock_t lock;              // 访问锁
};
```

**RX/TX 缓冲区的作用**：

RX 和 TX 缓冲区是**不同的缓冲区**，用于支持**间接消息传递（Indirect Messaging）**：

1. **直接消息（Direct Message）**：
   - 使用 `FFA_MSG_SEND_DIRECT_REQ` 接口
   - 数据通过 CPU 寄存器传递（x0-x7，共 8 个 64 位寄存器 = 64 字节）
   - **限制**：只能传递小数据量（最多 64 字节）
   - **优点**：快速、同步、无需额外内存

2. **间接消息（Indirect Message）**：
   - 使用 `FFA_MSG_SEND2` 接口
   - 数据放在 **TX 缓冲区**中，可以传递**任意大小**的数据
   - 接收方从 **RX 缓冲区**读取数据
   - **优点**：支持大数据传输（几 KB 到几 MB）

**工作流程**：

```
发送方（Sender）：
    │
    │ 1. 将消息数据写入自己的 TX 缓冲区
    │ 2. 调用 FFA_MSG_SEND2（指定数据大小）
    ↓
SPMC
    │
    │ 3. 验证消息和权限
    │ 4. 将数据从发送方的 TX 缓冲区复制到接收方的 RX 缓冲区
    │ 5. 通知接收方有消息到达
    ↓
接收方（Receiver）：
    │
    │ 6. 调用 FFA_MSG_WAIT 或检查邮箱状态
    │ 7. 从自己的 RX 缓冲区读取消息数据
    │ 8. 处理完成后调用 FFA_RX_RELEASE 释放缓冲区
```

**为什么 RX 和 TX 要独立？**

1. **方向分离**：
   - **TX 缓冲区**：发送方写入，SPMC 读取
   - **RX 缓冲区**：SPMC 写入，接收方读取
   - 避免发送方和接收方直接访问同一块内存

2. **内存隔离**：
   - 每个分区只能访问自己的 RX/TX 缓冲区
   - SPMC 负责在分区间安全地复制数据
   - 确保分区之间不能直接访问对方的内存

3. **并发安全**：
   - 发送和接收使用不同的缓冲区，避免竞争条件
   - 每个缓冲区有独立的锁保护

4. **大数据支持**：
   - 可以传递远超过 64 字节的数据
   - 例如：传递加密密钥、证书、大块数据等

**映射过程**：

分区在使用邮箱前，需要先映射 RX/TX 缓冲区：

```c
// 分区调用 FFA_RXTX_MAP 映射缓冲区
FFA_RXTX_MAP(
    tx_address,    // 发送缓冲区的物理地址
    rx_address,    // 接收缓冲区的物理地址（必须不同）
    page_count     // 缓冲区大小（页数）
);
```

**隔离特性**：
- 每个分区的 RX/TX 缓冲区在物理内存中是独立的
- 通过 FF-A 的 `FFA_RXTX_MAP` 接口映射各自的缓冲区
- 分区之间不能直接访问对方的邮箱缓冲区
- SPMC 负责在分区间安全地复制数据，确保隔离

#### 6.1.5 地址空间隔离（Address Space Isolation）

**独立的虚拟地址空间**

通过独立的页表，每个分区拥有独立的虚拟地址空间：

```
分区 1 的虚拟地址空间：
┌─────────────────────┐
│  代码段 (RX)        │  0x0000_0000
│  数据段 (RW)        │
│  堆 (RW)            │
│  栈 (RW)            │
│  共享内存 (按需)    │
└─────────────────────┘  0xFFFF_FFFF

分区 2 的虚拟地址空间：
┌─────────────────────┐
│  代码段 (RX)        │  0x0000_0000
│  数据段 (RW)        │
│  堆 (RW)            │
│  栈 (RW)            │
│  共享内存 (按需)    │
└─────────────────────┘  0xFFFF_FFFF

（两个分区使用相同的虚拟地址范围，但映射到不同的物理地址）
```

**关键点**：
- 每个分区可以使用相同的虚拟地址范围
- 但虚拟地址映射到不同的物理地址
- 通过切换页表基址（TTBR）实现分区切换

#### 6.1.6 运行时状态隔离（Runtime State Isolation）

**独立的状态机**

每个分区维护独立的运行时状态：

```c
enum sp_runtime_states {
    RT_STATE_WAITING,    // 等待状态
    RT_STATE_RUNNING,    // 运行状态
    RT_STATE_PREEMPTED,  // 被抢占状态
    RT_STATE_BLOCKED     // 阻塞状态
};

enum sp_runtime_model {
    RT_MODEL_DIR_REQ,    // 直接请求模型
    RT_MODEL_RUN,        // 运行模型
    RT_MODEL_INIT,       // 初始化模型
    RT_MODEL_INTR        // 中断模型
};
```

**状态管理**：
- 每个分区的状态独立维护，互不影响
- SPMC 根据分区状态决定是否调度该分区
- 分区状态切换不会影响其他分区

#### 6.1.7 异常级别隔离（Exception Level Isolation）

**不同的运行级别**

当 SPMC 运行在 S-EL2 时，支持分区运行在不同异常级别：

- **S-EL1 分区**：运行在 S-EL1，拥有更多权限，可以管理 S-EL0 分区
- **S-EL0 分区**：运行在 S-EL0，权限受限，由 S-EL1 分区或 SPMC 管理

**层级隔离**：
```
S-EL2 (SPMC)
    │
    ├── S-EL1 分区 1
    │       └── S-EL0 分区 1.1
    │
    └── S-EL1 分区 2
            └── S-EL0 分区 2.1
```

#### 6.1.8 上下文切换机制（Context Switching）

**安全的上下文保存和恢复**

当需要在分区间切换时，SPMC 执行完整的上下文切换：

```
分区 A 运行中
    │
    ↓ 收到切换到分区 B 的请求
SPMC
    │
    ├─ 保存分区 A 的上下文
    │  ├─ 通用寄存器 (x0-x30)
    │  ├─ 系统寄存器 (SCTLR, TCR, TTBR, 等)
    │  ├─ 栈指针
    │  └─ 程序计数器 (PC)
    │
    ├─ 加载分区 B 的上下文
    │  ├─ 恢复系统寄存器
    │  ├─ 切换页表 (TTBR)
    │  ├─ 恢复通用寄存器
    │  └─ 恢复程序计数器
    │
    ↓
分区 B 运行
```

**关键操作**：
1. **保存当前分区上下文**：将所有 CPU 状态保存到该分区的 `sp_exec_ctx` 结构
2. **切换页表**：加载目标分区的 `TTBR0_EL1`，切换到目标分区的地址空间
3. **恢复目标分区上下文**：从目标分区的 `sp_exec_ctx` 恢复所有 CPU 状态
4. **跳转到目标分区**：恢复程序计数器，继续执行目标分区代码

#### 6.1.9 共享内存的受控访问

虽然分区间内存是隔离的，但可以通过 FF-A 机制安全地共享内存：

1. **显式共享**：通过 `FFA_MEM_SHARE` 等接口显式共享内存
2. **权限控制**：共享内存具有明确的访问权限（读、写、执行）
3. **映射验证**：SPMC 验证所有内存映射请求，确保只有授权的分区可以访问
4. **生命周期管理**：共享内存有明确的所有权和生命周期管理

---

**总结**：多个安全分区通过以上多层隔离机制实现完全隔离。核心是**硬件级别的内存隔离（独立页表 + MMU）**和**软件级别的执行隔离（独立上下文 + 状态管理）**，确保分区之间无法直接访问对方的资源，只能通过 FF-A 标准接口进行受控的通信和资源共享。

### 6.2 多分区支持

- 支持多个独立的安全分区同时运行
- 每个分区可以运行在不同的异常级别（S-EL1 或 S-EL0）
- 支持多核执行（每个分区可以有多个执行上下文）

### 6.3 标准化接口

- 完全符合 FF-A 规范
- 提供标准化的 API，便于跨平台移植
- 支持版本协商和功能查询

### 6.4 灵活的内存模型

- **共享内存（SHARE）**：多方可访问，原所有者可回收
- **出借内存（LEND）**：转移所有权给接收者
- **捐赠内存（DONATE）**：完全转移所有权，原所有者无法访问

---

## 7. 错误处理

### 7.1 错误码定义

| 错误码 | 值 | 说明 |
|--------|-----|------|
| FFA_ERROR_NOT_SUPPORTED | -1 | 不支持的操作 |
| FFA_ERROR_INVALID_PARAMETER | -2 | 无效参数 |
| FFA_ERROR_NO_MEMORY | -3 | 内存不足 |
| FFA_ERROR_BUSY | -4 | 资源忙碌 |
| FFA_ERROR_INTERRUPTED | -5 | 操作被中断 |
| FFA_ERROR_DENIED | -6 | 操作被拒绝 |
| FFA_ERROR_RETRY | -7 | 需要重试 |

### 7.2 错误返回

- 所有 FF-A 接口在出错时返回 `FFA_ERROR` 函数 ID
- 错误码通过寄存器传递（x2 寄存器）
- 调用者需要检查返回值并处理错误情况

---

## 8. 应用场景

### 8.1 Trusted Execution Environment (TEE)
- 提供安全执行环境
- 运行可信应用（Trusted Applications）
- 与 Rich OS 安全通信

### 8.2 安全服务提供
- **加密服务**：密钥管理、加密解密
- **认证服务**：生物识别、密码验证
- **安全存储**：密钥存储、敏感数据保护

### 8.3 多租户安全环境
- 多个相互隔离的安全执行环境
- 不同服务提供商的安全分区
- 云安全服务场景

---

## 9. 与 TSP/TSPD 的关系

### 9.1 演进关系

```
传统架构 (TSP/TSPD)
    │
    │ - 单一 TSP 提供所有服务
    │ - 使用 SMC 接口
    │ - 简单但不够灵活
    ↓
现代架构 (SPM/SPMD + FF-A)
    │
    │ - 多个独立安全分区
    │ - 使用 FF-A 标准化接口
    │ - 更灵活、更安全、更标准化
```

### 9.2 主要区别

| 特性 | TSP/TSPD | SPM/SPMD + FF-A |
|------|----------|----------------|
| 服务组织 | 单一 TSP | 多个独立安全分区 |
| 接口标准 | SMC（实现相关） | FF-A（标准化） |
| 隔离粒度 | 正常世界 vs 安全世界 | 多个相互隔离的分区 |
| 扩展性 | 需要在 TSP 内添加功能 | 可以独立添加新分区 |
| 标准化 | 实现相关 | 完全标准化 |

---

## 10. 总结

ATF 中的 FFA 框架通过 **SPMD** 和 **SPMC** 实现了完整的 FF-A 规范支持：

1. **架构清晰**：SPMD 作为 EL3 的调度器，SPMC 作为核心管理器
2. **灵活配置**：支持 S-EL1、S-EL2 和 EL3 三种运行模式
3. **标准兼容**：完整实现 FF-A v1.2 规范（部分 v1.3 功能）
4. **功能完整**：支持消息传递、内存共享、分区管理等核心功能
5. **安全可靠**：提供严格的安全隔离和多分区管理

FF-A 框架为 ARM 平台提供了标准化的安全分区管理机制，使得不同厂商的实现可以互操作，同时为安全服务的开发和部署提供了强大的支持。

---

## 参考资料

1. [Arm Firmware Framework for Arm A-profile Architecture Specification](https://developer.arm.com/docs/den0077/latest)
2. [ARM Trusted Firmware-A Documentation](https://trustedfirmware-a.readthedocs.io/)
3. [Secure Partition Manager Documentation](https://trustedfirmware-a.readthedocs.io/en/latest/components/secure-partition-manager.html)
4. ATF 源代码：`arm-trusted-firmware/services/std_svc/spmd/`
5. ATF 源代码：`arm-trusted-firmware/services/std_svc/spm/el3_spmc/`
6. ATF 头文件：`arm-trusted-firmware/include/services/ffa_svc.h`

---

*文档创建日期：2025年*
*基于 ATF/TF-A 代码分析*

