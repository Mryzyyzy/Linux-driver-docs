# SPMD、SPMC、SPM、SP 关系说明

## 概述

在 ARM Trusted Firmware (ATF) 的 FF-A 框架中，SPMD、SPMC、SPM、SP 是四个相关但不同的概念。本文档详细说明它们之间的关系和区别。

---

## 1. 术语定义

### 1.1 SPM（Secure Partition Manager）

**全称**：Secure Partition Manager（安全分区管理器）

**定义**：
- **SPM 是一个通用术语**，指代整个安全分区管理系统
- 包括两个主要组件：**SPMD** 和 **SPMC**
- 也可以指代传统的、基于 MM 规范的安全分区管理器实现

**角色**：
- 管理安全分区（Secure Partition）的完整系统
- 提供安全分区的生命周期管理、资源分配、消息路由等功能

**位置**：
- 不是一个单独的组件，而是 SPMD + SPMC 的组合

### 1.2 SPMD（Secure Partition Manager Dispatcher）

**全称**：Secure Partition Manager Dispatcher（安全分区管理器调度器/分发器）

**定义**：
- SPM 的一个核心组件，负责**调度和分发**工作
- 位于 EL3，作为 FF-A 协议的中转站

**主要职责**：
1. **接收 FF-A 调用**：接收来自 Normal World 的 FF-A SMC 调用
2. **转发请求**：将请求转发到 SPMC 进行处理
3. **上下文切换管理**：管理 Normal World 和 Secure World 之间的上下文切换
4. **协议中转**：作为 FF-A 协议的中转站，不直接管理安全分区

**运行位置**：
- **EL3**（Exception Level 3，最高特权级别）

**特点**：
- 通常系统中只有一个 SPMD
- 必须运行在 EL3
- 是 ATF 的一部分（BL31）

### 1.3 SPMC（Secure Partition Manager Core）

**全称**：Secure Partition Manager Core（安全分区管理器核心）

**定义**：
- SPM 的另一个核心组件，负责**实际的管理工作**
- 实现 FF-A 规范的核心功能

**主要职责**：
1. **管理安全分区**：管理多个安全分区的生命周期（创建、初始化、运行、销毁）
2. **消息路由**：根据目标分区 ID 将消息路由到正确的安全分区
3. **内存管理**：管理安全分区的内存分配、共享、隔离
4. **上下文切换**：在多个安全分区之间切换执行上下文
5. **实现 FF-A 接口**：实现 FF-A 规范定义的所有接口

**运行位置**：
- **S-EL1**（当不支持 FEAT_SEL2 时）
- **S-EL2**（当支持 FEAT_SEL2 时，推荐）
- **EL3**（特殊配置，通常只支持单个分区）

**特点**：
- 系统中只有一个 SPMC
- 可以运行在不同的异常级别（取决于硬件支持）
- 可以是 ATF 的一部分（EL3 SPMC）或独立镜像（S-EL2 SPMC，如 Hafnium）

### 1.4 SP（Secure Partition）

**全称**：Secure Partition（安全分区）

**定义**：
- 实际提供安全服务的执行实体
- 运行在隔离环境中的独立安全服务提供者

**主要职责**：
1. **提供安全服务**：如加密、认证、密钥管理等
2. **实现业务逻辑**：实现特定的安全功能
3. **响应服务请求**：通过 FF-A 接口响应来自其他分区或 Normal World 的请求

**运行位置**：
- **S-EL1**（当 SPMC 在 S-EL2 时）
- **S-EL0**（当 SPMC 在 S-EL2 时，可选的更安全但受限的模式）
- **S-EL1**（当 SPMC 在 EL3 时）

**特点**：
- 系统中可以有**多个 SP**
- 每个 SP 运行在完全隔离的环境中
- 通过 FF-A 接口与其他组件通信

---

## 2. 关系图解

### 2.1 层次关系

```
┌─────────────────────────────────────────────────┐
│              SPM（安全分区管理器）                │
│          （SPM = SPMD + SPMC）                   │
└─────────────────────────────────────────────────┘
                      │
        ┌─────────────┴─────────────┐
        ↓                           ↓
┌──────────────────┐        ┌──────────────────┐
│   SPMD (EL3)     │        │   SPMC           │
│  （调度器/分发器） │        │  (S-EL1/S-EL2/EL3)│
│                  │        │  （核心管理器）   │
│  - 接收FF-A调用  │        │                  │
│  - 转发到SPMC    │←──────→│  - 管理多个SP    │
│  - 上下文切换    │        │  - 消息路由      │
└──────────────────┘        │  - 内存管理      │
                            └──────────────────┘
                                    │
                    ┌───────────────┼───────────────┐
                    ↓               ↓               ↓
            ┌──────────┐    ┌──────────┐    ┌──────────┐
            │   SP1    │    │   SP2    │    │   SP3    │
            │ (安全分区) │    │ (安全分区) │    │ (安全分区) │
            │          │    │          │    │          │
            │ 提供安全 │    │ 提供安全 │    │ 提供安全 │
            │ 服务1    │    │ 服务2    │    │ 服务3    │
            └──────────┘    └──────────┘    └──────────┘
```

