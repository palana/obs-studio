#include "profiler.h"

#include "darray.h"
#include "dstr.h"
#include "platform.h"
#include "threading.h"

#include <math.h>

typedef struct profiler_time_entry profiler_time_entry;

typedef struct profile_call profile_call;
struct profile_call {
	const char *name;
	uint64_t overhead_start;
	uint64_t start_time;
	uint64_t end_time;
	uint64_t overhead_end;
	uint64_t expected_time_between_calls;
	DARRAY(profile_call) children;
	profile_call *parent;
};

typedef struct profile_times_table_entry profile_times_table_entry;
struct profile_times_table_entry {
	size_t probes;
	profiler_time_entry entry;
};

typedef struct profile_times_table profile_times_table;
struct profile_times_table {
	size_t size;
	size_t occupied;
	size_t max_probe_count;
	profile_times_table_entry *entries;

	size_t old_start_index;
	size_t old_occupied;
	profile_times_table_entry *old_entries;
};

typedef struct profile_entry profile_entry;
struct profile_entry {
	const char *name;
	profile_times_table times;
	profile_times_table overhead;
	uint64_t expected_time_between_calls;
	profile_times_table times_between_calls;
	DARRAY(profile_entry) children;
};

typedef struct profile_root_entry profile_root_entry;
struct profile_root_entry {
	pthread_mutex_t *mutex;
	const char *name;
	profile_entry *entry;
	profile_call *prev_call;
};

static inline uint64_t diff_ns_to_usec(uint64_t prev, uint64_t next)
{
	return (next - prev + 500) / 1000;
}

static inline void update_max_probes(profile_times_table *map, size_t val)
{
	map->max_probe_count = map->max_probe_count < val ?
		val : map->max_probe_count;
}

static void migrate_old_entries(profile_times_table *map, bool limit_items);
static void grow_hashmap(profile_times_table *map,
		uint64_t usec, uint64_t count);

static void add_hashmap_entry(profile_times_table *map, uint64_t usec,
		size_t count)
{
	size_t probes = 1;

	size_t start = usec % map->size;
	for (;; probes += 1) {
		size_t idx = (start + probes) % map->size;
		profile_times_table_entry *entry = &map->entries[idx];
		if (!entry->probes) {
			entry->probes = probes;
			entry->entry.time_delta = usec;
			entry->entry.count = count;
			map->occupied += 1;
			update_max_probes(map, probes);
			return;
		}

		if (entry->entry.time_delta == usec) {
			entry->entry.count += count;
			return;
		}

		if (entry->probes >= probes)
			continue;

		if (map->occupied/(double)map->size > 0.7) {
			grow_hashmap(map, usec, count);
			return;
		}

		size_t old_probes = entry->probes;
		uint64_t old_count  = entry->entry.count;
		uint64_t old_usec   = entry->entry.time_delta;

		entry->probes = probes;
		entry->entry.count = count;
		entry->entry.time_delta = usec;

		update_max_probes(map, probes);

		probes = old_probes;
		count  = old_count;
		usec   = old_usec;

		start = usec % map->size;
	}
}

static void init_hashmap(profile_times_table *map, size_t size)
{
	map->size = size;
	map->occupied = 0;
	map->max_probe_count = 0;
	map->entries = bzalloc(sizeof(profile_times_table_entry) * size);
	map->old_start_index = 0;
	map->old_occupied = 0;
	map->old_entries = NULL;
}

static void migrate_old_entries(profile_times_table *map, bool limit_items)
{
	if (!map->old_entries)
		return;

	if (!map->old_occupied) {
		bfree(map->old_entries);
		map->old_entries = NULL;
		return;
	}

	for (size_t i = 0; !limit_items || i < 8; i++, map->old_start_index++) {
		if (!map->old_occupied)
			return;

		profile_times_table_entry *entry =
			&map->old_entries[map->old_start_index];
		if (!entry->probes)
			continue;

		add_hashmap_entry(map, entry->entry.time_delta,
				entry->entry.count);
		map->old_occupied -= 1;
	}
}

static void grow_hashmap(profile_times_table *map,
		uint64_t usec, uint64_t count)
{
	migrate_old_entries(map, false);

	size_t old_size     = map->size;
	size_t old_occupied = map->occupied;
	profile_times_table_entry *entries = map->entries;

	init_hashmap(map, (old_size * 2 < 16) ? 16 : (old_size * 2));
	//blog(LOG_INFO, "map %p grew from %zu to %zu", map, old_size, map->size);

	map->old_occupied = old_occupied;
	map->old_entries  = entries;

	add_hashmap_entry(map, usec, count);
}

