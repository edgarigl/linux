// SPDX-License-Identifier: GPL-2.0+
/*
 * FF-A bus implementation for Virtio message transport.
 *
 * Copyright (C) 2025 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This implements the FF-A (Arm Firmware Framework) bus for Virtio msg
 * transport.
 */

#define pr_fmt(fmt) "virtio-msg-ffa: " fmt

#include <linux/arm_ffa.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/virtio.h>
#include <uapi/linux/virtio_msg_ffa.h>

#include "virtio_msg_internal.h"

struct virtio_msg_indirect_data {
	struct completion completion;
	struct virtio_msg *response;
};

struct virtio_msg_device_data {
	struct virtio_msg_device vmdev;
	struct virtio_msg_indirect_data idata;
};

/* Represents area shared with a partition */
struct shared_area {
	u64 handle;
	u32 id;
	void *vaddr;
	dma_addr_t dma_handle;
	size_t n_pages;
	u32 count;
	struct list_head list;
};

/* Represents FF-A corresponding to a partition */
struct virtio_msg_ffa_device {
	struct ffa_device *ffa_dev;
	struct reserved_mem *rmem;
	struct virtio_msg_indirect_data idata;
	struct virtio_msg_device_data *vmdevs;
	int (*send)(struct virtio_msg_ffa_device *vmfdev,
		    struct virtio_msg *request,
		    struct virtio_msg *response,
		    struct virtio_msg_indirect_data *idata);
	struct task_struct *used_event_task;
	struct virtio_msg_user_device vmudev;
	int vmdev_count;
	u32 features;
	u16 msg_size;
	bool passive;

	dma_addr_t rmem_dma_handle;
	struct ida area_id_map;
	struct list_head area_list;
	struct mutex lock; /* protects area_list */
};

#define to_vmdevdata(_vmdev) \
	container_of(_vmdev, struct virtio_msg_device_data, vmdev)
#define to_vmfdev(_vmdev) ((struct virtio_msg_ffa_device *)(_vmdev)->bus_data)
#define vmudev_to_vmfdev(_vmudev) \
	container_of(_vmudev, struct virtio_msg_ffa_device, vmudev)

static inline dma_addr_t ffa_to_dma(u32 area_id, dma_addr_t offset)
{
	return ((u64) area_id << VIRTIO_MSG_FFA_AREA_ID_OFFSET) |
		(offset & VIRTIO_MSG_FFA_OFFSET_MASK);
}

static inline u32 dma_to_ffa(dma_addr_t dma_handle, dma_addr_t *offset)
{
	*offset = dma_handle & VIRTIO_MSG_FFA_OFFSET_MASK;

	return dma_handle >> VIRTIO_MSG_FFA_AREA_ID_OFFSET;
}

static int vmsg_ffa_send_direct(struct virtio_msg_ffa_device *vmfdev,
				struct virtio_msg *request,
				struct virtio_msg *response,
				struct virtio_msg_indirect_data *idata_unused)
{
	struct ffa_device *ffa_dev = vmfdev->ffa_dev;
	struct ffa_send_direct_data2 ffa_data;
	int ret;

	memcpy(&ffa_data, request, request->msg_size);

	ret = ffa_dev->ops->msg_ops->sync_send_receive2(ffa_dev, &ffa_data);
	if (ret) {
		dev_dbg(&ffa_dev->dev,
			"Unable to send direct FF-A message: %d\n", ret);
		return ret;
	}

	if (response)
		memcpy(response, &ffa_data, vmfdev->msg_size);

	return 0;
}

static int vmsg_ffa_send_indirect(struct virtio_msg_ffa_device *vmfdev,
				  struct virtio_msg *request,
				  struct virtio_msg *response,
				  struct virtio_msg_indirect_data *idata)
{
	struct ffa_device *ffa_dev = vmfdev->ffa_dev;
	struct device *dev = &ffa_dev->dev;
	int ret, count = 10;

