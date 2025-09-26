// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtio message transport.
 *
 * Copyright (C) 2025 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * The virtio-msg transport encapsulates virtio operations as discrete message
 * exchanges rather than relying on PCI or memory-mapped I/O regions. It
 * separates bus-level functionality (e.g., device enumeration, hotplug events)
 * from device-specific operations (e.g., feature negotiation, virtqueue setup),
 * ensuring that a single, generic transport layer can be reused across multiple
 * bus implementations (like ARM Firmware Framework (FF-A), IPC, etc.).
 */

#define pr_fmt(fmt) "virtio-msg: " fmt

#include <linux/err.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <uapi/linux/virtio_msg.h>

#include "virtio_msg.h"

#define to_virtio_msg_device(_dev)	container_of(_dev, struct virtio_msg_device, vdev)

static void msg_prepare(struct virtio_msg *vmsg, bool bus, u8 msg_id,
			u16 dev_id, u16 payload_size)
{
	u16 size = sizeof(*vmsg) + payload_size;

	if (bus) {
		vmsg->type = VIRTIO_MSG_TYPE_BUS;
	} else {
		vmsg->type = VIRTIO_MSG_TYPE_TRANSPORT;
		vmsg->dev_id = cpu_to_le16(dev_id);
	}

	vmsg->msg_id = msg_id;
	vmsg->msg_size = cpu_to_le16(size);
}

static void transport_msg_prepare(struct virtio_msg_device *vmdev, u8 msg_id,
				  u16 payload_size)
{
	memset(vmdev->request, 0, vmdev->msg_size);
	msg_prepare(vmdev->request, false, msg_id, vmdev->dev_id, payload_size);
}

void virtio_msg_prepare(struct virtio_msg *vmsg, u8 msg_id, u16 payload_size)
{
	msg_prepare(vmsg, true, msg_id, 0, payload_size);
}
EXPORT_SYMBOL_GPL(virtio_msg_prepare);

static int virtio_msg_xfer(struct virtio_msg_device *vmdev)
{
	int ret;

	memset(vmdev->response, 0, vmdev->msg_size);

	ret = vmdev->ops->transfer(vmdev, vmdev->request, vmdev->response);
	if (ret) {
		dev_err(&vmdev->vdev.dev, "%s: Failed to send request (%d)\n",
			__func__, ret);
	}

	return ret;
}

static inline int virtio_msg_send(struct virtio_msg_device *vmdev)
{
	return vmdev->ops->transfer(vmdev, vmdev->request, NULL);
}

static int virtio_msg_get_device_info(struct virtio_msg_device *vmdev)
{
	struct get_device_info_resp *payload = virtio_msg_payload(vmdev->response);
	struct virtio_device *vdev = &vmdev->vdev;
	int ret;

	transport_msg_prepare(vmdev, VIRTIO_MSG_DEVICE_INFO, 0);

	ret = virtio_msg_xfer(vmdev);
	if (ret)
		return ret;

	vdev->id.device = le32_to_cpu(payload->device_id);
	if (vdev->id.device == 0) {
		/*
		 * virtio-msg device with an ID 0 is a (dummy) placeholder
		 * with no function.
		 */
		return -ENODEV;
	}

	vdev->id.vendor = le32_to_cpu(payload->vendor_id);
	vmdev->num_feature_bits = le32_to_cpu(payload->num_feature_bits);
	vmdev->config_size = le32_to_cpu(payload->config_size);

	/* FIXME: Linux supports 64 feature bits */
	if (vmdev->num_feature_bits != 64) {
		dev_err(&vdev->dev, "%s: Incompatible num_feature_bits (%u)\n",
			__func__, vmdev->num_feature_bits);
		return -EINVAL;
	}

	return 0;
}

static u64 virtio_msg_get_features(struct virtio_device *vdev)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);
	struct get_features *req_payload = virtio_msg_payload(vmdev->request);
	struct get_features_resp *res_payload = virtio_msg_payload(vmdev->response);
	__le32 *features;
	int ret;

	transport_msg_prepare(vmdev, VIRTIO_MSG_GET_DEV_FEATURES, sizeof(*req_payload));
	req_payload->index = 0;

	/* FIXME: Linux supports 64 feature bits */
	req_payload->num = cpu_to_le32(2);

	ret = virtio_msg_xfer(vmdev);
	if (ret)
		return ret;

	features = (__le32 *)res_payload->features;
	return ((u64)(le32_to_cpu(features[1])) << 32) | le32_to_cpu(features[0]);
}

