// SPDX-License-Identifier: GPL-2.0+
/*
 * Loopback bus implementation for Virtio message transport.
 *
 * Copyright (C) 2025 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This implements the Loopback bus for Virtio msg transport.
 */

#define pr_fmt(fmt) "virtio-msg-loopback: " fmt

#include <linux/err.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/virtio.h>
#include <uapi/linux/virtio_msg.h>
#include <uapi/linux/virtio_msg_lb.h>

#include "virtio_msg_internal.h"

struct vmlb_device {
	struct virtio_msg_device vmdev;
	struct list_head list;
};

struct virtio_msg_lb {
	/* Serializes transfers and protects list */
	struct mutex lock;
	struct list_head list;
	struct miscdevice misc;
	struct virtio_msg_user_device vmudev;
	struct virtio_msg *response;
	struct reserved_mem *rmem;
	struct device *dev;
};

#define vmdev_to_vmlb(_vmdev) \
	((struct virtio_msg_lb *)(_vmdev)->bus_data)
#define vmudev_to_vmlb(_vmudev) \
	container_of(_vmudev, struct virtio_msg_lb, vmudev)
#define mdev_to_vmlb(_mdev) \
	container_of(_mdev, struct virtio_msg_lb, misc)

static struct vmlb_device *find_vmlbdev(struct virtio_msg_lb *vmlb, u16 dev_id)
{
	struct vmlb_device *vmlbdev;

	list_for_each_entry(vmlbdev, &vmlb->list, list) {
		if (vmlbdev->vmdev.dev_id == dev_id)
			return vmlbdev;
	}

	return NULL;
}

static const char *virtio_msg_lb_bus_info(struct virtio_msg_device *vmdev,
					  u16 *msg_size, u32 *rev)
{
	struct virtio_msg_lb *vmlb = vmdev_to_vmlb(vmdev);

	*msg_size = VIRTIO_MSG_MIN_SIZE;
	*rev = VIRTIO_MSG_REVISION_1;

	return dev_name(vmlb->dev);
}

static int virtio_msg_lb_transfer(struct virtio_msg_device *vmdev,
				  struct virtio_msg *request,
				  struct virtio_msg *response)
{
	struct virtio_msg_lb *vmlb = vmdev_to_vmlb(vmdev);
	struct virtio_msg_user_device *vmudev = &vmlb->vmudev;

	/*
	 * Allow only one transaction to progress at once.
	 */
	guard(mutex)(&vmlb->lock);

	/*
	 * Set `vmsg` to `request` and finish the completion to wake up the
	 * `read()` thread.
	 */
	vmudev->vmsg = request;
	vmlb->response = response;
	complete(&vmudev->r_completion);

	/*
	 * Wait here for the `write()` thread to finish and not return
	 * before the operation is finished to avoid any potential
	 * races.
	 *
	 * Can't make a sleep-able call here as this may get called from under
	 * RCU locks (eg: vsock).
	 */
	while (!try_wait_for_completion(&vmudev->w_completion))
		cpu_relax();

	/* Clear the pointers, just to be safe */
	vmudev->vmsg = NULL;
	vmlb->response = NULL;

	return 0;
}

static struct virtio_msg_ops virtio_msg_lb_ops = {
	.transfer = virtio_msg_lb_transfer,
	.bus_info = virtio_msg_lb_bus_info,
};

static int virtio_msg_lb_user_handle(struct virtio_msg_user_device *vmudev,
				     struct virtio_msg *vmsg)
{
	struct virtio_msg_lb *vmlb = vmudev_to_vmlb(vmudev);
	struct vmlb_device *vmlbdev;

	/* Response message */
	if (vmsg->type & VIRTIO_MSG_TYPE_RESPONSE) {
		if (vmlb->response)
			memcpy(vmlb->response, vmsg, VIRTIO_MSG_MIN_SIZE);

		return 0;
	}

	/* Only support EVENT_USED virtio request messages */
	if (vmsg->type & VIRTIO_MSG_TYPE_BUS || vmsg->msg_id != VIRTIO_MSG_EVENT_USED) {
		dev_err(vmlb->dev, "Unsupported message received\n");
		return 0;
	}

	vmlbdev = find_vmlbdev(vmlb, le16_to_cpu(vmsg->dev_id));
	if (!vmlbdev)
		return 0;

	virtio_msg_event(&vmlbdev->vmdev, vmsg);
	return 0;
}

static struct virtio_msg_user_ops vmlb_user_ops = {
	.handle = virtio_msg_lb_user_handle,
};

static int vmlbdev_add(struct virtio_msg_lb *vmlb,
		       struct vmsg_lb_dev_info *info)
{
	struct vmlb_device *vmlbdev;
	int ret;

	scoped_guard(mutex, &vmlb->lock) {
		if (find_vmlbdev(vmlb, info->dev_id))
			return -EEXIST;

		vmlbdev = kzalloc(sizeof(*vmlbdev), GFP_KERNEL);
		if (!vmlbdev)
			return -ENOMEM;

		vmlbdev->vmdev.dev_id = info->dev_id;
		vmlbdev->vmdev.ops = &virtio_msg_lb_ops;
		vmlbdev->vmdev.vdev.dev.parent = vmlb->dev;
		vmlbdev->vmdev.bus_data = vmlb;

		list_add(&vmlbdev->list, &vmlb->list);
	}

	ret = virtio_msg_register(&vmlbdev->vmdev);
	if (ret) {
		dev_err(vmlb->dev, "Failed to register virtio msg lb device (%d)\n", ret);
		goto out;
	}