	/*
	 * Store the response pointer in idata structure. This will be updated
	 * by vmsg_ffa_notifier_cb() later.
	 */
	if (idata)
		idata->response = response;

try_again:
	ret = ffa_dev->ops->msg_ops->indirect_send(ffa_dev, request,
						   request->msg_size);
	if (ret == -EBUSY && --count) {
		cpu_relax();
		goto try_again;
	}

	if (ret) {
		dev_err(dev, "Failed sending indirect FF-A message: %d\n", ret);
		return ret;
	}

	/*
	 * Always wait for the operation to finish, otherwise we may start
	 * another operation while the previous one is still ongoing.
	 */
	if (idata) {
		ret = wait_for_completion_interruptible_timeout(&idata->completion, 1000);
		if (ret < 0) {
			dev_err(dev, "Interrupted - waiting for a response: %d\n", ret);
		} else if (!ret) {
			dev_err(dev, "Timed out waiting for a response\n");
			ret = -ETIMEDOUT;
		} else {
			ret = 0;
		}
	}

	return ret;
}

static struct virtio_msg_device *
find_vmdev(struct virtio_msg_ffa_device *vmfdev, u16 dev_id)
{
	int i;

	/* Find the device corresponding to a dev_id */
	for (i = 0; i < vmfdev->vmdev_count; i++) {
		if (vmfdev->vmdevs[i].vmdev.dev_id == dev_id)
			return &vmfdev->vmdevs[i].vmdev;
	}

	dev_err(&vmfdev->ffa_dev->dev, "Couldn't find matching vmdev: %d\n",
		dev_id);
	return NULL;
}

static int used_event_task(void *data)
{
	struct virtio_msg_ffa_device *vmfdev = data;
	struct virtio_msg_device *vmdev;
	u8 buf[VIRTIO_MSG_FFA_BUS_MSG_SIZE];
	struct virtio_msg *vmsg = (struct virtio_msg *)&buf;
	struct event_used *payload = virtio_msg_payload(vmsg);
	struct virtqueue *vq;
	u32 index;
	int i;

	virtio_msg_prepare(vmsg, VIRTIO_MSG_EVENT_USED, sizeof(*payload));

	while (!kthread_should_stop()) {
		for (i = 0; i < vmfdev->vmdev_count; i++) {
			vmdev = &vmfdev->vmdevs[i].vmdev;
			index = 0;

			virtio_device_for_each_vq(&vmdev->vdev, vq) {
				payload->index = cpu_to_le32(index++);
				virtio_msg_event(vmdev, vmsg);
			}
		}

		/* sleep for 1ms */
		fsleep(1000);
	}

	return 0;
}

static void handle_event_passive(struct virtio_msg_ffa_device *vmfdev,
				 struct virtio_msg *vmsg)
{
	/*
	 * Set `vmudev.vmsg` to `vmsg` and finish the completion to wake up the
	 * `read()` thread.
	 */
	vmfdev->vmudev.vmsg = vmsg;
	complete(&vmfdev->vmudev.r_completion);

	/*
	 * Wait here for the `write()` thread to finish and not return
	 * before the operation is finished to avoid any potential
	 * races.
	 *
	 * Can't make a sleep-able call here.
	 */
	while (!try_wait_for_completion(&vmfdev->vmudev.w_completion))
		cpu_relax();
}

static void vmsg_ffa_notifier_cb(int notify_id, void *cb_data, void *buf)
{
	struct virtio_msg_ffa_device *vmfdev = cb_data;
	struct ffa_device *ffa_dev = vmfdev->ffa_dev;
	struct virtio_msg_indirect_data *idata;
	struct virtio_msg_device *vmdev;
	struct virtio_msg *vmsg = buf;

	if (vmfdev->passive) {
		handle_event_passive(vmfdev, vmsg);
		return;
	}

	/*
	 * We can either receive a response message (to a previously sent
	 * request), or an EVENT_USED request message.
	 */
	if (vmsg->type & VIRTIO_MSG_TYPE_RESPONSE) {
		if (vmsg->type & VIRTIO_MSG_TYPE_BUS) {
			idata = &vmfdev->idata;
		} else {
			vmdev = find_vmdev(vmfdev, le16_to_cpu(vmsg->dev_id));
			if (!vmdev)
				return;

			idata = &to_vmdevdata(vmdev)->idata;
		}

		if (idata->response)
			memcpy(idata->response, vmsg, vmsg->msg_size);

		complete(&idata->completion);

		return;
	}

	/* Only support EVENT_USED virtio request messages */
	if (vmsg->type & VIRTIO_MSG_TYPE_BUS ||
	    vmsg->msg_id != VIRTIO_MSG_EVENT_USED) {
		dev_err(&ffa_dev->dev, "Unsupported message received\n");
		return;
	}

	vmdev = find_vmdev(vmfdev, le16_to_cpu(vmsg->dev_id));
	if (!vmdev)
		return;

	virtio_msg_event(vmdev, vmsg);
}

