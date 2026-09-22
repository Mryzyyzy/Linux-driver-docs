# CCA (Confidential Computing Architecture) 实验室

ARM64 机密计算架构验证与开发平台

## 目录结构

```
cca_lab/
├── README.md              # 本文件
├── docs/                  # 文档目录
│   └── kdump_setup_guide.md  # Kdump 配置指南
├── scripts/               # 脚本目录
│   ├── setup_kdump.sh     # Kdump 自动配置脚本
│   └── test_kdump.sh      # Kdump 测试脚本
├── experiments/           # 实验代码目录
└── notes/                 # 笔记和调试记录
```

## 功能模块

### 1. Kdump 崩溃转储

#### ARM64 平台 (QEMU virt)

**文档**: [docs/kdump_setup_guide.md](docs/kdump_setup_guide.md) | [docs/kdump_qemu_busybox.md](docs/kdump_qemu_busybox.md)

**快速开始**:
```bash
# 配置内核
~/cca_lab/scripts/config_kernel_kdump.sh ~/linux-core/linux-5.10

# 编译 kexec-tools
~/cca_lab/scripts/build_kexec_static.sh

# 启动 QEMU
~/cca_lab/scripts/run_qemu_kdump.sh
```

#### ARM32 平台 (QEMU vexpress-a9)

**文档**: [docs/kdump_vexpress_a9.md](docs/kdump_vexpress_a9.md)

**快速开始**:
```bash
# 配置内核 (ARM 32位)
~/cca_lab/scripts/config_kernel_kdump_arm32.sh ~/linux-core/linux-5.10

# 编译 kexec-tools (ARM 32位)
~/cca_lab/scripts/build_kexec_arm32.sh

# 启动 QEMU vexpress-a9
~/cca_lab/scripts/run_qemu_vexpress_kdump.sh
```

**主要功能**:
- ARM64/ARM32 平台 kdump 配置
- QEMU + BusyBox 环境支持
- 自动检测和配置
- 崩溃转储分析

## 开发计划

- [ ] Kdump 功能验证
- [ ] CCA Realm 管理扩展支持
- [ ] FF-A (Firmware Framework for Arm) 集成
- [ ] 机密计算应用示例

## 参考资料

- [ARM CCA 规范](https://developer.arm.com/documentation/den0125/latest/)
- [Linux Kdump 文档](https://www.kernel.org/doc/html/latest/admin-guide/kdump/kdump.html)
- [FF-A 规范](https://developer.arm.com/documentation/den0077/latest/)
