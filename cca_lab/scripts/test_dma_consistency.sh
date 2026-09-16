#!/bin/bash
#
# DMA 一致性测试脚本
# 编译并加载测试模块
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="${KERNEL_DIR:-/lib/modules/$(uname -r)/build}"
TEST_MODULE="$SCRIPT_DIR/../experiments/dma_consistency_test.c"

if [ ! -f "$TEST_MODULE" ]; then
    echo "错误: 测试模块不存在: $TEST_MODULE"
    exit 1
fi

echo "=========================================="
echo "  DMA 一致性测试"
echo "=========================================="
echo "内核目录: $KERNEL_DIR"
echo ""

# 编译测试模块
echo "[1/3] 编译测试模块..."
make -C "$KERNEL_DIR" M="$(dirname $TEST_MODULE)" \
    modules 2>&1 | grep -E "(error|warning|Building)" || true

if [ ! -f "$(dirname $TEST_MODULE)/dma_consistency_test.ko" ]; then
    echo "错误: 编译失败"
    exit 1
fi

echo "✓ 编译成功"
echo ""

# 加载模块
echo "[2/3] 加载测试模块..."
sudo insmod "$(dirname $TEST_MODULE)/dma_consistency_test.ko"

echo "✓ 模块已加载"
echo ""

# 查看测试输出
echo "[3/3] 查看测试结果..."
sleep 1
dmesg | tail -30 | grep -A 5 "DMA\|Test"

# 卸载模块
echo ""
echo "卸载模块..."
sudo rmmod dma_consistency_test 2>/dev/null || true

echo ""
echo "测试完成！"





