#include "perftop.h"

/* Per-CPU data structures */
static DEFINE_PER_CPU(struct perftop_cpu_data, perftop_cpu_data);

/* Global configuration */
struct perftop_config perftop_cfg = {
	.mode = PERFTOP_MODE_REALTIME,
	.sample_interval = PERFTOP_SAMPLE_INTERVAL_DEFAULT,
	.max_nodes = PERFTOP_MAX_NODES_DEFAULT,
};

/* Slab caches */
struct kmem_cache *perftop_hash_cache;
struct kmem_cache *perftop_rbtree_cache;

/* Get per-CPU data */
struct perftop_cpu_data *perftop_get_cpu_data(int cpu)
{
	return &per_cpu(perftop_cpu_data, cpu);
}

/* Initialize data structures */
int perftop_data_init(void)
{
	int cpu;
	struct perftop_cpu_data *cpu_data;

	/* Create slab caches */
	perftop_hash_cache = kmem_cache_create("perftop_hash",
						sizeof(struct schedule_hash_entry),
						0, 0, NULL);
	if (!perftop_hash_cache) {
		pr_err("perftop: Failed to create hash cache\n");
		return -ENOMEM;
	}

	perftop_rbtree_cache = kmem_cache_create("perftop_rbtree",
						  sizeof(struct schedule_rbtree_entry),
						  0, 0, NULL);
	if (!perftop_rbtree_cache) {
		pr_err("perftop: Failed to create rbtree cache\n");
		kmem_cache_destroy(perftop_hash_cache);
		return -ENOMEM;
	}

	/* Initialize per-CPU data */
	for_each_possible_cpu(cpu) {
		cpu_data = perftop_get_cpu_data(cpu);
		hash_init(cpu_data->scheduleHashTable);
		cpu_data->scheduleTreeRoot = RB_ROOT;
		spin_lock_init(&cpu_data->lock);
		cpu_data->total_runtime = 0;
		cpu_data->schedule_count = 0;
		cpu_data->context_switch_latency_sum = 0;
		cpu_data->context_switch_count = 0;
	}

	spin_lock_init(&perftop_cfg.config_lock);

	perftop_debug("perftop: Data structures initialized\n");
	return 0;
}

/* Cleanup data structures */
void perftop_data_cleanup(void)
{
	int cpu;
	struct perftop_cpu_data *cpu_data;
	struct schedule_hash_entry *hash_entry;
	struct hlist_node *tmp;
	struct rb_node *rb_node;
	struct schedule_rbtree_entry *rbtree_entry;
	int bkt;

	/* Cleanup per-CPU data */
	for_each_possible_cpu(cpu) {
		cpu_data = perftop_get_cpu_data(cpu);

		/* Cleanup hashtable */
		hash_for_each_safe(cpu_data->scheduleHashTable, bkt, tmp,
				   hash_entry, hashTableNode) {
			hash_del(&hash_entry->hashTableNode);
			kmem_cache_free(perftop_hash_cache, hash_entry);
		}

		/* Cleanup rbtree */
		while ((rb_node = rb_first(&cpu_data->scheduleTreeRoot))) {
			rbtree_entry = rb_entry(rb_node, struct schedule_rbtree_entry,
						rbentry);
			rb_erase(rb_node, &cpu_data->scheduleTreeRoot);
			kmem_cache_free(perftop_rbtree_cache, rbtree_entry);
		}
	}

	/* Destroy slab caches */
	if (perftop_hash_cache)
		kmem_cache_destroy(perftop_hash_cache);
	if (perftop_rbtree_cache)
		kmem_cache_destroy(perftop_rbtree_cache);

	perftop_debug("perftop: Data structures cleaned up\n");
}

/* Delete previous RBTree entry */
static bool delete_previous_rbtree_entry(int cpu, unsigned int key,
					 u64 *time_ret, unsigned int *sc_count)
{
	struct perftop_cpu_data *cpu_data = perftop_get_cpu_data(cpu);
	struct rb_node *temp_node;
	struct schedule_rbtree_entry *del_rbtree_node;

	temp_node = rb_first(&cpu_data->scheduleTreeRoot);
	while (temp_node) {
		del_rbtree_node = rb_entry(temp_node, struct schedule_rbtree_entry,
					   rbentry);
		if (del_rbtree_node->key == key) {
			*sc_count = del_rbtree_node->numberOfSchedules;
			*time_ret = del_rbtree_node->cputime;
			rb_erase(&del_rbtree_node->rbentry,
				 &cpu_data->scheduleTreeRoot);
			kmem_cache_free(perftop_rbtree_cache, del_rbtree_node);
			return true;
		}
		temp_node = rb_next(temp_node);
	}
	*time_ret = 0;
	*sc_count = 0;
	return false;
}

