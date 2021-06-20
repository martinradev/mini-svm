#include <stddef.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>

#include "mini-svm-user-ioctl.h"
#include "mini-svm-exit-codes.h"
#include "mini-svm-common-structures.h"
#include "mini-svm-vmcb.h"

#include "hv-util.h"
#include "hv-microbench-structures.h"

#define MINI_SVM_MAX_PHYS_SIZE (32UL * 1024UL * 1024UL)

#define EFER_SVME (1UL << 12U)
#define EFER_LME (1UL << 8U)
#define EFER_LMA (1UL << 10)
#define CR0_PE (1UL << 0U)
#define CR0_ET (1UL << 4U)
#define CR0_NW (1UL << 29U)
#define CR0_CD (1UL << 30U)
#define CR0_PG (1UL << 31U)
#define CR4_PAE (1UL << 5U)
#define CR4_PGE (1UL << 7U)

// The start of guest physical memory is for the GPT which currently just takes two physical pages
// Writes to memory at an address lower than this one should be forbidden when they go via write_virt_memory.
#define PHYS_BASE_OFFSET 0x3000U

static void *guest_memory = NULL;

int mini_svm_mm_write_phys_memory(void *phys_base, __u64 phys_address, void *bytes, __u64 num_bytes) {
	if (phys_address + num_bytes > MINI_SVM_MAX_PHYS_SIZE) {
		return false;
	}

	memcpy((unsigned char *)phys_base + phys_address, bytes, num_bytes);

	return true;
}

bool mini_svm_mm_write_virt_memory(void *phys_base, __u64 virt_address, void *bytes, __u64 num_bytes) {
	if (virt_address < PHYS_BASE_OFFSET) {
		return false;
	}
	return mini_svm_mm_write_phys_memory(phys_base, virt_address, bytes, num_bytes);
}

int mini_svm_construct_1gb_gpt(void *phys_base) {
	// We just need 2 pages for the page table, which will start at physical address 0 and will have length of 1gig.
	const __u64 pml4e = mini_svm_create_entry(1024 * 1024 + 0x1000, MINI_SVM_PRESENT_MASK | MINI_SVM_USER_MASK | MINI_SVM_WRITEABLE_MASK);
	const __u64 pdpe = mini_svm_create_entry(0x0, MINI_SVM_PRESENT_MASK | MINI_SVM_USER_MASK | MINI_SVM_WRITEABLE_MASK | MINI_SVM_LEAF_MASK);
	if (!mini_svm_mm_write_phys_memory(phys_base, 1024 * 1024, (void *)&pml4e, sizeof(pml4e))) {
		return false;
	}
	if (!mini_svm_mm_write_phys_memory(phys_base, 1024 * 1024 + 0x1000, (void *)&pdpe, sizeof(pdpe))) {
		return false;
	}
	return true;
}

bool load_vm_program(const char *filename, void *phys_base) {
	int fd;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		printf("Failed to open file: %s\n", filename);
		return false;
	}

	size_t sz = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);

	void *buffer = malloc(sz);
	if (!buffer) {
		printf("Failed to allocate buffer for the image\n");
		return false;
	}
	ssize_t nread = read(fd, buffer, sz);
	if (nread < 0 || nread != sz) {
		printf("Failed to read file. Read: %zd. Expected: %zu\n", nread, sz);
		return false;
	}
	close(fd);

	const __u64 image_base = 2 * 1024 * 1024;
	if (!mini_svm_mm_write_virt_memory(phys_base, image_base, buffer, nread)) {
		return false;
	}

	return true;
}

static void setup_ctrl(struct mini_svm_vmcb_control *ctrl) {
	memset(&ctrl->excp_vec_intercepts, 0xFF, sizeof(ctrl->excp_vec_intercepts));
	ctrl->vec3.hlt_intercept = 1;
	ctrl->vec3.cpuid_intercept = 1;
	ctrl->vec4.vmrun_intercept = 1;
	ctrl->vec4.vmmcall_intercept = 1;
	//ctrl->vec3.rdtsc_intercept = 1;
	//ctrl->vec4.rdtscp_intercept = 1;
}