### 2.2 架构视图

```
┌─────────────────────────────────────────────┐
│         Normal World                        │
│  (Hypervisor / OS Kernel / Applications)    │
└─────────────────────────────────────────────┘
                    │
                    │ FF-A SMC 调用
                    ↓
┌─────────────────────────────────────────────┐
│              EL3                            │
│  ┌──────────────────────────────────────┐  │
│  │         SPMD                         │  │
│  │  - 接收并转发 FF-A 调用               │  │
│  │  - 管理 Normal ↔ Secure 切换          │  │
│  └──────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
                    │
                    │ 转发请求
                    ↓
┌─────────────────────────────────────────────┐
│         S-EL2 (或 S-EL1/EL3)               │
│  ┌──────────────────────────────────────┐  │
│  │         SPMC                         │  │
│  │  - 管理多个 SP                        │  │
│  │  - 路由消息到目标 SP                  │  │
│  │  - 管理内存和隔离                     │  │
│  └──────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
                    │
        ┌───────────┼───────────┐
        ↓           ↓           ↓
┌───────────┐ ┌───────────┐ ┌───────────┐
│   S-EL1   │ │   S-EL1   │ │   S-EL0   │
│    SP1    │ │    SP2    │ │    SP3    │
│ (加密服务) │ │ (认证服务) │ │(密钥管理) │
└───────────┘ └───────────┘ └───────────┘
```

---

## 3. 详细关系说明

### 3.1 SPM 与 SPMD、SPMC 的关系

**SPM = SPMD + SPMC**

- **SPM** 是整体系统的总称
- **SPMD** 和 **SPMC** 是 SPM 的两个核心组件
- 它们共同实现安全分区管理的完整功能

**分工**：
- **SPMD**：负责调度和分发（Dispatcher）
- **SPMC**：负责核心管理（Core/Manager）

### 3.2 SPMD 与 SPMC 的关系

**SPMD ←→ SPMC：协作关系**

1. **SPMD 依赖 SPMC**：
   - SPMD 将 FF-A 调用转发给 SPMC
   - SPMD 不直接管理安全分区，而是委托给 SPMC

2. **SPMC 依赖 SPMD**：
   - SPMC 通过 SPMD 接收来自 Normal World 的调用
   - SPMC 通过 SPMD 返回结果给 Normal World

3. **通信方式**：
   - 通过同步进入（`spmd_spm_core_sync_entry`）
   - 通过 FF-A 直接消息接口
   - 通过异常级别切换（EL3 ↔ S-EL2/S-EL1）

### 3.3 SPMC 与 SP 的关系

**SPMC → SP：管理关系**

1. **一对多关系**：
   - 一个 SPMC 管理多个 SP
   - SPMC 维护所有 SP 的描述符数组

2. **管理职责**：
   - **生命周期管理**：创建、初始化、运行、销毁 SP
   - **消息路由**：根据目标分区 ID 路由消息到正确的 SP
   - **资源管理**：分配内存、管理隔离
   - **上下文切换**：在多个 SP 之间切换执行上下文

3. **通信方式**：
   - 通过 FF-A 接口（`FFA_MSG_SEND_DIRECT_REQ` 等）
   - 通过上下文切换进入 SP
   - SP 通过 FF-A 接口返回结果

### 3.4 SP 之间的关系

**SP ←→ SP：对等关系（通过 SPMC 中介）**

1. **相互隔离**：
   - 每个 SP 运行在独立的地址空间中
   - 通过独立的页表实现内存隔离
   - 不能直接访问其他 SP 的内存

2. **通信方式**：
   - 通过 FF-A 接口进行通信
   - 消息通过 SPMC 路由
   - 可以共享内存（通过 `FFA_MEM_SHARE`）

3. **间接关系**：
   - SP 之间不直接通信
   - 所有通信都通过 SPMC 进行路由和管理

---

## 4. 数据流和调用链

### 4.1 正常世界调用安全分区

```
Normal World
    │
    │ 1. FFA_MSG_SEND_DIRECT_REQ (目标: SP2)
    ↓
SPMD (EL3)
    │
    │ 2. 接收 SMC 调用
    │ 3. 转发到 SPMC
    ↓
SPMC (S-EL2)
    │
    │ 4. 提取目标分区 ID (0x8002)
    │ 5. 查找 SP2: spmc_get_sp_ctx(0x8002)
    │ 6. 切换到 SP2 的上下文
    ↓
SP2 (S-EL1)
    │
    │ 7. 处理服务请求
    │ 8. 返回结果
    ↓
SPMC
    │
    │ 9. 路由结果
    ↓
SPMD
    │
    │ 10. 返回给 Normal World
    ↓
Normal World
```

