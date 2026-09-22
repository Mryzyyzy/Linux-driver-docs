# SPMD 如何跳转到多个安全分区（SP）

## 概述

在 ATF 的 FF-A 框架中，**SPMD（Secure Partition Manager Dispatcher）** 并不直接跳转到多个安全分区，而是通过 **SPMC（Secure Partition Manager Core）** 来管理多个安全分区。本文档详细说明从 SPMD 到多个安全分区的跳转机制。

---

## 1. 关键理解

### 1.1 架构层级

```
Normal World (调用者)
    │
    ↓ SMC调用
┌─────────────────────────┐
│   SPMD (EL3)            │  ← FF-A 调用的入口点
│   - 接收 FF-A 调用       │
│   - 转发到 SPMC          │
└─────────────────────────┘
    │
    ↓ 同步进入
┌─────────────────────────┐
│   SPMC (S-EL1/S-EL2/EL3)│  ← 管理多个 SP
│   - 路由消息到目标 SP    │
│   - 管理 SP 上下文切换   │
└─────────────────────────┘
    │
    ↓ 根据目标分区 ID 路由
┌──────────┐ ┌──────────┐ ┌──────────┐
│   SP1    │ │   SP2    │ │   SP3    │
│ (0x8001) │ │ (0x8002) │ │ (0x8003) │
└──────────┘ └──────────┘ └──────────┘
```

### 1.2 核心要点

- **SPMD**：位于 EL3，作为 FF-A 协议的入口点和转发器
- **SPMC**：管理多个安全分区，负责路由和上下文切换
- **SP**：实际的安全服务提供者，可以有多个

**SPMD 不直接跳转到多个 SP，而是跳转到 SPMC，由 SPMC 负责路由到具体的 SP。**

---

## 2. 完整的跳转流程

### 2.1 正常世界发起调用

```
Normal World (例如：Hypervisor 或 OS)
    │
    │ 调用 FFA_MSG_SEND_DIRECT_REQ
    │ 参数：
    │   - x0: FFA_MSG_SEND_DIRECT_REQ (函数 ID)
    │   - x1: (源ID << 16) | 目标ID
    │   - x2-x7: 消息数据
    │
    ↓ SMC指令触发异常到EL3
```

### 2.2 SPMD 接收和处理

**步骤 1：SPMD 接收 SMC 调用**

```c
// spmd_main.c
// SPMD 的 SMC 处理函数（简化）
uint64_t spmd_smc_handler(uint32_t smc_fid, ...)
{
    // 1. 检查是否为 FF-A 调用
    if (is_ffa_fid(smc_fid)) {
        // 2. 判断是否需要 SPMD 自己处理
        if (需要SPMD处理) {
            return spmd_handle_locally(...);
        }
        
        // 3. 转发到 SPMC
        return spmd_smc_forward(smc_fid, ...);
    }
}
```

**步骤 2：SPMD 转发到 SPMC**

```c
// spmd_main.c
static uint64_t spmd_smc_forward(uint32_t smc_fid,
                                 bool secure_origin,
                                 uint64_t x1, uint64_t x2, ...,
                                 void *handle, ...)
{
    spmd_spm_core_context_t *ctx = spmd_get_context();
    gp_regs_t *gpregs = get_gpregs_ctx(&ctx->cpu_ctx);
    
    // 1. 保存当前上下文（Normal World 或 Secure World）
    cm_el2_sysregs_context_save(secure_state_in);
    
    // 2. 设置 SPMC 的上下文
    cm_set_context(&(ctx->cpu_ctx), SECURE);
    
    // 3. 准备传递给 SPMC 的寄存器参数
    write_ctx_reg(gpregs, CTX_GPREG_X0, smc_fid);
    write_ctx_reg(gpregs, CTX_GPREG_X1, x1);
    // ... 设置 x2-x7
    
    // 4. 同步进入 SPMC
    return spmd_spm_core_sync_entry(ctx);
}
```

**步骤 3：SPMD 同步进入 SPMC**