	return 0;

out:
	scoped_guard(mutex, &vmlb->lock)
		list_del(&vmlbdev->list);

	kfree(vmlbdev);
	return ret;
}

static int vmlbdev_remove(struct virtio_msg_lb *vmlb,
			  struct vmsg_lb_dev_info *info)
{
	struct vmlb_device *vmlbdev;

	scoped_guard(mutex, &vmlb->lock) {
		vmlbdev = find_vmlbdev(vmlb, info->dev_id);
		if (vmlbdev) {
			list_del(&vmlbdev->list);
			virtio_msg_unregister(&vmlbdev->vmdev);
			return 0;
		}
	}

	dev_err(vmlb->dev, "Failed to find virtio msg lb device.\n");
	return -ENODEV;
}

static void vmlbdev_remove_all(struct virtio_msg_lb *vmlb)
{
	struct vmlb_device *vmlbdev, *tvmlbdev;

	guard(mutex)(&vmlb->lock);

	list_for_each_entry_safe(vmlbdev, tvmlbdev, &vmlb->list, list) {
		virtio_msg_unregister(&vmlbdev->vmdev);
		list_del(&vmlbdev->list);
	}
}

static int vmlb_open(struct inode *inode, struct file *file)
{
	struct miscdevice *mdev = file->private_data;

	file->private_data = mdev_to_vmlb(mdev);
	return 0;
}

static long vmlb_ioctl(struct file *file, unsigned int cmd, unsigned long data)
{
	struct virtio_msg_lb *vmlb = file->private_data;
	struct vmsg_lb_dev_info info;

	if (copy_from_user(&info, (void __user *)data, sizeof(info)))
		return -EFAULT;

	switch (cmd) {
	case IOCTL_VMSG_LB_ADD:
		return vmlbdev_add(vmlb, &info);

	case IOCTL_VMSG_LB_REMOVE:
		return vmlbdev_remove(vmlb, &info);

	default:
		return -ENOTTY;
	}
}

static int vmlb_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct virtio_msg_lb *vmlb = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if (offset > vmlb->rmem->size - size)
		return -EINVAL;

	return remap_pfn_range(vma, vma->vm_start,
			(vmlb->rmem->base + offset) >> PAGE_SHIFT,
			size,
			vma->vm_page_prot);
}

static loff_t vmlb_llseek(struct file *file, loff_t offset, int whence)
{
	struct virtio_msg_lb *vmlb = file->private_data;

	return fixed_size_llseek(file, offset, whence, vmlb->rmem->size);
}

static const struct file_operations vmlb_miscdev_fops = {
	.owner = THIS_MODULE,
	.open = vmlb_open,
	.unlocked_ioctl = vmlb_ioctl,
	.mmap = vmlb_mmap,
	.llseek = vmlb_llseek,
};

static int virtio_msg_lb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct virtio_msg_lb *vmlb;
	int ret;

	vmlb = devm_kzalloc(dev, sizeof(*vmlb), GFP_KERNEL);
	if (!vmlb)
		return -ENOMEM;

	vmlb->rmem = of_reserved_mem_lookup(dev->of_node);
	if (!vmlb->rmem) {
		dev_err(dev, "failed to acquire memory region\n");
		return -ENODEV;
	}

	ret = reserved_mem_device_init(dev, vmlb->rmem);
	if (ret)
		return ret;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		dev_warn(dev, "Failed to enable 64-bit or 32-bit DMA\n");

	INIT_LIST_HEAD(&vmlb->list);
	mutex_init(&vmlb->lock);
	vmlb->dev = dev;
	vmlb->vmudev.parent = dev;
	vmlb->vmudev.ops = &vmlb_user_ops;

	vmlb->misc.name = "virtio-msg-lb";
	vmlb->misc.minor = MISC_DYNAMIC_MINOR;
	vmlb->misc.fops = &vmlb_miscdev_fops;

	ret = misc_register(&vmlb->misc);
	if (ret)
		goto mem_release;

	/* Register with virtio-msg UAPI */
	ret = virtio_msg_user_register(&vmlb->vmudev);
	if (ret) {
		dev_err(dev, "Could not register virtio-msg user API\n");
		goto unregister;
	}

	platform_set_drvdata(pdev, vmlb);

	return 0;

unregister:
	misc_deregister(&vmlb->misc);
mem_release:
	of_reserved_mem_device_release(dev);
	return ret;
}

static void virtio_msg_lb_remove(struct platform_device *pdev)
{
	struct virtio_msg_lb *vmlb = platform_get_drvdata(pdev);

	virtio_msg_user_unregister(&vmlb->vmudev);
	misc_deregister(&vmlb->misc);
	of_reserved_mem_device_release(&pdev->dev);
	vmlbdev_remove_all(vmlb);
}

static const struct of_device_id virtio_msg_lb_match_table[] = {
	{ .compatible = "virtio-msg,loopback" },
	{ }
};
MODULE_DEVICE_TABLE(of, virtio_msg_lb_match_table);

static struct platform_driver virtio_msg_lb_driver = {
	.probe = virtio_msg_lb_probe,
	.remove = virtio_msg_lb_remove,
	.driver = {
		.name = "virtio-msg-loopback",
		.of_match_table = virtio_msg_lb_match_table,
	},
};
module_platform_driver(virtio_msg_lb_driver);

MODULE_AUTHOR("Viresh Kumar <viresh.kumar@linaro.org>");
MODULE_DESCRIPTION("Virtio message loopback bus driver");
MODULE_LICENSE("GPL");