static void setup_save(struct mini_svm_vmcb_save_area *save) {
	// Setup long mode.
	save->efer = EFER_SVME | EFER_LME | EFER_LMA;
	save->cr0 = (CR0_PE | CR0_PG);
	save->cr3 = (1024 * 1024);
	save->cr4 = (CR4_PAE | CR4_PGE);

	// Setup gdt
	save->reg_gdtr.base = 0x0;
	save->reg_gdtr.limit = -1;

	// Setup segments
	save->reg_cs.base = 0x0;
	save->reg_cs.limit = -1;
	save->reg_cs.attribute = 0x029b;
	save->reg_cs.selector = 0x8;

	save->reg_ss.base = 0;
	save->reg_ss.limit = -1;
	save->reg_ss.attribute = 0x0a93;
	save->reg_ss.selector = 0x10;

	memcpy(&save->reg_ds, &save->reg_ss, sizeof(save->reg_ss));
	memcpy(&save->reg_ss, &save->reg_ss, sizeof(save->reg_ss));
	memcpy(&save->reg_fs, &save->reg_ss, sizeof(save->reg_ss));
	memcpy(&save->reg_gs, &save->reg_ss, sizeof(save->reg_ss));

	// Everything index is cacheable.
	save->g_pat = 0x0606060606060606ULL;
}

static void dump_regs(struct mini_svm_vmcb *vmcb, struct mini_svm_vm_state *state) {
	printf("rip = %llx\n", state->regs.rip);
	printf("rcx = %llx\n", state->regs.rcx);
	printf("rbx = %llx\n", state->regs.rbx);
	printf("rax = %llx\n", state->regs.rax);
}

static void mini_svm_handle_exception(const enum MINI_SVM_EXCEPTION excp) {
	printf("Received exception. # = %x. Name: %s\n", (unsigned)excp, translate_mini_svm_exception_number_to_str(excp));
}

void mini_svm_intercept_rdtsc(struct mini_svm_vm_state *state) {
	state->regs.rax = (state->clock & 0xFFFFFFFFUL);
	state->regs.rdx = ((state->clock >> 32U) & 0xFFFFFFFFUL);
	state->clock++;
}

void mini_svm_intercept_rdtscp(struct mini_svm_vm_state *state) {
	state->regs.rcx = 0x1337UL;
	mini_svm_intercept_rdtsc(state);
}

int mini_svm_intercept_npf(struct mini_svm_vmcb *vmcb) {
	__u64 fault_phys_address = vmcb->control.exitinfo_v2;
	printf("Received NPF at phys addr: 0x%llx\n", vmcb->control.exitinfo_v2);
	if (fault_phys_address >= MINI_SVM_MAX_PHYS_SIZE) {
		return 1;
	}
	return 1;
}

int mini_svm_intercept_cpuid(struct mini_svm_vm_state *state) {
	return 0;
}

int mini_svm_intercept_vmmcall(struct mini_svm_vm_state *state) {
	const unsigned long cmd = state->regs.rax;
	const unsigned long arg1 = state->regs.rdi;
	const unsigned long arg2 = state->regs.rsi;
	const unsigned long arg3 = state->regs.rdx;

	switch(cmd) {
		case VMMCALL_REQUEST_RANDOM_DATA_ACCESS_SEQUENCE:
		{
			const unsigned long start_rand_va = arg1;
			const unsigned long num_elements = arg2;
			const size_t cache_line_size = 64UL;
			std::vector<unsigned long> seq;
			printf("Generate random sequence: %lx %lx\n", start_rand_va, num_elements);
			generate_random_unique_sequence(num_elements, seq);
			unsigned long prev_index = 0;
			for (unsigned long i = 0; i < num_elements; ++i) {
				unsigned long next_index = seq[i] * cache_line_size + start_rand_va;
				if (!mini_svm_mm_write_virt_memory(guest_memory, start_rand_va + cache_line_size * prev_index, &next_index, sizeof(next_index))) {
					printf("Failed to write index\n");
					return -1;
				}
				prev_index = seq[i];
			}
			break;
		}
		case VMMCALL_REPORT_RESULT:
		{
			printf("Result is: %lx\n", arg1);
			break;
		}
		default:
		{
			printf("Unknown cmd: %lx\n", cmd);
			break;
		}
	}

	return 0;
}