static int virtio_msg_finalize_features(struct virtio_device *vdev)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);
	struct set_features *payload = virtio_msg_payload(vmdev->request);
	__le32 *features = (__le32 *)payload->features;

	/* Give virtio_ring a chance to accept features. */
	vring_transport_features(vdev);

	transport_msg_prepare(vmdev, VIRTIO_MSG_SET_DRV_FEATURES, sizeof(*payload) + 2 * 4);
	payload->index = 0;

	/* FIXME: Linux supports 64 feature bits */
	payload->num = cpu_to_le32(2);

	features[0] = cpu_to_le32((u32)vmdev->vdev.features);
	features[1] = cpu_to_le32(vmdev->vdev.features >> 32);

	return virtio_msg_xfer(vmdev);
}

static void virtio_msg_get(struct virtio_device *vdev, unsigned int offset,
			   void *buf, unsigned int len)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);
	struct get_config *req_payload = virtio_msg_payload(vmdev->request);
	struct get_config_resp *res_payload = virtio_msg_payload(vmdev->response);

	BUG_ON(len > 8);

	if (offset + len > vmdev->config_size) {
		dev_err(&vmdev->vdev.dev, "%s: Invalid config read operation: %u: %u: %u\n",
			__func__, offset, len, vmdev->config_size);
		return;
	}

	transport_msg_prepare(vmdev, VIRTIO_MSG_GET_CONFIG, sizeof(*req_payload));
	req_payload->offset = cpu_to_le32(offset);
	req_payload->size = cpu_to_le32(len);

	if (virtio_msg_xfer(vmdev))
		return;

	/* Buffer holds the data in little endian */
	memcpy(buf, res_payload->config, len);
	vmdev->generation_count = le32_to_cpu(res_payload->generation);
}

static void virtio_msg_set(struct virtio_device *vdev, unsigned int offset,
			   const void *buf, unsigned int len)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);
	struct set_config *payload = virtio_msg_payload(vmdev->request);

	BUG_ON(len > 8);

	if (offset + len > vmdev->config_size) {
		dev_err(&vmdev->vdev.dev,
			"%s: Invalid config write operation: %u: %u: %u\n",
			__func__, offset, len, vmdev->config_size);
		return;
	}

	transport_msg_prepare(vmdev, VIRTIO_MSG_SET_CONFIG, sizeof(*payload));
	payload->offset = cpu_to_le32(offset);
	payload->size = cpu_to_le32(len);
	payload->generation = cpu_to_le32(vmdev->generation_count);

	/* Buffer holds the data in little endian */
	memcpy(payload->config, buf, len);

	virtio_msg_xfer(vmdev);
}

static u32 virtio_msg_generation(struct virtio_device *vdev)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);

	virtio_msg_get(vdev, 0, NULL, 0);
	return vmdev->generation_count;
}

static u8 virtio_msg_get_status(struct virtio_device *vdev)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);
	struct get_device_status_resp *payload = virtio_msg_payload(vmdev->response);

	transport_msg_prepare(vmdev, VIRTIO_MSG_GET_DEVICE_STATUS, 0);

	if (virtio_msg_xfer(vmdev))
		return 0;

	return (u8) le32_to_cpu(payload->status);
}

static void virtio_msg_set_status(struct virtio_device *vdev, u8 status)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);
	struct set_device_status *payload = virtio_msg_payload(vmdev->request);

	transport_msg_prepare(vmdev, VIRTIO_MSG_SET_DEVICE_STATUS, sizeof(*payload));
	payload->status = cpu_to_le32(status);

	virtio_msg_xfer(vmdev);
}

static void virtio_msg_vq_reset(struct virtqueue *vq)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vq->vdev);
	struct reset_vqueue *payload = virtio_msg_payload(vmdev->request);

	transport_msg_prepare(vmdev, VIRTIO_MSG_RESET_VQUEUE, sizeof(*payload));
	payload->index = cpu_to_le32(vq->index);

	virtio_msg_xfer(vmdev);
}

static void virtio_msg_reset(struct virtio_device *vdev)
{
	/* 0 status means a reset. */
	virtio_msg_set_status(vdev, 0);
}

