#!/bin/bash

# PCIe设备管理脚本
# 使用方法: ./pcie_manager.sh {status|load|unload|info|logs|monitor}

# 配置：修改为你的设备ID
VENDOR_ID="1234"
DEVICE_ID="5678"
DEVICE_PATTERN="${VENDOR_ID}:${DEVICE_ID}"
DRIVER_NAME="pcie_driver"
DEVICE_NODE="/dev/pcie_dev"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 查找设备总线地址
find_device_bus() {
    local bus=$(lspci -n | grep "$DEVICE_PATTERN" | head -1 | cut -d' ' -f1)
    echo "$bus"
}

# 显示设备状态
show_status() {
    echo -e "${GREEN}=== PCIe Device Status ===${NC}"
    local bus=$(find_device_bus)
    if [ -z "$bus" ]; then
        echo -e "${RED}Device $DEVICE_PATTERN not found on PCIe bus${NC}"
        return 1
    fi
    
    echo "Bus: $bus"
    lspci -s "$bus"
    echo ""
    
    echo -e "${GREEN}=== Driver Status ===${NC}"
    if lsmod | grep -q "^${DRIVER_NAME}"; then
        echo -e "${GREEN}Driver is loaded${NC}"
        lsmod | grep "^${DRIVER_NAME}"
    else
        echo -e "${YELLOW}Driver is not loaded${NC}"
    fi
    echo ""
    
    echo -e "${GREEN}=== Device Node ===${NC}"
    if [ -e "$DEVICE_NODE" ]; then
        ls -l "$DEVICE_NODE"
        echo -e "${GREEN}Device node exists${NC}"
    else
        echo -e "${YELLOW}Device node not found${NC}"
    fi
    echo ""
    
    echo -e "${GREEN}=== Interrupt Status ===${NC}"
    local irq=$(cat /proc/interrupts | grep -i "$DRIVER_NAME" | awk '{print $1}' | tr -d ':')
    if [ -n "$irq" ]; then
        echo "IRQ: $irq"
        cat /proc/interrupts | grep -i "$DRIVER_NAME"
    else
        echo -e "${YELLOW}No interrupt registered${NC}"
    fi
}

# 加载驱动
load_driver() {
    echo -e "${GREEN}Loading driver...${NC}"
    
    if lsmod | grep -q "^${DRIVER_NAME}"; then
        echo -e "${YELLOW}Driver already loaded${NC}"
        return 0
    fi
    
    if [ ! -f "${DRIVER_NAME}.ko" ]; then
        echo -e "${RED}Driver file ${DRIVER_NAME}.ko not found${NC}"
        echo "Please compile the driver first: make"
        return 1
    fi
    
    sudo insmod "${DRIVER_NAME}.ko"
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}Driver loaded successfully${NC}"
        sleep 1
        echo ""
        echo "Recent kernel messages:"
        dmesg | tail -10 | grep -i "pcie\|${DRIVER_NAME}" || dmesg | tail -5
    else
        echo -e "${RED}Failed to load driver${NC}"
        echo "Check dmesg for details"
        return 1
    fi
}

# 卸载驱动
unload_driver() {
    echo -e "${GREEN}Unloading driver...${NC}"
    
    if ! lsmod | grep -q "^${DRIVER_NAME}"; then
        echo -e "${YELLOW}Driver is not loaded${NC}"
        return 0
    fi
    
    sudo rmmod "$DRIVER_NAME"
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}Driver unloaded successfully${NC}"
        sleep 1
        echo ""
        echo "Recent kernel messages:"
        dmesg | tail -10 | grep -i "pcie\|${DRIVER_NAME}" || dmesg | tail -5
    else
        echo -e "${RED}Failed to unload driver${NC}"
        echo "Check dmesg for details"
        return 1
    fi
}

# 显示设备详细信息
show_info() {
    local bus=$(find_device_bus)
    if [ -z "$bus" ]; then
        echo -e "${RED}Device $DEVICE_PATTERN not found${NC}"
        return 1
    fi
    
    echo -e "${GREEN}=== Detailed Device Information ===${NC}"
    lspci -vvv -s "$bus"
    echo ""
    
    echo -e "${GREEN}=== PCIe Link Status ===${NC}"
    lspci -vvv -s "$bus" | grep -A 5 "LnkSta"
    echo ""
    
    echo -e "${GREEN}=== Device Resources ===${NC}"
    lspci -vvv -s "$bus" | grep -A 10 "Region"
}

# 显示日志
show_logs() {
    echo -e "${GREEN}=== Recent PCIe Driver Logs ===${NC}"
    dmesg | grep -i "pcie\|${DRIVER_NAME}" | tail -30
    if [ ${PIPESTATUS[0]} -ne 0 ]; then
        echo -e "${YELLOW}No logs found${NC}"
    fi
}

# 实时监控
monitor() {
    echo -e "${GREEN}=== Real-time Monitoring (Press Ctrl+C to exit) ===${NC}"
    echo ""
    
    while true; do
        clear
        echo -e "${GREEN}=== PCIe Device Monitor ===${NC}"
        echo "Time: $(date)"
        echo ""
        
        local bus=$(find_device_bus)
        if [ -n "$bus" ]; then
            echo -e "${GREEN}Device Status:${NC}"
            lspci -s "$bus"
            echo ""
            
            echo -e "${GREEN}Link Status:${NC}"
            lspci -vvv -s "$bus" 2>/dev/null | grep -A 3 "LnkSta" | head -4
            echo ""
        fi
        
        echo -e "${GREEN}Interrupt Statistics:${NC}"
        cat /proc/interrupts | grep -i "$DRIVER_NAME" || echo "No interrupts"
        echo ""
        
        echo -e "${GREEN}Recent Logs:${NC}"
        dmesg | grep -i "pcie\|${DRIVER_NAME}" | tail -5
        echo ""
        
        echo "Press Ctrl+C to exit"
        sleep 2
    done
}

# 主函数
main() {
    case "$1" in
        status)
            show_status
            ;;
        load)
            load_driver
            ;;
        unload)
            unload_driver
            ;;
        reload)
            unload_driver
            sleep 1
            load_driver
            ;;
        info)
            show_info
            ;;
        logs)
            show_logs
            ;;
        monitor)
            monitor
            ;;
        *)
            echo "Usage: $0 {status|load|unload|reload|info|logs|monitor}"
            echo ""
            echo "Commands:"
            echo "  status   - Show device and driver status"
            echo "  load     - Load the driver module"
            echo "  unload   - Unload the driver module"
            echo "  reload   - Reload the driver module"
            echo "  info     - Show detailed device information"
            echo "  logs     - Show recent kernel logs"
            echo "  monitor  - Real-time monitoring (Ctrl+C to exit)"
            exit 1
            ;;
    esac
}

# 检查root权限（某些操作需要）
if [ "$1" = "load" ] || [ "$1" = "unload" ] || [ "$1" = "reload" ]; then
    if [ "$EUID" -ne 0 ]; then
        echo -e "${YELLOW}Note: This operation requires root privileges${NC}"
        echo "The script will use sudo when needed"
    fi
fi

main "$@"

