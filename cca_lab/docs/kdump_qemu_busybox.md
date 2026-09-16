# QEMU + BusyBox 环境 Kdump 配置指南

## 环境说明

- **平台**: QEMU ARM64
- **内核版本**: Linux 5.10
- **文件系统**: BusyBox 自制根文件系统
- **特点**: 最小化环境，需要手动配置所有组件

---

## 1. 内核编译配置

### 1.1 必需的内核配置选项

在 `make menuconfig` 或直接修改 `.config` 文件：

```bash
# 进入内核源码目录
cd ~/linux-core/linux-5.10

# 配置 kdump 相关选项
make ARCH=arm64 menuconfig
```

**必需配置**:

```
# 通用设置
CONFIG_KEXEC=y
CONFIG_CRASH_DUMP=y
CONFIG_PROC_VMCORE=y
CONFIG_PROC_KCORE=y

# ARM64 特定
CONFIG_PHYSICAL_START=0x80000000
CONFIG_ARM64_VA_BITS_48=y

# 调试支持（可选但推荐）
CONFIG_DEBUG_INFO=y
CONFIG_DEBUG_INFO_DWARF4=y
```

### 1.2 快速配置脚本

创建配置脚本 `scripts/config_kdump.sh`:

```bash
#!/bin/bash
# 在内核源码目录运行

cd linux-5.10

# 启用 kexec
./scripts/config --enable CONFIG_KEXEC
./scripts/config --enable CONFIG_CRASH_DUMP
./scripts/config --enable CONFIG_PROC_VMCORE
./scripts/config --enable CONFIG_PROC_KCORE

# ARM64 设置
./scripts/config --set-val CONFIG_PHYSICAL_START 0x80000000

# 调试信息（用于 crash 分析）
./scripts/config --enable CONFIG_DEBUG_INFO
./scripts/config --set-str CONFIG_DEBUG_INFO_DWARF4 y

echo "配置完成，运行 make 编译"
```

---

## 2. QEMU 启动参数配置

### 2.1 基本 QEMU 启动命令（带 crashkernel）

```bash
qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a57 \
    -smp 2 \
    -m 2G \
    -kernel Image \
    -initrd initrd.img \
    -append "console=ttyAMA0 root=/dev/ram0 rw crashkernel=256M@512M" \
    -nographic
```

### 2.2 关键参数说明

- **`-m 2G`**: 总内存 2GB（需要足够内存给 crashkernel）
- **`crashkernel=256M@512M`**: 
  - 从 512MB 偏移处预留 256MB 给捕获内核
  - 格式：`大小@偏移` 或 `大小`（自动分配）

### 2.3 内存布局示例

```
0x40000000 - 0x60000000: 正常内核和用户空间 (512MB)
0x60000000 - 0x70000000: crashkernel 预留区域 (256MB)
0x70000000 - 0x80000000: 剩余可用内存
```

---

## 3. BusyBox 环境配置

### 3.1 编译 kexec-tools（静态链接）

由于 BusyBox 环境最小化，需要静态编译 kexec-tools：

```bash
# 下载 kexec-tools
cd ~/cca_lab/experiments
git clone https://github.com/horms/kexec-tools.git
cd kexec-tools

# 配置为静态编译
./bootstrap
./configure \
    --host=aarch64-linux-gnu \
    --target=aarch64-linux-gnu \
    LDFLAGS="-static" \
    CFLAGS="-static"

# 编译
make -j$(nproc)

# 安装到根文件系统
sudo cp build/sbin/kexec /path/to/rootfs/sbin/
sudo chmod +x /path/to/rootfs/sbin/kexec
```

### 3.2 创建最小 kdump 脚本

在 BusyBox 根文件系统中创建 `/etc/init.d/kdump`:

```bash
#!/bin/sh
# /etc/init.d/kdump - BusyBox 环境下的 kdump 启动脚本

KERNEL_VERSION=$(uname -r)
KERNEL_IMAGE="/boot/Image"
INITRD="/boot/initrd.img"

# 检查文件是否存在
if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "错误: 内核镜像不存在: $KERNEL_IMAGE"
    exit 1
fi

# 获取当前启动参数（排除 crashkernel）
CMDLINE=$(cat /proc/cmdline | sed 's/crashkernel=[^ ]*//')

# 加载捕获内核
kexec -p "$KERNEL_IMAGE" \
    --initrd="$INITRD" \
    --append="$CMDLINE nr_cpus=1 reset_devices" \
    --dtb=/sys/firmware/fdt 2>&1

if [ $? -eq 0 ]; then
    echo "Kdump 已加载"
    echo 1 > /sys/kernel/kexec_crash_loaded
else
    echo "Kdump 加载失败"
    exit 1
fi
```

### 3.3 创建转储目录和脚本

