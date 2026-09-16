#!/bin/bash
#
# Shell脚本示例：读取PCIe设备信息
# 演示如何使用shell命令直接读取PCIe配置空间
#
# 使用方法: ./read_pcie_info.sh <bus:device.function>
# 示例: ./read_pcie_info.sh 0000:00:01.0

if [ $# -lt 1 ]; then
    echo "Usage: $0 <pci_device>"
    echo "Example: $0 0000:00:01.0"
    echo ""
    echo "Find devices with: lspci"
    exit 1
fi

DEVICE="$1"
CONFIG="/sys/bus/pci/devices/${DEVICE}/config"

if [ ! -f "$CONFIG" ]; then
    echo "Error: Config file not found: $CONFIG"
    echo "Make sure the device exists: lspci | grep $DEVICE"
    exit 1
fi

echo "=== Reading PCIe Device: $DEVICE ==="
echo "Config file: $CONFIG"
echo ""

# 读取Vendor ID和Device ID
vendor=$(hexdump -n 2 -s 0x00 -e '1/2 "%04x\n"' "$CONFIG" 2>/dev/null)
device=$(hexdump -n 2 -s 0x02 -e '1/2 "%04x\n"' "$CONFIG" 2>/dev/null)

echo "=== Basic Information ==="
echo "Vendor ID: 0x$vendor"
echo "Device ID: 0x$device"

# 读取Class Code
class_lo=$(hexdump -n 1 -s 0x09 -e '1/1 "%02x\n"' "$CONFIG" 2>/dev/null)
class_mid=$(hexdump -n 1 -s 0x0a -e '1/1 "%02x\n"' "$CONFIG" 2>/dev/null)
class_hi=$(hexdump -n 1 -s 0x0b -e '1/1 "%02x\n"' "$CONFIG" 2>/dev/null)
echo "Class Code: 0x${class_hi}${class_mid}${class_lo}"

# 读取Capability Pointer
cap_ptr=$(hexdump -n 1 -s 0x34 -e '1/1 "%02x\n"' "$CONFIG" 2>/dev/null)
cap_ptr_dec=$((0x$cap_ptr))

echo ""
echo "=== Capability List ==="
echo "Capability Pointer: 0x$cap_ptr"

if [ "$cap_ptr" = "00" ] || [ "$cap_ptr" = "ff" ]; then
    echo "No capabilities found"
    exit 0
fi

# 简化的PCIe Capability查找（实际应该遍历链表）
# 这里假设在常见位置查找
echo ""
echo "=== Searching for PCIe Capability ==="

# 尝试在常见位置查找（实际应该从cap_ptr开始遍历）
found=0
for offset in 0x40 0x50 0x60 0x70 0x80; do
    cap_id=$(hexdump -n 1 -s $offset -e '1/1 "%02x\n"' "$CONFIG" 2>/dev/null)
    if [ "$cap_id" = "10" ]; then  # PCIe Capability ID = 0x10
        echo "PCIe Capability found at offset 0x$(printf '%02x' $offset)"
        pcie_cap=$offset
        found=1
        break
    fi
done

if [ $found -eq 0 ]; then
    echo "PCIe Capability not found (this script uses simplified search)"
    echo "For accurate results, use the C program: read_link_status"
    exit 1
fi

# 读取Link Status寄存器（PCIe Cap + 0x12）
link_status_offset=$((pcie_cap + 0x12))
link_status=$(hexdump -n 2 -s $link_status_offset -e '1/2 "%04x\n"' "$CONFIG" 2>/dev/null)

echo ""
echo "=== PCIe Link Status ==="
echo "Link Status Register: 0x$link_status"

# 解析链路状态
speed=$((0x$link_status & 0x0f))
width=$(((0x$link_status >> 4) & 0x3f))
training=$(((0x$link_status >> 11) & 0x01))
clock=$(((0x$link_status >> 15) & 0x01))

echo ""
echo "--- Current Link Status ---"
case $speed in
    1) speed_str="2.5 GT/s (Gen1)" ;;
    2) speed_str="5.0 GT/s (Gen2)" ;;
    3) speed_str="8.0 GT/s (Gen3)" ;;
    4) speed_str="16.0 GT/s (Gen4)" ;;
    5) speed_str="32.0 GT/s (Gen5)" ;;
    *) speed_str="Unknown" ;;
esac

echo "Speed: $speed_str"
echo "Width: x$width"
echo "Link Training: $([ $training -eq 1 ] && echo 'In Progress' || echo 'Complete')"
echo "Slot Clock: $([ $clock -eq 1 ] && echo 'Active' || echo 'Inactive')"

# 读取Link Capabilities（PCIe Cap + 0x0c）
link_cap_offset=$((pcie_cap + 0x0c))
link_cap=$(hexdump -n 4 -s $link_cap_offset -e '1/4 "%08x\n"' "$CONFIG" 2>/dev/null)

max_speed=$((0x$link_cap & 0x0f))
max_width=$(((0x$link_cap >> 4) & 0x3f))

echo ""
echo "--- Link Capabilities ---"
case $max_speed in
    1) max_speed_str="2.5 GT/s (Gen1)" ;;
    2) max_speed_str="5.0 GT/s (Gen2)" ;;
    3) max_speed_str="8.0 GT/s (Gen3)" ;;
    4) max_speed_str="16.0 GT/s (Gen4)" ;;
    5) max_speed_str="32.0 GT/s (Gen5)" ;;
    *) max_speed_str="Unknown" ;;
esac

echo "Maximum Speed: $max_speed_str"
echo "Maximum Width: x$max_width"

# 检查降级
if [ $speed -lt $max_speed ] || [ $width -lt $max_width ]; then
    echo ""
    echo "⚠ Warning: Link operating below maximum capability!"
fi

