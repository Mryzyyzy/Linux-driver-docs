# ARM64 Kdump 配置与使用指南

## 1. Kdump 概述

**Kdump** 是 Linux 内核崩溃转储机制，用于在系统崩溃时自动保存内核内存快照（vmcore），以便后续分析崩溃原因。

### 工作原理

1. **主内核（Primary Kernel）**：正常运行的内核
2. **捕获内核（Capture Kernel）**：预留内存中加载的小内核，用于崩溃时启动
3. **崩溃流程**：
   - 主内核崩溃 → 触发 kexec 机制 → 启动捕获内核 → 保存 vmcore → 重启系统

---

## 2. ARM64 平台 Kdump 配置步骤

### 2.1 检查系统支持

```bash
# 检查内核是否支持 kexec
grep KEXEC /boot/config-$(uname -r) 2>/dev/null || \
grep CONFIG_KEXEC /proc/config.gz 2>/dev/null | gunzip

# 检查是否支持 kdump
grep KDUMP /boot/config-$(uname -r) 2>/dev/null || \
grep CONFIG_CRASH_DUMP /proc/config.gz 2>/dev/null | gunzip

# 检查内核是否支持 crashkernel 参数
cat /proc/cmdline | grep crashkernel
```

### 2.2 内核配置要求

确保内核编译时启用了以下选项：

```
CONFIG_KEXEC=y
CONFIG_CRASH_DUMP=y
CONFIG_PROC_VMCORE=y
CONFIG_CRASH_DUMP=y
CONFIG_PHYSICAL_START=0x80000000  # 根据你的平台调整
```

### 2.3 预留崩溃内存（crashkernel）

在启动参数中添加 `crashkernel` 参数，为捕获内核预留内存。

#### 方法 1: 修改 GRUB 配置（UEFI 启动）

编辑 `/etc/default/grub`：

```bash
GRUB_CMDLINE_LINUX_DEFAULT="crashkernel=256M"
```

或者更精确的配置（推荐）：

```bash
# 为 ARM64 平台预留内存（格式：大小@偏移）
GRUB_CMDLINE_LINUX_DEFAULT="crashkernel=256M@512M"
```

更新 GRUB：

```bash
sudo update-grub
```

#### 方法 2: 修改设备树（Device Tree Boot）

在设备树中添加：

```dts
chosen {
    bootargs = "crashkernel=256M@512M ...";
};
```

#### 方法 3: U-Boot 命令行

```bash
setenv bootargs "crashkernel=256M@512M ..."
saveenv
boot
```

### 2.4 安装必要工具

```bash
# Ubuntu/Debian
sudo apt-get install kexec-tools crash

# 或者从源码编译
git clone https://github.com/horms/kexec-tools.git
cd kexec-tools
./bootstrap
./configure --target=aarch64-linux-gnu
make
sudo make install
```

### 2.5 配置 Kdump

#### 创建 kdump 配置文件

编辑 `/etc/default/kdump-tools`：

```bash
USE_KDUMP=1
KDUMP_SYSCTL="kernel.panic_on_oops=1"
```

#### 配置 kdump 服务

编辑 `/etc/kdump.conf`：

```bash
# 转储目标：本地文件系统
path /var/crash
core_collector makedumpfile -c --message-level 1 -d 31
# 或者使用简单的 cp 命令
# core_collector cp

# 转储格式：压缩的 ELF 格式
# 或者使用 vmcore-dmesg 保存 dmesg
```

### 2.6 准备捕获内核

#### 方法 1: 使用主内核（简单但占用内存多）

```bash
# 使用当前运行的内核作为捕获内核
sudo kexec -p /boot/vmlinuz-$(uname -r) \
    --initrd=/boot/initrd.img-$(uname -r) \
    --append="$(cat /proc/cmdline) nr_cpus=1 reset_devices"
```

#### 方法 2: 编译专用捕获内核（推荐）

编译一个精简的内核作为捕获内核：

```bash
# 1. 获取内核源码
cd ~/linux-core/linux-6.12.34

# 2. 配置最小内核（只包含必要的驱动）
make ARCH=arm64 defconfig
make ARCH=arm64 menuconfig
# 启用: KEXEC, CRASH_DUMP, PROC_VMCORE 等

# 3. 编译
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)

# 4. 安装捕获内核
sudo cp arch/arm64/boot/Image /boot/vmlinuz-crash
```

### 2.7 启动 Kdump 服务

```bash
# 启用服务
sudo systemctl enable kdump-tools

# 启动服务
sudo systemctl start kdump-tools

# 检查状态
sudo systemctl status kdump-tools

# 验证 kexec 是否加载
cat /sys/kernel/kexec_crash_loaded
# 应该输出: 1
```

