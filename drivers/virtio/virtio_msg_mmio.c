// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtio message transport - MMIO based channel interface.
 *
 * Copyright (C) 2024 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This implements the channel interface for Virtio msg transport via memory
 * mapped IO.
 */

#define pr_fmt(fmt) "virtio-msg-mmio: " fmt

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <uapi/linux/virtio_mmio.h>

#include "virtio_msg.h"

struct virtio_msg_mmio_device {
	struct virtio_msg_device vmdev;
	struct platform_device *pdev;

	void __iomem *base;
};

#define to_virtio_msg_mmio_device(_vmdev) \
	container_of(_vmdev, struct virtio_msg_mmio_device, vmdev)

static irqreturn_t vm_interrupt(int irq, void *opaque)
{
	struct virtio_msg_mmio_device *vmmdev = opaque;
	struct virtio_msg_vq *info;
	struct virtio_msg msg;
	bool handled = false;

	/* We don't have any msg here, lets create one to make it work */
	memset(&msg, 0, sizeof(msg));
	msg.id = VIRTIO_MSG_EVENT_USED;
	msg.event_used.index = 0;

	/* Call the interrupt handler for each virtqueue */
	list_for_each_entry(info, &vmmdev->vmdev.virtqueues, node) {
		if (!virtio_msg_receive(&vmmdev->vmdev, &msg)) {
			handled = true;
			break;
		}
		msg.event_used.index++;
	}

	/* Interrupt should belong to one of the virtqueues at least */
	if (!handled) {
		pr_err("%s: Failed to find virtqueue for message", __func__);
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static int virtio_msg_mmio_send(struct virtio_msg_device *vmdev,
				struct virtio_msg *request,
				struct virtio_msg *response)
{
	struct virtio_msg_mmio_device *vmmdev = to_virtio_msg_mmio_device(vmdev);
	int i, len = sizeof(*request) / sizeof(u64);
	u64 *data = (u64 *) request;
	u64 *addr = vmmdev->base;

	for (i = 0; i < len; i++)
		writeq(*(data + i), addr + i);

	if (!response)
		return 0;

	data = (u64 *) response;

	for (i = 0; i < len; i++)
		*(data + i) = readq(addr + i);

	return 0;
}

static const char *virtio_msg_mmio_bus_name(struct virtio_msg_device *vmdev)
{
	struct virtio_msg_mmio_device *vmmdev = to_virtio_msg_mmio_device(vmdev);

	return vmmdev->pdev->name;
}

static void virtio_msg_mmio_synchronize_cbs(struct virtio_msg_device *vmdev)
{
	struct virtio_msg_mmio_device *vmmdev = to_virtio_msg_mmio_device(vmdev);

	synchronize_irq(platform_get_irq(vmmdev->pdev, 0));
}

static void virtio_msg_mmio_release(struct virtio_msg_device *vmdev)
{
	struct virtio_msg_mmio_device *vmmdev = to_virtio_msg_mmio_device(vmdev);

	kfree(vmmdev);
}

static int virtio_msg_mmio_vqs_prepare(struct virtio_msg_device *vmdev)
{
	struct virtio_msg_mmio_device *vmmdev = to_virtio_msg_mmio_device(vmdev);
	int ret, irq = platform_get_irq(vmmdev->pdev, 0);

	if (irq < 0)
		return irq;

	ret = request_irq(irq, vm_interrupt, IRQF_SHARED, dev_name(&vmdev->vdev.dev),
			  vmmdev);
	if (ret)
		return ret;

	if (of_property_read_bool(vmmdev->pdev->dev.of_node, "wakeup-source"))
		enable_irq_wake(irq);

	return 0;
}

static void virtio_msg_mmio_vqs_release(struct virtio_msg_device *vmdev)
{
	struct virtio_msg_mmio_device *vmmdev = to_virtio_msg_mmio_device(vmdev);

	free_irq(platform_get_irq(vmmdev->pdev, 0), vmmdev);
}

static struct virtio_msg_ops vmm_ops = {
	.send = virtio_msg_mmio_send,
	.bus_name = virtio_msg_mmio_bus_name,
	.synchronize_cbs = virtio_msg_mmio_synchronize_cbs,
	.release = virtio_msg_mmio_release,
	.prepare_vqs = virtio_msg_mmio_vqs_prepare,
	.release_vqs = virtio_msg_mmio_vqs_release,
};

#ifdef CONFIG_PM_SLEEP
static int virtio_msg_mmio_freeze(struct device *dev)
{
	struct virtio_msg_mmio_device *vmmdev = dev_get_drvdata(dev);

	return virtio_msg_freeze(&vmmdev->vmdev);
}

static int virtio_msg_mmio_restore(struct device *dev)
{
	struct virtio_msg_mmio_device *vmmdev = dev_get_drvdata(dev);

	return virtio_msg_restore(&vmmdev->vmdev);
}

const struct dev_pm_ops virtio_msg_mmio_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(virtio_msg_mmio_freeze, virtio_msg_mmio_restore)
};
#endif

static int virtio_msg_mmio_probe(struct platform_device *pdev)
{
	struct virtio_msg_mmio_device *vmmdev;
	int ret;

	vmmdev = kzalloc(sizeof(*vmmdev), GFP_KERNEL);
	if (!vmmdev)
		return -ENOMEM;

	vmmdev->vmdev.vdev.dev.parent = &pdev->dev;
	vmmdev->vmdev.ops = &vmm_ops;
	vmmdev->pdev = pdev;

	vmmdev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vmmdev->base)) {
		ret = PTR_ERR(vmmdev->base);
		goto free_vmmdev;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		dev_warn(&pdev->dev, "Failed to enable 64-bit or 32-bit DMA\n");

	platform_set_drvdata(pdev, vmmdev);

	return virtio_msg_register(&vmmdev->vmdev);

free_vmmdev:
	kfree(vmmdev);
	return ret;
}

static void virtio_msg_mmio_remove(struct platform_device *pdev)
{
	struct virtio_msg_mmio_device *vmmdev = platform_get_drvdata(pdev);

	virtio_msg_unregister(&vmmdev->vmdev);
}

static const struct of_device_id virtio_msg_mmio_match[] = {
	{ .compatible = "virtio,msg-mmio", },
	{},
};
MODULE_DEVICE_TABLE(of, virtio_msg_mmio_match);

static struct platform_driver virtio_msg_mmio_driver = {
	.probe		= virtio_msg_mmio_probe,
	.remove_new	= virtio_msg_mmio_remove,
	.driver		= {
		.name	= "virtio-msg-mmio",
		.of_match_table	= virtio_msg_mmio_match,
#ifdef CONFIG_PM_SLEEP
		.pm	= &virtio_msg_mmio_pm_ops,
#endif
	},
};

static int __init virtio_msg_mmio_init(void)
{
	return platform_driver_register(&virtio_msg_mmio_driver);
}
module_init(virtio_msg_mmio_init);

static void __exit virtio_msg_mmio_exit(void)
{
	platform_driver_unregister(&virtio_msg_mmio_driver);
}
module_exit(virtio_msg_mmio_exit);

MODULE_AUTHOR("Viresh Kumar <viresh.kumar@linaro.org>");
MODULE_DESCRIPTION("Virtio message transport (MMIO)");
MODULE_LICENSE("GPL");
