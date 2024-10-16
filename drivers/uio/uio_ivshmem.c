// SPDX-License-Identifier: GPL-2.0
/*
 * UIO driver for Inter-VM shared memory PCI device
 *
 * Copyright (c) Siemens AG, 2019
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 */

#include <linux/ivshmem.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/uio_driver.h>

#define DRV_NAME "uio_ivshmem"

struct ivshm_dev {
	struct uio_info info;
	struct pci_dev *pdev;
	struct ivshm_regs __iomem *regs;
	int vectors;
};

static irqreturn_t ivshm_irq_handler(int irq, void *dev_id)
{
	struct ivshm_dev *ivshm_dev = (struct ivshm_dev *)dev_id;

	/* nothing else to do, we configured one-shot interrupt mode */
	uio_event_notify(&ivshm_dev->info);

	return IRQ_HANDLED;
}

static int ivshm_release(struct uio_info *info, struct inode *inode)
{
	struct ivshm_dev *ivshm_dev =
		container_of(info, struct ivshm_dev, info);

	writel(0, &ivshm_dev->regs->int_status);
	return 0;
}

static int ivshm_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ivshm_dev *ivshm_dev;
	phys_addr_t section_addr;
	int err, i;
	struct uio_mem *mem;
	char *device_name;

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

	device_name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s[%s]", DRV_NAME,
				     dev_name(&pdev->dev));
	if (!device_name) {
		err = -ENOMEM;
		goto error;
        }

	ivshm_dev->info.name = device_name;
	ivshm_dev->info.version = "1";
	ivshm_dev->info.release = ivshm_release;

	err = pcim_iomap_regions(pdev, BIT(0), device_name);
	if (err) {
		goto error;
	}

	ivshm_dev->regs = pcim_iomap_table(pdev)[0];

	mem = &ivshm_dev->info.mem[0];

	mem->name = "ivshmem-mmr";
	mem->addr = pci_resource_start(pdev, 0);
	if (!mem->addr) {
		err = -ENODEV;
		goto error;
	}

	/*
	 * Setting mem->size as 4k is a hack. This probably only works on arm64
	 * and other archs that has a 4k page size. What happens here is that,
	 * although the MMRs only requires 16 bytes (or 256, if the 240 bytes of
	 * reserved area is taken in to account), the uio driver, when handling
	 * the mmap() syscall, will allocate a VMA minimum chunck of a page size
	 * and will check if the allocated VMA size is greater than mem->size,
	 * and will fail if it is. Ideally, pci_resource_len(pdev, 0) should
	 * be used to set mem->size below, but this will, as explained, fail the
	 * uio mmap() calls. Maybe that should be a conditional: when the
	 * pci_resource_len returns a size less than a page size, then page size
	 * is used instead because mmap() can't map less than a page size.
	 */
	mem->size = 4096;
	mem->memtype = UIO_MEM_PHYS;

	dev_info(&pdev->dev, "%s at %pa, size %pa\n", mem->name, &mem->addr,
		 &mem->size);

	if (pci_resource_len(pdev, 2) > 0) {
		section_addr = pci_resource_start(pdev, 2);
	} else {
		err = -ENODEV;
		goto error;
	}

	mem++;
	mem->name = "ivshmem-shmem";
	mem->addr = section_addr;
	mem->size = pci_resource_len(pdev, 2);
	mem->memtype = UIO_MEM_IOVA;
	if (!devm_request_mem_region(&pdev->dev, mem->addr, mem->size,
				     device_name)) {
		err = -EBUSY;
		goto error;
	}
	dev_info(&pdev->dev, "%s at %pa, size %pa\n", mem->name, &mem->addr,
		 &mem->size);

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

	for (i = 0; i < ivshm_dev->vectors; i++) {
		err = request_irq(pci_irq_vector(pdev, i), ivshm_irq_handler,
				  IRQF_SHARED, ivshm_dev->info.name, ivshm_dev);
		if (err)
			goto error_irq;
	}

	ivshm_dev->info.irq = UIO_IRQ_CUSTOM;

	err = uio_register_device(&pdev->dev, &ivshm_dev->info);
	if (err)
		goto error_irq;

	pci_set_master(pdev);

	pci_set_drvdata(pdev, ivshm_dev);

	dev_info(&pdev->dev, "module successfully loaded\n");

	return 0;

error_irq:
	while (--i > 0)
		free_irq(pci_irq_vector(pdev, i), ivshm_dev);
	pci_free_irq_vectors(pdev);

error:
	dev_info(&pdev->dev, "module load failed!\n");

	return err;
}

static void ivshm_remove(struct pci_dev *pdev)
{
	struct ivshm_dev *ivshm_dev = pci_get_drvdata(pdev);
	int i;

	writel(0, &ivshm_dev->regs->int_mask);
	pci_clear_master(pdev);

	uio_unregister_device(&ivshm_dev->info);

	for (i = 0; i < ivshm_dev->vectors; i++)
		free_irq(pci_irq_vector(pdev, i), ivshm_dev);

	pci_free_irq_vectors(pdev);
}

static const struct pci_device_id ivshm_device_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_IVSHMEM, PCI_DEVICE_ID_IVSHMEM) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, ivshm_device_id_table);

static struct pci_driver uio_ivshm_driver = {
	.name = DRV_NAME,
	.id_table = ivshm_device_id_table,
	.probe = ivshm_probe,
	.remove = ivshm_remove,
};
module_pci_driver(uio_ivshm_driver);

MODULE_AUTHOR("Jan Kiszka <jan.kiszka@siemens.com>");
MODULE_LICENSE("GPL v2");
