# FF-A 框架文档目录

本目录包含 ARM Trusted Firmware (ATF) 中 FF-A（Firmware Framework for Arm）框架的相关文档。

## 文档列表

### 1. ATF_FFA框架总结.md
ATF 中 FFA 框架的全面总结，包括：
- 架构组件（SPMD、SPMC、SP）
- FF-A 接口说明
- 工作流程
- 安全隔离机制
- 关键特性

### 2. SPM_SPMD_FFA关系分析.md
分析 SPM、SPMD 和 FF-A 之间的关系：
- 基本概念定义
- 层次关系
- 协作关系
- 工作流程
- 与 TSP/TSPD 的对比

### 3. SPMD_SPMC_SPM_SP关系说明.md
详细说明 SPMD、SPMC、SPM、SP 四个概念的关系：
- 术语定义
- 关系图解
- 数据流和调用链
- 代码体现
- 类比理解

### 4. SPMD跳转到多个SP的机制.md
说明 SPMD 如何跳转到多个安全分区：
- 完整的跳转流程
- 关键代码路径
- 多分区路由机制
- 上下文切换机制
- 调用链示例

### 5. 创建多个安全分区指南.md
如何创建和管理多个安全分区的详细指南：
- 前提条件
- 创建步骤
- 配置示例
- 最佳实践

### 6. ATF中SP的限制和实际情况.md
澄清 ATF 中安全分区的实际限制：
- SP 不是嵌入在 ATF 中的
- ATF EL3 SPMC 只支持单个 SP
- 多 SP 支持需要 S-EL2 SPMC（如 Hafnium）
- 实际开发场景

## 文档关系

这些文档从不同角度描述了 FF-A 框架：

1. **入门**：从 `ATF_FFA框架总结.md` 开始，了解整体框架
2. **概念理解**：通过 `SPMD_SPMC_SPM_SP关系说明.md` 理解各个组件的关系
3. **深入机制**：`SPMD跳转到多个SP的机制.md` 说明具体实现机制
4. **实践指南**：`创建多个安全分区指南.md` 提供实际操作指南
5. **限制说明**：`ATF中SP的限制和实际情况.md` 澄清实际限制

## 相关资源

- ARM FF-A 规范：https://developer.arm.com/docs/den0077/latest
- ATF 官方文档：https://trustedfirmware-a.readthedocs.io/
- Hafnium 项目：https://hafnium.readthedocs.io/

---

*最后更新：2025年*

