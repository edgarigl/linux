// SPDX-License-Identifier: GPL-2.0
/*
 * Virtio-msg driver. Based on the virtio-msg-ivshmem driver.
 *
 * Copyright (c) Linaro Ltd, 2024
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/pci.h>
#include <linux/completion.h>

#include "virtio_msg_amp.h"

#define DRV_NAME "virtio_msg_sapphire"

struct sapphire_regs {
	u32 int_status;
};

struct sapphire_dev {
	struct virtio_msg_amp amp_dev;
	struct pci_dev *pdev;
	uint32_t __iomem *cfg_bram;
	struct sapphire_regs __iomem *regs;
	struct hrtimer poll_timer; /* Broken MSI.  */

	int vectors;

	dma_addr_t shmem_dma;

	bool probed_ok;
};

/**
 *  sapphire_irq_handler: IRQ from our PCI device
 */
static irqreturn_t sapphire_irq_handler(int irq, void *dev_id)
{
	struct sapphire_dev *sapphire_dev = (struct sapphire_dev *)dev_id;
	int err;

	/* we always use notify index 0 */
	err = virtio_msg_amp_notify_rx(&sapphire_dev->amp_dev, 0);
	if (err)
		dev_err(&sapphire_dev->pdev->dev, "sapphire IRQ error %d", err);

	return IRQ_HANDLED;
}

/**
 *  sapphire_tx_notify: request from AMP layer to notify our peer
 */
static int sapphire_tx_notify(struct virtio_msg_amp *_amp_dev, u32 notify_idx) {
	struct sapphire_dev *sapphire_dev =
		container_of(_amp_dev, struct sapphire_dev, amp_dev);

	if (notify_idx != 0) {
		dev_warn(&sapphire_dev->pdev->dev, "ivshmem tx_notify_idx not 0");
		notify_idx = 0;
	}

	smp_wmb();
	writel(1, &sapphire_dev->regs->int_status);
	readl(&sapphire_dev->regs->int_status);
	return 0;
}

static struct device *sapphire_get_device(struct virtio_msg_amp *_amp_dev) {
	struct sapphire_dev *sapphire_dev =
		container_of(_amp_dev, struct sapphire_dev, amp_dev);

	return &sapphire_dev->pdev->dev;
}

/**
 *  sapphire_release: release from virtio-msg-amp layer
 *  disable notifications but leave free to the PCI layer callback
 */
static void sapphire_release(struct virtio_msg_amp *_amp_dev) {
	struct sapphire_dev *sapphire_dev =
		container_of(_amp_dev, struct sapphire_dev, amp_dev);

	/* Disable interrupts before we go */
	writel(0, &sapphire_dev->regs->int_status);
	pci_clear_master(sapphire_dev->pdev);
}

static struct virtio_msg_amp_ops sapphire_amp_ops = {
	.tx_notify = sapphire_tx_notify,
	.get_device  = sapphire_get_device,
	.release   = sapphire_release
};

static enum hrtimer_restart sapphire_poll_timer_expired(struct hrtimer *hrtimer)
{
	struct sapphire_dev *sapphire_dev =
		        container_of(hrtimer, struct sapphire_dev, poll_timer);
	int err;

	if (sapphire_dev->probed_ok) {
		printk("STOP polled notifications\n");
		return HRTIMER_NORESTART;
	}

	/* we always use notify index 0 */
	err = virtio_msg_amp_notify_rx(&sapphire_dev->amp_dev, 0);
	if (err)
		dev_err(&sapphire_dev->pdev->dev, "sapphire NOTIFY error %d", err);

	hrtimer_forward_now(hrtimer, ms_to_ktime(50));
        return HRTIMER_RESTART;
}