```c
// spmd_main.c
uint64_t spmd_spm_core_sync_entry(spmd_spm_core_context_t *spmc_ctx)
{
    // 1. 设置 SPMC 的 CPU 上下文
    cm_set_context(&(spmc_ctx->cpu_ctx), SECURE);
    
    // 2. 恢复 SPMC 的系统寄存器
    cm_el2_sysregs_context_restore(SECURE);  // S-EL2 SPMC
    // 或
    cm_el1_sysregs_context_restore(SECURE);  // S-EL1/EL3 SPMC
    
    // 3. 设置下一个异常返回上下文为 SECURE
    cm_set_next_eret_context(SECURE);
    
    // 4. 进入 SPMC（这里会发生实际的异常级别切换）
    rc = spmd_spm_core_enter(&spmc_ctx->c_rt_ctx);
    
    // 5. 从 SPMC 返回后，保存 SPMC 状态
    cm_el2_sysregs_context_save(SECURE);
    
    return rc;  // 返回 SPMC 的处理结果
}
```

### 2.3 SPMC 接收和路由

**步骤 4：SPMC 处理 FF-A 调用**

SPMC 接收到来自 SPMD 的调用后，根据 FF-A 函数 ID 进行路由：

```c
// spmc_main.c (简化)
uint64_t spmc_smc_handler(uint32_t smc_fid, ...)
{
    switch (GET_SMC_NUM(smc_fid)) {
        case FFA_FNUM_MSG_SEND_DIRECT_REQ:
        case FFA_FNUM_MSG_SEND_DIRECT_REQ2:
            // 处理直接消息请求
            return direct_req_smc_handler(smc_fid, ...);
            
        case FFA_FNUM_MEM_SHARE:
            // 处理内存共享
            return mem_share_handler(...);
            
        // ... 其他 FF-A 接口
    }
}
```

**步骤 5：SPMC 路由到目标分区**

```c
// spmc_main.c
static uint64_t direct_req_smc_handler(uint32_t smc_fid,
                                       bool secure_origin,
                                       uint64_t x1, uint64_t x2, ...,
                                       void *handle, ...)
{
    // 1. 从 x1 寄存器提取源 ID 和目标 ID
    uint16_t src_id = ffa_endpoint_source(x1);  // 发送方 ID
    uint16_t dst_id = ffa_endpoint_destination(x1);  // 接收方 ID
    
    // 2. 根据目标 ID 查找对应的安全分区
    struct secure_partition_desc *sp = spmc_get_sp_ctx(dst_id);
    
    if (sp == NULL) {
        // 目标分区不存在
        return spmc_ffa_error_return(handle, FFA_ERROR_INVALID_PARAMETER);
    }
    
    // 3. 验证分区是否可以接收直接消息
    if (!direct_msg_receivable(sp->properties, dir_req_funcid)) {
        return spmc_ffa_error_return(handle, FFA_ERROR_DENIED);
    }
    
    // 4. 获取分区的执行上下文
    struct sp_exec_ctx *ec = spmc_get_sp_ec(sp);
    
    // 5. 切换到目标安全分区的上下文
    return spmc_sp_synchronous_entry(ec, ...);
}
```

**步骤 6：SPMC 根据分区 ID 查找 SP**

```c
// spmc_main.c
struct secure_partition_desc *spmc_get_sp_ctx(uint16_t id)
{
    // 遍历所有安全分区描述符数组
    for (unsigned int i = 0U; i < SECURE_PARTITION_COUNT; i++) {
        if (sp_desc[i].sp_id == id) {
            // 找到匹配的分区 ID，返回分区描述符
            return &(sp_desc[i]);
        }
    }
    return NULL;  // 未找到
}
```

**分区描述符数组**：

```c
// spmc_main.c
// 静态分配的安全分区描述符数组
static struct secure_partition_desc sp_desc[SECURE_PARTITION_COUNT];

// 每个分区在初始化时被分配一个唯一的 ID
// 例如：
// sp_desc[0].sp_id = 0x8001;  // SP1
// sp_desc[1].sp_id = 0x8002;  // SP2
// sp_desc[2].sp_id = 0x8003;  // SP3
```

**步骤 7：SPMC 切换到目标 SP**

