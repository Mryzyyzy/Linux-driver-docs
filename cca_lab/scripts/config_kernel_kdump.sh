#!/bin/bash
#
# 配置 Linux 5.10 内核支持 kdump
#

set -e

if [ $# -lt 1 ]; then
    echo "用法: $0 <内核源码目录>"
    echo "示例: $0 ~/linux-core/linux-5.10"
    exit 1
fi

KERNEL_DIR="$1"

if [ ! -d "$KERNEL_DIR" ]; then
    echo "错误: 内核目录不存在: $KERNEL_DIR"
    exit 1
fi

cd "$KERNEL_DIR"

echo "=========================================="
echo "  配置内核支持 Kdump"
echo "=========================================="
echo "内核目录: $KERNEL_DIR"
echo ""

# 检查是否有 scripts/config
if [ ! -f "scripts/config" ]; then
    echo "错误: 找不到 scripts/config"
    echo "请确保这是有效的内核源码目录"
    exit 1
fi

# 配置 kexec 和 kdump
echo "[1/5] 配置 KEXEC..."
./scripts/config --enable CONFIG_KEXEC

echo "[2/5] 配置 CRASH_DUMP..."
./scripts/config --enable CONFIG_CRASH_DUMP

echo "[3/5] 配置 PROC_VMCORE..."
./scripts/config --enable CONFIG_PROC_VMCORE

echo "[4/5] 配置 PROC_KCORE..."
./scripts/config --enable CONFIG_PROC_KCORE

echo "[5/5] 配置调试信息（用于 crash 分析）..."
./scripts/config --enable CONFIG_DEBUG_INFO
./scripts/config --set-str CONFIG_DEBUG_INFO_DWARF4 y

# ARM64 特定配置
if [ -f "arch/arm64/configs/defconfig" ]; then
    echo ""
    echo "检查 ARM64 配置..."
    
    # 设置物理起始地址
    if grep -q "CONFIG_PHYSICAL_START" .config 2>/dev/null; then
        ./scripts/config --set-val CONFIG_PHYSICAL_START 0x80000000
    fi
fi

echo ""
echo "=========================================="
echo "  配置完成！"
echo "=========================================="
echo ""
echo "验证配置:"
echo "  grep -E 'CONFIG_KEXEC|CONFIG_CRASH_DUMP|CONFIG_PROC_VMCORE' .config"
echo ""
echo "编译内核:"
echo "  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j\$(nproc)"
echo ""





