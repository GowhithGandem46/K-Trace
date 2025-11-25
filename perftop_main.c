#include "perftop.h"

/* Module parameters */
unsigned int perftop_sample_interval = PERFTOP_SAMPLE_INTERVAL_DEFAULT;
module_param(perftop_sample_interval, uint, 0644);
MODULE_PARM_DESC(perftop_sample_interval, "Sample interval for sampled mode");

unsigned int perftop_max_nodes = PERFTOP_MAX_NODES_DEFAULT;
module_param(perftop_max_nodes, uint, 0644);
MODULE_PARM_DESC(perftop_max_nodes, "Maximum number of RBTree nodes to retain");

bool perftop_realtime_mode = true;
module_param(perftop_realtime_mode, bool, 0644);
MODULE_PARM_DESC(perftop_realtime_mode, "Enable real-time mode (true) or sampled mode (false)");

/* Debugfs directory */
struct dentry *perftop_debugfs_dir;

/* Proc file operations */
static int perftop_proc_show(struct seq_file *sf, void *v)
{
	int cpu;
	unsigned long flags;

	seq_printf(sf, "=== CPU Profiler Summary ===\n\n");
	for_each_online_cpu(cpu) {
		struct perftop_cpu_data *cpu_data = perftop_get_cpu_data(cpu);
		spin_lock_irqsave(&cpu_data->lock, flags);
		seq_printf(sf, "CPU %d: Schedules=%lu, Total Runtime=%llu rdtsc ticks\n",
			   cpu, cpu_data->schedule_count, cpu_data->total_runtime);
		if (cpu_data->context_switch_count > 0) {
			seq_printf(sf, "  Avg Context Switch Latency=%llu rdtsc ticks\n",
				   cpu_data->context_switch_latency_sum /
				   cpu_data->context_switch_count);
		}
		spin_unlock_irqrestore(&cpu_data->lock, flags);
	}

	seq_printf(sf, "\n=== Top %d Tasks (by CPU time) ===\n\n", PROFILER_OUTPUT_NO);
	for_each_online_cpu(cpu) {
		seq_printf(sf, "--- CPU %d ---\n", cpu);
		perftop_print_rbtree_max_nodes(sf, cpu, PROFILER_OUTPUT_NO);
		seq_printf(sf, "\n");
	}
	return 0;
}

static int perftop_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, perftop_proc_show, NULL);
}

