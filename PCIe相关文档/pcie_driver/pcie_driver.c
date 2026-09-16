/*
 * PCIe Driver Example
 * 
 * A simple PCIe driver demonstrating basic PCIe device operations
 * including device detection, initialization, interrupt handling,
 * and cleanup.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include "pcie_driver.h"

#define DRIVER_NAME "pcie_driver"
#define DEVICE_NAME "pcie_dev"
#define CLASS_NAME "pcie_class"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("PCIe Driver Example");
MODULE_VERSION("1.0");

/* Device structure */
struct pcie_device {
    struct pci_dev *pdev;
    void __iomem *bar0;          /* BAR0 memory mapped I/O */
    void __iomem *bar1;          /* BAR1 memory mapped I/O */
    unsigned long bar0_phys;     /* BAR0 physical address */
    unsigned long bar1_phys;     /* BAR1 physical address */
    unsigned long bar0_len;      /* BAR0 length */
    unsigned long bar1_len;      /* BAR1 length */
    int irq;                     /* Interrupt number */
    struct cdev cdev;            /* Character device */
    dev_t devt;                  /* Device number */
    struct class *class;         /* Device class */
    struct device *device;       /* Device */
};

/* Global variables */
static struct pcie_device *pcie_dev;
static int major_number;
static struct class *pcie_class = NULL;
static struct device *pcie_device = NULL;

/* PCIe device IDs - modify these to match your device */
static const struct pci_device_id pcie_ids[] = {
    { PCI_DEVICE(0x1234, 0x5678), },  /* Vendor ID, Device ID */
    { 0, }
};
MODULE_DEVICE_TABLE(pci, pcie_ids);

/* File operations */
static int pcie_open(struct inode *inode, struct file *file)
{
    struct pcie_device *dev = container_of(inode->i_cdev, struct pcie_device, cdev);
    file->private_data = dev;
    return 0;
}

static int pcie_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t pcie_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
    struct pcie_device *dev = file->private_data;
    unsigned long value;
    
    if (!dev || !dev->bar0)
        return -ENODEV;
    
    /* Read from BAR0 register */
    value = ioread32(dev->bar0 + *pos);
    
    if (copy_to_user(buf, &value, min(count, sizeof(value))))
        return -EFAULT;
    
    return min(count, sizeof(value));
}

static ssize_t pcie_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
    struct pcie_device *dev = file->private_data;
    unsigned long value;
    
    if (!dev || !dev->bar0)
        return -ENODEV;
    
    if (copy_from_user(&value, buf, min(count, sizeof(value))))
        return -EFAULT;
    
    /* Write to BAR0 register */
    iowrite32(value, dev->bar0 + *pos);
    
    return min(count, sizeof(value));
}

static long pcie_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct pcie_device *dev = file->private_data;
    
    if (!dev)
        return -ENODEV;
    
    switch (cmd) {
    case PCIE_IOCTL_READ_REG:
        {
            struct pcie_reg_data reg_data;
            if (copy_from_user(&reg_data, (void __user *)arg, sizeof(reg_data)))
                return -EFAULT;
            
            if (reg_data.offset >= dev->bar0_len)
                return -EINVAL;
            
            reg_data.value = ioread32(dev->bar0 + reg_data.offset);
            
            if (copy_to_user((void __user *)arg, &reg_data, sizeof(reg_data)))
                return -EFAULT;
            
            return 0;
        }
    case PCIE_IOCTL_WRITE_REG:
        {
            struct pcie_reg_data reg_data;
            if (copy_from_user(&reg_data, (void __user *)arg, sizeof(reg_data)))
                return -EFAULT;
            
            if (reg_data.offset >= dev->bar0_len)
                return -EINVAL;
            
            iowrite32(reg_data.value, dev->bar0 + reg_data.offset);
            
            return 0;
        }
    default:
        return -ENOTTY;
    }
}

static const struct file_operations pcie_fops = {
    .owner = THIS_MODULE,
    .open = pcie_open,
    .release = pcie_release,
    .read = pcie_read,
    .write = pcie_write,
    .unlocked_ioctl = pcie_ioctl,
};

/* Interrupt handler */
static irqreturn_t pcie_interrupt_handler(int irq, void *dev_id)
{
    struct pcie_device *dev = dev_id;
    u32 status;
    
    if (!dev || !dev->bar0)
        return IRQ_NONE;
    
    /* Read interrupt status register */
    status = ioread32(dev->bar0 + PCIE_STATUS_REG);
    
    if (status & PCIE_STATUS_IRQ) {
        /* Handle interrupt */
        pr_info("PCIe interrupt occurred, status: 0x%x\n", status);
        
        /* Clear interrupt */
        iowrite32(status, dev->bar0 + PCIE_STATUS_REG);
        
        return IRQ_HANDLED;
    }
    
    return IRQ_NONE;
}

