#!/bin/bash
#
# 静态编译 kexec-tools（ARM 32位，用于 vexpress-a9）
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../experiments/kexec-tools-build-arm32"
INSTALL_DIR="$SCRIPT_DIR/../experiments/kexec-tools-install-arm32"

CROSS_COMPILE=${CROSS_COMPILE:-arm-linux-gnueabihf-}

echo "=========================================="
echo "  静态编译 kexec-tools (ARM 32位)"
echo "=========================================="
echo "交叉编译器: ${CROSS_COMPILE}gcc"
echo "构建目录: $BUILD_DIR"
echo "安装目录: $INSTALL_DIR"
echo ""

# 检查交叉编译器
if ! command -v ${CROSS_COMPILE}gcc &> /dev/null; then
    echo "错误: 找不到交叉编译器 ${CROSS_COMPILE}gcc"
    echo "请安装: sudo apt-get install gcc-arm-linux-gnueabihf"
    exit 1
fi

# 创建构建目录
mkdir -p "$BUILD_DIR"
mkdir -p "$INSTALL_DIR"

# 下载源码（如果不存在）
if [ ! -d "$BUILD_DIR/kexec-tools" ]; then
    echo "[1/4] 下载 kexec-tools 源码..."
    cd "$BUILD_DIR"
    git clone https://github.com/horms/kexec-tools.git || {
        echo "错误: git clone 失败，请检查网络连接"
        exit 1
    }
else
    echo "[1/4] 源码已存在，跳过下载"
fi

cd "$BUILD_DIR/kexec-tools"

# 检查是否有 bootstrap
if [ ! -f "configure" ]; then
    echo "[2/4] 运行 bootstrap..."
    if [ -f "bootstrap" ]; then
        ./bootstrap
    elif [ -f "autogen.sh" ]; then
        ./autogen.sh
    else
        echo "错误: 找不到 bootstrap 或 autogen.sh"
        exit 1
    fi
else
    echo "[2/4] configure 已存在，跳过 bootstrap"
fi

# 配置编译选项（ARM 32位）
echo "[3/4] 配置编译选项..."
./configure \
    --host=arm-linux-gnueabihf \
    --target=arm-linux-gnueabihf \
    --prefix="$INSTALL_DIR" \
    LDFLAGS="-static" \
    CFLAGS="-static -O2" \
    CC="${CROSS_COMPILE}gcc" \
    STRIP="${CROSS_COMPILE}strip" \
    2>&1 | tee configure.log

# 编译
echo "[4/4] 编译 kexec-tools..."
make -j$(nproc) 2>&1 | tee build.log

# 安装
echo "安装到: $INSTALL_DIR"
make install

echo ""
echo "=========================================="
echo "  编译完成！"
echo "=========================================="
echo ""
echo "可执行文件位置:"
echo "  $INSTALL_DIR/sbin/kexec"
echo ""
echo "复制到根文件系统:"
echo "  sudo cp $INSTALL_DIR/sbin/kexec /path/to/rootfs/sbin/"
echo "  sudo chmod +x /path/to/rootfs/sbin/kexec"
echo ""





