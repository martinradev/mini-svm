#include "mini-svm-intercept.h"
#include "mini-svm-mm.h"

#include <linux/kernel.h>

int mini_svm_intercept_npf(struct mini_svm_context *ctx) {
	u64 fault_phys_address = ctx->vcpu.vmcb->control.exitinfo_v2;
	printk("Received NPF at phys addr: 0x%llx\n", ctx->vcpu.vmcb->control.exitinfo_v2);
	if (fault_phys_address >= MINI_SVM_MAX_PHYS_SIZE) {
		return 1;
	}
	return 1;
}