/* Store to schedule RBTree */
int perftop_store_rbtree_entry(int cpu, unsigned int key, unsigned long *st_log,
				 unsigned int st_len, u64 time, pid_t pid)
{
	int i = 0;
	u64 prev_time = 0;
	unsigned int sched_count = 0;
	struct schedule_rbtree_entry *new_node, *attached_node;
	struct rb_node **current_rb_node, *rbparent_node = NULL;
	struct perftop_cpu_data *cpu_data = perftop_get_cpu_data(cpu);
	unsigned long flags;
	unsigned int max_nodes;

	spin_lock_irqsave(&cpu_data->lock, flags);

	/* Check max_nodes limit */
	max_nodes = perftop_cfg.max_nodes;
	if (rb_first(&cpu_data->scheduleTreeRoot)) {
		struct rb_node *first = rb_first(&cpu_data->scheduleTreeRoot);
		int node_count = 0;
		while (first) {
			node_count++;
			first = rb_next(first);
		}
		if (node_count >= max_nodes) {
			/* Remove smallest entry */
			struct rb_node *smallest = rb_first(&cpu_data->scheduleTreeRoot);
			if (smallest) {
				struct schedule_rbtree_entry *smallest_entry =
					rb_entry(smallest, struct schedule_rbtree_entry,
						 rbentry);
				rb_erase(smallest, &cpu_data->scheduleTreeRoot);
				kmem_cache_free(perftop_rbtree_cache, smallest_entry);
			}
		}
	}

	delete_previous_rbtree_entry(cpu, key, &prev_time, &sched_count);

	new_node = kmem_cache_alloc(perftop_rbtree_cache, GFP_ATOMIC);
	if (new_node != NULL) {
		new_node->key = key;
		new_node->numberOfSchedules = sched_count + 1;
		new_node->stackLogLength = st_len;
		new_node->cputime = time + prev_time;
		new_node->pid = pid;
		while ((i < st_len) && (st_len <= MAX_TRACE_SIZE)) {
			new_node->stackLog[i] = *st_log;
			st_log++;
			i++;
		}
		current_rb_node = &cpu_data->scheduleTreeRoot.rb_node;
		while (*current_rb_node != NULL) {
			rbparent_node = *current_rb_node;
			attached_node = rb_entry(rbparent_node,
						 struct schedule_rbtree_entry, rbentry);
			if (attached_node->cputime < new_node->cputime)
				current_rb_node = &((*current_rb_node)->rb_right);
			else
				current_rb_node = &((*current_rb_node)->rb_left);
		}
		rb_link_node(&new_node->rbentry, rbparent_node, current_rb_node);
		rb_insert_color(&(new_node->rbentry), &cpu_data->scheduleTreeRoot);
		spin_unlock_irqrestore(&cpu_data->lock, flags);
		return 0;
	} else {
		spin_unlock_irqrestore(&cpu_data->lock, flags);
		pr_err("perftop: No memory to add rbtree node\n");
		return -ENOMEM;
	}
}

/* Store to schedule HashTable */
int perftop_store_hash_entry(int cpu, unsigned int key, unsigned long *st_log,
			      unsigned int st_len, u64 time, pid_t pid)
{
	struct schedule_hash_entry *node;
	int i = 0;
	struct perftop_cpu_data *cpu_data = perftop_get_cpu_data(cpu);
	unsigned long flags;
	u64 current_time = perftop_rdtsc();

	spin_lock_irqsave(&cpu_data->lock, flags);

	hash_for_each_possible(cpu_data->scheduleHashTable, node, hashTableNode, key) {
		if (node != NULL && node->pid == pid) {
			u64 latency = 0;
			node->numberOfSchedules += 1;
			node->scheduleTime += time;
			if (node->last_schedule_time > 0) {
				latency = current_time - node->last_schedule_time;
				node->context_switch_latency_sum += latency;
				node->context_switch_count++;
				cpu_data->context_switch_latency_sum += latency;
				cpu_data->context_switch_count++;
			}
			node->last_schedule_time = current_time;
			cpu_data->total_runtime += time;
			cpu_data->schedule_count++;
			spin_unlock_irqrestore(&cpu_data->lock, flags);
			return 0;
		}
	}

	node = kmem_cache_alloc(perftop_hash_cache, GFP_ATOMIC);
	if (node != NULL) {
		node->numberOfSchedules = 1;
		while ((i < st_len) && (st_len <= MAX_TRACE_SIZE)) {
			node->stackLog[i] = st_log[i];
			i++;
		}
		node->stackLogLength = st_len;
		node->scheduleTime = time;
		node->pid = pid;
		node->last_schedule_time = current_time;
		node->context_switch_latency_sum = 0;
		node->context_switch_count = 0;
		hash_add(cpu_data->scheduleHashTable, &node->hashTableNode, key);
		cpu_data->total_runtime += time;
		cpu_data->schedule_count++;
		spin_unlock_irqrestore(&cpu_data->lock, flags);
		return 0;
	} else {
		spin_unlock_irqrestore(&cpu_data->lock, flags);
		pr_err("perftop: No memory to add hash map node\n");
		return -ENOMEM;
	}
}

