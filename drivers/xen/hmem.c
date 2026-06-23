// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/hmm.h>
#include <linux/interval_tree.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/vmalloc.h>
#include <xen/xen.h>
#include <xen/interface/memory.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/hypervisor.h>

#include "hmem.h"

struct hmem {
	domid_t domid;
	struct mm_struct *mm;
	struct rb_root_cached slots;
	struct mutex mutex;
};

/*
 * Choose SLOT_MAX_PAGES so that it is smaller to the maximum number of pages
 * a single XENMEM_add_to_physmap_range hypercall can map, which is USHRT_MAX.
 */
#define SLOT_MAX_PAGES 0x4000

/* HVA to GPA range map */
struct hmemslot {
	struct hmem *hmem;
	unsigned long gfn;			/* start GFN */
	unsigned long npages;			/* range size in pages */
	DECLARE_BITMAP(mapped, SLOT_MAX_PAGES);	/* bitmap of mapped HVAs */
	struct interval_tree_node node;
	struct mmu_interval_notifier notifier;
	struct mutex mutex;
};

/* Remove GPA range from guest p2m */
static int remove_from_physmap(domid_t domid, unsigned long npages,
			       unsigned long gfn)
{
	struct xen_add_to_physmap_range xatp;
	unsigned long *xatp_gfns, *xatp_hfns;
	int *xatp_errs, i, ret;

	xatp_errs = kvmalloc_array(npages, sizeof(*xatp_errs), GFP_KERNEL);
	xatp_gfns = kvmalloc_array(npages, sizeof(*xatp_gfns), GFP_KERNEL);
	xatp_hfns = kvmalloc_array(npages, sizeof(*xatp_hfns), GFP_KERNEL);
	if (!xatp_errs || !xatp_gfns || !xatp_hfns) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < npages; i++) {
		xatp_gfns[i] = gfn++;
		xatp_hfns[i] = XEN_INVALID_GFN;
	}

	xatp.domid = domid;
	xatp.space = XENMAPSPACE_gmfn_host;
	xatp.size = npages;

	set_xen_guest_handle(xatp.idxs, xatp_hfns);
	set_xen_guest_handle(xatp.gpfns, xatp_gfns);
	set_xen_guest_handle(xatp.errs, xatp_errs);

	ret = HYPERVISOR_memory_op(XENMEM_add_to_physmap_range, &xatp);
	if (ret) {
		pr_err_ratelimited("%s: XENMEM_add_to_physmap_range ret=%d\n",
				   __func__, ret);
		goto out;
	}

	for (i = 0; i < npages; i++) {
		if (xatp_errs[i]) {
			ret = -EINVAL;
			break;
		}
	}

out:
	kvfree(xatp_errs);
	kvfree(xatp_gfns);
	kvfree(xatp_hfns);

	return ret;
}

/* Map GPA range to an array of HPAs in guest p2m */
static int add_to_physmap(domid_t domid, unsigned long npages,
			  unsigned long gfn, unsigned long *hfns)

{
	struct xen_add_to_physmap_range xatp;
	unsigned long *xatp_gfns, *xatp_hfns = hfns;
	int *xatp_errs, i, ret;

	xatp_errs = kvmalloc_array(npages, sizeof(*xatp_errs), GFP_KERNEL);
	xatp_gfns = kvmalloc_array(npages, sizeof(*xatp_gfns), GFP_KERNEL);
	if (!xatp_errs || !xatp_gfns) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < npages; i++)
		xatp_gfns[i] = gfn++;

	xatp.domid = domid;
	xatp.space = XENMAPSPACE_gmfn_host;
	xatp.size = npages;

	set_xen_guest_handle(xatp.idxs, xatp_hfns);
	set_xen_guest_handle(xatp.gpfns, xatp_gfns);
	set_xen_guest_handle(xatp.errs, xatp_errs);

	ret = HYPERVISOR_memory_op(XENMEM_add_to_physmap_range, &xatp);
	if (ret) {
		pr_err_ratelimited("%s: ret=%d\n", __func__, ret);
		goto out;
	}

	for (i = 0; i < npages; i++) {
		if (xatp_errs[i]) {
			pr_err_ratelimited("%s: gfn=0x%lx err=%d\n", __func__,
					   xatp_gfns[i], xatp_errs[i]);
			ret = xatp_errs[i];
		}
	}