---

## 3. 测试 Kdump

### 3.1 手动触发崩溃（测试用）

⚠️ **警告：这会导致系统崩溃，只在测试环境使用！**

```bash
# 方法 1: 触发内核 panic
echo c | sudo tee /proc/sysrq-trigger

# 方法 2: 触发空指针解引用（内核模块）
# 需要加载测试模块

# 方法 3: 使用 crash 工具模拟
```

### 3.2 检查转储文件

崩溃后系统重启，检查转储文件：

```bash
# 查看转储目录
ls -lh /var/crash/

# 应该看到类似文件：
# vmcore
# vmcore-dmesg.txt
```

---

## 4. 分析崩溃转储

### 4.1 使用 crash 工具

```bash
# 安装 crash 工具
sudo apt-get install crash

# 分析 vmcore
crash /usr/lib/debug/boot/vmlinux-$(uname -r) /var/crash/vmcore

# 或者使用带符号的内核
crash vmlinux /var/crash/vmcore
```

### 4.2 crash 常用命令

```bash
# 进入 crash 后
crash> bt          # 查看崩溃时的调用栈
crash> ps          # 查看进程列表
crash> log         # 查看内核日志
crash> kmem        # 查看内存信息
crash> task        # 查看任务信息
crash> help        # 查看帮助
```

---

## 5. ARM64 特殊注意事项

### 5.1 内存布局

ARM64 平台的内存布局需要考虑：

```
正常内存区域: 0x80000000 - 0x80000000 + RAM_SIZE
预留区域:     0x80000000 + crashkernel_offset - 0x80000000 + crashkernel_offset + crashkernel_size
```

### 5.2 设备树支持

如果使用设备树启动，需要确保：

1. **保留内存节点**：
```dts
reserved-memory {
    #address-cells = <2>;
    #size-cells = <2>;
    ranges;

    crashkernel@512M {
        reg = <0x0 0x20000000 0x0 0x10000000>; /* 256MB @ 512MB */
        no-map;
    };
};
```

2. **启动参数**：
```dts
chosen {
    bootargs = "crashkernel=256M@512M ...";
};
```

### 5.3 调试内核地址

ARM64 内核通常加载在：

- **物理地址**: `0x80080000` (Image) 或 `0x80000000` (zImage)
- **虚拟地址**: `0xffff800008000000` (KASLR 可能偏移)

检查方法：

```bash
# 查看内核加载地址
cat /proc/iomem | grep "Kernel code"
cat /proc/iomem | grep "Kernel data"

# 查看内核符号
cat /proc/kallsyms | head -5
```

---

## 6. 故障排查

### 6.1 Kdump 未启动

```bash
# 检查 kexec 是否加载
cat /sys/kernel/kexec_crash_loaded
# 输出 0 表示未加载

# 检查预留内存
dmesg | grep -i crash
cat /proc/iomem | grep -i crash

# 检查服务日志
sudo journalctl -u kdump-tools -n 50
```

### 6.2 内存不足

```bash
# 检查可用内存
free -h

# 检查 crashkernel 预留
cat /proc/cmdline | grep crashkernel

# 如果内存不足，减少 crashkernel 大小
# 例如：crashkernel=128M@512M
```

### 6.3 捕获内核启动失败

```bash
# 检查捕获内核文件是否存在
ls -lh /boot/vmlinuz-* /boot/initrd.img-*

# 检查 kexec 加载状态
sudo kexec -l /boot/vmlinuz-$(uname -r) \
    --initrd=/boot/initrd.img-$(uname -r) \
    --append="$(cat /proc/cmdline) nr_cpus=1 reset_devices" -v

# 查看详细错误
dmesg | tail -50
```

---

## 7. 自动化脚本

创建测试和配置脚本（见 `scripts/` 目录）

---

## 8. 参考资料

- [Kdump 官方文档](https://www.kernel.org/doc/html/latest/admin-guide/kdump/kdump.html)
- [ARM64 Kdump 支持](https://www.kernel.org/doc/html/latest/arm64/kdump.html)
- [crash 工具手册](https://github.com/crash-utility/crash)

---

## 9. 快速检查清单

- [ ] 内核支持 KEXEC 和 CRASH_DUMP
- [ ] 启动参数包含 crashkernel
- [ ] 安装了 kexec-tools
- [ ] 配置了 /etc/kdump.conf
- [ ] kdump 服务已启动
- [ ] /sys/kernel/kexec_crash_loaded = 1
- [ ] 测试触发崩溃并验证转储文件生成

