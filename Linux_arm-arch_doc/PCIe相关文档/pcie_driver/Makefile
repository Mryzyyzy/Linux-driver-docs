# Makefile for PCIe Driver

obj-m += pcie_driver.o

# Kernel source directory
# Modify this path to match your kernel source location
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# Current directory
PWD := $(shell pwd)

# Default target
all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

# Clean target
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f *.o *.ko *.mod.c *.mod *.symvers *.order

# Install module
install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

# Load module
load:
	sudo insmod pcie_driver.ko

# Unload module
unload:
	sudo rmmod pcie_driver

# Show module info
info:
	modinfo pcie_driver.ko

# Show kernel messages
logs:
	dmesg | tail -20

.PHONY: all clean install load unload info logs

