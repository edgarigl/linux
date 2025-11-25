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
#include <linux/list.h>
#include <linux/iopoll.h>
#include <linux/mm.h>

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

	struct virtio_msg_user_device vmudev;
	struct spsc_queue user_drv2dev;
	struct spsc_queue user_dev2drv;
	spinlock_t user_lock;
	struct list_head user_pending;
	struct sapphire_user_msg *user_current;
	bool user_registered;
	dma_addr_t user_phys;
	size_t user_size;
	resource_size_t bar3_start;
	resource_size_t bar3_size;
};

struct sapphire_user_msg {
	struct list_head list;
	u16 len;
	struct virtio_msg msg;
};

#define SAPPHIRE_CFG_OFFSET	(0x4000 / sizeof(u32))
#define SAPPHIRE_CFG_READY	(SAPPHIRE_CFG_OFFSET + 0)
#define SAPPHIRE_CFG_ADDR_LO	(SAPPHIRE_CFG_OFFSET + 1)
#define SAPPHIRE_CFG_ADDR_HI	(SAPPHIRE_CFG_OFFSET + 2)

#define SAPPHIRE_PAGE_SIZE	SZ_4K
#define SAPPHIRE_MSG_BUF_SIZE	64

static int sapphire_tx_notify(struct virtio_msg_amp *_amp_dev, u32 notify_idx);

static inline struct sapphire_dev *vmudev_to_sapphire(struct virtio_msg_user_device *vmudev)
{
	return container_of(vmudev, struct sapphire_dev, vmudev);
}

static int sapphire_cfg_queue(struct sapphire_dev *sapphire_dev,
				dma_addr_t phys, u32 ready)
{
	u32 val;

	writel(lower_32_bits(phys), &sapphire_dev->cfg_bram[SAPPHIRE_CFG_ADDR_LO]);
	writel(upper_32_bits(phys), &sapphire_dev->cfg_bram[SAPPHIRE_CFG_ADDR_HI]);
	wmb();
	writel(ready, &sapphire_dev->cfg_bram[SAPPHIRE_CFG_READY]);
	wmb();

	return readl_poll_timeout(&sapphire_dev->cfg_bram[SAPPHIRE_CFG_READY],
				val, val == 0, 1, 1000);
}

static struct sapphire_user_msg *sapphire_user_msg_alloc(const struct virtio_msg *msg,
					      u16 len, gfp_t gfp)
{
	size_t alloc = sizeof(struct sapphire_user_msg) + len - sizeof(struct virtio_msg);
	struct sapphire_user_msg *node;

	node = kzalloc(alloc, gfp);
	if (!node)
		return NULL;

	node->len = len;
	memcpy(&node->msg, msg, len);

	return node;
}

static void sapphire_user_try_deliver_locked(struct sapphire_dev *sapphire_dev)
{
	struct sapphire_user_msg *node;

	if (sapphire_dev->user_current)
		return;

	if (READ_ONCE(sapphire_dev->vmudev.vmsg))
		return;

	if (list_empty(&sapphire_dev->user_pending))
		return;

	node = list_first_entry(&sapphire_dev->user_pending,
				struct sapphire_user_msg, list);
	list_del_init(&node->list);
	sapphire_dev->user_current = node;
	WRITE_ONCE(sapphire_dev->vmudev.vmsg, &node->msg);
	complete(&sapphire_dev->vmudev.r_completion);
	wake_up_interruptible(&sapphire_dev->vmudev.poll_wq);
}

static void sapphire_user_enqueue_rx(struct sapphire_dev *sapphire_dev,
				      struct virtio_msg *msg, u16 len,
				      gfp_t gfp)
{
	struct sapphire_user_msg *node;
	unsigned long flags;

	node = sapphire_user_msg_alloc(msg, len, gfp);
	if (!node)
		return;

	spin_lock_irqsave(&sapphire_dev->user_lock, flags);
	list_add_tail(&node->list, &sapphire_dev->user_pending);
	sapphire_user_try_deliver_locked(sapphire_dev);
	spin_unlock_irqrestore(&sapphire_dev->user_lock, flags);
}