static int vmsg_ffa_notify_setup(struct virtio_msg_ffa_device *vmfdev)
{
	struct ffa_device *ffa_dev = vmfdev->ffa_dev;
	int ret;

	ret = ffa_dev->ops->notifier_ops->fwk_notify_request(ffa_dev,
			&vmsg_ffa_notifier_cb, vmfdev, 0);
	if (ret)
		dev_err(&ffa_dev->dev, "Unable to request notifier: %d\n", ret);

	return ret;
}

static void vmsg_ffa_notify_cleanup(struct virtio_msg_ffa_device *vmfdev)
{
	struct ffa_device *ffa_dev = vmfdev->ffa_dev;
	int ret;

	ret = ffa_dev->ops->notifier_ops->fwk_notify_relinquish(ffa_dev, 0);
	if (ret)
		dev_err(&ffa_dev->dev, "Unable to relinquish notifier: %d\n", ret);
}

static int vmsg_ffa_bus_get_devices(struct virtio_msg_ffa_device *vmfdev,
				    u16 *map, u16 *count)
{
	u8 req_buf[VIRTIO_MSG_FFA_BUS_MSG_SIZE];
	u8 res_buf[VIRTIO_MSG_FFA_BUS_MSG_SIZE];
	struct virtio_msg *request = (struct virtio_msg *)&req_buf;
	struct virtio_msg *response = (struct virtio_msg *)&res_buf;
	struct bus_get_devices *req_payload = virtio_msg_payload(request);
	struct bus_get_devices_resp *res_payload = virtio_msg_payload(response);
	int ret;

	static_assert(sizeof(*request) + sizeof(*req_payload) <
		      VIRTIO_MSG_FFA_BUS_MSG_SIZE);
	static_assert(sizeof(*response) + sizeof(*res_payload) <
		      VIRTIO_MSG_FFA_BUS_MSG_SIZE);

	virtio_msg_prepare(request, VIRTIO_MSG_BUS_GET_DEVICES,
			   sizeof(*req_payload));
	req_payload->offset = 0;
	req_payload->num = cpu_to_le16(0xFF);

	ret = vmfdev->send(vmfdev, request, response, &vmfdev->idata);
	if (ret < 0)
		return ret;

	*count = le16_to_cpu(res_payload->num);
	if (!*count)
		return -ENODEV;

	if (res_payload->offset != req_payload->offset)
		return -EINVAL;

	/* Support up to 16 devices for now */
	if (res_payload->next_offset)
		return -EINVAL;

	map[0] = res_payload->devices[0];
	map[1] = res_payload->devices[1];

	return 0;
}