out:
	kvfree(xatp_errs);
	kvfree(xatp_gfns);

	return ret;
}

static inline unsigned long hva_to_gfn(unsigned long hva,
				       struct hmemslot *slot)
{
	unsigned long offset = (hva - slot->node.start) >> PAGE_SHIFT;
	return slot->gfn + offset;
}

static int get_hfns_from_pfnmap(struct vm_area_struct *vma, unsigned long hva,
				unsigned long npages, unsigned long *hfns)
{
	unsigned long fhva = hva;
	unsigned int i;
	int ret;

	for (i = 0; i < npages; i++, fhva += PAGE_SIZE) {
		struct follow_pfnmap_args args;

		args.vma = vma;
		args.address = fhva;
		ret = follow_pfnmap_start(&args);
		if (ret) {
			bool unlocked = false;

			ret = fixup_user_fault(vma->vm_mm, fhva,
					       FAULT_FLAG_WRITE,
					       &unlocked);
			if (unlocked)
				return -EAGAIN;
			if (!ret)
				ret = follow_pfnmap_start(&args);
		}
		if (ret) {
			pr_err_ratelimited("%s: failed [%d]\n", __func__, ret);
			return ret;
		}
		hfns[i] = args.pfn;
		follow_pfnmap_end(&args);
	}
	return 0;
}

static int get_hfns_from_hmm(struct hmm_range *range,
			     unsigned long npages, unsigned long *hfns)
{
	unsigned int i;
	int ret;

	ret = hmm_range_fault(range);
	if (ret == -EBUSY)
		return -EAGAIN;
	if (ret) {
		pr_err_ratelimited("%s: failed [%d]\n", __func__, ret);
		return ret;
	}

	for (i = 0; i < npages; i++) {
		if (!(hfns[i] & HMM_PFN_ERROR) && (hfns[i] & HMM_PFN_VALID)) {
			hfns[i] &= ~HMM_PFN_FLAGS;
		} else {
			pr_err_ratelimited("%s: failed flags=0x%lx\n",
					   __func__, hfns[i] & HMM_PFN_FLAGS);
			return -EFAULT;
		}
	}

	return 0;
}

/* Map an HVA range into a GPA range */
static int map_range(struct vm_area_struct *vma, struct hmemslot *slot,
		     unsigned long hva, unsigned long npages, unsigned long gfn,
		     unsigned long *hfns)
{
	struct hmem *hmem = slot->hmem;
	unsigned long slot_offs = (hva - slot->node.start) >> PAGE_SHIFT;
	unsigned int rs, re;
	unsigned long seq;
	int ret;

	seq = mmu_interval_read_begin(&slot->notifier);

	if (vma->vm_flags & (VM_IO | VM_PFNMAP)) {
		ret = get_hfns_from_pfnmap(vma, hva, npages, hfns);
	} else {
		struct hmm_range range;
		range.notifier = &slot->notifier;
		range.hmm_pfns = hfns;
		range.start = hva;
		range.end = hva + npages * PAGE_SIZE;
		range.pfn_flags_mask = 0;
		range.default_flags = HMM_PFN_REQ_FAULT | HMM_PFN_REQ_WRITE;
		range.dev_private_owner = NULL;
		range.notifier_seq = seq;
		ret = get_hfns_from_hmm(&range, npages, hfns);
	}
	if (ret)
		return ret;

	mutex_lock(&slot->mutex);
	if (mmu_interval_read_retry(&slot->notifier, seq)) {
		mutex_unlock(&slot->mutex);
		return -EAGAIN;
	}

	rs = slot_offs;
	for_each_clear_bitrange_from(rs, re, slot->mapped, slot_offs + npages) {
		ret = add_to_physmap(hmem->domid, re - rs,
				     gfn + rs - slot_offs, hfns + rs - slot_offs);
		if (!ret)
			bitmap_set(slot->mapped, rs, re - rs);
	}
	mutex_unlock(&slot->mutex);

	return ret;
}