```c
// spm_common.c (简化)
uint64_t spmc_sp_synchronous_entry(struct sp_exec_ctx *ec, ...)
{
    cpu_context_t *ctx = &ec->cpu_ctx;
    
    // 1. 保存当前上下文（可能是另一个 SP 或 SPMC）
    
    // 2. 设置目标 SP 的上下文
    cm_set_context(ctx, SECURE);
    
    // 3. 恢复目标 SP 的系统寄存器
    cm_el1_sysregs_context_restore(SECURE);
    
    // 4. 切换页表（TTBR0_EL1）到目标 SP 的页表
    // 这是内存隔离的关键！
    write_ctx_reg(get_el1_sysregs_ctx(ctx), CTX_TTBR0_EL1,
                  sp->xlat_ctx_handle->base_table);
    
    // 5. 设置下一个异常返回上下文
    cm_set_next_eret_context(SECURE);
    
    // 6. 进入目标 SP（异常级别切换）
    rc = spm_secure_partition_enter(ec->c_rt_ctx, ...);
    
    // 7. 从 SP 返回后，保存 SP 状态
    cm_el1_sysregs_context_save(SECURE);
    
    return rc;
}
```

### 2.4 安全分区处理

**步骤 8：目标 SP 接收和处理**

目标安全分区被调度执行，从寄存器中读取消息参数并处理：

```c
// 在安全分区内部（简化示例）
void sp_entry_point(void)
{
    // 从寄存器读取 FF-A 调用参数
    uint32_t func_id = read_register(X0);
    uint64_t arg1 = read_register(X1);
    // ...
    
    if (func_id == FFA_MSG_SEND_DIRECT_REQ) {
        // 处理直接消息请求
        handle_direct_request(arg1, ...);
        
        // 发送响应
        ffa_msg_send_direct_resp(...);
    }
}
```

### 2.5 返回路径

**步骤 9-12：返回到调用者**

处理完成后，按照相反的顺序返回：

```
SP → SPMC → SPMD → Normal World
```

---

## 3. 关键代码路径总结

### 3.1 从 Normal World 到 SPMD

```
Normal World
    ↓ SMC 指令
EL3 异常处理
    ↓
spmd_smc_handler()
    ↓
spmd_smc_forward()
```

### 3.2 从 SPMD 到 SPMC

```
spmd_smc_forward()
    ↓
spmd_spm_core_sync_entry()
    ↓
spmd_spm_core_enter()  // 实际的异常级别切换
    ↓
SPMC (S-EL1/S-EL2/EL3)
```

### 3.3 从 SPMC 到具体 SP

```
SPMC 的 SMC 处理函数
    ↓
direct_req_smc_handler()
    ↓
spmc_get_sp_ctx(dst_id)  // 根据目标 ID 查找 SP
    ↓
spmc_sp_synchronous_entry()
    ↓
spm_secure_partition_enter()  // 切换到目标 SP
    ↓
目标 SP (S-EL1/S-EL0)
```

---

## 4. 多分区路由的关键机制

### 4.1 分区 ID 到分区的映射

SPMC 维护一个分区描述符数组，通过分区 ID 查找：

```c
// 分区描述符数组（在 SPMC 初始化时填充）
static struct secure_partition_desc sp_desc[SECURE_PARTITION_COUNT];

// 查找函数
struct secure_partition_desc *spmc_get_sp_ctx(uint16_t id)
{
    for (unsigned int i = 0U; i < SECURE_PARTITION_COUNT; i++) {
        if (sp_desc[i].sp_id == id) {
            return &(sp_desc[i]);  // 找到目标分区
        }
    }
    return NULL;
}
```

**分区 ID 分配**：
- 0x8001 - SP1
- 0x8002 - SP2
- 0x8003 - SP3
- ...

### 4.2 消息路由逻辑

```c
// 从 FF-A 消息中提取目标分区 ID
uint16_t dst_id = ffa_endpoint_destination(x1);

// 根据 ID 查找分区
struct secure_partition_desc *target_sp = spmc_get_sp_ctx(dst_id);

// 切换到目标分区
spmc_sp_synchronous_entry(target_sp->ec[...], ...);
```

### 4.3 上下文切换机制

每次切换到不同的 SP 时：

1. **保存当前上下文**：寄存器、系统寄存器、页表等
2. **加载目标 SP 上下文**：从目标 SP 的描述符中恢复
3. **切换页表**：切换到目标 SP 的独立页表（内存隔离的关键）
4. **跳转执行**：异常返回（ERET）到目标 SP

---

## 5. 完整的调用链示例

### 5.1 示例：Normal World 调用 SP2