static int mini_svm_handle_exit(struct mini_svm_vmcb *vmcb, struct mini_svm_vm_state *state) {
	__u64 exitcode = get_exitcode(&vmcb->control);
	int should_exit = 0;

	printf("exitcode: %llx. Name: %s\n", exitcode, translate_mini_svm_exitcode_to_str((enum MINI_SVM_EXITCODE)exitcode));

	// TODO: Doing this through function pointers for the respective handlers is probably better.
	switch((enum MINI_SVM_EXITCODE)exitcode) {
		case MINI_SVM_EXITCODE_VMEXIT_EXCP_0 ... MINI_SVM_EXITCODE_VMEXIT_EXCP_15:
			mini_svm_handle_exception((enum MINI_SVM_EXCEPTION)(exitcode - MINI_SVM_EXITCODE_VMEXIT_EXCP_0));
			should_exit = 1;
			break;
		case MINI_SVM_EXITCODE_VMEXIT_RDTSC:
			mini_svm_intercept_rdtsc(state);
			break;
		case MINI_SVM_EXITCODE_VMEXIT_RDTSCP:
			mini_svm_intercept_rdtscp(state);
			break;
		case MINI_SVM_EXITCODE_VMEXIT_INVALID:
			should_exit = 1;
			break;
		case MINI_SVM_EXITCODE_VMEXIT_HLT:
			should_exit = 1;
			break;
		case MINI_SVM_EXITCODE_VMEXIT_SHUTDOWN:
			should_exit = 1;
			break;
		case MINI_SVM_EXITCODE_VMEXIT_NPF:
			should_exit = mini_svm_intercept_npf(vmcb);
			break;
		case MINI_SVM_EXITCODE_VMEXIT_CPUID:
			should_exit = mini_svm_intercept_cpuid(state);
			break;
		case MINI_SVM_EXITCODE_VMEXIT_VMMCALL:
			should_exit = mini_svm_intercept_vmmcall(state);
			break;
		default:
			printf("Unkown exit code\n");
			should_exit = 1;
			break;
	}
	return should_exit;
}

void mini_svm_setup_regs(struct mini_svm_vm_regs *regs) {
	printf("asd\n");
	regs->rip = 2 * 1024 * 1024;
	regs->rax = 0x0;
	regs->rbx = 0;
	regs->rcx = 0xdeadbeefUL;
	regs->rdx = 0x484848UL;
	regs->rdi = 0;
	regs->rsi = 0;
	regs->rbp = 0;
	regs->rsp = 0x8000;
	regs->r8 = 0;
	regs->r9 = 0;
	regs->r10 = 0;
	regs->r11 = 0;
	regs->r12 = 0;
	regs->r13 = 0;
	regs->r14 = 0;
	regs->r15 = 0;
}

int main() {
	int fd = open("/dev/mini_svm", O_RDWR);
	if (fd < 0) {
		printf("Failed to open mini-svm\n");
		return -1;
	}

	struct mini_svm_vmcb *vmcb = NULL;
	struct mini_svm_vm_state *state = NULL;

	void *pages = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MINI_SVM_MMAP_VM_VMCB);
	if (pages == MAP_FAILED) {
		printf("Failed to mmap vmcb\n");
		return -1;
	}
	vmcb = (struct mini_svm_vmcb *)pages;

	pages = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MINI_SVM_MMAP_VM_STATE);
	if (pages == MAP_FAILED) {
		printf("Failed to mmap vm state\n");
		return -1;
	}
	state = (struct mini_svm_vm_state *)pages;

	guest_memory = mmap(0, 32UL * 1024UL * 1024UL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MINI_SVM_MMAP_VM_PHYS_MEM);
	if (guest_memory == MAP_FAILED) {
		printf("Failed to retrieve guest memory\n");
		return -1;
	}

	if (!mini_svm_construct_1gb_gpt(guest_memory)) {
		printf("Failed to create GPT\n");
		return -1;
	}

	if (!load_vm_program("./vm-program", guest_memory)) {
		printf("Failed to load vm image\n");
		return -1;
	}

	setup_ctrl(&vmcb->control);
	setup_save(&vmcb->save);
	mini_svm_setup_regs(&state->regs);

	int r = ioctl(fd, MINI_SVM_IOCTL_START, 0);
	if (r < 0) {
		printf("Failed to ioctl mini-svm\n");
		return -1;
	}

	vmcb->control.vmcb_clean = -1;

	int should_exit;
	do {
		//dump_regs(vmcb, state);
		should_exit = mini_svm_handle_exit(vmcb, state);
		if (should_exit) {
			break;
		}
		vmcb->control.vmcb_clean = -1;
		int r = ioctl(fd, MINI_SVM_IOCTL_RESUME, 0);
		if (r < 0) {
			printf("Failed to ioctl mini-svm\n");
			return -1;
		}
	} while(1);

	return 0;
}