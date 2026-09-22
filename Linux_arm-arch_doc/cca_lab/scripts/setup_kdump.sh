#!/bin/bash
#
# ARM64 Kdump 自动配置脚本
# 用法: sudo ./setup_kdump.sh [crashkernel_size] [crashkernel_offset]
#

set -e

CRASHKERNEL_SIZE=${1:-256M}
CRASHKERNEL_OFFSET=${2:-512M}
KERNEL_VERSION=$(uname -r)

echo "=========================================="
echo "  ARM64 Kdump 配置脚本"
echo "=========================================="
echo "内核版本: $KERNEL_VERSION"
echo "预留内存: $CRASHKERNEL_SIZE@$CRASHKERNEL_OFFSET"
echo ""

# 检查是否为 root
if [ "$EUID" -ne 0 ]; then 
    echo "错误: 请使用 sudo 运行此脚本"
    exit 1
fi

# 1. 检查内核支持
echo "[1/6] 检查内核支持..."
if ! grep -q "CONFIG_KEXEC=y" /boot/config-$KERNEL_VERSION 2>/dev/null && \
   ! zcat /proc/config.gz 2>/dev/null | grep -q "CONFIG_KEXEC=y"; then
    echo "警告: 内核可能不支持 KEXEC"
    echo "请检查内核配置: CONFIG_KEXEC=y"
fi

if ! grep -q "CONFIG_CRASH_DUMP=y" /boot/config-$KERNEL_VERSION 2>/dev/null && \
   ! zcat /proc/config.gz 2>/dev/null | grep -q "CONFIG_CRASH_DUMP=y"; then
    echo "警告: 内核可能不支持 CRASH_DUMP"
    echo "请检查内核配置: CONFIG_CRASH_DUMP=y"
fi

# 2. 检查 crashkernel 参数
echo "[2/6] 检查启动参数..."
if grep -q "crashkernel" /proc/cmdline; then
    echo "✓ 已找到 crashkernel 参数:"
    grep -o "crashkernel=[^ ]*" /proc/cmdline
else
    echo "✗ 未找到 crashkernel 参数"
    echo "请修改 GRUB 或设备树添加: crashkernel=$CRASHKERNEL_SIZE@$CRASHKERNEL_OFFSET"
    echo ""
    echo "GRUB 配置示例:"
    echo "  sudo vi /etc/default/grub"
    echo "  GRUB_CMDLINE_LINUX_DEFAULT=\"crashkernel=$CRASHKERNEL_SIZE@$CRASHKERNEL_OFFSET\""
    echo "  sudo update-grub"
    read -p "是否继续配置其他部分? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# 3. 安装工具
echo "[3/6] 检查并安装工具..."
if ! command -v kexec &> /dev/null; then
    echo "安装 kexec-tools..."
    if command -v apt-get &> /dev/null; then
        apt-get update
        apt-get install -y kexec-tools
    elif command -v yum &> /dev/null; then
        yum install -y kexec-tools
    else
        echo "错误: 无法自动安装 kexec-tools，请手动安装"
        exit 1
    fi
else
    echo "✓ kexec-tools 已安装"
fi

# 4. 配置 kdump
echo "[4/6] 配置 kdump..."
mkdir -p /var/crash

# 创建 kdump 配置文件
if [ ! -f /etc/kdump.conf ]; then
    cat > /etc/kdump.conf <<EOF
# Kdump 配置文件
path /var/crash
core_collector makedumpfile -c --message-level 1 -d 31
EOF
    echo "✓ 创建 /etc/kdump.conf"
else
    echo "✓ /etc/kdump.conf 已存在"
fi

# 配置 kdump-tools
if [ -f /etc/default/kdump-tools ]; then
    sed -i 's/USE_KDUMP=.*/USE_KDUMP=1/' /etc/default/kdump-tools
    echo "✓ 更新 /etc/default/kdump-tools"
else
    cat > /etc/default/kdump-tools <<EOF
USE_KDUMP=1
KDUMP_SYSCTL="kernel.panic_on_oops=1"
EOF
    echo "✓ 创建 /etc/default/kdump-tools"
fi

# 5. 加载捕获内核
echo "[5/6] 加载捕获内核..."
if [ -f /boot/vmlinuz-$KERNEL_VERSION ] && [ -f /boot/initrd.img-$KERNEL_VERSION ]; then
    CMDLINE=$(cat /proc/cmdline | sed 's/crashkernel=[^ ]*//')
    kexec -p /boot/vmlinuz-$KERNEL_VERSION \
        --initrd=/boot/initrd.img-$KERNEL_VERSION \
        --append="$CMDLINE nr_cpus=1 reset_devices" 2>&1 || {
        echo "警告: kexec 加载失败，可能需要手动配置"
    }
    echo "✓ 捕获内核已加载"
else
    echo "✗ 未找到内核文件: /boot/vmlinuz-$KERNEL_VERSION"
    echo "请确保内核文件存在"
fi

# 6. 启动服务
echo "[6/6] 启动 kdump 服务..."
if systemctl is-enabled kdump-tools &> /dev/null; then
    systemctl restart kdump-tools
    echo "✓ kdump 服务已重启"
else
    systemctl enable kdump-tools
    systemctl start kdump-tools
    echo "✓ kdump 服务已启用并启动"
fi

# 验证
echo ""
echo "=========================================="
echo "  验证配置"
echo "=========================================="

if [ -f /sys/kernel/kexec_crash_loaded ]; then
    LOADED=$(cat /sys/kernel/kexec_crash_loaded)
    if [ "$LOADED" = "1" ]; then
        echo "✓ Kexec 已加载 (kexec_crash_loaded = 1)"
    else
        echo "✗ Kexec 未加载 (kexec_crash_loaded = 0)"
    fi
else
    echo "✗ /sys/kernel/kexec_crash_loaded 不存在"
fi

if systemctl is-active kdump-tools &> /dev/null; then
    echo "✓ kdump-tools 服务运行中"
else
    echo "✗ kdump-tools 服务未运行"
    systemctl status kdump-tools --no-pager -l
fi

echo ""
echo "=========================================="
echo "  配置完成！"
echo "=========================================="
echo ""
echo "下一步:"
echo "1. 确保启动参数包含 crashkernel=$CRASHKERNEL_SIZE@$CRASHKERNEL_OFFSET"
echo "2. 重启系统使 crashkernel 生效"
echo "3. 测试: echo c | sudo tee /proc/sysrq-trigger (会崩溃系统!)"
echo "4. 检查转储: ls -lh /var/crash/"
echo ""