static profile_entry *init_entry(profile_entry *entry, const char *name)
{
	entry->name = name;
	init_hashmap(&entry->times, 1);
	init_hashmap(&entry->overhead, 1);
	entry->expected_time_between_calls = 0;
	init_hashmap(&entry->times_between_calls, 1);
	return entry;
}

static profile_entry *get_child(profile_entry *parent, const char *name)
{
	const size_t num = parent->children.num;
	for (size_t i = 0; i < num; i++) {
		profile_entry *child = &parent->children.array[i];
		if (child->name == name)
			return child;
	}

	return init_entry(da_push_back_new(parent->children), name);
}

static void merge_call(profile_entry *entry, profile_call *call,
		profile_call *prev_call)
{
	const size_t num = call->children.num;
	for (size_t i = 0; i < num; i++) {
		profile_call *child = &call->children.array[i];
		merge_call(get_child(entry, child->name), child, NULL);
	}

	if (entry->expected_time_between_calls != 0 && prev_call) {
		migrate_old_entries(&entry->times_between_calls, true);
		uint64_t usec = diff_ns_to_usec(prev_call->start_time,
				call->start_time);
		add_hashmap_entry(&entry->times_between_calls, usec, 1);
	}

	migrate_old_entries(&entry->times, true);
	uint64_t usec = diff_ns_to_usec(call->start_time, call->end_time);
	add_hashmap_entry(&entry->times, usec, 1);

	migrate_old_entries(&entry->overhead, true);
	usec  = diff_ns_to_usec(call->overhead_start, call->start_time);
	usec += diff_ns_to_usec(call->end_time, call->overhead_end);
	add_hashmap_entry(&entry->overhead, usec, 1);
}

static bool enabled = true;
static pthread_mutex_t root_mutex = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(profile_root_entry) root_entries;

#ifdef _MSC_VER
static __declspec(thread) profile_call *thread_context = NULL;
static __declspec(thread) bool thread_enabled = true;
#else
static __thread profile_call *thread_context = NULL;
static __thread bool thread_enabled = true;
#endif

static bool lock_root(void)
{
	pthread_mutex_lock(&root_mutex);
	if (!enabled) {
		pthread_mutex_unlock(&root_mutex);
		thread_enabled = false;
		return false;
	}

	return true;
}

static profile_root_entry *get_root_entry(const char *name)
{
	profile_root_entry *r_entry = NULL;

	for (size_t i = 0; i < root_entries.num; i++) {
		if (root_entries.array[i].name == name) {
			r_entry = &root_entries.array[i];
			break;
		}
	}

	if (!r_entry) {
		r_entry = da_push_back_new(root_entries);
		r_entry->mutex = bmalloc(sizeof(pthread_mutex_t));
		pthread_mutex_init(r_entry->mutex, NULL);

		r_entry->name = name;
		r_entry->entry = bzalloc(sizeof(profile_entry));
		init_entry(r_entry->entry, name);
	}

	return r_entry;
}

void profile_register_root(const char *name,
		uint64_t expected_time_between_calls)
{
	if (!lock_root())
		return;

	get_root_entry(name)->entry->expected_time_between_calls =
		(expected_time_between_calls + 500) / 1000;
	pthread_mutex_unlock(&root_mutex);
}

static void free_call_context(profile_call *context);

static void merge_context(profile_call *context)
{
	pthread_mutex_t *mutex = NULL;
	profile_entry *entry = NULL;
	profile_call *prev_call = NULL;

	if (!lock_root()) {
		free_call_context(context);
		return;
	}

	profile_root_entry *r_entry = get_root_entry(context->name);

	mutex     = r_entry->mutex;
	entry     = r_entry->entry;
	prev_call = r_entry->prev_call;

	r_entry->prev_call = context;

	pthread_mutex_lock(mutex);
	pthread_mutex_unlock(&root_mutex);

	merge_call(entry, context, prev_call);

	pthread_mutex_unlock(mutex);

	free_call_context(prev_call);
}

void profile_start(const char *name)
{
	if (!thread_enabled)
		return;

	profile_call new_call = {
		.name = name,
		.overhead_start = os_gettime_ns(),
		.parent = thread_context,
	};

	profile_call *call = NULL;

	if (new_call.parent) {
		size_t idx = da_push_back(new_call.parent->children, &new_call);
		call = &new_call.parent->children.array[idx];
	} else {
		call = bmalloc(sizeof(profile_call));
		memcpy(call, &new_call, sizeof(profile_call));
	}

	thread_context = call;
	call->start_time = os_gettime_ns();
}

