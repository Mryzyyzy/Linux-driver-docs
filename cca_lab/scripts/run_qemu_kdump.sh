#!/bin/bash
#
# QEMU 启动脚本（带 kdump 支持）
#

set -e

# 配置参数
KERNEL_IMAGE="${KERNEL_IMAGE:-Image}"
INITRD="${INITRD:-initrd.img}"
ROOTFS="${ROOTFS:-rootfs.ext4}"
MEMORY="${MEMORY:-2G}"
CRASHKERNEL_SIZE="${CRASHKERNEL_SIZE:-256M}"
CRASHKERNEL_OFFSET="${CRASHKERNEL_OFFSET:-512M}"
CPU="${CPU:-cortex-a57}"
SMP="${SMP:-2}"

# 检查文件
if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "错误: 内核镜像不存在: $KERNEL_IMAGE"
    exit 1
fi

if [ ! -f "$INITRD" ]; then
    echo "警告: initrd 不存在: $INITRD"
fi

# 构建启动参数
APPEND_ARGS="console=ttyAMA0 root=/dev/ram0 rw"
APPEND_ARGS="$APPEND_ARGS crashkernel=${CRASHKERNEL_SIZE}@${CRASHKERNEL_OFFSET}"

# 如果有根文件系统，使用它
if [ -f "$ROOTFS" ]; then
    APPEND_ARGS="console=ttyAMA0 root=/dev/vda rw"
    DRIVE_ARG="-drive file=$ROOTFS,format=raw,if=virtio,id=hd0 -device virtio-blk-device,drive=hd0"
else
    DRIVE_ARG=""
fi

# QEMU 命令
QEMU_CMD="qemu-system-aarch64 \
    -machine virt \
    -cpu $CPU \
    -smp $SMP \
    -m $MEMORY \
    -kernel $KERNEL_IMAGE \
    -initrd $INITRD \
    $DRIVE_ARG \
    -append '$APPEND_ARGS' \
    -nographic \
    -serial stdio"

echo "=========================================="
echo "  启动 QEMU (带 Kdump 支持)"
echo "=========================================="
echo "内核: $KERNEL_IMAGE"
echo "Initrd: $INITRD"
echo "内存: $MEMORY"
echo "Crashkernel: ${CRASHKERNEL_SIZE}@${CRASHKERNEL_OFFSET}"
echo "CPU: $CPU"
echo ""
echo "启动命令:"
echo "$QEMU_CMD"
echo ""
echo "=========================================="
echo ""

# 运行 QEMU
eval $QEMU_CMD





