/*
 * PCIe Driver Header File
 */

#ifndef PCIE_DRIVER_H
#define PCIE_DRIVER_H

#include <linux/ioctl.h>

/* IOCTL commands */
#define PCIE_IOCTL_MAGIC 'P'

#define PCIE_IOCTL_READ_REG  _IOWR(PCIE_IOCTL_MAGIC, 1, struct pcie_reg_data)
#define PCIE_IOCTL_WRITE_REG _IOW(PCIE_IOCTL_MAGIC, 2, struct pcie_reg_data)

#define PCIE_IOCTL_MAXNR 2

/* Register offsets (example - modify according to your device) */
#define PCIE_STATUS_REG   0x00
#define PCIE_CONTROL_REG  0x04
#define PCIE_DATA_REG     0x08

/* Status register bits */
#define PCIE_STATUS_IRQ   0x01
#define PCIE_STATUS_READY 0x02
#define PCIE_STATUS_ERROR 0x04

/* Control register bits */
#define PCIE_CTRL_ENABLE  0x01
#define PCIE_CTRL_RESET   0x02

/* Register data structure for IOCTL */
struct pcie_reg_data {
    unsigned int offset;
    unsigned int value;
};

#endif /* PCIE_DRIVER_H */