void profile_end(const char *name)
{
	uint64_t end = os_gettime_ns();
	if (!thread_enabled)
		return;

	profile_call *call = thread_context;
	if (!call) {
		blog(LOG_ERROR, "Called profile end with no active profile");
		return;
	}

	if (!call->name)
		call->name = name;

	if (call->name != name) {
		blog(LOG_ERROR, "Called profile end with mismatching name: "
				"start(\"%s\"[%p]) <-> end(\"%s\"[%p])",
				call->name, call->name, name, name);

		profile_call *parent = call->parent;
		while (parent && parent->parent &&  parent->name != name)
			parent = parent->parent;

		if (!parent || parent->name != name)
			return;

		while (call->name != name) {
			profile_end(call->name);
			call = call->parent;
		}
	}

	thread_context = call->parent;

	call->end_time = end;
	call->overhead_end = os_gettime_ns();

	if (call->parent)
		return;

	merge_context(call);
}

static int profiler_time_entry_compare(const void *first, const void *second)
{
	int64_t diff = ((profiler_time_entry*)second)->time_delta -
		((profiler_time_entry*)first)->time_delta;
	return diff < 0 ? -1 : (diff > 0 ? 1 : 0);
}

typedef DARRAY(profiler_time_entry) profiler_time_array;

static uint64_t copy_map_to_array(profile_times_table *map,
		profiler_time_array *entry_buffer,
		uint64_t *min_, uint64_t *max_)
{
	migrate_old_entries(map, false);

	da_reserve((*entry_buffer), map->occupied);
	da_resize((*entry_buffer), 0);

	uint64_t min__ = ~(uint64_t)0;
	uint64_t max__ = 0;

	uint64_t calls = 0;
	for (size_t i = 0; i < map->size; i++) {
		if (!map->entries[i].probes)
			continue;

		profiler_time_entry *entry = &map->entries[i].entry;

		da_push_back((*entry_buffer), entry);

		calls += entry->count;
		min__ = (min__ < entry->time_delta) ? min__ : entry->time_delta;
		max__ = (max__ > entry->time_delta) ? max__ : entry->time_delta;
	}

	if (min_)
		*min_ = min__;
	if (max_)
		*max_ = max__;

	qsort(entry_buffer->array, entry_buffer->num,
			sizeof(profiler_time_entry),
			profiler_time_entry_compare);

	return calls;
}

static void gather_stats(uint64_t expected_time_between_calls,
		profile_times_table *map,
		profiler_time_array *entry_buffer,
		uint64_t *calls,
		uint64_t *min_, uint64_t *max_, uint64_t *percentile99,
		double *percent_within_bounds)
{
	if (!map->occupied) {
		*min_ = 0;
		*max_ = 0;
		*percentile99 = 0;
		*percent_within_bounds = 0.;
		return;
	}

	*calls = copy_map_to_array(map, entry_buffer, min_, max_);

	/*if (entry_buffer->num > 2)
		blog(LOG_INFO, "buffer-size %lu, overall count %llu\n"
				"map-size %lu, occupied %lu, probes %lu",
				entry_buffer->num, calls,
				map->size, map->occupied,
				map->max_probe_count);*/
	uint64_t accu = 0;
	for (size_t i = 0; i < entry_buffer->num; i++) {
		accu += entry_buffer->array[i].count;
		if (accu < *calls * 0.01)
			continue;

		*percentile99 = entry_buffer->array[i].time_delta;
		break;
	}

	*percent_within_bounds = 0.;
	if (!expected_time_between_calls)
		return;

	accu = 0;
	for (size_t i = 0; i < entry_buffer->num; i++) {
		profiler_time_entry *entry = &entry_buffer->array[i];
		if (entry->time_delta < expected_time_between_calls)
			break;

		accu += entry->count;
	}
	*percent_within_bounds = (1. - (double)accu / *calls) * 100;
}

static void make_indent_string(struct dstr *indent_buffer, unsigned indent,
		uint64_t active)
{
	indent_buffer->len = 0;

	if (!indent) {
		dstr_cat_ch(indent_buffer, 0);
		return;
	}

	for (size_t i = 0; i < indent; i++) {
		const char *fragment = "";
		bool last = i + 1 == indent;
		if (active & ((uint64_t)1 << i))
			fragment = last ? " ┣" : " ┃";
		else
			fragment = last ? " ┗" : "  ";
		
		dstr_cat(indent_buffer, fragment);
	}
}

typedef profile_times_table *(*select_times_table)(profile_entry *entry);

static inline profile_times_table *get_times(profile_entry *entry)
{
	return &entry->times;
}

