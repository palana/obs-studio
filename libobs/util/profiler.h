#pragma once

#include "base.h"
#include "darray.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct profiler_snapshot profiler_snapshot_t;
typedef struct profiler_snapshot_entry profiler_snapshot_entry_t;
typedef struct profiler_time_entry profiler_time_entry_t;

/* ------------------------------------------------------------------------- */
/* Profiling */

EXPORT void profile_register_root(const char *name,
		uint64_t expected_time_between_calls);

EXPORT void profile_start(const char *name);
EXPORT void profile_end(const char *name);

EXPORT void profile_print(profiler_snapshot_t *snap);
EXPORT void profile_print_time_between_calls(profiler_snapshot_t *snap);

EXPORT void profile_free(void);

/* ------------------------------------------------------------------------- */
/* Profiler name storage */

#ifndef _MSC_VER
#define PRINTFATTR(f, a) __attribute__((__format__(__printf__, f, a)))
#else
#define PRINTFATTR(f, a)
#endif

PRINTFATTR(1, 2)
EXPORT const char *profile_store_name(const char *format, ...);
EXPORT void profile_free_names(void);

#undef PRINTFATTR

/* ------------------------------------------------------------------------- */
/* Profiler data access */

struct profiler_time_entry {
	uint64_t time_delta;
	uint64_t count;
};

typedef DARRAY(profiler_time_entry_t) profiler_time_entries_t;

typedef bool (*profiler_entry_enum_func)(void *context,
		profiler_snapshot_entry_t *entry);

EXPORT profiler_snapshot_t *profile_snapshot_create(void);
EXPORT void profile_snapshot_free(profiler_snapshot_t *snap);

EXPORT size_t profiler_snapshot_num_roots(profiler_snapshot_t *snap);
EXPORT void profiler_snapshot_enumerate_roots(profiler_snapshot_t *snap,
		profiler_entry_enum_func func, void *context);

EXPORT size_t profiler_snapshot_num_children(profiler_snapshot_entry_t *entry);
EXPORT void profiler_snapshot_enumerate_children(
		profiler_snapshot_entry_t *entry,
		profiler_entry_enum_func func, void *context);

EXPORT const char *profiler_snapshot_entry_name(
		profiler_snapshot_entry_t *entry);

EXPORT profiler_time_entries_t *profiler_snapshot_entry_times(
		profiler_snapshot_entry_t *entry);
EXPORT uint64_t profiler_snapshot_entry_min_time(
		profiler_snapshot_entry_t *entry);
EXPORT uint64_t profiler_snapshot_entry_max_time(
		profiler_snapshot_entry_t *entry);
EXPORT uint64_t profiler_snapshot_entry_overall_count(
		profiler_snapshot_entry_t *entry);

EXPORT profiler_time_entries_t *profiler_snapshot_entry_times_between_calls(
		profiler_snapshot_entry_t *entry);
EXPORT uint64_t profiler_snapshot_entry_expected_time_between_calls(
		profiler_snapshot_entry_t *entry);
EXPORT uint64_t profiler_snapshot_entry_min_time_between_calls(
		profiler_snapshot_entry_t *entry);
EXPORT uint64_t profiler_snapshot_entry_max_time_between_calls(
		profiler_snapshot_entry_t *entry);
EXPORT uint64_t profiler_snapshot_entry_overall_between_calls_count(
		profiler_snapshot_entry_t *entry);

#ifdef __cplusplus
}

/* ------------------------------------------------------------------------- */
/* C++ convenience */

struct OBSScopeProfiler {
	const char *name;
	bool enabled = true;

	OBSScopeProfiler(const char *name)
		: name(name)
	{
		profile_start(name);
	}

	~OBSScopeProfiler() { Stop(); }

	OBSScopeProfiler(const OBSScopeProfiler &) = delete;
	OBSScopeProfiler(OBSScopeProfiler &&other)
		: name(other.name),
		  enabled(other.enabled)
	{
		other.enabled = false;
	}

	OBSScopeProfiler &operator=(const OBSScopeProfiler &) = delete;
	OBSScopeProfiler &operator=(OBSScopeProfiler &&other) = delete;

	void Stop()
	{
		if (!enabled)
			return;

		profile_end(name);
		enabled = false;
	}
};

#define OBSScopeProfiler_NameConcatImpl(x, y) x ## y
#define OBSScopeProfiler_NameConcat(x, y) OBSScopeProfiler_NameConcatImpl(x, y)

#ifdef __COUNTER__
#define OBSScopeProfiler_Name(x) OBSScopeProfiler_NameConcat(x, __COUNTER__)
#else
#define OBSScopeProfiler_Name(x) OBSScopeProfiler_NameConcat(x, __LINE__)
#endif

#define OBSProfileScope(x) OBSScopeProfiler \
	OBSScopeProfiler_Name(OBS_SCOPE_PROFILE){x}
#endif