/* Called with hmem->mutex held */
static int hmem_range_sync(struct hmem *hmem,
			   unsigned long start, unsigned long npages)
{
	unsigned long last = start + (npages << PAGE_SHIFT) - 1;
	unsigned int max_hfns = min_t(unsigned int, SLOT_MAX_PAGES, npages);
	struct interval_tree_node *node;
	unsigned long *hfns;
	int ret = 0;

	hfns = kvmalloc_array(max_hfns, sizeof(*hfns), GFP_KERNEL);
	if (!hfns)
		return -ENOMEM;

	mmap_read_lock(hmem->mm);

	for (node = interval_tree_iter_first(&hmem->slots, start, last);
	     node; node = interval_tree_iter_next(node, start, last)) {
		struct hmemslot *slot;
		unsigned long hva, ehva;
		unsigned long gfn;
		unsigned long nr;

		slot = container_of(node, struct hmemslot, node);
		hva = max(start, slot->node.start);
		ehva = min(last, slot->node.last);
		gfn = hva_to_gfn(hva, slot);
		nr = ((ehva - hva) >> PAGE_SHIFT) + 1;

		while (nr) {
			struct vm_area_struct *vma;
			unsigned int vma_npages;

			vma = vma_lookup(hmem->mm, hva);
			if (!vma) {
				pr_err("%s: hva=0x%lx vma_lookup failed\n",
					__func__, hva);
				ret = -EINVAL;
				break;
			}

			vma_npages = min_t(unsigned int, nr,
					   (vma->vm_end - hva) / PAGE_SIZE);
			ret = map_range(vma, slot, hva, vma_npages, gfn, hfns);
			if (ret == -EAGAIN)
				continue;
			if (ret)
				break;

			nr -= vma_npages;
			hva += vma_npages * PAGE_SIZE;
			gfn += vma_npages;
		}

		if (ret)
			break;
	}

	mmap_read_unlock(hmem->mm);
	kvfree(hfns);

	return ret;
}

static int hmem_range_unmap(struct hmemslot *slot,
			    unsigned long start, unsigned long last)
{
	struct hmem *hmem = slot->hmem;
	domid_t domid = hmem->domid;
	unsigned long shva, ehva;
	unsigned long offset, nr;
	unsigned int rs, re;
	int ret;

	shva = max(start, slot->node.start);
	ehva = min(last, slot->node.last);
	offset = (shva - slot->node.start) >> PAGE_SHIFT;
	nr = ((ehva - shva) >> PAGE_SHIFT) + 1;

	rs = offset;

	for_each_set_bitrange_from(rs, re, slot->mapped, offset + nr) {
		ret = remove_from_physmap(domid, re - rs, slot->gfn + rs);
		if (ret) {
			pr_err("%s: failed hva=0x%lx gfn=0x%lx nr=%d\n",
			       __func__, start + rs * PAGE_SIZE,
			       slot->gfn + rs, re - rs);
			return ret;
		}
		bitmap_clear(slot->mapped, rs, re - rs);
	}

	return 0;
}

static bool hmem_invalidate(struct mmu_interval_notifier *mni,
			    const struct mmu_notifier_range *range,
			    unsigned long cur_seq)
{
	struct hmemslot *slot;
	unsigned long start = range->start;
	unsigned long last = range->end - 1;
	int ret;

	slot = container_of(mni, struct hmemslot, notifier);

	if (!mmu_notifier_range_blockable(range))
		return false;

	mutex_lock(&slot->mutex);
	mmu_interval_set_seq(mni, cur_seq);
	ret = hmem_range_unmap(slot, start, last);
	mutex_unlock(&slot->mutex);

	WARN_ONCE(ret, "Failed to unmap host mem from guest\n");

	return true;
}

static const struct mmu_interval_notifier_ops hmem_mn_ops = {
	.invalidate = hmem_invalidate,
};

static struct hmemslot *create_slot(struct hmem *hmem, unsigned long start,
				    unsigned int npages, unsigned long gfn)
{
	struct hmemslot *slot;

	slot = vzalloc(sizeof(*slot));
	if (!slot)
		return NULL;

	slot->node.start = start;
	slot->node.last = start + (npages << PAGE_SHIFT) - 1;
	slot->gfn = gfn;
	slot->npages = npages;
	slot->hmem = hmem;
	mutex_init(&slot->mutex);
	return slot;
}

