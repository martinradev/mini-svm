#ifndef UTIL_H
#define UTIL_H

#include "hv-microbench-structures.h"

#define BAR_RDTSC(PRE, MID, POST) \
	asm volatile( \
		PRE \
		MID \
		POST \
		"shl $32, %%rdx\n\t" \
		"or %%rdx, %%rax\n\t" \
		"mov %%rax, %0\n\t" \
		: "=r"(tsc) \
		: \
		: "%rax", "%rdx" \
	)

static inline unsigned long rdtsc_and_bar(void) {
	unsigned long tsc;
	BAR_RDTSC("", "rdtsc\n\t", "mfence\n\t");
	return tsc;
}

static inline unsigned long bar_and_rdtsc(void) {
	unsigned long tsc;
	BAR_RDTSC("mfence\n\t", "rdtsc\n\t", "");
	return tsc;
}

static inline unsigned long rdtsc(void) {
	unsigned long tsc;
	BAR_RDTSC("", "rdtsc\n\t", "");
	return tsc;
}

static inline unsigned long rdtscp(void) {
	unsigned long tsc;
	BAR_RDTSC("", "rdtscp\n\t", "");
	return tsc;
}

#undef BAR_RDTSC

static inline unsigned long rd_aperf(void) {
	unsigned long clock;
	asm volatile(
		"mov $0xe8, %%rcx\n\t"
		"rdmsr\n\t"
		"shl $32, %%rdx\n\t"
		"or %%rdx, %%rax\n\t"
		"mov %%rax, %0\n\t"
		: "=r"(clock)
		:
		: "%rax", "%rdx", "%rcx"
	);
	return clock;
}

static inline void vmmcall(
		VmmCall cmd,
		unsigned long arg1,
		unsigned long arg2,
		unsigned long arg3) {
	asm volatile(
		"movq %0, %%rax\n\t"
		"movq %1, %%rdi\n\t"
		"movq %2, %%rsi\n\t"
		"movq %3, %%rdx\n\t"
		"vmmcall\n\t"
		:
		: "r"((unsigned long)cmd), "r"(arg1), "r"(arg2), "r"(arg3)
		: "%rax", "%rdi", "%rsi", "%rdx"
	);
}

static inline void vmmcall(VmmCall cmd, unsigned long arg1, unsigned long arg2) {
	vmmcall(cmd, arg1, arg2, 0);
}

static inline void vmmcall(VmmCall cmd, unsigned long arg1) {
	vmmcall(cmd, arg1, 0);
}

static inline void vmmcall(VmmCall cmd) {
	vmmcall(cmd, 0);
}

static inline void hlt() {
	asm volatile("hlt\n\t");
}

#define MEASURE(OPS) \
	{ \
		asm volatile( \
			"mov $0xe8, %%rcx\n\t" \
			"rdmsr\n\t" \
			/* "rdtsc\n\t" */ \
			"shl $32, %%rdx\n\t" \
			"or %%rdx, %%rax\n\t" \
			"mov %%rax, %%r8\n\t" \
			OPS \
			"mov $0xe8, %%rcx\n\t" \
			"rdmsr\n\t" \
			/* "rdtsc\n\t" */ \
			"shl $32, %%rdx\n\t" \
			"or %%rdx, %%rax\n\t" \
			"sub %%r8, %%rax\n\t" \
			"mov %%rax, %0\n\t" \
			: "=r"(delta_result) \
			: \
			: "%rax", "%rdx", "%rcx", "%r8", "%rbx" \
		); \
	}

#define MEASURE_AVERAGE(OUT, OPS, NUM) \
{ \
		unsigned long total = 0; \
		for (unsigned long i = 0; i < NUM; ++i) { \
				unsigned long delta_result = 0; \
				MEASURE( \
					OPS \
				); \
				total += delta_result; \
		} \
		OUT = total / (unsigned long)NUM; \
}

void measure_random_access_linked_list(const unsigned long *start, unsigned long *out);

#endif
