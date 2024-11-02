// SPDX-License-Identifier: GPL-2.0
/*
 * Virtio-msg-amp driver for Inter-VM shared memory PCI device
 *
 * Copyright (c) Linaro Ltd, 2024
 *
 * Based partially on a uio driver for ivshmem PCI driver by:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 */

#include <linux/ivshmem.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/completion.h>

#include "virtio_msg_amp.h"

#define DRV_NAME "virtio_msg_ivshmem"

struct ivshm_dev {
	struct virtio_msg_amp amp_dev;
	struct pci_dev *pdev;
	struct ivshm_regs __iomem *regs;
	int vectors;
	u32 our_vmid;
	u32 peer_vmid;
};

/**
 *  ivshm_irq_handler: IRQ from our PCI device
 */
static irqreturn_t ivshm_irq_handler(int irq, void *dev_id)
{
	struct ivshm_dev *ivshm_dev = (struct ivshm_dev *)dev_id;
	int err;

	/* we always use notify index 0 */
	err = virtio_msg_amp_notify_rx(&ivshm_dev->amp_dev, 0);
	if (err)
		dev_err(&ivshm_dev->pdev->dev, "ivshmem IRQ error %d", err);
	//else
	//	dev_info(&ivshm_dev->pdev->dev, "ivshmem IRQ fired");

	return IRQ_HANDLED;
}

/**
 *  ivshm_tx_notify: request from AMP layer to notify our peer
 */
static int ivshm_tx_notify(struct virtio_msg_amp *_amp_dev, u32 notify_idx) {
	struct ivshm_dev *ivshm_dev =
		container_of(_amp_dev, struct ivshm_dev, amp_dev);

	if (notify_idx != 0) {
		dev_warn(&ivshm_dev->pdev->dev, "ivshmem tx_notify_idx not 0");
		notify_idx = 0;
	}

	/* Notify peer by writing its peer ID to the DOORBELL register */
	writel((ivshm_dev->peer_vmid << 16) | notify_idx,
		&ivshm_dev->regs->doorbell);

	return 0;
}

static struct device *ivshm_get_device(struct virtio_msg_amp *_amp_dev) {
	struct ivshm_dev *ivshm_dev =
		container_of(_amp_dev, struct ivshm_dev, amp_dev);

	return &ivshm_dev->pdev->dev;
}

/**
 *  ivshm_release: release from virtio-msg-amp layer
 *  disable notifications but leave free to the PCI layer callback
 */
static void ivshm_release(struct virtio_msg_amp *_amp_dev) {
	struct ivshm_dev *ivshm_dev =
		container_of(_amp_dev, struct ivshm_dev, amp_dev);

	/* Mask interrupts before we go */
	writel(0, &ivshm_dev->regs->int_mask);
	pci_clear_master(ivshm_dev->pdev);
}

static struct virtio_msg_amp_ops ivshm_amp_ops = {
	.tx_notify = ivshm_tx_notify,
	.get_device  = ivshm_get_device,
	.release   = ivshm_release
};

static int ivshm_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ivshm_dev *ivshm_dev;
	int err, irq;
	const char *device_name;
	const char *name;
	phys_addr_t addr;
	resource_size_t	size;
	u32 vmid;

	ivshm_dev = devm_kzalloc(&pdev->dev, sizeof(struct ivshm_dev),
				 GFP_KERNEL);
	if (!ivshm_dev) {
		err = -ENOMEM;
		goto error;
	}

	err = pcim_enable_device(pdev);
	if (err) {
		goto error;
	}

	device_name = dev_name(&pdev->dev);
	dev_info(&pdev->dev, "device_name=%s\n", device_name);
	//devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s[%s]", DRV_NAME,
	//			     dev_name(&pdev->dev));
	if (!device_name) {
		err = -ENOMEM;
		goto error;
	}

	err = pcim_iomap_regions(pdev, BIT(2) | BIT(0), device_name);
	if (err) {
		goto error;
	}

	name = "mmr (BAR0)";
	addr = pci_resource_start(pdev, 0);
	size = pci_resource_len(pdev, 0);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);

	name = "msix (BAR1)";
	addr = pci_resource_start(pdev, 1);
	size = pci_resource_len(pdev, 1);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);

	name = "shmem (BAR2)";
	addr = pci_resource_start(pdev, 2);
	size = pci_resource_len(pdev, 2);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);
	ivshm_dev->amp_dev.shmem_size = size;

	ivshm_dev->regs  = pcim_iomap_table(pdev)[0];
	ivshm_dev->amp_dev.shmem = pcim_iomap_table(pdev)[2];

	vmid = readl(&ivshm_dev->regs->ivposition);
	ivshm_dev->our_vmid = vmid;
	dev_info(&pdev->dev, "VMID=%x\n", vmid);

	/* HACK: fixme, get from AMP info published by peer */
	ivshm_dev->peer_vmid = 0;

	dev_info(&pdev->dev, "SHMEM @ 0: %32ph \n", ivshm_dev->amp_dev.shmem);

	/*
	 * Grab all vectors although we can only coalesce them into a single
	 * notifier. This avoids missing any event.
	 */
	ivshm_dev->vectors = pci_msix_vec_count(pdev);
	if (ivshm_dev->vectors < 0)
		ivshm_dev->vectors = 1;

	err = pci_alloc_irq_vectors(pdev, ivshm_dev->vectors,
				    ivshm_dev->vectors,
				    PCI_IRQ_INTX | PCI_IRQ_MSIX);
	if (err < 0)
		goto error;

	for (irq = 0; irq < ivshm_dev->vectors; irq++) {
		err = request_irq(pci_irq_vector(pdev, irq), ivshm_irq_handler,
				  IRQF_SHARED, device_name, ivshm_dev);
		if (err)
			goto error_irq;
	}

	pci_set_drvdata(pdev, ivshm_dev);
	ivshm_dev->pdev = pdev;

	pci_set_master(pdev);

	ivshm_dev->amp_dev.ops = &ivshm_amp_ops;
	err = virtio_msg_amp_register(&ivshm_dev->amp_dev);
	if (err)
		goto error_reg;

	dev_info(&pdev->dev, "probe successful\n");

	return 0;

error_reg:
	pci_clear_master(pdev);

error_irq:
	while (--irq >= 0)
		free_irq(pci_irq_vector(pdev, irq), ivshm_dev);
	pci_free_irq_vectors(pdev);

error:
	dev_info(&pdev->dev, "probe failed!\n");

	return err;
}

static void ivshm_remove(struct pci_dev *pdev)
{
	struct ivshm_dev *ivshm_dev = pci_get_drvdata(pdev);
	int i;

	writel(0, &ivshm_dev->regs->int_mask);
	pci_clear_master(pdev);

	virtio_msg_amp_unregister(&ivshm_dev->amp_dev);

	for (i = 0; i < ivshm_dev->vectors; i++)
		free_irq(pci_irq_vector(pdev, i), ivshm_dev);

	pci_free_irq_vectors(pdev);
	dev_info(&pdev->dev, "device removed\n");
}

static const struct pci_device_id ivshm_device_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_IVSHMEM, PCI_DEVICE_ID_IVSHMEM) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, ivshm_device_id_table);

static struct pci_driver virtio_msg_ivshm_driver = {
	.name = DRV_NAME,
	.id_table = ivshm_device_id_table,
	.probe = ivshm_probe,
	.remove = ivshm_remove,
};
module_pci_driver(virtio_msg_ivshm_driver);

MODULE_AUTHOR("Bill Mills <bill.mills@linaro.org>");
MODULE_LICENSE("GPL v2");
