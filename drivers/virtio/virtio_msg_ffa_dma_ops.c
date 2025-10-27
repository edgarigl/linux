// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtio message transport DMA OPS.
 *
 * Copyright (C) 2025 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * The Virtio message transport allows virtio devices to be used over a virtual
 * virtio-msg channel. The channel interface is meant to be implemented using
 * the architecture specific hardware-assisted fast path, like ARM Firmware
 * Framework (FFA).
 */

#define pr_fmt(fmt) "virtio-msg-dma-ops: " fmt

#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/of_reserved_mem.h>
#include <linux/virtio.h>
#include <linux/virtio_anchor.h>
#include <uapi/linux/virtio_msg.h>

#include "virtio_msg_internal.h"

static void *virtio_msg_dma_alloc(struct device *dev, size_t size,
				  dma_addr_t *dma_handle, gfp_t gfp,
				  unsigned long attrs)
{
	size_t n_pages = PFN_UP(size);
	void *vaddr;
	int ret;

	vaddr = dma_direct_alloc(dev, size, dma_handle, gfp, attrs);
	if (!vaddr)
		return NULL;

	ret = vmsg_ffa_bus_area_share(dev, vaddr, n_pages, dma_handle);
	if (ret) {
		dma_direct_free(dev, size, vaddr, *dma_handle, attrs);
		return NULL;
	}

	return vaddr;
}

static void virtio_msg_dma_free(struct device *dev, size_t size, void *vaddr,
				dma_addr_t dma_handle, unsigned long attrs)
{
	size_t n_pages = PFN_UP(size);
	int ret;

	ret = vmsg_ffa_bus_area_unshare(dev, &dma_handle, n_pages);
	if (ret)
		dev_err(dev, "%s: Failed to unshare area: %d", __func__, ret);

	dma_direct_free(dev, n_pages * PAGE_SIZE, vaddr, dma_handle, attrs);
}

static struct page *virtio_msg_dma_alloc_pages(struct device *dev, size_t size,
					      dma_addr_t *dma_handle,
					      enum dma_data_direction dir,
					      gfp_t gfp)
{
	void *vaddr;

	vaddr = virtio_msg_dma_alloc(dev, size, dma_handle, gfp, 0);
	if (!vaddr)
		return NULL;

	return virt_to_page(vaddr);
}

static void virtio_msg_dma_free_pages(struct device *dev, size_t size,
				     struct page *page, dma_addr_t dma_handle,
				     enum dma_data_direction dir)
{
	virtio_msg_dma_free(dev, size, page_to_virt(page), dma_handle, 0);
}

static dma_addr_t virtio_msg_dma_map_page(struct device *dev, struct page *page,
					 unsigned long offset, size_t size,
					 enum dma_data_direction dir,
					 unsigned long attrs)
{
	size_t n_pages = PFN_UP(offset + size);
	unsigned long dma_offset;
	dma_addr_t dma_handle;

	if (WARN_ON(dir == DMA_NONE))
		return DMA_MAPPING_ERROR;

	if (!is_swiotlb_force_bounce(dev))
		return DMA_MAPPING_ERROR;

	dma_handle = swiotlb_map(dev, page_to_phys(page) + offset, size, dir, attrs);
	if (dma_handle == DMA_MAPPING_ERROR)
		return DMA_MAPPING_ERROR;

	dma_offset = offset_in_page(dma_handle);
	dma_handle -= dma_offset;
	if (vmsg_ffa_bus_area_share(dev, phys_to_virt(dma_handle), n_pages,
				    &dma_handle))
		return DMA_MAPPING_ERROR;

	return dma_handle + dma_offset;
}