### 4.2 SP 之间的通信

```
SP1
    │
    │ 1. FFA_MSG_SEND_DIRECT_REQ (目标: SP2)
    ↓
SPMC (S-EL2)
    │
    │ 2. 接收来自 SP1 的请求
    │ 3. 查找目标 SP2
    │ 4. 切换到 SP2 的上下文
    ↓
SP2 (S-EL1)
    │
    │ 5. 处理请求
    │ 6. 返回响应
    ↓
SPMC
    │
    │ 7. 切换回 SP1
    ↓
SP1
```

---

## 5. 代码中的体现

### 5.1 SPMD 的实现

```c
// services/std_svc/spmd/spmd_main.c

// SPMD 的 SMC 处理函数
uint64_t spmd_smc_handler(uint32_t smc_fid, ...)
{
    // 接收 FF-A 调用
    if (is_ffa_fid(smc_fid)) {
        // 转发到 SPMC
        return spmd_smc_forward(smc_fid, ...);
    }
}

// 转发到 SPMC
static uint64_t spmd_smc_forward(...)
{
    // 切换到 SPMC
    return spmd_spm_core_sync_entry(ctx);
}
```

### 5.2 SPMC 的实现

```c
// services/std_svc/spm/el3_spmc/spmc_main.c

// 安全分区描述符数组
static struct secure_partition_desc sp_desc[SECURE_PARTITION_COUNT];

// 根据分区 ID 查找 SP
struct secure_partition_desc *spmc_get_sp_ctx(uint16_t id)
{
    for (unsigned int i = 0U; i < SECURE_PARTITION_COUNT; i++) {
        if (sp_desc[i].sp_id == id) {
            return &(sp_desc[i]);  // 找到目标 SP
        }
    }
    return NULL;
}

// 路由消息到目标 SP
static uint64_t direct_req_smc_handler(...)
{
    uint16_t dst_id = ffa_endpoint_destination(x1);
    struct secure_partition_desc *sp = spmc_get_sp_ctx(dst_id);
    
    // 切换到目标 SP
    return spmc_sp_synchronous_entry(sp->ec[...], ...);
}
```

---

## 6. 关键区别总结

| 组件 | 全称 | 位置 | 数量 | 主要职责 | 直接管理SP？ |
|------|------|------|------|----------|-------------|
| **SPM** | Secure Partition Manager | N/A（概念） | N/A | 安全分区管理系统的总称 | N/A |
| **SPMD** | Secure Partition Manager Dispatcher | EL3 | 1个 | 调度、分发、转发FF-A调用 | ❌ 否 |
| **SPMC** | Secure Partition Manager Core | S-EL1/S-EL2/EL3 | 1个 | 管理多个SP、路由消息、内存管理 | ✅ 是 |
| **SP** | Secure Partition | S-EL1/S-EL0 | 多个 | 提供安全服务 | ❌ 否 |

---

## 7. 类比理解

为了更好理解，可以用以下类比：

### 7.1 公司组织架构类比

- **SPM** = 整个管理部门（总称）
- **SPMD** = 前台/接待处（EL3，接收外部请求，转发到内部）
- **SPMC** = 部门经理（S-EL2，管理多个员工）
- **SP** = 员工（S-EL1，实际工作的人）

### 7.2 操作系统类比

- **SPM** = 操作系统内核（总称）
- **SPMD** = 系统调用接口（接收用户空间调用，转发到内核）
- **SPMC** = 进程调度器（管理多个进程）
- **SP** = 用户进程（实际运行的应用程序）

### 7.3 路由器网络类比

- **SPM** = 整个网络管理系统
- **SPMD** = 边界路由器（接收外部流量，转发到内部网络）
- **SPMC** = 核心交换机（管理多个终端，路由数据包）
- **SP** = 终端设备（提供服务的服务器）

---

## 8. 总结

### 8.1 核心关系

1. **SPM** 是总称，包含 **SPMD** 和 **SPMC**
2. **SPMD** 和 **SPMC** 是协作关系，共同实现 SPM 功能
3. **SPMC** 管理多个 **SP**（一对多关系）
4. **SP** 之间相互隔离，通过 SPMC 进行通信

### 8.2 数据流方向

```
Normal World
    ↓ (FF-A调用)
SPMD (EL3) ──── 转发 ────→ SPMC (S-EL2)
                                ↓ (路由)
                            SP1, SP2, SP3, ...
```

### 8.3 关键要点

- **SPMD**：不直接管理SP，只负责转发和调度
- **SPMC**：直接管理多个SP，是实际的管理者
- **SP**：被管理的对象，提供安全服务
- **SPM**：整个系统的总称

---

## 参考资料

1. ATF 源代码：
   - `services/std_svc/spmd/spmd_main.c`
   - `services/std_svc/spm/el3_spmc/spmc_main.c`

2. ARM FF-A 规范文档

3. ATF 文档：Secure Partition Manager

---

*文档创建日期：2025年*

