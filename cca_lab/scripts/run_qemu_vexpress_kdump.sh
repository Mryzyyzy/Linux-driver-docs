#!/bin/bash
#
# QEMU vexpress-a9 启动脚本（带 kdump 支持）
# 基于你的实际启动命令
#

set -e

# 配置参数（根据你的环境调整）
KERNEL_IMAGE="${KERNEL_IMAGE:-zImage}"
DTB="${DTB:-vexpress-v2p-ca9.dtb}"
ROOTFS="${ROOTFS:-rootfs.ext3}"
SHARED_DIR="${SHARED_DIR:-/home/fbt1709/tftpboot/shared-dir}"
MEMORY="${MEMORY:-512M}"
CRASHKERNEL_SIZE="${CRASHKERNEL_SIZE:-64M}"
CRASHKERNEL_OFFSET="${CRASHKERNEL_OFFSET:-128M}"

# 检查文件
if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "错误: 内核镜像不存在: $KERNEL_IMAGE"
    exit 1
fi

if [ ! -f "$DTB" ]; then
    echo "错误: 设备树文件不存在: $DTB"
    exit 1
fi

if [ ! -f "$ROOTFS" ]; then
    echo "错误: 根文件系统不存在: $ROOTFS"
    exit 1
fi

if [ ! -d "$SHARED_DIR" ]; then
    echo "警告: 共享目录不存在: $SHARED_DIR"
    echo "创建目录..."
    mkdir -p "$SHARED_DIR"
fi

# 构建启动参数
# 注意: 512M 内存，需要合理分配 crashkernel
APPEND_ARGS="root=/dev/mmcblk0 rw console=ttyAMA0"
APPEND_ARGS="$APPEND_ARGS crashkernel=${CRASHKERNEL_SIZE}@${CRASHKERNEL_OFFSET}"

# QEMU 命令（基于你的原始命令）
QEMU_CMD="qemu-system-arm \
    -M vexpress-a9 \
    -m $MEMORY \
    -kernel $KERNEL_IMAGE \
    -dtb $DTB \
    -nographic \
    -append '$APPEND_ARGS' \
    -device virtio-9p-device,fsdev=host_share,mount_tag=host_share \
    -fsdev local,id=host_share,path=$SHARED_DIR,security_model=none \
    -drive file=$ROOTFS,format=raw,if=sd"

echo "=========================================="
echo "  启动 QEMU vexpress-a9 (带 Kdump 支持)"
echo "=========================================="
echo "内核: $KERNEL_IMAGE"
echo "设备树: $DTB"
echo "根文件系统: $ROOTFS"
echo "内存: $MEMORY"
echo "Crashkernel: ${CRASHKERNEL_SIZE}@${CRASHKERNEL_OFFSET}"
echo "共享目录: $SHARED_DIR"
echo ""
echo "启动命令:"
echo "$QEMU_CMD"
echo ""
echo "=========================================="
echo ""
echo "提示:"
echo "  - 在 QEMU 内挂载共享目录: mount -t 9p host_share /mnt"
echo "  - 检查 crashkernel: cat /proc/cmdline | grep crashkernel"
echo "  - 加载 kdump: /etc/init.d/kdump start"
echo ""

# 运行 QEMU
eval $QEMU_CMD