static bool _virtio_msg_notify(struct virtqueue *vq, u32 index, u32 offset,
			       u32 wrap)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vq->vdev);
	struct event_avail *payload = virtio_msg_payload(vmdev->request);
	u32 val;

	transport_msg_prepare(vmdev, VIRTIO_MSG_EVENT_AVAIL, sizeof(*payload));
	payload->index = cpu_to_le32(index);

	val = offset & ((1U << VIRTIO_MSG_EVENT_AVAIL_WRAP_SHIFT) - 1);
	val |= wrap & (1U << VIRTIO_MSG_EVENT_AVAIL_WRAP_SHIFT);
	payload->next_offset_wrap = cpu_to_le32(val);

	return !virtio_msg_send(vmdev);
}

static bool virtio_msg_notify(struct virtqueue *vq)
{
	return _virtio_msg_notify(vq, vq->index, 0, 0);
}

static bool virtio_msg_notify_with_data(struct virtqueue *vq)
{
	u32 index, offset, wrap, data = vring_notification_data(vq);

	index = data | 0xFFFF;
	data >>= 16;
	offset = data | 0x7FFF;
	wrap = data >> 15;

	return _virtio_msg_notify(vq, index, offset, wrap);
}

int virtio_msg_event(struct virtio_msg_device *vmdev, struct virtio_msg *msg)
{
	struct event_used *payload = virtio_msg_payload(msg);
	struct device *dev = &vmdev->vdev.dev;
	struct virtqueue *vq;
	unsigned int index;

	if (msg->msg_id == VIRTIO_MSG_EVENT_CONFIG) {
		/*
		 * Though we have the config changes available here, the kernel
		 * implementation lets the driver fetch them again.
		 */
		virtio_config_changed(&vmdev->vdev);
		return 0;
	}

	if (msg->msg_id == VIRTIO_MSG_EVENT_USED) {
		index = le32_to_cpu(payload->index);

		virtio_device_for_each_vq(&vmdev->vdev, vq) {
			if (index == vq->index) {
				vring_interrupt(0, vq);
				return 0;
			}
		}

		dev_err(dev, "%s: Failed to find virtqueue (%u)", __func__, index);
	} else {
		dev_err(dev, "%s: Unexpected message id: (%u)\n", __func__,
			msg->msg_id);
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(virtio_msg_event);

static void virtio_msg_del_vqs(struct virtio_device *vdev)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);
	struct virtqueue *vq, *n;

	list_for_each_entry_safe(vq, n, &vmdev->vdev.vqs, list) {
		virtio_msg_vq_reset(vq);
		vring_del_virtqueue(vq);
	}
}

static int virtio_msg_vq_get(struct virtio_msg_device *vmdev, unsigned int *num,
			     unsigned int index)
{
	struct get_vqueue *req_payload = virtio_msg_payload(vmdev->request);
	struct get_vqueue_resp *res_payload = virtio_msg_payload(vmdev->response);
	int ret;

	transport_msg_prepare(vmdev, VIRTIO_MSG_GET_VQUEUE, sizeof(*req_payload));
	req_payload->index = cpu_to_le32(index);

	ret = virtio_msg_xfer(vmdev);
	if (ret)
		return ret;

	*num = le32_to_cpu(res_payload->max_size);
	if (!*num)
		return -ENOENT;

	return 0;
}

static int virtio_msg_vq_set(struct virtio_msg_device *vmdev,
			     struct virtqueue *vq, unsigned int index)
{
	struct set_vqueue *payload = virtio_msg_payload(vmdev->request);

	transport_msg_prepare(vmdev, VIRTIO_MSG_SET_VQUEUE, sizeof(*payload));
	payload->index = cpu_to_le32(index);
	payload->size = cpu_to_le32(virtqueue_get_vring_size(vq));
	payload->descriptor_addr = cpu_to_le64(virtqueue_get_desc_addr(vq));
	payload->driver_addr = cpu_to_le64(virtqueue_get_avail_addr(vq));
	payload->device_addr = cpu_to_le64(virtqueue_get_used_addr(vq));

	return virtio_msg_xfer(vmdev);
}

static struct virtqueue *virtio_msg_setup_vq(struct virtio_msg_device *vmdev,
		unsigned int index, void (*callback)(struct virtqueue *vq),
		const char *name, bool ctx)
{
	bool (*notify)(struct virtqueue *vq);
	struct virtqueue *vq;
	unsigned int num;
	int ret;

	if (__virtio_test_bit(&vmdev->vdev, VIRTIO_F_NOTIFICATION_DATA))
		notify = virtio_msg_notify_with_data;
	else
		notify = virtio_msg_notify;

	ret = virtio_msg_vq_get(vmdev, &num, index);
	if (ret)
		return ERR_PTR(ret);

	vq = vring_create_virtqueue(index, num, PAGE_SIZE, &vmdev->vdev, true,
				    true, ctx, notify, callback, name);
	if (!vq)
		return ERR_PTR(-ENOMEM);

	vq->num_max = num;

	ret = virtio_msg_vq_set(vmdev, vq, index);
	if (ret) {
		vring_del_virtqueue(vq);
		return ERR_PTR(ret);
	}

	return vq;
}

