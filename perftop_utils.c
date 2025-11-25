#include "perftop.h"

/* Timestamping using rdtsc */
u64 perftop_rdtsc(void)
{
	unsigned int low, high;
	asm volatile("rdtsc" : "=a" (low), "=d" (high));
	return ((u64)high << 32) | low;
}

/* Hash function for stack traces */
u32 perftop_hash_stack(unsigned long *stack, unsigned int len)
{
	return jhash2((u32 *)stack, (len + 1) * 2, 0);
}

/* Kallsyms lookup helper */
void *perftop_kallsyms_lookup(const char *name)
{
	return (void *)kallsyms_lookup_name(name);
}

