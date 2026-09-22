# ARM vexpress-a9 平台 Kdump 配置指南

## 环境说明

- **平台**: QEMU vexpress-a9 (ARM 32位)
- **内存**: 512MB（有限，需要合理配置）
- **内核**: zImage 格式
- **根文件系统**: BusyBox on SD 卡
- **文件共享**: virtio-9p

---

## 1. 内存限制考虑

**512MB 总内存的分配建议**:

```
总内存: 512MB
├─ 正常系统: ~400MB
├─ crashkernel: 64MB @ 128MB 偏移
└─ 剩余缓冲: ~48MB
```

**crashkernel 参数**: `crashkernel=64M@128M`

⚠️ **注意**: 如果内存不足，可以减小到 `crashkernel=32M@128M`

---

## 2. 内核编译配置（ARM 32位）

### 2.1 必需配置

```bash
cd ~/linux-core/linux-5.10

make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- menuconfig
```

**必需选项**:

```
# Kexec 支持
CONFIG_KEXEC=y
CONFIG_ATAGS_PROC=y  # vexpress 可能需要

# Crash dump 支持
CONFIG_CRASH_DUMP=y
CONFIG_PROC_VMCORE=y

# 物理内存布局（根据你的平台）
CONFIG_PHYS_OFFSET=0x60000000  # vexpress-a9 的物理起始地址
```

### 2.2 快速配置脚本

创建 `scripts/config_kernel_kdump_arm.sh`:

```bash
#!/bin/bash
# ARM 32位内核 kdump 配置

cd linux-5.10

./scripts/config --enable CONFIG_KEXEC
./scripts/config --enable CONFIG_CRASH_DUMP
./scripts/config --enable CONFIG_PROC_VMCORE
./scripts/config --enable CONFIG_ATAGS_PROC

# vexpress 特定
./scripts/config --set-val CONFIG_PHYS_OFFSET 0x60000000
```

---

## 3. QEMU 启动配置

### 3.1 修改启动命令

在你的原始命令基础上添加 `crashkernel` 参数：

```bash
qemu-system-arm \
    -M vexpress-a9 \
    -m 512M \
    -kernel zImage \
    -dtb vexpress-v2p-ca9.dtb \
    -nographic \
    -append "root=/dev/mmcblk0 rw console=ttyAMA0 crashkernel=64M@128M" \
    -device virtio-9p-device,fsdev=host_share,mount_tag=host_share \
    -fsdev local,id=host_share,path=/home/fbt1709/tftpboot/shared-dir,security_model=none \
    -drive file=rootfs.ext3,format=raw,if=sd
```

### 3.2 使用提供的脚本

```bash
cd ~/cca_lab

# 设置环境变量（可选）
export KERNEL_IMAGE=zImage
export DTB=vexpress-v2p-ca9.dtb
export ROOTFS=rootfs.ext3
export SHARED_DIR=/home/fbt1709/tftpboot/shared-dir

# 启动
./scripts/run_qemu_vexpress_kdump.sh
```

---

## 4. 编译 kexec-tools（ARM 32位）

### 4.1 静态编译

```bash
cd ~/cca_lab/experiments

# 下载源码
git clone https://github.com/horms/kexec-tools.git
cd kexec-tools

# 配置（ARM 32位）
./bootstrap
./configure \
    --host=arm-linux-gnueabihf \
    --target=arm-linux-gnueabihf \
    --prefix=$(pwd)/../kexec-tools-install-arm \
    LDFLAGS="-static" \
    CFLAGS="-static -O2" \
    CC="arm-linux-gnueabihf-gcc"

# 编译
make -j$(nproc)
make install

# 复制到根文件系统
sudo cp ../kexec-tools-install-arm/sbin/kexec /path/to/rootfs/sbin/
```

### 4.2 使用自动化脚本

修改 `scripts/build_kexec_static.sh` 支持 ARM 32位：

```bash
# 设置交叉编译器
export CROSS_COMPILE=arm-linux-gnueabihf-
export ARCH=arm

# 运行脚本（需要修改脚本支持 ARM）
~/cca_lab/scripts/build_kexec_static.sh
```

---

## 5. BusyBox 根文件系统配置

### 5.1 创建 kdump 初始化脚本

在根文件系统中创建 `/etc/init.d/kdump`:

```bash
#!/bin/sh
# vexpress-a9 平台的 kdump 初始化

KERNEL_IMAGE="/boot/zImage"
INITRD="/boot/initrd.img"  # 如果有的话

# 检查 crashkernel
if ! grep -q "crashkernel" /proc/cmdline; then
    echo "警告: 未找到 crashkernel 参数"
    exit 1
fi

# 创建转储目录
mkdir -p /var/crash

# 获取启动参数
CMDLINE=$(cat /proc/cmdline | sed 's/crashkernel=[^ ]*//')

# 加载捕获内核
if [ -f "$KERNEL_IMAGE" ]; then
    kexec -p "$KERNEL_IMAGE" \
        --append="$CMDLINE nr_cpus=1 reset_devices" \
        2>&1
    
    if [ $? -eq 0 ]; then
        echo "✓ Kdump 已加载"
    else
        echo "✗ Kdump 加载失败"
    fi
else
    echo "错误: 内核镜像不存在: $KERNEL_IMAGE"
fi
```