static int vmsg_ffa_bus_version(struct virtio_msg_ffa_device *vmfdev)
{
	u8 req_buf[VIRTIO_MSG_FFA_BUS_MSG_SIZE];
	u8 res_buf[VIRTIO_MSG_FFA_BUS_MSG_SIZE];
	struct virtio_msg *request = (struct virtio_msg *)&req_buf;
	struct virtio_msg *response = (struct virtio_msg *)&res_buf;
	struct bus_ffa_version *req_payload = virtio_msg_payload(request);
	struct bus_ffa_version_resp *res_payload = virtio_msg_payload(response);
	u32 features;
	int ret;

	static_assert(sizeof(*request) + sizeof(*req_payload) <
		      VIRTIO_MSG_FFA_BUS_MSG_SIZE);
	static_assert(sizeof(*response) + sizeof(*res_payload) <
		      VIRTIO_MSG_FFA_BUS_MSG_SIZE);

	virtio_msg_prepare(request, VIRTIO_MSG_FFA_BUS_VERSION,
			   sizeof(*req_payload));
	req_payload->driver_version = cpu_to_le32(VIRTIO_MSG_FFA_BUS_VERSION_1_0);
	req_payload->vmsg_revision = cpu_to_le32(VIRTIO_MSG_REVISION_1);
	req_payload->vmsg_features = cpu_to_le32(VIRTIO_MSG_FEATURES);
	req_payload->features = cpu_to_le32(VIRTIO_MSG_FFA_FEATURE_BOTH_SUPP);
	req_payload->area_num = cpu_to_le16(VIRTIO_MSG_FFA_AREA_ID_MAX);

	ret = vmfdev->send(vmfdev, request, response, &vmfdev->idata);
	if (ret < 0)
		return ret;

	if (le32_to_cpu(res_payload->device_version) != VIRTIO_MSG_FFA_BUS_VERSION_1_0)
		return -EINVAL;

	if (le32_to_cpu(res_payload->vmsg_revision) != VIRTIO_MSG_REVISION_1)
		return -EINVAL;

	if (le32_to_cpu(res_payload->vmsg_features) != VIRTIO_MSG_FEATURES)
		return -EINVAL;

	features = le32_to_cpu(res_payload->features);

	/*
	 * - Direct message must be supported if it already worked.
	 * - Indirect message must be supported if it already worked
	 * - And direct message must not be supported since it didn't work.
	 */
	if ((ffa_partition_supports_direct_recv(vmfdev->ffa_dev) &&
	     !(features & VIRTIO_MSG_FFA_FEATURE_DIRECT_MSG_RX_SUPP)) ||
	    (ffa_partition_supports_indirect_msg(vmfdev->ffa_dev) &&
	     !(features & VIRTIO_MSG_FFA_FEATURE_INDIRECT_MSG_SUPP))) {
		dev_err(&vmfdev->ffa_dev->dev, "Invalid features\n");
		return -EINVAL;
	}

	vmfdev->features = features;

	return 0;
}

