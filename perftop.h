#ifndef PERFTOP_H
#define PERFTOP_H

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kprobes.h>
#include <linux/hashtable.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/stacktrace.h>
#include <linux/slab.h>
#include <linux/jhash.h>
#include <linux/kallsyms.h>
#include <linux/rbtree.h>
#include <linux/debugfs.h>
#include <linux/rcupdate.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/smp.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gowhith Gandem");
MODULE_DESCRIPTION("Research-Grade Multi-Core CPU Profiler");

/* Configuration */
#define FUNCTION_NAME_LENGTH 20
#define MAX_TRACE_SIZE 51
#define PROFILER_OUTPUT_NO 20
#define PERFTOP_HASH_BITS 10
#define PERFTOP_MAX_NODES_DEFAULT 1000
#define PERFTOP_SAMPLE_INTERVAL_DEFAULT 1

/* Debug flag */
#ifdef CONFIG_PERFTOP_DEBUG
#define perftop_debug(fmt, ...) pr_info("perftop: " fmt, ##__VA_ARGS__)
#else
#define perftop_debug(fmt, ...) do {} while (0)
#endif

/* Module parameters */
extern unsigned int perftop_sample_interval;
extern unsigned int perftop_max_nodes;
extern bool perftop_realtime_mode;

/* Runtime configuration */
enum perftop_mode {
	PERFTOP_MODE_REALTIME = 0,
	PERFTOP_MODE_SAMPLED = 1
};

struct perftop_config {
	enum perftop_mode mode;
	unsigned int sample_interval;
	unsigned int max_nodes;
	spinlock_t config_lock;
};

/* Data structures */
struct schedule_rbtree_entry {
	unsigned int key;
	unsigned int numberOfSchedules;
	unsigned long stackLog[MAX_TRACE_SIZE];
	unsigned int stackLogLength;
	u64 cputime;
	pid_t pid;
	struct rb_node rbentry;
};

struct schedule_hash_entry {
	unsigned int numberOfSchedules;
	unsigned long stackLog[MAX_TRACE_SIZE];
	unsigned int stackLogLength;
	u64 scheduleTime;
	pid_t pid;
	u64 last_schedule_time;
	u64 context_switch_latency_sum;
	unsigned int context_switch_count;
	struct hlist_node hashTableNode;
	struct rcu_head rcu;
};

struct perftop_cpu_data {
	DECLARE_HASHTABLE(scheduleHashTable, PERFTOP_HASH_BITS);
	struct rb_root scheduleTreeRoot;
	spinlock_t lock;
	u64 total_runtime;
	unsigned long schedule_count;
	u64 context_switch_latency_sum;
	unsigned long context_switch_count;
};

/* Global state */
extern struct perftop_config perftop_cfg;
extern struct kmem_cache *perftop_hash_cache;
extern struct kmem_cache *perftop_rbtree_cache;
extern struct dentry *perftop_debugfs_dir;

/* Function declarations */
/* perftop_utils.c */
u64 perftop_rdtsc(void);
u32 perftop_hash_stack(unsigned long *stack, unsigned int len);
void *perftop_kallsyms_lookup(const char *name);

/* perftop_data.c */
int perftop_data_init(void);
void perftop_data_cleanup(void);
int perftop_store_hash_entry(int cpu, unsigned int key, unsigned long *st_log,
			      unsigned int st_len, u64 time, pid_t pid);
int perftop_store_rbtree_entry(int cpu, unsigned int key, unsigned long *st_log,
				 unsigned int st_len, u64 time, pid_t pid);
void perftop_print_rbtree_max_nodes(struct seq_file *sf, int cpu, int nodeCount);
void perftop_print_hash_table(struct seq_file *sf, int cpu);
void perftop_reset_stats(int cpu);
void perftop_reset_all_stats(void);
struct perftop_cpu_data *perftop_get_cpu_data(int cpu);

/* perftop_probe.c */
int perftop_probe_init(void);
void perftop_probe_cleanup(void);

/* perftop_main.c */
int perftop_main_init(void);
void perftop_main_cleanup(void);

#endif /* PERFTOP_H */