static const struct file_operations perftop_proc_fops = {
	.owner = THIS_MODULE,
	.open = perftop_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Debugfs: summary */
static int perftop_debugfs_summary_show(struct seq_file *sf, void *v)
{
	int cpu;
	unsigned long total_schedules = 0;
	u64 total_runtime = 0;
	u64 total_latency = 0;
	unsigned long total_switches = 0;

	seq_printf(sf, "=== System-Wide Summary ===\n\n");
	for_each_online_cpu(cpu) {
		struct perftop_cpu_data *cpu_data = perftop_get_cpu_data(cpu);
		unsigned long flags;

		spin_lock_irqsave(&cpu_data->lock, flags);
		total_schedules += cpu_data->schedule_count;
		total_runtime += cpu_data->total_runtime;
		total_latency += cpu_data->context_switch_latency_sum;
		total_switches += cpu_data->context_switch_count;
		spin_unlock_irqrestore(&cpu_data->lock, flags);
	}

	seq_printf(sf, "Total Schedules: %lu\n", total_schedules);
	seq_printf(sf, "Total Runtime: %llu rdtsc ticks\n", total_runtime);
	if (total_switches > 0) {
		seq_printf(sf, "Average Context Switch Latency: %llu rdtsc ticks\n",
			   total_latency / total_switches);
	}
	seq_printf(sf, "\n=== Top 10 Tasks System-Wide ===\n\n");

	/* Aggregate top tasks across all CPUs */
	{
		struct schedule_rbtree_entry *entries[10] = {NULL};
		int cpu, i, j;
		struct rb_node *node;
		struct schedule_rbtree_entry *entry;
		unsigned long flags;

		for_each_online_cpu(cpu) {
			struct perftop_cpu_data *cpu_data = perftop_get_cpu_data(cpu);
			spin_lock_irqsave(&cpu_data->lock, flags);
			for (node = rb_last(&cpu_data->scheduleTreeRoot); node;
			     node = rb_prev(node)) {
				entry = rb_entry(node, struct schedule_rbtree_entry, rbentry);
				/* Insert into top 10 */
				for (i = 0; i < 10; i++) {
					if (entries[i] == NULL ||
					    entries[i]->cputime < entry->cputime) {
						for (j = 9; j > i; j--)
							entries[j] = entries[j - 1];
						entries[i] = entry;
						break;
					}
				}
			}
			spin_unlock_irqrestore(&cpu_data->lock, flags);
		}

		for (i = 0; i < 10 && entries[i]; i++) {
			seq_printf(sf, "%d. PID: %d, CPU Time: %llu rdtsc ticks, "
				   "Schedules: %d\n",
				   i + 1, entries[i]->pid, entries[i]->cputime,
				   entries[i]->numberOfSchedules);
		}
	}

	return 0;
}

static int perftop_debugfs_summary_open(struct inode *inode, struct file *file)
{
	return single_open(file, perftop_debugfs_summary_show, NULL);
}

static const struct file_operations perftop_debugfs_summary_fops = {
	.owner = THIS_MODULE,
	.open = perftop_debugfs_summary_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Debugfs: per-CPU stats */
static int perftop_debugfs_cpu_show(struct seq_file *sf, void *v)
{
	int cpu = (long)sf->private;
	struct perftop_cpu_data *cpu_data = perftop_get_cpu_data(cpu);
	unsigned long flags;

	spin_lock_irqsave(&cpu_data->lock, flags);
	seq_printf(sf, "=== CPU %d Statistics ===\n\n", cpu);
	seq_printf(sf, "Total Schedules: %lu\n", cpu_data->schedule_count);
	seq_printf(sf, "Total Runtime: %llu rdtsc ticks\n", cpu_data->total_runtime);
	if (cpu_data->schedule_count > 0) {
		seq_printf(sf, "Average Runtime per Schedule: %llu rdtsc ticks\n",
			   cpu_data->total_runtime / cpu_data->schedule_count);
	}
	if (cpu_data->context_switch_count > 0) {
		seq_printf(sf, "Context Switches: %lu\n", cpu_data->context_switch_count);
		seq_printf(sf, "Average Context Switch Latency: %llu rdtsc ticks\n",
			   cpu_data->context_switch_latency_sum /
			   cpu_data->context_switch_count);
	}
	seq_printf(sf, "\n=== Top 10 Tasks on CPU %d ===\n\n", cpu);
	spin_unlock_irqrestore(&cpu_data->lock, flags);

	perftop_print_rbtree_max_nodes(sf, cpu, 10);
	return 0;
}

static int perftop_debugfs_cpu_open(struct inode *inode, struct file *file)
{
	return single_open(file, perftop_debugfs_cpu_show, inode->i_private);
}

static const struct file_operations perftop_debugfs_cpu_fops = {
	.owner = THIS_MODULE,
	.open = perftop_debugfs_cpu_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* Debugfs: config read */
static ssize_t perftop_debugfs_config_read(struct file *file, char __user *user_buf,
					    size_t count, loff_t *ppos)
{
	char buf[256];
	int len;
	unsigned long flags;

	spin_lock_irqsave(&perftop_cfg.config_lock, flags);
	len = snprintf(buf, sizeof(buf),
		       "mode=%s\ninterval=%u\nmax_nodes=%u\n",
		       perftop_cfg.mode == PERFTOP_MODE_REALTIME ? "realtime" : "sampled",
		       perftop_cfg.sample_interval,
		       perftop_cfg.max_nodes);
	spin_unlock_irqsave(&perftop_cfg.config_lock, flags);

	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

/* Debugfs: config write */
static ssize_t perftop_debugfs_config_write(struct file *file, const char __user *user_buf,
					     size_t count, loff_t *ppos)
{
	char buf[256];
	char *token, *value;
	unsigned long flags;
	unsigned int new_interval, new_max_nodes;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	buf[count] = '\0';

	spin_lock_irqsave(&perftop_cfg.config_lock, flags);

	token = buf;
	while ((token = strsep(&token, " \n")) != NULL) {
		char *key, *val;
		if (!token || !*token)
			continue;

		val = strchr(token, '=');
		if (!val)
			continue;
		*val++ = '\0';
		key = token;

		if (strcmp(key, "mode") == 0) {
			if (strcmp(val, "realtime") == 0)
				perftop_cfg.mode = PERFTOP_MODE_REALTIME;
			else if (strcmp(val, "sampled") == 0)
				perftop_cfg.mode = PERFTOP_MODE_SAMPLED;
		} else if (strcmp(key, "interval") == 0) {
			if (kstrtouint(val, 10, &new_interval) == 0)
				perftop_cfg.sample_interval = new_interval;
		} else if (strcmp(key, "max_nodes") == 0) {
			if (kstrtouint(val, 10, &new_max_nodes) == 0)
				perftop_cfg.max_nodes = new_max_nodes;
		} else if (strcmp(key, "reset") == 0) {
			if (strcmp(val, "1") == 0 || strcmp(val, "yes") == 0) {
				spin_unlock_irqrestore(&perftop_cfg.config_lock, flags);
				perftop_reset_all_stats();
				spin_lock_irqsave(&perftop_cfg.config_lock, flags);
			}
		}
	}

	spin_unlock_irqrestore(&perftop_cfg.config_lock, flags);

	return count;
}

static const struct file_operations perftop_debugfs_config_fops = {
	.owner = THIS_MODULE,
	.read = perftop_debugfs_config_read,
	.write = perftop_debugfs_config_write,
	.llseek = default_llseek,
};

/* Initialize main module */
int perftop_main_init(void)
{
	int cpu;
	char name[32];
	struct dentry *cpu_dentry;

	/* Initialize configuration from module parameters */
	spin_lock(&perftop_cfg.config_lock);
	perftop_cfg.mode = perftop_realtime_mode ? PERFTOP_MODE_REALTIME :
						   PERFTOP_MODE_SAMPLED;
	perftop_cfg.sample_interval = perftop_sample_interval;
	perftop_cfg.max_nodes = perftop_max_nodes;
	spin_unlock(&perftop_cfg.config_lock);

	/* Create proc entry */
	proc_create("perftop", 0, NULL, &perftop_proc_fops);

	/* Create debugfs directory */
	perftop_debugfs_dir = debugfs_create_dir("perftop", NULL);
	if (!perftop_debugfs_dir) {
		pr_warn("perftop: Failed to create debugfs directory\n");
	} else {
		/* Create debugfs files */
		debugfs_create_file("summary", 0444, perftop_debugfs_dir, NULL,
				    &perftop_debugfs_summary_fops);
		debugfs_create_file("config", 0644, perftop_debugfs_dir, NULL,
				    &perftop_debugfs_config_fops);

		/* Create per-CPU files */
		for_each_possible_cpu(cpu) {
			snprintf(name, sizeof(name), "cpu_%d", cpu);
			cpu_dentry = debugfs_create_file(name, 0444, perftop_debugfs_dir,
							  (void *)(long)cpu,
							  &perftop_debugfs_cpu_fops);
			if (!cpu_dentry)
				pr_warn("perftop: Failed to create debugfs file for CPU %d\n",
					cpu);
		}
	}

	pr_info("perftop: Module inserted successfully\n");
	return 0;
}

/* Cleanup main module */
void perftop_main_cleanup(void)
{
	remove_proc_entry("perftop", NULL);
	if (perftop_debugfs_dir)
		debugfs_remove_recursive(perftop_debugfs_dir);
	pr_info("perftop: Module removed\n");
}

/* Module init */
static int __init perftop_init(void)
{
	int ret;

	ret = perftop_data_init();
	if (ret)
		return ret;

	ret = perftop_probe_init();
	if (ret) {
		perftop_data_cleanup();
		return ret;
	}

	ret = perftop_main_init();
	if (ret) {
		perftop_probe_cleanup();
		perftop_data_cleanup();
		return ret;
	}

	return 0;
}

/* Module exit */
static void __exit perftop_exit(void)
{
	perftop_probe_cleanup();
	perftop_main_cleanup();
	perftop_data_cleanup();
}

module_init(perftop_init);
module_exit(perftop_exit);