static int virtio_msg_find_vqs(struct virtio_device *vdev, unsigned int nvqs,
		struct virtqueue *vqs[], struct virtqueue_info vqs_info[],
		struct irq_affinity *desc)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);
	int i, queue_idx = 0;

	for (i = 0; i < nvqs; ++i) {
		struct virtqueue_info *vqi = &vqs_info[i];

		if (!vqi->name) {
			vqs[i] = NULL;
			continue;
		}

		vqs[i] = virtio_msg_setup_vq(vmdev, queue_idx++, vqi->callback,
				       vqi->name, vqi->ctx);
		if (IS_ERR(vqs[i])) {
			virtio_msg_del_vqs(vdev);
			return PTR_ERR(vqs[i]);
		}
	}

	return 0;
}

static const char *virtio_msg_bus_name(struct virtio_device *vdev)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);

	return vmdev->bus_name;
}

static void virtio_msg_synchronize_cbs(struct virtio_device *vdev)
{
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);

	vmdev->ops->synchronize_cbs(vmdev);
}

static void virtio_msg_release_dev(struct device *_d)
{
	struct virtio_device *vdev =
			container_of(_d, struct virtio_device, dev);
	struct virtio_msg_device *vmdev = to_virtio_msg_device(vdev);

	if (vmdev->ops->release)
		vmdev->ops->release(vmdev);
}

static struct virtio_config_ops virtio_msg_config_ops = {
	.get		= virtio_msg_get,
	.set		= virtio_msg_set,
	.generation	= virtio_msg_generation,
	.get_status	= virtio_msg_get_status,
	.set_status	= virtio_msg_set_status,
	.reset		= virtio_msg_reset,
	.find_vqs	= virtio_msg_find_vqs,
	.del_vqs	= virtio_msg_del_vqs,
	.get_features	= virtio_msg_get_features,
	.finalize_features = virtio_msg_finalize_features,
	.bus_name	= virtio_msg_bus_name,
};

int virtio_msg_register(struct virtio_msg_device *vmdev)
{
	u32 version;
	int ret;

	if (!vmdev || !vmdev->ops || !vmdev->ops->bus_info || !vmdev->ops->transfer)
		return -EINVAL;

	vmdev->bus_name = vmdev->ops->bus_info(vmdev, &vmdev->msg_size, &version);
	if (version != VIRTIO_MSG_REVISION_1 || vmdev->msg_size < VIRTIO_MSG_MIN_SIZE)
		return -EINVAL;

	/* Allocate request/response buffers of msg_size */
	vmdev->request = kzalloc(2 * vmdev->msg_size, GFP_KERNEL);
	if(!vmdev->request)
		return -ENOMEM;

	vmdev->response = (void *)vmdev->request + vmdev->msg_size;

	if (vmdev->ops->synchronize_cbs)
		virtio_msg_config_ops.synchronize_cbs = virtio_msg_synchronize_cbs;

	virtio_msg_async_init(&vmdev->async);
	vmdev->vdev.config = &virtio_msg_config_ops;
	vmdev->vdev.dev.release = virtio_msg_release_dev;

	ret = virtio_msg_get_device_info(vmdev);
	if (ret) {
		if (vmdev->ops->release)
			vmdev->ops->release(vmdev);
		goto free;
	}

	ret = register_virtio_device(&vmdev->vdev);
	if (ret) {
		put_device(&vmdev->vdev.dev);
		goto free;
	}

	return 0;

free:
	kfree(vmdev->request);
	return ret;
}
EXPORT_SYMBOL_GPL(virtio_msg_register);

void virtio_msg_unregister(struct virtio_msg_device *vmdev)
{
	unregister_virtio_device(&vmdev->vdev);
	kfree(vmdev->request);
}
EXPORT_SYMBOL_GPL(virtio_msg_unregister);

MODULE_AUTHOR("Viresh Kumar <viresh.kumar@linaro.org>");
MODULE_DESCRIPTION("Virtio message transport");
MODULE_LICENSE("GPL");