static void virtio_msg_dma_unmap_page(struct device *dev, dma_addr_t dma_handle,
				     size_t size, enum dma_data_direction dir,
				     unsigned long attrs)
{
	unsigned long dma_offset = offset_in_page(dma_handle);
	unsigned int n_pages = PFN_UP(size);
	int ret;

	if (WARN_ON(dir == DMA_NONE))
		return;

	dma_handle -= dma_offset;

	ret = vmsg_ffa_bus_area_unshare(dev, &dma_handle, n_pages);
	if (ret)
		dev_err(dev, "%s: Failed to unshare area: %d", __func__, ret);

	dma_handle += dma_offset;
	swiotlb_tbl_unmap_single(dev, dma_to_phys(dev, dma_handle), size, dir,
			attrs);
}

static void virtio_msg_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
				   int nents, enum dma_data_direction dir,
				   unsigned long attrs)
{
	struct scatterlist *s;
	unsigned int i;

	if (WARN_ON(dir == DMA_NONE))
		return;

	for_each_sg(sg, s, nents, i)
		virtio_msg_dma_unmap_page(dev, s->dma_address, sg_dma_len(s), dir,
				attrs);
}

static int virtio_msg_dma_map_sg(struct device *dev, struct scatterlist *sg,
				int nents, enum dma_data_direction dir,
				unsigned long attrs)
{
	struct scatterlist *s;
	unsigned int i;

	if (WARN_ON(dir == DMA_NONE))
		return -EINVAL;

	for_each_sg(sg, s, nents, i) {
		s->dma_address = virtio_msg_dma_map_page(dev, sg_page(s), s->offset,
				s->length, dir, attrs);
		if (s->dma_address == DMA_MAPPING_ERROR)
			goto out;

		sg_dma_len(s) = s->length;
	}

	return nents;

out:
	virtio_msg_dma_unmap_sg(dev, sg, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);
	sg_dma_len(sg) = 0;

	return -EIO;
}

static int virtio_msg_dma_supported(struct device *dev, u64 mask)
{
	return mask == DMA_BIT_MASK(64);
}

const struct dma_map_ops virtio_msg_ffa_dma_ops = {
	.alloc = virtio_msg_dma_alloc,
	.free = virtio_msg_dma_free,
	.alloc_pages_op = virtio_msg_dma_alloc_pages,
	.free_pages = virtio_msg_dma_free_pages,
	.mmap = dma_common_mmap,
	.get_sgtable = dma_common_get_sgtable,
	.map_page = virtio_msg_dma_map_page,
	.unmap_page = virtio_msg_dma_unmap_page,
	.map_sg = virtio_msg_dma_map_sg,
	.unmap_sg = virtio_msg_dma_unmap_sg,
	.dma_supported = virtio_msg_dma_supported,
};

int virtio_msg_ffa_share_rmem(struct device *dev, struct reserved_mem *rmem,
			      dma_addr_t *dma_handle)
{
	int ret;

	if (IS_ERR(rmem))
		return 0;

	ret = vmsg_ffa_bus_area_share(dev, phys_to_virt(rmem->base),
				      PFN_UP(rmem->size), dma_handle);
	if (!ret)
		dev->dma_ops = &virtio_msg_ffa_dma_ops;
	else if (ret != EOPNOTSUPP)
		return 0;

	return ret;
}
EXPORT_SYMBOL_GPL(virtio_msg_ffa_share_rmem);

void virtio_msg_ffa_unshare_rmem(struct device *dev, struct reserved_mem *rmem,
				 dma_addr_t *dma_handle)
{
	if (!IS_ERR(rmem))
		vmsg_ffa_bus_area_unshare(dev, dma_handle, PFN_UP(rmem->size));
}
EXPORT_SYMBOL_GPL(virtio_msg_ffa_unshare_rmem);

static bool virtio_msg_dma_ops_init(struct virtio_device *dev)
{
	/* DMA OPS should already be set by the underlying bus driver */
	if (dev->dev.parent->dma_ops)
		return true;

	return false;
}

int virtio_msg_ffa_dma_init(void)
{
	virtio_set_mem_acc_cb(virtio_msg_dma_ops_init);
	return 0;
}