/* Print schedule RBTree max nodes */
void perftop_print_rbtree_max_nodes(struct seq_file *sf, int cpu, int nodeCount)
{
	int i = 0, j;
	struct rb_node *curr_node;
	struct schedule_rbtree_entry *read_node;
	struct perftop_cpu_data *cpu_data = perftop_get_cpu_data(cpu);
	unsigned long flags;

	spin_lock_irqsave(&cpu_data->lock, flags);
	curr_node = rb_last(&cpu_data->scheduleTreeRoot);
	for (i = 0; i < nodeCount; i++) {
		if (curr_node != NULL) {
			read_node = rb_entry(curr_node, struct schedule_rbtree_entry,
					     rbentry);
			seq_printf(sf, "PID: %d\t", read_node->pid);
			seq_printf(sf, "CPU: %d\t", cpu);
			seq_printf(sf, "Schedules: %d\t", read_node->numberOfSchedules);
			seq_printf(sf, "CPU Time: %llu rdtsc ticks\n", read_node->cputime);
			if (read_node->stackLogLength == 1) {
				seq_printf(sf, "  No Stack Trace available for this task.\n");
			} else {
				seq_printf(sf, "  Stack Trace:\n");
				for (j = 1; j < read_node->stackLogLength; j++) {
					seq_printf(sf, "    %pS\n", (void *)read_node->stackLog[j]);
				}
			}
		}
		curr_node = rb_prev(curr_node);
	}
	spin_unlock_irqrestore(&cpu_data->lock, flags);
}

/* Print schedule HashTable */
void perftop_print_hash_table(struct seq_file *sf, int cpu)
{
	int hashBucket, i;
	struct schedule_hash_entry *read_node;
	struct perftop_cpu_data *cpu_data = perftop_get_cpu_data(cpu);
	unsigned long flags;

	spin_lock_irqsave(&cpu_data->lock, flags);
	if (!(hash_empty(cpu_data->scheduleHashTable))) {
		hash_for_each(cpu_data->scheduleHashTable, hashBucket, read_node,
			      hashTableNode) {
			if (read_node != NULL) {
				seq_printf(sf, "PID: %d\t", read_node->pid);
				seq_printf(sf, "CPU: %d\t", cpu);
				seq_printf(sf, "Schedules: %d\t",
					   read_node->numberOfSchedules);
				seq_printf(sf, "CPU Time: %llu rdtsc ticks\n",
					   read_node->scheduleTime);
				if (read_node->stackLogLength == 1) {
					seq_printf(sf, "  No Stack Trace available.\n");
				} else {
					seq_printf(sf, "  Stack Trace:\n");
					for (i = 1; i < read_node->stackLogLength; i++) {
						seq_printf(sf, "    %pS\n",
							   (void *)read_node->stackLog[i]);
					}
				}
			}
		}
	}
	spin_unlock_irqrestore(&cpu_data->lock, flags);
}

/* Reset stats for a specific CPU */
void perftop_reset_stats(int cpu)
{
	struct perftop_cpu_data *cpu_data = perftop_get_cpu_data(cpu);
	struct schedule_hash_entry *hash_entry;
	struct hlist_node *tmp;
	struct rb_node *rb_node;
	struct schedule_rbtree_entry *rbtree_entry;
	int bkt;
	unsigned long flags;

	spin_lock_irqsave(&cpu_data->lock, flags);

	/* Cleanup hashtable */
	hash_for_each_safe(cpu_data->scheduleHashTable, bkt, tmp,
			   hash_entry, hashTableNode) {
		hash_del(&hash_entry->hashTableNode);
		kmem_cache_free(perftop_hash_cache, hash_entry);
	}

	/* Cleanup rbtree */
	while ((rb_node = rb_first(&cpu_data->scheduleTreeRoot))) {
		rbtree_entry = rb_entry(rb_node, struct schedule_rbtree_entry, rbentry);
		rb_erase(rb_node, &cpu_data->scheduleTreeRoot);
		kmem_cache_free(perftop_rbtree_cache, rbtree_entry);
	}

	cpu_data->total_runtime = 0;
	cpu_data->schedule_count = 0;
	cpu_data->context_switch_latency_sum = 0;
	cpu_data->context_switch_count = 0;

	spin_unlock_irqrestore(&cpu_data->lock, flags);
}

/* Reset all stats */
void perftop_reset_all_stats(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		perftop_reset_stats(cpu);
	}
}