static void sapphire_user_process_rx(struct sapphire_dev *sapphire_dev)
{
	u8 buf[SAPPHIRE_MSG_BUF_SIZE];
	struct virtio_msg *msg = (struct virtio_msg *)buf;
	u32 len;

	while (spsc_recv(&sapphire_dev->user_dev2drv, buf, sizeof(buf))) {
		len = le16_to_cpu(msg->msg_size);
		if (!len)
			len = VIRTIO_MSG_MIN_SIZE;
		len = min_t(u32, len, VIRTIO_MSG_MAX_SIZE);
		len = min_t(u32, len, (u32)sizeof(buf));
		sapphire_user_enqueue_rx(sapphire_dev, msg, (u16)len, GFP_ATOMIC);
	}
}

static int sapphire_user_handle(struct virtio_msg_user_device *vmudev,
				   struct virtio_msg *msg)
{
	struct sapphire_dev *sapphire_dev = vmudev_to_sapphire(vmudev);
	u32 len = le16_to_cpu(msg->msg_size);

	if (!len)
		len = VIRTIO_MSG_MIN_SIZE;
	len = min_t(u32, len, VIRTIO_MSG_MAX_SIZE);
	len = min_t(u32, len, (u32)SAPPHIRE_MSG_BUF_SIZE);

	if (!spsc_send(&sapphire_dev->user_drv2dev, msg, len))
		return -EBUSY;

	smp_wmb();
	sapphire_tx_notify(&sapphire_dev->amp_dev, 0);

	return 0;
}

static void sapphire_user_refill(struct virtio_msg_user_device *vmudev)
{
	struct sapphire_dev *sapphire_dev = vmudev_to_sapphire(vmudev);
	struct sapphire_user_msg *node;
	unsigned long flags;

	spin_lock_irqsave(&sapphire_dev->user_lock, flags);
	node = sapphire_dev->user_current;
	sapphire_dev->user_current = NULL;
	WRITE_ONCE(vmudev->vmsg, NULL);
	if (node)
		kfree(node);
	sapphire_user_try_deliver_locked(sapphire_dev);
	spin_unlock_irqrestore(&sapphire_dev->user_lock, flags);
}

static int sapphire_user_mmap(struct virtio_msg_user_device *vmudev,
			    struct vm_area_struct *vma)
{
	struct sapphire_dev *sapphire_dev = vmudev_to_sapphire(vmudev);
	unsigned long size = vma->vm_end - vma->vm_start;
	resource_size_t offset = (resource_size_t)vma->vm_pgoff << PAGE_SHIFT;
	resource_size_t phys;

	if (!sapphire_dev->bar3_size)
		return -ENODEV;

	if (offset >= sapphire_dev->bar3_size)
		return -EINVAL;

	if (size > sapphire_dev->bar3_size - offset)
		return -EINVAL;

	phys = sapphire_dev->bar3_start + offset;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);

	if (remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT,
			 size, vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

static struct virtio_msg_user_ops sapphire_user_uops = {
	.handle = sapphire_user_handle,
	.refill = sapphire_user_refill,
};

static void sapphire_user_cleanup(struct sapphire_dev *sapphire_dev)
{
	struct sapphire_user_msg *node;
	unsigned long flags;

	if (!sapphire_dev->user_registered)
		return;

	virtio_msg_user_unregister(&sapphire_dev->vmudev);
	sapphire_dev->user_registered = false;

	spin_lock_irqsave(&sapphire_dev->user_lock, flags);
	if (sapphire_dev->user_current) {
		kfree(sapphire_dev->user_current);
		sapphire_dev->user_current = NULL;
	}
	while (!list_empty(&sapphire_dev->user_pending)) {
		node = list_first_entry(&sapphire_dev->user_pending,
					 struct sapphire_user_msg, list);
		list_del(&node->list);
		kfree(node);
	}
	spin_unlock_irqrestore(&sapphire_dev->user_lock, flags);
}

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

	sapphire_user_process_rx(sapphire_dev);

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

	sapphire_user_process_rx(sapphire_dev);

	hrtimer_forward_now(hrtimer, ms_to_ktime(50));
        return HRTIMER_RESTART;
}

