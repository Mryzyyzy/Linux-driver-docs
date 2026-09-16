# QEMU + BusyBox Kdump 快速开始

## 5 分钟快速配置

### 步骤 1: 配置内核

```bash
cd ~/linux-core/linux-5.10
~/cca_lab/scripts/config_kernel_kdump.sh .
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- menuconfig
# 确认 CONFIG_KEXEC=y, CONFIG_CRASH_DUMP=y
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
```

### 步骤 2: 编译 kexec-tools

```bash
~/cca_lab/scripts/build_kexec_static.sh
# 复制到根文件系统
sudo cp ~/cca_lab/experiments/kexec-tools-install/sbin/kexec /path/to/rootfs/sbin/
```

### 步骤 3: 配置根文件系统

```bash
# 复制初始化脚本
sudo cp ~/cca_lab/experiments/busybox_kdump_init.sh /path/to/rootfs/etc/init.d/kdump
sudo chmod +x /path/to/rootfs/etc/init.d/kdump

# 在 /etc/rc.local 或启动脚本中调用
echo "/etc/init.d/kdump" >> /path/to/rootfs/etc/rc.local
```

### 步骤 4: 启动 QEMU

```bash
cd ~/cca_lab
./scripts/run_qemu_kdump.sh
```

### 步骤 5: 在 QEMU 内测试

```bash
# 检查配置
cat /proc/cmdline | grep crashkernel
cat /sys/kernel/kexec_crash_loaded

# 触发崩溃（会重启）
echo c > /proc/sysrq-trigger
```

## 完整文档

详见: [kdump_qemu_busybox.md](kdump_qemu_busybox.md)