### 5.2 挂载共享目录（用于转储文件）

```bash
# 在 QEMU 内
mkdir -p /mnt
mount -t 9p host_share /mnt

# 修改转储脚本，保存到共享目录
cat > /usr/local/bin/save_vmcore.sh << 'EOF'
#!/bin/sh
DUMP_DIR="/mnt/crash"  # 使用共享目录
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$DUMP_DIR"

if [ -f /proc/vmcore ]; then
    echo "保存 vmcore 到共享目录..."
    cp /proc/vmcore "$DUMP_DIR/vmcore-$TIMESTAMP"
fi

dmesg > "$DUMP_DIR/vmcore-dmesg-$TIMESTAMP.txt"
uname -a > "$DUMP_DIR/system-info-$TIMESTAMP.txt"
EOF

chmod +x /usr/local/bin/save_vmcore.sh
```

---

## 6. 内存布局（vexpress-a9）

### 6.1 物理内存映射

```
vexpress-a9 内存布局:
0x60000000 - 0x80000000: 512MB RAM
├─ 0x60000000 - 0x68000000: 正常系统 (~128MB)
├─ 0x68000000 - 0x6C000000: crashkernel 预留 (64MB @ 128MB偏移)
└─ 0x6C000000 - 0x80000000: 剩余可用内存
```

### 6.2 检查内存布局

在 QEMU 内：

```bash
# 查看内存信息
cat /proc/meminfo

# 查看 iomem
cat /proc/iomem

# 查看启动参数
cat /proc/cmdline
```

---

## 7. 测试步骤

### 7.1 启动 QEMU

```bash
cd ~/cca_lab
./scripts/run_qemu_vexpress_kdump.sh
```

### 7.2 在 QEMU 内配置

```bash
# 1. 挂载共享目录
mkdir -p /mnt
mount -t 9p host_share /mnt

# 2. 检查 crashkernel
cat /proc/cmdline | grep crashkernel
# 应该看到: crashkernel=64M@128M

# 3. 检查预留内存
dmesg | grep -i crash

# 4. 加载 kdump
/etc/init.d/kdump start

# 5. 验证
cat /sys/kernel/kexec_crash_loaded
# 应该输出: 1
```

### 7.3 触发崩溃测试

```bash
# ⚠️ 会崩溃系统！
echo c > /proc/sysrq-trigger
```

### 7.4 检查转储文件

系统重启后，在主机上检查：

```bash
# 转储文件应该在共享目录
ls -lh /home/fbt1709/tftpboot/shared-dir/crash/
```

---

## 8. 常见问题

### 8.1 内存不足

**问题**: crashkernel 预留失败

**解决**:
- 减小 crashkernel 大小: `crashkernel=32M@128M`
- 或增加 QEMU 内存: `-m 1G`

### 8.2 kexec 加载失败

**问题**: `kexec -p` 失败

**解决**:
- 检查内核镜像路径
- 检查启动参数格式
- 查看 `dmesg` 获取错误信息

### 8.3 捕获内核无法启动

**问题**: 崩溃后不进入捕获内核

**解决**:
- 检查预留内存区域
- 确认内核支持 kexec
- 使用 QEMU 调试: `-d int -D qemu.log`

---

## 9. 优化建议

### 9.1 内存优化

由于只有 512MB 内存：

1. **使用精简捕获内核**: 编译最小内核作为捕获内核
2. **减小 crashkernel**: 32MB 可能足够小转储
3. **使用压缩转储**: `makedumpfile -c` 压缩 vmcore

### 9.2 转储优化

```bash
# 使用 makedumpfile 压缩转储（如果可用）
makedumpfile -c -d 31 /proc/vmcore /mnt/crash/vmcore-compressed
```

---

## 10. 完整工作流程

1. **配置内核**: 启用 KEXEC 和 CRASH_DUMP
2. **编译内核**: `make ARCH=arm ...`
3. **编译 kexec-tools**: ARM 32位静态版本
4. **配置根文件系统**: 添加 kexec 和初始化脚本
5. **启动 QEMU**: 带 crashkernel 参数
6. **加载 kdump**: 在 QEMU 内运行初始化脚本
7. **测试**: 触发崩溃并验证转储

---

## 11. 参考

- [ARM vexpress 平台文档](https://www.kernel.org/doc/Documentation/arm/VExpress.txt)
- [ARM 32位 Kdump 支持](https://www.kernel.org/doc/html/latest/arm/kdump.html)