static int sapphire_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct sapphire_dev *sapphire_dev;
	int err, irq, ret;
	const char *device_name;
	const char *name;
	phys_addr_t addr;
	resource_size_t	size;
	void *bar;
	u64 bar64;
	char *shmem;

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

	addr = pci_resource_start(pdev, 2);
	size = pci_resource_len(pdev, 2);
	sapphire_dev->bar3_start = addr;
	sapphire_dev->bar3_size = size;
	dev_info(&pdev->dev, "BAR3 (user window) at %pa, size %pa\n", &addr, &size);

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
	sapphire_dev->amp_dev.shmem = dma_alloc_coherent(&pdev->dev, 16 * 1024,
				                                 &sapphire_dev->shmem_dma,
	                                                        GFP_KERNEL);
	sapphire_dev->amp_dev.shmem_size = 16 * 1024;
	memset(sapphire_dev->amp_dev.shmem, 0, sapphire_dev->amp_dev.shmem_size);
	printk("%s: shmem=%p %llx\n", __func__,
            sapphire_dev->amp_dev.shmem,
            sapphire_dev->shmem_dma);

	spin_lock_init(&sapphire_dev->user_lock);
	INIT_LIST_HEAD(&sapphire_dev->user_pending);
	sapphire_dev->user_current = NULL;
	sapphire_dev->user_registered = false;

	sapphire_dev->user_phys = sapphire_dev->shmem_dma + 2 * SAPPHIRE_PAGE_SIZE;
	sapphire_dev->user_size = 2 * SAPPHIRE_PAGE_SIZE;

	shmem = sapphire_dev->amp_dev.shmem;

	spsc_init(&sapphire_dev->user_drv2dev, "user-drv2dev",
		  spsc_capacity(SAPPHIRE_PAGE_SIZE),
		  shmem + 2 * SAPPHIRE_PAGE_SIZE);
	spsc_init(&sapphire_dev->user_dev2drv, "user-dev2drv",
		  spsc_capacity(SAPPHIRE_PAGE_SIZE),
		  shmem + 3 * SAPPHIRE_PAGE_SIZE);

	sapphire_dev->vmudev.ops = &sapphire_user_uops;
	sapphire_dev->vmudev.parent = &pdev->dev;
	sapphire_dev->vmudev.mmap = sapphire_user_mmap;

ret = virtio_msg_user_register(&sapphire_dev->vmudev);
if (ret) {
	err = ret;
	goto error_user_register;
}

	sapphire_dev->user_registered = true;

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

	addr = sapphire_dev->user_phys;
	ret = sapphire_cfg_queue(sapphire_dev, addr, 1);
	if (ret)
		dev_warn(&pdev->dev, "Timeout configuring userspace queue\n");

	addr = sapphire_dev->shmem_dma;
	ret = sapphire_cfg_queue(sapphire_dev, addr, 2);
	if (ret)
		dev_warn(&pdev->dev, "Timeout configuring kernel queue\n");

	sapphire_dev->probed_ok = true;

	dev_info(&pdev->dev, "probe successful\n");

	return 0;

error_user_register:
	goto error_reg;

error_reg:
	sapphire_user_cleanup(sapphire_dev);
	printk("free coherent\n");
	dma_free_coherent(&pdev->dev, 16 * 1024,
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

	sapphire_user_cleanup(sapphire_dev);

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
	sapphire_user_cleanup(sapphire_dev);
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