static int sapphire_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct sapphire_dev *sapphire_dev;
	int err, irq;
	const char *device_name;
	const char *name;
	phys_addr_t addr;
	resource_size_t	size;
	void *bar;
	u64 bar64;

	printk("%s\n", __func__);
	sapphire_dev = devm_kzalloc(&pdev->dev, sizeof(struct sapphire_dev),
				 GFP_KERNEL);
	if (!sapphire_dev) {
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

	err = pcim_iomap_regions(pdev, BIT(0) | BIT(1), device_name);
	if (err) {
		goto error;
	}

	name = "msix (BAR1)";
	addr = pci_resource_start(pdev, 0);
	size = pci_resource_len(pdev, 0);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);

	name = "shmem (BAR2)";
	addr = pci_resource_start(pdev, 1);
	size = pci_resource_len(pdev, 1);
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", name, &addr, &size);

	bar = pcim_iomap_table(pdev)[1];
	bar64 = (uintptr_t) bar;
	sapphire_dev->cfg_bram = bar;
	sapphire_dev->regs = bar + 0x50000 / sizeof(*bar);

	printk("BAR1 %p %p %lx\n", pcim_iomap_table(pdev)[1], bar, (uintptr_t) bar + 0x4000);
	printk("bar=%p\n", bar);
	printk("bar64=0x%llx %llx\n", bar64, bar64 + 0x4000);
	printk("bram=%p\n", sapphire_dev->cfg_bram);
	printk("regs=%p\n", sapphire_dev->regs);

	/*
	 * Grab all vectors although we can only coalesce them into a single
	 * notifier. This avoids missing any event.
	 */
	sapphire_dev->vectors = pci_msix_vec_count(pdev);
	printk("vectors %d\n", sapphire_dev->vectors);
	if (sapphire_dev->vectors < 0)
		sapphire_dev->vectors = 1;

	err = pci_alloc_irq_vectors(pdev, sapphire_dev->vectors,
				    sapphire_dev->vectors,
				    PCI_IRQ_INTX | PCI_IRQ_MSIX);
	if (err < 0)
		goto error;

	for (irq = 0; irq < sapphire_dev->vectors; irq++) {
		err = request_irq(pci_irq_vector(pdev, irq), sapphire_irq_handler,
				  IRQF_SHARED, device_name, sapphire_dev);
		if (err)
			goto error_irq;
	}

	pci_set_drvdata(pdev, sapphire_dev);
	sapphire_dev->pdev = pdev;

	printk("%s: enable bus mastering queue dma 0x%llx\n", __func__,
            sapphire_dev->shmem_dma);
	pci_set_master(pdev);

	/* dma map shmem.  */
	sapphire_dev->amp_dev.shmem = dma_alloc_coherent(&pdev->dev, 8 * 1024,
			                                 &sapphire_dev->shmem_dma,
                                                         GFP_KERNEL);
	sapphire_dev->amp_dev.shmem_size = 8 * 1024;
	memset(sapphire_dev->amp_dev.shmem, 0, sapphire_dev->amp_dev.shmem_size);
	printk("%s: shmem=%p %llx\n", __func__,
            sapphire_dev->amp_dev.shmem,
            sapphire_dev->shmem_dma);

	dev_info(&pdev->dev, "SHMEM @ 0: %32ph \n", sapphire_dev->amp_dev.shmem);

	hrtimer_setup(&sapphire_dev->poll_timer, &sapphire_poll_timer_expired,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	if (0) {
		hrtimer_start(&sapphire_dev->poll_timer, ms_to_ktime(50),
			      HRTIMER_MODE_REL);
	}

	sapphire_dev->amp_dev.ops = &sapphire_amp_ops;
	err = virtio_msg_amp_register(&sapphire_dev->amp_dev);
	if (err)
		goto error_reg;

	addr = sapphire_dev->shmem_dma;
	sapphire_dev->cfg_bram[0x4000/4 + 1] = addr;
	sapphire_dev->cfg_bram[0x4000/4 + 2] = addr >> 32;
	smp_wmb();
	sapphire_dev->cfg_bram[0x4000/4 + 0] = 1;

	sapphire_dev->probed_ok = true;

	dev_info(&pdev->dev, "probe successful\n");

	return 0;

error_reg:
	printk("free coherent\n");
	dma_free_coherent(&pdev->dev, 8 * 1024,
			 sapphire_dev->amp_dev.shmem, sapphire_dev->shmem_dma);

	printk("free coherent done\n");
	pci_clear_master(pdev);

error_irq:
	while (--irq >= 0)
		free_irq(pci_irq_vector(pdev, irq), sapphire_dev);
	pci_free_irq_vectors(pdev);

error:
	dev_info(&pdev->dev, "probe failed!\n");

	return err;
}

static void sapphire_remove(struct pci_dev *pdev)
{
	struct sapphire_dev *sapphire_dev = pci_get_drvdata(pdev);
	int i;

	writel(0, &sapphire_dev->regs->int_status);
	pci_clear_master(pdev);

	virtio_msg_amp_unregister(&sapphire_dev->amp_dev);

	for (i = 0; i < sapphire_dev->vectors; i++)
		free_irq(pci_irq_vector(pdev, i), sapphire_dev);

	pci_free_irq_vectors(pdev);
	dev_info(&pdev->dev, "device removed\n");
}

/* Do the minimal to make device harmless. */
static void sapphire_shutdown(struct pci_dev *pdev)
{
	struct sapphire_dev *sapphire_dev = pci_get_drvdata(pdev);

	/* Need to tell our virtio-msg peer we're going down.  */
	virtio_msg_amp_unregister(&sapphire_dev->amp_dev);
	pci_clear_master(pdev);
}

static const struct pci_device_id sapphire_device_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, 0x9038) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, sapphire_device_id_table);

static struct pci_driver virtio_msg_sapphire_driver = {
	.name = DRV_NAME,
	.id_table = sapphire_device_id_table,
	.probe = sapphire_probe,
	.remove = sapphire_remove,
	.shutdown = sapphire_shutdown,
};
module_pci_driver(virtio_msg_sapphire_driver);

MODULE_AUTHOR("Edgar E. Iglesias <edgar.iglesiass@amd.com>");
MODULE_DESCRIPTION("Virtio-msg generic AMP PCI bus");
MODULE_LICENSE("GPL v2");