static inline profile_times_table *get_overhead(profile_entry *entry)
{
	return &entry->overhead;
}

static inline profile_times_table *get_times_between_calls(profile_entry *entry)
{
	return &entry->times_between_calls;
}

typedef void (*profile_entry_print_func)(profile_entry *entry,
		select_times_table get_table, profiler_time_array *entry_buffer,
		struct dstr *indent_buffer, struct dstr *output_buffer,
		unsigned indent, uint64_t active, uint64_t parent_calls);

static void profile_print_entry(profile_entry *entry,
		select_times_table get_table, profiler_time_array *entry_buffer,
		struct dstr *indent_buffer, struct dstr *output_buffer,
		unsigned indent, uint64_t active, uint64_t parent_calls)
{
	uint64_t calls = 0;
	uint64_t min_ = 0;
	uint64_t max_ = 0;
	uint64_t percentile99 = 0;
	double percent_within_bounds = 0.;
	gather_stats(entry->expected_time_between_calls,
			get_table(entry), entry_buffer, &calls,
			&min_, &max_, &percentile99, &percent_within_bounds);

	make_indent_string(indent_buffer, indent, active);

	if (min_ == max_) {
		dstr_printf(output_buffer, "%s%s: %g ms",
				indent_buffer->array, entry->name,
				min_ / 1000.);
	} else {
		dstr_printf(output_buffer, "%s%s: min=%g ms, max=%g ms, 99th "
				"percentile=%g ms",
				indent_buffer->array, entry->name,
				min_ / 1000., max_ / 1000.,
				percentile99 / 1000.);

		if (entry->expected_time_between_calls) {
			double expected_ms =
				entry->expected_time_between_calls / 1000.;
			dstr_catf(output_buffer, ", %g%% below %g ms",
					percent_within_bounds, expected_ms);
		}
	}

	if (parent_calls && calls != parent_calls) {
		double calls_per_parent = (double)calls / parent_calls;
		if (lround(calls_per_parent * 10) != 10)
			dstr_catf(output_buffer, ", %g calls per parent call",
					calls_per_parent);
	}

	blog(LOG_INFO, "%s", output_buffer->array);

	active |= (uint64_t)1 << indent;
	for (size_t i = 0; i < entry->children.num; i++) {
		if ((i + 1) == entry->children.num)
			active &= (1 << indent) - 1;
		profile_print_entry(&entry->children.array[i],
				get_table, entry_buffer,
				indent_buffer, output_buffer,
				indent + 1, active, calls);
	}
}

static void gather_stats_between(profile_times_table *map, 
		profiler_time_array *entry_buffer,
		uint64_t lower_bound, uint64_t upper_bound, double *percent,
		uint64_t *min_, uint64_t *max_,
		double *lower, double *higher)
{
	if (!map->occupied) {
		*percent = 0.;
		*min_ = 0;
		*max_ = 0;
		*lower = 0.;
		*higher = 0.;
		return;
	}

	uint64_t calls = copy_map_to_array(map, entry_buffer, min_, max_);

	uint64_t accu = 0;
	bool found_upper_bound = false;
	bool found_lower_bound = false;
	for (size_t i = 0; i < entry_buffer->num; i++) {
		uint64_t delta = entry_buffer->array[i].time_delta;

		if (!found_upper_bound && delta <= upper_bound) {
			*higher = (double)accu / calls * 100;
			accu = 0;
			found_upper_bound = true;
		}

		if (!found_lower_bound && delta < lower_bound) {
			*percent = (double)accu / calls * 100;
			accu = 0;
			found_lower_bound = true;
		}

		accu += entry_buffer->array[i].count;
	}

	if (!found_upper_bound)
		*higher = 100.;

	if (!found_lower_bound) {
		*percent = (double)accu / calls * 100;
		*lower = 0.;

	} else {
		*lower = (double)accu / calls * 100;
	}
}