```bash
# 在根文件系统中
mkdir -p /var/crash

# 创建转储脚本 /usr/local/bin/save_vmcore.sh
cat > /usr/local/bin/save_vmcore.sh << 'EOF'
#!/bin/sh
# 保存 vmcore 的脚本

DUMP_DIR="/var/crash"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$DUMP_DIR"

# 保存 vmcore
if [ -f /proc/vmcore ]; then
    echo "保存 vmcore..."
    cp /proc/vmcore "$DUMP_DIR/vmcore-$TIMESTAMP"
    echo "vmcore 已保存到: $DUMP_DIR/vmcore-$TIMESTAMP"
fi

# 保存 dmesg
dmesg > "$DUMP_DIR/vmcore-dmesg-$TIMESTAMP.txt"

# 保存系统信息
uname -a > "$DUMP_DIR/system-info-$TIMESTAMP.txt"
cat /proc/meminfo >> "$DUMP_DIR/system-info-$TIMESTAMP.txt"
cat /proc/cmdline >> "$DUMP_DIR/system-info-$TIMESTAMP.txt"
EOF

chmod +x /usr/local/bin/save_vmcore.sh
```

---

## 4. 捕获内核准备

### 4.1 使用主内核作为捕获内核（简单方法）

```bash
# 在根文件系统中
cp /boot/Image /boot/Image-crash
cp /boot/initrd.img /boot/initrd-crash.img
```

### 4.2 编译精简捕获内核（推荐）

```bash
cd linux-5.10

# 使用最小配置
make ARCH=arm64 defconfig

# 只启用必要选项
make ARCH=arm64 menuconfig
# 启用: KEXEC, CRASH_DUMP, PROC_VMCORE
# 禁用: 大部分驱动和功能

# 编译
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)

# 复制到根文件系统
cp arch/arm64/boot/Image /path/to/rootfs/boot/Image-crash
```

---

## 5. 完整启动流程

### 5.1 创建 QEMU 启动脚本

创建 `scripts/run_qemu_with_kdump.sh`:

```bash
#!/bin/bash

KERNEL_IMAGE="Image"
INITRD="initrd.img"
ROOTFS="rootfs.ext4"  # 或你的根文件系统

QEMU_CMD="qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a57 \
    -smp 2 \
    -m 2G \
    -kernel $KERNEL_IMAGE \
    -initrd $INITRD \
    -append 'console=ttyAMA0 root=/dev/ram0 rw crashkernel=256M@512M' \
    -nographic"

echo "启动 QEMU (带 kdump 支持)..."
echo "命令: $QEMU_CMD"
eval $QEMU_CMD
```

### 5.2 在 QEMU 内部启动 kdump

```bash
# 进入 QEMU 后
/etc/init.d/kdump start

# 或手动加载
kexec -p /boot/Image \
    --initrd=/boot/initrd.img \
    --append="$(cat /proc/cmdline | sed 's/crashkernel=[^ ]*//') nr_cpus=1 reset_devices"
```

---

## 6. 测试 Kdump

### 6.1 检查配置

```bash
# 检查 crashkernel
cat /proc/cmdline | grep crashkernel

# 检查预留内存
dmesg | grep -i crash

# 检查 kexec 加载
cat /sys/kernel/kexec_crash_loaded
# 应该输出: 1
```

### 6.2 触发崩溃测试

```bash
# 方法 1: 触发 panic
echo c > /proc/sysrq-trigger

# 方法 2: 空指针解引用（需要内核模块）
# insmod crash_test.ko

# 方法 3: 直接调用 panic
echo 1 > /proc/sys/kernel/panic
```

### 6.3 检查转储文件

系统重启后（或捕获内核启动后）：

```bash
# 检查转储目录
ls -lh /var/crash/

# 应该看到:
# vmcore-YYYYMMDD_HHMMSS
# vmcore-dmesg-YYYYMMDD_HHMMSS.txt
```

---

## 7. 常见问题

### 7.1 crashkernel 未生效

**问题**: `dmesg | grep crash` 没有输出

**解决**:
- 检查启动参数是否正确传递
- 确保内存足够（总内存 > crashkernel 大小）
- 检查内核配置 `CONFIG_CRASH_DUMP=y`

### 7.2 kexec 加载失败

**问题**: `kexec -p` 失败

**解决**:
- 检查内核镜像和 initrd 是否存在
- 检查启动参数是否正确
- 确保 `/sys/kernel/kexec_crash_loaded` 可写
- 查看 `dmesg` 获取详细错误

### 7.3 捕获内核无法启动

**问题**: 崩溃后捕获内核不启动

**解决**:
- 检查捕获内核镜像是否正确
- 检查预留内存区域是否足够
- 使用 `-d` 参数查看 QEMU 调试信息

### 7.4 BusyBox 缺少工具

**问题**: 缺少某些命令

**解决**:
- 重新编译 BusyBox，启用需要的 applet
- 或使用静态编译的独立工具

---

## 8. 自动化脚本

见 `scripts/` 目录下的自动化脚本。

---

## 9. 调试技巧

### 9.1 启用 QEMU 调试

```bash
qemu-system-aarch64 \
    ... \
    -d int \
    -D qemu_debug.log
```

### 9.2 查看内核日志

```bash
# 在 QEMU 内部
dmesg | tail -100

# 或通过串口
qemu-system-aarch64 ... -serial stdio
```

### 9.3 检查内存布局

```bash
cat /proc/iomem
cat /proc/meminfo
```

---

## 10. 完整示例

见 `experiments/qemu_kdump_example/` 目录。