#if IS_REACHABLE(CONFIG_VIRTIO_MSG_FFA_DMA_OPS)
static int vmsg_ffa_bus_area_share_single(struct ffa_device *ffa_dev, void *vaddr,
		size_t n_pages, dma_addr_t *dma_handle)
{
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;
	struct ffa_mem_region_attributes mem_attr = {
		.receiver = ffa_dev->vm_id,
		.attrs = FFA_MEM_RW,
	};
	struct ffa_mem_ops_args args = {
		.use_txbuf = true,
		.attrs = &mem_attr,
		.nattrs = 1,
	};
	struct page **pages __free(kfree) = NULL;
	u8 req_buf[VIRTIO_MSG_FFA_BUS_MSG_SIZE];
	u8 res_buf[VIRTIO_MSG_FFA_BUS_MSG_SIZE];
	struct virtio_msg *request = (struct virtio_msg *)&req_buf;
	struct virtio_msg *response = (struct virtio_msg *)&res_buf;
	struct bus_area_share *req_payload = virtio_msg_payload(request);
	struct bus_area_share_resp *res_payload = virtio_msg_payload(response);
	struct shared_area *area;
	struct sg_table sgt;
	int ret, i;

	static_assert(sizeof(*request) + sizeof(*req_payload) <
		      VIRTIO_MSG_FFA_BUS_MSG_SIZE);
	static_assert(sizeof(*response) + sizeof(*res_payload) <
		      VIRTIO_MSG_FFA_BUS_MSG_SIZE);

	pages = kmalloc(sizeof(*pages) * n_pages, GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	for (i = 0; i < n_pages; i++)
		pages[i] = virt_to_page((void *)((u64)vaddr + PAGE_SIZE * i));

	area = kzalloc(sizeof(*area), GFP_KERNEL);
	if (!area)
		return -ENOMEM;

	ret = ida_alloc_range(&vmfdev->area_id_map, 1, U32_MAX - 1, GFP_KERNEL);
	if (ret < 0)
		goto free_area;
	area->id = ret;

	/* Share the pages */
	ret = sg_alloc_table_from_pages(&sgt, pages, n_pages, 0,
					n_pages * PAGE_SIZE, GFP_KERNEL);
	if (ret)
		goto free_ida;

	args.sg = sgt.sgl;
	ret = ffa_dev->ops->mem_ops->memory_share(&args);
	sg_free_table(&sgt);

	if (ret)
		goto free_ida;

	area->handle = args.g_handle;
	area->vaddr = vaddr;
	area->dma_handle = virt_to_phys(vaddr);
	area->n_pages = n_pages;
	area->count = 1;

	virtio_msg_prepare(request, VIRTIO_MSG_FFA_BUS_AREA_SHARE,
			   sizeof(*req_payload));
	req_payload->area_id = cpu_to_le16(area->id);
	req_payload->mem_handle = cpu_to_le64(area->handle);
	req_payload->count = cpu_to_le32(n_pages);

	ret = vmfdev->send(vmfdev, request, response, &vmfdev->idata);
	if (ret < 0)
		goto mem_reclaim;

	if (le16_to_cpu(res_payload->result))
		return -EINVAL;

	if (dma_handle)
		*dma_handle = ffa_to_dma(area->id, 0);

	list_add(&area->list, &vmfdev->area_list);

	return 0;

mem_reclaim:
	ffa_dev->ops->mem_ops->memory_reclaim(area->handle, 0);
free_ida:
	ida_free(&vmfdev->area_id_map, area->id);
free_area:
	kfree(area);

	return ret;
}

/* vaddr is always page aligned */
int vmsg_ffa_bus_area_share(struct device *dev, void *vaddr, size_t n_pages,
			    dma_addr_t *dma_handle)
{
	struct ffa_device *ffa_dev = to_ffa_dev(dev);
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;
	struct shared_area *area;
	int ret = 0;

	guard(mutex)(&vmfdev->lock);

	/*
	 * If "restricted-dma-pool" is supported, we should have already mapped
	 * a big enough area at initialization time. Make sure that "dma_handle"
	 * lies within that and update dma_handle properly.
	 */
	if (vmfdev->rmem_dma_handle) {
		BUG_ON(!list_is_singular(&vmfdev->area_list));

		area = list_first_entry(&vmfdev->area_list, struct shared_area,
					list);

		if ((*dma_handle < area->dma_handle) ||
		    ((*dma_handle + n_pages * PAGE_SIZE) >
		     (area->dma_handle + area->n_pages * PAGE_SIZE))) {
			dev_err(dev, "Vaddr out of range\n");
			ret = -EINVAL;
		} else {
			*dma_handle = ffa_to_dma(area->id, *dma_handle - area->dma_handle);
			area->count++;
		}

		return ret;
	}

	/* Check if area is already mapped */
	list_for_each_entry(area, &vmfdev->area_list, list) {
		/* TODO: Only support exact page match for now */
		if (area->vaddr == vaddr && area->n_pages == n_pages) {
			*dma_handle = ffa_to_dma(area->id, 0);
			area->count++;
			return 0;
		}
	}

	return vmsg_ffa_bus_area_share_single(ffa_dev, vaddr, n_pages, dma_handle);
}
EXPORT_SYMBOL_GPL(vmsg_ffa_bus_area_share);

static int vmsg_ffa_bus_area_unshare_single(struct ffa_device *ffa_dev,
		struct shared_area *area)
{
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;
	u8 req_buf[VIRTIO_MSG_FFA_BUS_MSG_SIZE];
	u8 res_buf[VIRTIO_MSG_FFA_BUS_MSG_SIZE];
	struct virtio_msg *request = (struct virtio_msg *)&req_buf;
	struct virtio_msg *response = (struct virtio_msg *)&res_buf;
	struct bus_area_unshare *req_payload = virtio_msg_payload(request);
	struct bus_area_unshare_resp *res_payload = virtio_msg_payload(response);
	int ret;

	static_assert(sizeof(*request) + sizeof(*req_payload) <
		      VIRTIO_MSG_FFA_BUS_MSG_SIZE);
	static_assert(sizeof(*response) + sizeof(*res_payload) <
		      VIRTIO_MSG_FFA_BUS_MSG_SIZE);

	virtio_msg_prepare(request, VIRTIO_MSG_FFA_BUS_AREA_UNSHARE,
			   sizeof(*req_payload));
	req_payload->area_id = cpu_to_le16(area->id);

	ret = vmfdev->send(vmfdev, request, response, &vmfdev->idata);
	if (!ret && le16_to_cpu(res_payload->result))
		ret = -EINVAL;

	if (!ret) {
		/* reclaim shared memory */
		ret = ffa_dev->ops->mem_ops->memory_reclaim(area->handle, 0);
	}

	ida_free(&vmfdev->area_id_map, area->id);
	kfree(area);

	return ret;
}

int vmsg_ffa_bus_area_unshare(struct device *dev, dma_addr_t *dma_handle,
			      size_t n_pages)
{
	struct ffa_device *ffa_dev = to_ffa_dev(dev);
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;
	struct shared_area *area;
	dma_addr_t offset;
	u32 area_id;

	area_id = dma_to_ffa(*dma_handle, &offset);

	guard(mutex)(&vmfdev->lock);

	list_for_each_entry(area, &vmfdev->area_list, list) {
		if (area->id == area_id) {
			*dma_handle = area->dma_handle + offset;

			if (--area->count)
				return 0;

			WARN_ON(area->n_pages != n_pages);
			list_del(&area->list);

			return vmsg_ffa_bus_area_unshare_single(ffa_dev, area);
		}
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(vmsg_ffa_bus_area_unshare);
#endif

static int virtio_msg_ffa_transfer(struct virtio_msg_device *vmdev,
				   struct virtio_msg *request,
				   struct virtio_msg *response)
{
	struct virtio_msg_indirect_data *idata = &to_vmdevdata(vmdev)->idata;
	struct virtio_msg_ffa_device *vmfdev = to_vmfdev(vmdev);

	return vmfdev->send(vmfdev, request, response, idata);
}

static const char *virtio_msg_ffa_bus_info(struct virtio_msg_device *vmdev,
					   u16 *msg_size, u32 *rev)
{
	struct virtio_msg_ffa_device *vmfdev = to_vmfdev(vmdev);

	*msg_size = vmfdev->msg_size;
	*rev = VIRTIO_MSG_REVISION_1;

	return dev_name(&vmfdev->ffa_dev->dev);
}

static struct virtio_msg_ops vmf_ops = {
	.transfer = virtio_msg_ffa_transfer,
	.bus_info = virtio_msg_ffa_bus_info,
};

static int virtio_msg_ffa_user_handle(struct virtio_msg_user_device *vmudev,
				      struct virtio_msg *vmsg)
{
	struct virtio_msg_ffa_device *vmfdev = vmudev_to_vmfdev(vmudev);

	vmfdev->send(vmfdev, vmsg, NULL, NULL);
	return 0;
}

static struct virtio_msg_user_ops vmf_user_ops = {
	.handle = virtio_msg_ffa_user_handle,
};

static void remove_vmdevs(struct virtio_msg_ffa_device *vmfdev, int count)
{
	while (count--)
		virtio_msg_unregister(&vmfdev->vmdevs[count].vmdev);
}

static int virtio_msg_ffa_rmem_init(struct virtio_msg_ffa_device *vmfdev)
{
	struct device *dev = &vmfdev->ffa_dev->dev;
	struct device_node *np __free(device_node) =
		of_find_compatible_node(NULL, NULL, "virtio-msg,ffa");

	if (!np) {
		dev_info(dev, "Continuing without reserved-memory block\n");
		return 0;
	}

	vmfdev->rmem = of_reserved_mem_lookup(np);
	if (IS_ERR(vmfdev->rmem))
		return PTR_ERR(vmfdev->rmem);

	return reserved_mem_device_init(dev, vmfdev->rmem);
}

static int virtio_msg_ffa_probe(struct ffa_device *ffa_dev)
{
	struct virtio_msg_ffa_device *vmfdev;
	struct device *dev = &ffa_dev->dev;
	struct virtio_msg_device *vmdev;
	unsigned long devices = 0;
	int ret, i = 0, bit;
	u16 count;

	vmfdev = devm_kzalloc(dev, sizeof(*vmfdev), GFP_KERNEL);
	if (!vmfdev)
		return -ENOMEM;

	if (ffa_partition_supports_direct_recv(ffa_dev)) {
		vmfdev->send = vmsg_ffa_send_direct;
	} else if (ffa_partition_supports_indirect_msg(ffa_dev)) {
		vmfdev->send = vmsg_ffa_send_indirect;
	} else {
		dev_err(dev, "Direct or Indirect messages not supported\n");
		return -EINVAL;
	}
	vmfdev->ffa_dev = ffa_dev;
	vmfdev->msg_size = VIRTIO_MSG_FFA_BUS_MSG_SIZE;
	vmfdev->rmem = ERR_PTR(-ENOMEM);
	ffa_dev_set_drvdata(ffa_dev, vmfdev);
	init_completion(&vmfdev->idata.completion);
	INIT_LIST_HEAD(&vmfdev->area_list);
	ida_init(&vmfdev->area_id_map);
	mutex_init(&vmfdev->lock);

	/* Activate passive mode on host domain */
	vmfdev->passive = ffa_dev->vm_id != 1;

	ret = vmsg_ffa_notify_setup(vmfdev);
	if (ret && ffa_partition_supports_indirect_msg(ffa_dev))
		goto ida_destroy;

	if (vmfdev->passive) {
		vmfdev->vmudev.ops = &vmf_user_ops;
		vmfdev->vmudev.parent = &ffa_dev->dev;

		ret = virtio_msg_user_register(&vmfdev->vmudev);
		if (ret) {
			dev_err(&ffa_dev->dev, "Could not register virtio-msg user device\n");
			goto notify_cleanup;
		}

		return 0;
	}

	ret = vmsg_ffa_bus_version(vmfdev);
	if (ret)
		goto notify_cleanup;

	ret = vmsg_ffa_bus_get_devices(vmfdev, (u16 *)&devices, &count);
	if (ret)
		goto notify_cleanup;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		dev_warn(dev, "Failed to enable 64-bit or 32-bit DMA\n");

	ret = virtio_msg_ffa_rmem_init(vmfdev);
	if (ret)
		goto notify_cleanup;

	ret = virtio_msg_ffa_share_rmem(dev, vmfdev->rmem, &vmfdev->rmem_dma_handle);
	if (ret)
		goto rmem_free;

	vmfdev->vmdevs = devm_kcalloc(dev, count, sizeof(*vmfdev->vmdevs),
				      GFP_KERNEL);
	if (!vmfdev->vmdevs) {
		ret = -ENOMEM;
		goto mem_unshare;
	}
	vmfdev->vmdev_count = count;

	for_each_set_bit(bit, &devices, sizeof(devices)) {
		init_completion(&vmfdev->vmdevs[i].idata.completion);
		vmdev = &vmfdev->vmdevs[i].vmdev;
		vmdev->dev_id = bit;
		vmdev->ops = &vmf_ops;
		vmdev->vdev.dev.parent = dev;
		vmdev->bus_data = vmfdev;

		ret = virtio_msg_register(vmdev);
		if (ret) {
			dev_err(dev, "Failed to register virtio-msg device (%d)\n", ret);
			goto unregister;
		}

		i++;
	}

	/* Run the kthread if indirect messages aren't supported */
	if (!(vmfdev->features & VIRTIO_MSG_FFA_FEATURE_INDIRECT_MSG_SUPP)) {
		vmfdev->used_event_task = kthread_run(used_event_task, vmfdev, "vmsg-ffa-ue");
		if (IS_ERR(vmfdev->used_event_task)) {
			ret = PTR_ERR(vmfdev->used_event_task);
			goto unregister;
		}
	}

	return 0;

unregister:
	remove_vmdevs(vmfdev, i);
mem_unshare:
	virtio_msg_ffa_unshare_rmem(dev, vmfdev->rmem, &vmfdev->rmem_dma_handle);
rmem_free:
	if (!IS_ERR(vmfdev->rmem))
		of_reserved_mem_device_release(dev);
notify_cleanup:
	vmsg_ffa_notify_cleanup(vmfdev);
ida_destroy:
	ida_destroy(&vmfdev->area_id_map);
	return ret;
}

static void virtio_msg_ffa_remove(struct ffa_device *ffa_dev)
{
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;

	if (vmfdev->passive) {
		virtio_msg_user_unregister(&vmfdev->vmudev);
	} else {
		kthread_stop(vmfdev->used_event_task);
		remove_vmdevs(vmfdev, vmfdev->vmdev_count);
		virtio_msg_ffa_unshare_rmem(&ffa_dev->dev, vmfdev->rmem,
					    &vmfdev->rmem_dma_handle);

		if (!IS_ERR(vmfdev->rmem))
			of_reserved_mem_device_release(&ffa_dev->dev);
	}

	vmsg_ffa_notify_cleanup(vmfdev);
	ida_destroy(&vmfdev->area_id_map);
}

static const struct ffa_device_id virtio_msg_ffa_device_ids[] = {
	/* c66028b5-2498-4aa1-9de7-77da6122abf0 */
	{ UUID_INIT(0xc66028b5, 0x2498, 0x4aa1,
		    0x9d, 0xe7, 0x77, 0xda, 0x61, 0x22, 0xab, 0xf0) },
	{}
};

static int __maybe_unused virtio_msg_ffa_suspend(struct device *dev)
{
	struct virtio_msg_ffa_device *vmfdev = dev_get_drvdata(dev);
	int ret, i, index;

	for (i = 0; i < vmfdev->vmdev_count; i++) {
		index = vmfdev->vmdev_count - i - 1;
		ret = virtio_device_freeze(&vmfdev->vmdevs[index].vmdev.vdev);
		if (ret)
			return ret;
	}

	return 0;
}

static int __maybe_unused virtio_msg_ffa_resume(struct device *dev)
{
	struct virtio_msg_ffa_device *vmfdev = dev_get_drvdata(dev);
	int ret, i;

	for (i = 0; i < vmfdev->vmdev_count; i++) {
		ret = virtio_device_restore(&vmfdev->vmdevs[i].vmdev.vdev);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct dev_pm_ops virtio_msg_ffa_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(virtio_msg_ffa_suspend, virtio_msg_ffa_resume)
};

static struct ffa_driver virtio_msg_ffa_driver = {
	.name = "virtio-msg-ffa",
	.probe = virtio_msg_ffa_probe,
	.remove = virtio_msg_ffa_remove,
	.id_table = virtio_msg_ffa_device_ids,
	.driver = {
		.pm = &virtio_msg_ffa_pm_ops,
	},
};

static int virtio_msg_ffa_init(void)
{
	virtio_msg_ffa_dma_init();

	if (IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT))
		return ffa_register(&virtio_msg_ffa_driver);
	else
		return -EOPNOTSUPP;
}
module_init(virtio_msg_ffa_init);

static void virtio_msg_ffa_exit(void)
{
	if (IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT))
		ffa_unregister(&virtio_msg_ffa_driver);
}
module_exit(virtio_msg_ffa_exit);

MODULE_AUTHOR("Viresh Kumar <viresh.kumar@linaro.org>");
MODULE_DESCRIPTION("Virtio message FF-A bus driver");
MODULE_LICENSE("GPL");