static void profile_print_entry_expected(profile_entry *entry,
		select_times_table get_table, profiler_time_array *entry_buffer,
		struct dstr *indent_buffer, struct dstr *output_buffer,
		unsigned indent, uint64_t active, uint64_t parent_calls)
{
	UNUSED_PARAMETER(parent_calls);

	if (!entry->expected_time_between_calls)
		return;

	uint64_t expected_time = entry->expected_time_between_calls;

	uint64_t min_ = 0;
	uint64_t max_ = 0;
	double percent = 0.;
	double lower = 0.;
	double higher = 0.;
	gather_stats_between(get_table(entry), entry_buffer,
			(uint64_t)(expected_time * 0.98),
			(uint64_t)(expected_time * 1.02 + 0.5),
			&percent,
			&min_, &max_, &lower, &higher);

	make_indent_string(indent_buffer, indent, active);

	blog(LOG_INFO, "%s%s: min=%g ms, max=%g ms, %g%% "
			"within ±2%% of %g ms (%g%% lower, %g%% higher)",
			indent_buffer->array, entry->name,
			min_ / 1000., max_ / 1000., percent,
			expected_time / 1000.,
			lower, higher);

	active |= (uint64_t)1 << indent;
	for (size_t i = 0; i < entry->children.num; i++) {
		if ((i + 1) == entry->children.num)
			active &= (1 << indent) - 1;
		profile_print_entry_expected(&entry->children.array[i],
				get_table, entry_buffer,
				indent_buffer, output_buffer,
				indent + 1, active, 0);
	}
}

void profile_print_func(const char *intro, profile_entry_print_func print,
		select_times_table get_table)
{
	profiler_time_array entry_buffer = {0};
	struct dstr indent_buffer = {0};
	struct dstr output_buffer = {0};

	pthread_mutex_lock(&root_mutex);
	blog(LOG_INFO, "%s", intro);
	for (size_t i = 0; i < root_entries.num; i++) {
		pthread_mutex_lock(root_entries.array[i].mutex);
		print(root_entries.array[i].entry, get_table,
				&entry_buffer, &indent_buffer, &output_buffer,
				0, 0, 0);
		pthread_mutex_unlock(root_entries.array[i].mutex);
	}
	blog(LOG_INFO, "=================================================");
	pthread_mutex_unlock(&root_mutex);

	dstr_free(&output_buffer);
	dstr_free(&indent_buffer);
	da_free(entry_buffer);
}

void profile_print(void)
{
	profile_print_func("== Profiler Results =============================",
			profile_print_entry, get_times);
}

void profile_print_overhead(void)
{
	profile_print_func("== Profiler Overhead ============================",
			profile_print_entry, get_overhead);
}

void profile_print_time_between_calls(void)
{
	profile_print_func("== Profiler Time Between Calls ==================",
			profile_print_entry_expected, get_times_between_calls);
}

static void free_call_children(profile_call *call)
{
	if (!call)
		return;

	const size_t num = call->children.num;
	for (size_t i = 0; i < num; i++)
		free_call_children(&call->children.array[i]);

	da_free(call->children);
}

static void free_call_context(profile_call *context)
{
	free_call_children(context);
	bfree(context);
}

static void free_hashmap(profile_times_table *map)
{
	map->size = 0;
	bfree(map->entries);
	map->entries = NULL;
	bfree(map->old_entries);
	map->old_entries = NULL;
}

static void free_profile_entry(profile_entry *entry)
{
	for (size_t i = 0; i < entry->children.num; i++)
		free_profile_entry(&entry->children.array[i]);

	free_hashmap(&entry->times);
	free_hashmap(&entry->overhead);
	free_hashmap(&entry->times_between_calls);
	da_free(entry->children);
}

void profile_free(void)
{
	DARRAY(profile_root_entry) old_root_entries = {0};

	pthread_mutex_lock(&root_mutex);
	enabled = false;
	da_move(old_root_entries, root_entries);
	pthread_mutex_unlock(&root_mutex);

	for (size_t i = 0; i < old_root_entries.num; i++) {
		profile_root_entry *entry = &old_root_entries.array[i];

		pthread_mutex_lock(entry->mutex);
		pthread_mutex_unlock(entry->mutex);

		pthread_mutex_destroy(entry->mutex);
		bfree(entry->mutex);
		entry->mutex = NULL;

		free_call_context(entry->prev_call);

		free_profile_entry(entry->entry);
		bfree(entry->entry);
	}

	da_free(old_root_entries);
}

static pthread_mutex_t name_store_mutex = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(char*) name_store;

const char *profile_store_name(const char *format, ...)
{
	va_list args;
	va_start(args, format);

	struct dstr str = {0};
	dstr_vprintf(&str, format, args);

	va_end(args);

	const char *result = NULL;

	pthread_mutex_lock(&name_store_mutex);
	size_t idx = da_push_back(name_store, &str.array);
	result = name_store.array[idx];
	pthread_mutex_unlock(&name_store_mutex);

	return result;
}

void profile_free_names(void)
{
	pthread_mutex_lock(&name_store_mutex);
	for (size_t i = 0; i < name_store.num; i++)
		bfree(name_store.array[i]);

	da_free(name_store);
	pthread_mutex_unlock(&name_store_mutex);
}