/*
 * Called after slot has been removed from hmem->slots and its notifier
 * has been unregistered, so there is no concurrent access to slot->mapped.
 */
static int split_slot(struct hmem *hmem, struct hmemslot *slot,
		      unsigned long start, unsigned long last)
{
	struct hmemslot *new0 = NULL, *new1 = NULL;
	int ret = 0;

	if (start > slot->node.start) {
		unsigned long npages;

		npages = (start - slot->node.start) >> PAGE_SHIFT;

		new0 = create_slot(hmem, slot->node.start, npages, slot->gfn);
		if (!new0)
			return -ENOMEM;

		bitmap_copy(new0->mapped, slot->mapped, npages);
		interval_tree_insert(&new0->node, &hmem->slots);
		ret = mmu_interval_notifier_insert(&new0->notifier, hmem->mm,
						   new0->node.start,
						   new0->npages << PAGE_SHIFT,
						   &hmem_mn_ops);
		if (ret) {
			interval_tree_remove(&new0->node, &hmem->slots);
			return ret;
		}
	}

	if (last < slot->node.last) {
		unsigned long npages, offs;

		npages = (slot->node.last - last) >> PAGE_SHIFT;
		offs = slot->npages - npages;

		new1 = create_slot(hmem, last + 1, npages, slot->gfn + offs);
		if (!new1) {
			ret = -ENOMEM;
			goto out;
		}

		bitmap_shift_right(new1->mapped, slot->mapped, offs, slot->npages);
		interval_tree_insert(&new1->node, &hmem->slots);
		ret = mmu_interval_notifier_insert(&new1->notifier, hmem->mm,
						   new1->node.start,
						   new1->npages << PAGE_SHIFT,
						   &hmem_mn_ops);
		if (ret) {
			interval_tree_remove(&new1->node, &hmem->slots);
			vfree(new1);
		}
	}

out:
	if (ret) {
		if (new0) {
			interval_tree_remove(&new0->node, &hmem->slots);
			mmu_interval_notifier_remove(&new0->notifier);
			vfree(new0);
		}
		pr_err_ratelimited("%s: failed\n", __func__);
	}
	return ret;
}

/* Called with hmem->mutex held */
static int hmem_range_remove(struct hmem *hmem, unsigned long hva,
			     unsigned long npages)
{
	struct interval_tree_node *node;
	unsigned long start, last;

	start = hva;
	last = hva + (npages << PAGE_SHIFT) - 1;

	while ((node = interval_tree_iter_first(&hmem->slots, start, last))) {
		struct hmemslot *slot;
		unsigned long shva, ehva;
		unsigned long offset, nr;
		unsigned int rs, re;
		int ret;

		slot = container_of(node, struct hmemslot, node);
		interval_tree_remove(&slot->node, &hmem->slots);
		mmu_interval_notifier_remove(&slot->notifier);

		shva = max(start, slot->node.start);
		ehva = min(last, slot->node.last);
		offset = (shva - slot->node.start) >> PAGE_SHIFT;
		nr = ((ehva - shva) >> PAGE_SHIFT) + 1;

		if (start > slot->node.start || last < slot->node.last) {
			ret = split_slot(hmem, slot, start, last);
			/* If split fails, unmap the entire slot */
			if (ret) {
				offset = 0;
				nr = slot->npages;
			}
		}

		rs = offset;
		for_each_set_bitrange_from(rs, re, slot->mapped, offset + nr) {
			remove_from_physmap(hmem->domid, re - rs, slot->gfn + rs);
			bitmap_clear(slot->mapped, rs, re - rs);
		}

		vfree(slot);
	}

	return 0;
}

