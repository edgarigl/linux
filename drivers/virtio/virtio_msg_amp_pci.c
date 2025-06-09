// SPDX-License-Identifier: GPL-2.0
/*
 * Virtio-msg-bus AMP PCI driver. 
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/pci.h>
#include <linux/completion.h>

#include "virtio_msg_amp.h"

#define DRV_NAME "virtio_msg_amp_pci"

struct vmsg_regs {
	u32 version;
	u32 features;
	u64 res1[3];

	/* 32B offset. */
	uint32_t notify[4];
};

struct vmsg_dev {
	struct virtio_msg_amp amp_dev;
	struct pci_dev *pdev;
	struct vmsg_regs __iomem *regs;
	void __iomem *ram;

	int vectors;
};

/**
 *  vmsg_irq_handler: IRQ from our PCI device
 */
static irqreturn_t vmsg_irq_handler(int irq, void *dev_id)
{
	struct vmsg_dev *vmsg_dev = (struct vmsg_dev *)dev_id;
	int err;

	/* we always use notify index 0 */
	err = virtio_msg_amp_notify_rx(&vmsg_dev->amp_dev, 0);
	if (err)
		dev_err(&vmsg_dev->pdev->dev, "sapphire IRQ error %d", err);

	return IRQ_HANDLED;
}

/**
 *  vmsg_tx_notify: request from AMP layer to notify our peer
 */
static int vmsg_tx_notify(struct virtio_msg_amp *_amp_dev, u32 notify_idx) {
	struct vmsg_dev *vmsg_dev =
		container_of(_amp_dev, struct vmsg_dev, amp_dev);

	if (notify_idx != 0) {
		dev_warn(&vmsg_dev->pdev->dev, "vmsg: tx_notify_idx not 0");
		notify_idx = 0;
	}

	smp_wmb();
	writel(1, &vmsg_dev->regs->notify[0]);
	return 0;
}

static struct device *vmsg_get_device(struct virtio_msg_amp *_amp_dev) {
	struct vmsg_dev *vmsg_dev =
		container_of(_amp_dev, struct vmsg_dev, amp_dev);

	return &vmsg_dev->pdev->dev;
}

/**
 *  vmsg_release: release from virtio-msg-amp layer
 *  disable notifications but leave free to the PCI layer callback
 */
static void vmsg_release(struct virtio_msg_amp *_amp_dev) {
	struct vmsg_dev *vmsg_dev =
		container_of(_amp_dev, struct vmsg_dev, amp_dev);
	pci_clear_master(vmsg_dev->pdev);
}

static struct virtio_msg_amp_ops vmsg_amp_ops = {
	.tx_notify = vmsg_tx_notify,
	.get_device  = vmsg_get_device,
	.release   = vmsg_release
};

static int vmsg_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct vmsg_dev *vmsg_dev;
	int err, irq;
	const char *device_name;
	const char *name;
	phys_addr_t addr;
	resource_size_t	size;
	int i;

	vmsg_dev = devm_kzalloc(&pdev->dev, sizeof(struct vmsg_dev),
			        GFP_KERNEL);
	if (!vmsg_dev) {
		err = -ENOMEM;
		goto error;
	}

	err = pcim_enable_device(pdev);
	if (err) {
		goto error;
	}

	device_name = dev_name(&pdev->dev);
	dev_info(&pdev->dev, "device_name=%s\n", device_name);
	if (!device_name) {
		err = -ENOMEM;
		goto error;
	}

	/* Request 3 BARs.  */
	for (i = 0; i < 3; i++) {
		err = pcim_request_region(pdev, i, device_name);
		if (err) {
			goto error;
		}
	}

	name = "mmr (BAR0)";
	addr = pci_resource_start(pdev, 0);
	size = pci_resource_len(pdev, 0);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);

	name = "mmr (BAR1)";
	addr = pci_resource_start(pdev, 1);
	size = pci_resource_len(pdev, 1);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);

	name = "mmr (BAR2)";
	addr = pci_resource_start(pdev, 2);
	size = pci_resource_len(pdev, 2);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);

	vmsg_dev->regs = pcim_iomap(pdev, 0, 0);
	vmsg_dev->ram = pci_iomap_wc(pdev, 1, 0);

	printk("ram=%p\n", vmsg_dev->ram);
	if (!vmsg_dev->regs || !vmsg_dev->ram) {
		err = -ENOMEM;
		goto error;
	}

	/*
	 * Grab all vectors although we can only coalesce them into a single
	 * notifier. This avoids missing any event.
	 */
	vmsg_dev->vectors = pci_msix_vec_count(pdev);
	printk("vectors %d\n", vmsg_dev->vectors);
	if (vmsg_dev->vectors < 0)
		vmsg_dev->vectors = 1;

	err = pci_alloc_irq_vectors(pdev, vmsg_dev->vectors,
				    vmsg_dev->vectors,
				    PCI_IRQ_MSIX);
	if (err < 0) {
		printk("FAILED: alloc irq\n");
		goto error;
	}

	for (irq = 0; irq < vmsg_dev->vectors; irq++) {
		err = request_irq(pci_irq_vector(pdev, irq), vmsg_irq_handler,
				  IRQF_SHARED, device_name, vmsg_dev);
		if (err) {
			printk("FAILED: request_irq %d\n", irq);
			goto error_irq;
		}
	}

	pci_set_drvdata(pdev, vmsg_dev);
	vmsg_dev->pdev = pdev;

	pci_set_master(pdev);

	/* dma map shmem.  */
	vmsg_dev->amp_dev.shmem = vmsg_dev->ram;
	vmsg_dev->amp_dev.shmem_size = 8 * 1024;

	dev_info(&pdev->dev, "SHMEM @ 0: %32ph \n", vmsg_dev->amp_dev.shmem);

	vmsg_dev->amp_dev.ops = &vmsg_amp_ops;
	err = virtio_msg_amp_register(&vmsg_dev->amp_dev);
	if (err)
		goto error_reg;

	dev_info(&pdev->dev, "probe successful\n");

	return 0;

error_reg:
	pci_clear_master(pdev);

error_irq:
	while (--irq >= 0)
		free_irq(pci_irq_vector(pdev, irq), vmsg_dev);
	pci_free_irq_vectors(pdev);

	pci_iounmap(pdev, vmsg_dev->ram);
error:
	dev_info(&pdev->dev, "probe failed!\n");

	return err;
}

static void vmsg_remove(struct pci_dev *pdev)
{
	struct vmsg_dev *vmsg_dev = pci_get_drvdata(pdev);
	int i;

	pci_iounmap(pdev, vmsg_dev->ram);
	pci_clear_master(pdev);

	virtio_msg_amp_unregister(&vmsg_dev->amp_dev);

	for (i = 0; i < vmsg_dev->vectors; i++)
		free_irq(pci_irq_vector(pdev, i), vmsg_dev);

	pci_free_irq_vectors(pdev);
	dev_info(&pdev->dev, "device removed\n");
}

static const struct pci_device_id vmsg_device_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, 0x9038) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, vmsg_device_id_table);

static struct pci_driver virtio_msg_vmsg_driver = {
	.name = DRV_NAME,
	.id_table = vmsg_device_id_table,
	.probe = vmsg_probe,
	.remove = vmsg_remove,
};
module_pci_driver(virtio_msg_vmsg_driver);

MODULE_AUTHOR("Edgar E. Iglesias <edgar.iglesiass@amd.com>");
MODULE_DESCRIPTION("Virtio-msg generic AMP PCI bus");
MODULE_LICENSE("GPL v2");
