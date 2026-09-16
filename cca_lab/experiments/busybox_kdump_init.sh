#!/bin/sh
#
# BusyBox 环境下的 kdump 初始化脚本
# 放在根文件系统的 /etc/init.d/kdump 或 /etc/rc.local
#

KERNEL_VERSION=$(uname -r)
KERNEL_IMAGE="/boot/Image"
INITRD="/boot/initrd.img"

# 检查 crashkernel 参数
if ! grep -q "crashkernel" /proc/cmdline; then
    echo "警告: 未找到 crashkernel 参数"
    echo "请在 QEMU 启动参数中添加: crashkernel=256M@512M"
    exit 1
fi

# 检查内核镜像
if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "错误: 内核镜像不存在: $KERNEL_IMAGE"
    exit 1
fi

# 创建转储目录
mkdir -p /var/crash

# 获取启动参数（排除 crashkernel）
CMDLINE=$(cat /proc/cmdline | sed 's/crashkernel=[^ ]*//')

# 加载捕获内核
echo "加载 kdump 捕获内核..."
kexec -p "$KERNEL_IMAGE" \
    --initrd="$INITRD" \
    --append="$CMDLINE nr_cpus=1 reset_devices" \
    2>&1

if [ $? -eq 0 ]; then
    echo "✓ Kdump 已加载"
    # 设置标志（如果存在）
    if [ -f /sys/kernel/kexec_crash_loaded ]; then
        echo 1 > /sys/kernel/kexec_crash_loaded
    fi
else
    echo "✗ Kdump 加载失败"
    exit 1
fi

# 创建保存 vmcore 的脚本
cat > /usr/local/bin/save_vmcore.sh << 'EOF'
#!/bin/sh
DUMP_DIR="/var/crash"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$DUMP_DIR"

if [ -f /proc/vmcore ]; then
    echo "保存 vmcore..."
    cp /proc/vmcore "$DUMP_DIR/vmcore-$TIMESTAMP"
    echo "vmcore 已保存到: $DUMP_DIR/vmcore-$TIMESTAMP"
fi

dmesg > "$DUMP_DIR/vmcore-dmesg-$TIMESTAMP.txt"
uname -a > "$DUMP_DIR/system-info-$TIMESTAMP.txt"
cat /proc/meminfo >> "$DUMP_DIR/system-info-$TIMESTAMP.txt"
cat /proc/cmdline >> "$DUMP_DIR/system-info-$TIMESTAMP.txt"
EOF

chmod +x /usr/local/bin/save_vmcore.sh

echo "Kdump 初始化完成"