/* Called with hmem->mutex held */
static int hmem_range_insert(struct hmem *hmem,
			     unsigned long hva, unsigned long npages,
			     unsigned long gfn)
{
	struct interval_tree_node *node;
	unsigned long start = hva;
	unsigned long last = start + (npages << PAGE_SHIFT) - 1;
	int ret = 0, done = 0;

	node = interval_tree_iter_first(&hmem->slots, start, last);
	if (node) {
		pr_err_ratelimited("%s: already exists 0x%lx-0x%lx\n",
				   __func__, start, last);
		return -EEXIST;
	}

	while (done < npages) {
		struct hmemslot *slot;
		unsigned long slot_npages;

		slot_npages = min_t(unsigned long, SLOT_MAX_PAGES, npages - done);
		slot = create_slot(hmem, hva, slot_npages, gfn);
		if (!slot) {
			ret = -ENOMEM;
			break;
		}

		interval_tree_insert(&slot->node, &hmem->slots);
		ret = mmu_interval_notifier_insert(&slot->notifier, hmem->mm,
						   hva, slot_npages << PAGE_SHIFT,
						   &hmem_mn_ops);
		if (ret) {
			interval_tree_remove(&slot->node, &hmem->slots);
			vfree(slot);
			break;
		}

		done += slot_npages;
		gfn += slot_npages;
		hva += slot_npages << PAGE_SHIFT;
	}

	if (!ret)
		ret = hmem_range_sync(hmem, start, npages);

	if (ret)
		hmem_range_remove(hmem, start, npages);

	return ret;
}

static void hmem_tree_destroy(struct hmem *hmem)
{
	struct interval_tree_node *node;

	mutex_lock(&hmem->mutex);
	while ((node = interval_tree_iter_first(&hmem->slots, 0, ULONG_MAX))) {
		struct hmemslot *slot;
		unsigned int rs, re;

		slot = container_of(node, struct hmemslot, node);
		interval_tree_remove(&slot->node, &hmem->slots);
		mmu_interval_notifier_remove(&slot->notifier);

		rs = 0;
		for_each_set_bitrange_from(rs, re, slot->mapped, slot->npages) {
			remove_from_physmap(hmem->domid, re - rs, slot->gfn + rs);
			bitmap_clear(slot->mapped, rs, re - rs);
		}

		vfree(slot);
	}
	mutex_unlock(&hmem->mutex);
}

int hmem_handle_op(struct hmem *hmem, struct privcmd_hmem_op op)
{
	int ret = -EINVAL;

	if (!op.num) {
		pr_warn_ratelimited("%s: npages == 0\n", __func__);
		return 0;
	}

	/* HVAs should be page aligned */
	if (op.hva & ~PAGE_MASK) {
		pr_err_ratelimited("%s: HVA not page aligned\n", __func__);
		return ret;
	}

	mutex_lock(&hmem->mutex);
	if ((hmem->domid != DOMID_INVALID) && (hmem->domid != op.dom)) {
		pr_err_ratelimited("%s: hmem->domid=%u != op.dom=%u\n",
				   __func__, hmem->domid, op.dom);
		mutex_unlock(&hmem->mutex);
		return -EPERM;
	}
	if (!hmem->mm) {
		hmem->mm = current->mm;
		mmgrab(hmem->mm);
		hmem->domid = op.dom;
	}

	switch (op.op) {
	case HMEM_ADD_MAPPING:
		ret = hmem_range_insert(hmem, op.hva, op.num, op.gfn);
		break;
	case HMEM_REMOVE_MAPPING:
		ret = hmem_range_remove(hmem, op.hva, op.num);
		break;
	case HMEM_SYNC_MAPPING:
		ret = hmem_range_sync(hmem, op.hva, op.num);
		break;
	default:
		pr_err_ratelimited("%s: unknown op %u\n", __func__, op.op);
		break;
	}
	mutex_unlock(&hmem->mutex);

	return ret;
}

struct hmem *hmem_init(domid_t domid)
{
	struct hmem *hmem;

	hmem = kzalloc(sizeof(*hmem), GFP_KERNEL);
	if (!hmem)
		return NULL;
	hmem->domid = domid;
	hmem->slots = RB_ROOT_CACHED;
	mutex_init(&hmem->mutex);

	return hmem;
}

void hmem_destroy(struct hmem *hmem)
{
	if (!hmem)
		return;

	hmem_tree_destroy(hmem);
	if (hmem->mm)
		mmdrop(hmem->mm);
	kfree(hmem);
}