/* Initialize PCIe device */
static int pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int ret;
    struct pcie_device *dev;
    
    pr_info("PCIe device detected: Vendor=0x%04x Device=0x%04x\n",
            pdev->vendor, pdev->device);
    
    /* Allocate device structure */
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        pr_err("Failed to allocate device structure\n");
        return -ENOMEM;
    }
    
    dev->pdev = pdev;
    pci_set_drvdata(pdev, dev);
    
    /* Enable PCIe device */
    ret = pci_enable_device(pdev);
    if (ret) {
        pr_err("Failed to enable PCIe device\n");
        goto err_free_dev;
    }
    
    /* Request memory regions */
    ret = pci_request_regions(pdev, DRIVER_NAME);
    if (ret) {
        pr_err("Failed to request PCIe regions\n");
        goto err_disable_device;
    }
    
    /* Map BAR0 (memory or I/O) */
    dev->bar0 = pci_iomap(pdev, 0, 0);
    if (!dev->bar0) {
        pr_err("Failed to map BAR0\n");
        ret = -ENOMEM;
        goto err_release_regions;
    }
    
    /* Get BAR0 physical address and length */
    dev->bar0_phys = pci_resource_start(pdev, 0);
    dev->bar0_len = pci_resource_len(pdev, 0);
    
    pr_info("BAR0: phys=0x%lx, len=0x%lx, virt=%p\n",
            dev->bar0_phys, dev->bar0_len, dev->bar0);
    
    /* Map BAR1 if available */
    if (pci_resource_len(pdev, 1) > 0) {
        dev->bar1 = pci_iomap(pdev, 1, 0);
        if (dev->bar1) {
            dev->bar1_phys = pci_resource_start(pdev, 1);
            dev->bar1_len = pci_resource_len(pdev, 1);
            pr_info("BAR1: phys=0x%lx, len=0x%lx, virt=%p\n",
                    dev->bar1_phys, dev->bar1_len, dev->bar1);
        }
    }
    
    /* Enable bus mastering */
    pci_set_master(pdev);
    
    /* Request interrupt */
    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_LEGACY | PCI_IRQ_MSI);
    if (ret < 0) {
        pr_err("Failed to allocate IRQ vectors\n");
        goto err_unmap_bars;
    }
    
    dev->irq = pci_irq_vector(pdev, 0);
    ret = request_irq(dev->irq, pcie_interrupt_handler,
                      IRQF_SHARED, DRIVER_NAME, dev);
    if (ret) {
        pr_err("Failed to request IRQ %d\n", dev->irq);
        goto err_free_irq_vectors;
    }
    
    pr_info("IRQ %d registered successfully\n", dev->irq);
    
    /* Initialize character device */
    ret = alloc_chrdev_region(&dev->devt, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("Failed to allocate chrdev region\n");
        goto err_free_irq;
    }
    
    major_number = MAJOR(dev->devt);
    cdev_init(&dev->cdev, &pcie_fops);
    dev->cdev.owner = THIS_MODULE;
    
    ret = cdev_add(&dev->cdev, dev->devt, 1);
    if (ret < 0) {
        pr_err("Failed to add cdev\n");
        goto err_unregister_chrdev;
    }
    
    /* Create device class */
    if (!pcie_class) {
        pcie_class = class_create(THIS_MODULE, CLASS_NAME);
        if (IS_ERR(pcie_class)) {
            pr_err("Failed to create device class\n");
            ret = PTR_ERR(pcie_class);
            goto err_del_cdev;
        }
    }
    
    /* Create device */
    pcie_device = device_create(pcie_class, NULL, dev->devt, NULL, DEVICE_NAME);
    if (IS_ERR(pcie_device)) {
        pr_err("Failed to create device\n");
        ret = PTR_ERR(pcie_device);
        goto err_destroy_class;
    }
    
    dev->class = pcie_class;
    dev->device = pcie_device;
    pcie_dev = dev;
    
    pr_info("PCIe driver loaded successfully\n");
    pr_info("Device node: /dev/%s\n", DEVICE_NAME);
    
    return 0;

err_destroy_class:
    class_destroy(pcie_class);
err_del_cdev:
    cdev_del(&dev->cdev);
err_unregister_chrdev:
    unregister_chrdev_region(dev->devt, 1);
err_free_irq:
    free_irq(dev->irq, dev);
err_free_irq_vectors:
    pci_free_irq_vectors(pdev);
err_unmap_bars:
    if (dev->bar1)
        pci_iounmap(pdev, dev->bar1);
    if (dev->bar0)
        pci_iounmap(pdev, dev->bar0);
err_release_regions:
    pci_release_regions(pdev);
err_disable_device:
    pci_disable_device(pdev);
err_free_dev:
    kfree(dev);
    return ret;
}

/* Remove PCIe device */
static void pcie_remove(struct pci_dev *pdev)
{
    struct pcie_device *dev = pci_get_drvdata(pdev);
    
    if (!dev)
        return;
    
    pr_info("Removing PCIe device\n");
    
    /* Remove device */
    if (pcie_device) {
        device_destroy(pcie_class, dev->devt);
        pcie_device = NULL;
    }
    
    /* Remove class */
    if (pcie_class) {
        class_destroy(pcie_class);
        pcie_class = NULL;
    }
    
    /* Remove character device */
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devt, 1);
    
    /* Free interrupt */
    if (dev->irq) {
        free_irq(dev->irq, dev);
        pci_free_irq_vectors(pdev);
    }
    
    /* Unmap BARs */
    if (dev->bar1)
        pci_iounmap(pdev, dev->bar1);
    if (dev->bar0)
        pci_iounmap(pdev, dev->bar0);
    
    /* Release regions */
    pci_release_regions(pdev);
    
    /* Disable device */
    pci_disable_device(pdev);
    
    /* Free device structure */
    kfree(dev);
    pcie_dev = NULL;
    
    pr_info("PCIe driver unloaded\n");
}

/* PCIe driver structure */
static struct pci_driver pcie_driver = {
    .name = DRIVER_NAME,
    .id_table = pcie_ids,
    .probe = pcie_probe,
    .remove = pcie_remove,
};

/* Module initialization */
static int __init pcie_init(void)
{
    pr_info("Loading PCIe driver module\n");
    return pci_register_driver(&pcie_driver);
}

/* Module cleanup */
static void __exit pcie_exit(void)
{
    pr_info("Unloading PCIe driver module\n");
    pci_unregister_driver(&pcie_driver);
}

module_init(pcie_init);
module_exit(pcie_exit);

