#include "mini-svm-mm.h"

#include <asm/pgtable.h>
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <asm/io.h>
#include <linux/vmalloc.h>

static const __u64 MINI_SVM_PRESENT_MASK = 0x1UL;
static const __u64 MINI_SVM_WRITEABLE_MASK = 0x2UL;
static const __u64 MINI_SVM_USER_MASK = 0x4UL;
static const __u64 MINI_SVM_PDE_LEAF_MASK = (1UL << 7U);

static __u64 mini_svm_create_entry(__u64 pa, __u64 mask) {
	return pa | mask;
}

int mini_svm_create_mm(struct mini_svm_mm **out_mm) {
	struct mini_svm_mm *mm = NULL;
	int r;

	mm = kzalloc(sizeof(*mm), GFP_KERNEL);
	if (!mm) {
		r = -ENOMEM;
		goto fail;
	}

	*out_mm = mm;

	return 0;

fail:
	return r;
}

void mini_svm_destroy_mm(struct mini_svm_mm *mm) {
	BUG_ON(!mm);

	mini_svm_destroy_nested_table(mm);
	kfree(mm);
}

int mini_svm_construct_debug_mm_one_page(struct mini_svm_mm *mm) {
	int r;
	int i;
	struct mini_svm_nested_table_pml4 *pml4 = &mm->pml4;

	pml4->va = get_zeroed_page(GFP_KERNEL);
	if (!pml4->va) {
		r = -ENOMEM;
		goto fail;
	}
	pml4->pa = virt_to_phys(pml4->va);

	pml4->pdp.va = get_zeroed_page(GFP_KERNEL);
	if (!pml4->pdp.va) {
		r = -ENOMEM;
		goto fail;
	}
	pml4->pdp.pa = virt_to_phys(pml4->pdp.va);

	pml4->pdp.pd.va = get_zeroed_page(GFP_KERNEL);
	if (!pml4->pdp.pd.va) {
		r = -ENOMEM;
		goto fail;
	}
	pml4->pdp.pd.pa = virt_to_phys(pml4->pdp.pd.va);

	pml4->pdp.pd.memory_2mb_va[0] = __get_free_pages(GFP_KERNEL, get_order(2 * 1024 * 1024));
	if (!pml4->pdp.pd.memory_2mb_va[0]) {
		r = -ENOMEM;
		goto fail;
	}
	pml4->pdp.pd.memory_2mb_pa[0] = virt_to_phys(pml4->pdp.pd.memory_2mb_va[0]);

	pml4->va[0] = mini_svm_create_entry(pml4->pdp.pa, MINI_SVM_PRESENT_MASK | MINI_SVM_WRITEABLE_MASK | MINI_SVM_USER_MASK);
	pml4->pdp.va[0] = mini_svm_create_entry(pml4->pdp.pd.pa, MINI_SVM_PRESENT_MASK | MINI_SVM_WRITEABLE_MASK | MINI_SVM_USER_MASK);

	pml4->pdp.pd.va[0] = mini_svm_create_entry(pml4->pdp.pd.memory_2mb_pa[0], MINI_SVM_PRESENT_MASK | MINI_SVM_WRITEABLE_MASK | MINI_SVM_USER_MASK | MINI_SVM_PDE_LEAF_MASK);

	return 0;

fail:
	if (pml4->va) {
		free_page(pml4->va);
	}
	if (pml4->pdp.va) {
		free_page(pml4->pdp.va);
	}
	if (pml4->pdp.pd.va) {
		free_page(pml4->pdp.pd.va);
	}
	if (pml4->pdp.pd.memory_2mb_va[0]) {
		free_pages(pml4->pdp.pd.memory_2mb_va[0], get_order(2 * 1024 * 1024));
	}
	return r;
}

void mini_svm_destroy_nested_table(struct mini_svm_mm *mm) {
	int i;
	struct mini_svm_nested_table_pml4 *pml4 = &mm->pml4;

	if (pml4->va) {
		free_page(pml4->va);
	}
	if (pml4->pdp.va) {
		free_page(pml4->pdp.va);
	}
	if (pml4->pdp.pd.va) {
		free_page(pml4->pdp.pd.va);
	}
	if (pml4->pdp.pd.memory_2mb_va[0]) {
		free_pages(pml4->pdp.pd.memory_2mb_va[0], get_order(2 * 1024 * 1024));
	}
}

// The start of guest physical memory is for the GPT which currently just takes two physical pages
// Writes to memory at an address lower than this one should be forbidden when they go via write_virt_memory.
#define PHYS_BASE_OFFSET 0x3000U

int mini_svm_mm_write_virt_memory(struct mini_svm_mm *mm, u64 virt_address, void *bytes, u64 num_bytes) {
	if (virt_address < PHYS_BASE_OFFSET) {
		return -EINVAL;
	}
	return mini_svm_mm_write_phys_memory(mm, virt_address, bytes, num_bytes);
}

int mini_svm_mm_write_phys_memory(struct mini_svm_mm *mm, u64 phys_address, void *bytes, u64 num_bytes) {
	void *page_2mb_va;
	unsigned int page_index;
	unsigned int page_offset;
	if (phys_address + num_bytes > 2U * 1024U * 1024U) {
		return -EINVAL;
	}

	memcpy((unsigned char *)mm->pml4.pdp.pd.memory_2mb_va[0] + phys_address, bytes, num_bytes);

	return 0;
}

void mini_svm_construct_1gb_gpt(struct mini_svm_mm *mm) {
	// We just need 2 pages for the page table, which will start at physical address 0 and will have length of 1gig.
	const u64 pml4e = mini_svm_create_entry(0x1000, MINI_SVM_PRESENT_MASK | MINI_SVM_USER_MASK | MINI_SVM_WRITEABLE_MASK);
	const u64 pdpe = mini_svm_create_entry(0x0, MINI_SVM_PRESENT_MASK | MINI_SVM_USER_MASK | MINI_SVM_WRITEABLE_MASK | MINI_SVM_PDE_LEAF_MASK);
	mini_svm_mm_write_phys_memory(mm, 0, &pml4e, sizeof(pml4e));
	mini_svm_mm_write_phys_memory(mm, 0x1000, &pdpe, sizeof(pdpe));
}