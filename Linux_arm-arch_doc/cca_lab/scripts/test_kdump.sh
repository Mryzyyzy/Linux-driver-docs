#!/bin/bash
#
# Kdump 测试脚本
# 用于验证 kdump 配置是否正确
#

set -e

echo "=========================================="
echo "  Kdump 配置检查"
echo "=========================================="
echo ""

# 检查 kexec 加载状态
echo "[1] 检查 kexec 加载状态..."
if [ -f /sys/kernel/kexec_crash_loaded ]; then
    LOADED=$(cat /sys/kernel/kexec_crash_loaded)
    if [ "$LOADED" = "1" ]; then
        echo "✓ Kexec 已加载"
    else
        echo "✗ Kexec 未加载"
        echo "  运行: sudo systemctl restart kdump-tools"
    fi
else
    echo "✗ /sys/kernel/kexec_crash_loaded 不存在"
fi

# 检查 crashkernel 参数
echo ""
echo "[2] 检查 crashkernel 参数..."
if grep -q "crashkernel" /proc/cmdline; then
    echo "✓ 找到 crashkernel 参数:"
    grep -o "crashkernel=[^ ]*" /proc/cmdline
else
    echo "✗ 未找到 crashkernel 参数"
fi

# 检查预留内存
echo ""
echo "[3] 检查预留内存..."
if dmesg | grep -i "crashkernel" | head -3; then
    echo "✓ 找到 crashkernel 相关信息"
else
    echo "✗ 未找到 crashkernel 内存预留信息"
fi

# 检查服务状态
echo ""
echo "[4] 检查 kdump 服务状态..."
if systemctl is-active kdump-tools &> /dev/null; then
    echo "✓ kdump-tools 服务运行中"
    systemctl status kdump-tools --no-pager -l | head -10
else
    echo "✗ kdump-tools 服务未运行"
fi

# 检查转储目录
echo ""
echo "[5] 检查转储目录..."
if [ -d /var/crash ]; then
    echo "✓ /var/crash 目录存在"
    ls -lh /var/crash/ 2>/dev/null | head -5 || echo "  目录为空"
else
    echo "✗ /var/crash 目录不存在"
fi

# 检查内核支持
echo ""
echo "[6] 检查内核配置..."
KERNEL_VERSION=$(uname -r)
if [ -f /boot/config-$KERNEL_VERSION ]; then
    if grep -q "CONFIG_KEXEC=y" /boot/config-$KERNEL_VERSION; then
        echo "✓ CONFIG_KEXEC=y"
    else
        echo "✗ CONFIG_KEXEC 未启用"
    fi
    
    if grep -q "CONFIG_CRASH_DUMP=y" /boot/config-$KERNEL_VERSION; then
        echo "✓ CONFIG_CRASH_DUMP=y"
    else
        echo "✗ CONFIG_CRASH_DUMP 未启用"
    fi
else
    echo "? 无法检查内核配置 (/boot/config-$KERNEL_VERSION 不存在)"
fi

echo ""
echo "=========================================="
echo "  检查完成"
echo "=========================================="
echo ""
echo "⚠️  手动触发崩溃测试 (会崩溃系统!):"
echo "   echo c | sudo tee /proc/sysrq-trigger"
echo ""