```
1. Normal World (ID: 0x0000)
   │
   │ FFA_MSG_SEND_DIRECT_REQ
   │ x1 = (0x0000 << 16) | 0x8002  // 目标：SP2
   │
   ↓ SMC #0

2. EL3 异常处理
   │
   ↓

3. SPMD (EL3)
   │
   │ spmd_smc_handler()
   │   → 识别为 FF-A 调用
   │   → spmd_smc_forward()
   │   → spmd_spm_core_sync_entry()
   │   → 保存 Normal World 上下文
   │   → 恢复 SPMC 上下文
   │   → ERET 到 SPMC
   │
   ↓ ERET (EL3 → S-EL2)

4. SPMC (S-EL2)
   │
   │ spmc_smc_handler()
   │   → direct_req_smc_handler()
   │   → 提取 dst_id = 0x8002
   │   → spmc_get_sp_ctx(0x8002)
   │   → 找到 sp_desc[1] (SP2)
   │   → spmc_sp_synchronous_entry()
   │   → 保存 SPMC 上下文
   │   → 恢复 SP2 上下文
   │   → 切换页表到 SP2
   │   → ERET 到 SP2
   │
   ↓ ERET (S-EL2 → S-EL1)

5. SP2 (S-EL1, ID: 0x8002)
   │
   │ 从寄存器读取消息参数
   │ 处理服务请求
   │ 调用 FFA_MSG_SEND_DIRECT_RESP
   │
   ↓ SMC #0

6. SPMC
   │
   │ 处理响应
   │ 切换回调用者上下文（Normal World）
   │
   ↓

7. SPMD
   │
   │ 返回到 Normal World
   │
   ↓

8. Normal World
   │
   │ 收到响应
```

---

## 6. 关键数据结构

### 6.1 SPMD 上下文

```c
// spmd_private.h
typedef struct spmd_spm_core_context {
    cpu_context_t cpu_ctx;        // SPMC 的 CPU 上下文
    void *c_rt_ctx;               // C 运行时上下文
    uint32_t state;               // SPMC 状态
    bool secure_interrupt_ongoing;
} spmd_spm_core_context_t;
```

### 6.2 安全分区描述符

```c
// spmc.h
struct secure_partition_desc {
    struct sp_exec_ctx ec[PLATFORM_CORE_COUNT];  // 执行上下文数组
    uint16_t sp_id;                              // 分区 ID（用于路由）
    uint32_t uuid[4];                            // UUID
    enum sp_runtime_el runtime_el;               // 异常级别
    xlat_ctx_t *xlat_ctx_handle;                // 页表上下文（内存隔离）
    struct mailbox mailbox;                      // 邮箱
    // ...
};
```

### 6.3 执行上下文

```c
// spmc.h
struct sp_exec_ctx {
    uint64_t c_rt_ctx;           // C 运行时上下文
    cpu_context_t cpu_ctx;       // CPU 架构上下文（寄存器等）
    enum sp_runtime_states rt_state;
    enum sp_runtime_model rt_model;
    // ...
};
```

---

## 7. 总结

### 7.1 关键要点

1. **SPMD 不直接跳转到多个 SP**
   - SPMD 只跳转到 SPMC
   - SPMC 负责管理多个 SP 并路由消息

2. **路由机制**
   - 通过分区 ID（0x8001, 0x8002, ...）标识不同的 SP
   - SPMC 维护分区描述符数组，通过 ID 查找
   - 每个消息包含目标分区 ID，用于路由

3. **上下文切换**
   - 每次切换都涉及完整的上下文保存和恢复
   - 关键操作：切换页表（TTBR0_EL1），实现内存隔离
   - 通过异常返回（ERET）实现异常级别切换

4. **隔离保证**
   - 每个 SP 有独立的页表，硬件强制内存隔离
   - 每个 SP 有独立的执行上下文，软件保证执行隔离
   - SPMC 负责所有切换，确保隔离不被破坏

### 7.2 调用链

```
Normal World
    ↓ SMC
SPMD (EL3) ──→ 转发所有 FF-A 调用
    ↓ 同步进入
SPMC (S-EL1/S-EL2/EL3) ──→ 根据分区 ID 路由
    ↓ 上下文切换
目标 SP (S-EL1/S-EL0) ──→ 处理服务请求
```

---

## 参考资料

1. ATF 源代码：
   - `services/std_svc/spmd/spmd_main.c`
   - `services/std_svc/spm/el3_spmc/spmc_main.c`
   - `services/std_svc/spm/common/spm_common.c`

2. ARM FF-A 规范文档

3. ATF 文档：Secure Partition Manager

---

*文档创建日期：2025年*

