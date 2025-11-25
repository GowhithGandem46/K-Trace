#include "perftop.h"

static char probedFunctionName[FUNCTION_NAME_LENGTH] = "pick_next_task_fair";

/* Kallsyms lookup for stack_trace_save_user */
void *kallsyms_stack_trace_usr_save = NULL;

/* Per-CPU stack trace log */
static DEFINE_PER_CPU(unsigned long[MAX_TRACE_SIZE], stackTraceLog);

/* Entry handler - called when function returns */
static int perftop_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	unsigned int task_pid, st_length;
	unsigned int cpu = raw_smp_processor_id();
	u32 stackTraceHashKey;
	u64 task_timestamp;
	u64 *entry_data = (u64 *)ri->data;
	unsigned long *local_stack_log = this_cpu_ptr(stackTraceLog);
	unsigned long current_task_struct_pointer = regs->si;
	struct task_struct *curr_task = (struct task_struct *)current_task_struct_pointer;
	unsigned int sample_interval;
	static DEFINE_PER_CPU(unsigned int, event_counter);

	if (!curr_task)
		return 0;

	task_pid = (unsigned int)curr_task->pid;
	task_timestamp = perftop_rdtsc() - *entry_data;

	/* Sampling mode check */
	if (perftop_cfg.mode == PERFTOP_MODE_SAMPLED) {
		sample_interval = perftop_cfg.sample_interval;
		if (sample_interval > 1) {
			unsigned int *counter = this_cpu_ptr(&event_counter);
			(*counter)++;
			if ((*counter) % sample_interval != 0) {
				return 0;
			}
		}
	}

	local_stack_log[0] = task_pid;
	if (curr_task->mm == NULL) {
		st_length = stack_trace_save(&local_stack_log[1], MAX_TRACE_SIZE - 1, 6);
	} else {
		if (kallsyms_stack_trace_usr_save) {
			typedef unsigned int (*stack_trace_save_user_fn)(unsigned long *store,
									 unsigned int size);
			stack_trace_save_user_fn fn = (stack_trace_save_user_fn)
				kallsyms_stack_trace_usr_save;
			st_length = fn(&local_stack_log[1], MAX_TRACE_SIZE - 1);
		} else {
			st_length = stack_trace_save(&local_stack_log[1], MAX_TRACE_SIZE - 1, 6);
		}
	}

	stackTraceHashKey = perftop_hash_stack(local_stack_log, st_length + 1);

	perftop_store_hash_entry(cpu, stackTraceHashKey, local_stack_log,
				 st_length + 1, task_timestamp, task_pid);
	perftop_store_rbtree_entry(cpu, stackTraceHashKey, local_stack_log,
				    st_length + 1, task_timestamp, task_pid);

	return 0;
}

/* Return handler - called when function is entered */
static int perftop_return_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	u64 *ret_data = (u64 *)ri->data;
	*ret_data = perftop_rdtsc();
	return 0;
}

static struct kretprobe perftopKernelReturnProbe = {
	.handler = perftop_return_handler,
	.entry_handler = perftop_entry_handler,
	.maxactive = NR_CPUS * 2,
	.data_size = sizeof(u64),
};

/* Initialize probe */
int perftop_probe_init(void)
{
	int kprobeRegistrationStatus;

	kallsyms_stack_trace_usr_save = perftop_kallsyms_lookup("stack_trace_save_user");
	if (!kallsyms_stack_trace_usr_save) {
		perftop_debug("perftop: stack_trace_save_user not found, using kernel trace\n");
	}

	perftopKernelReturnProbe.kp.symbol_name = probedFunctionName;
	kprobeRegistrationStatus = register_kretprobe(&perftopKernelReturnProbe);
	if (kprobeRegistrationStatus < 0) {
		pr_err("perftop: Module insertion failed. Cannot register a kretprobe for function %s!\n",
		       probedFunctionName);
		return -1;
	}

	perftop_debug("perftop: Kretprobe registered for %s\n", probedFunctionName);
	return 0;
}

/* Cleanup probe */
void perftop_probe_cleanup(void)
{
	unregister_kretprobe(&perftopKernelReturnProbe);
	perftop_debug("perftop: Kretprobe unregistered\n");
}

