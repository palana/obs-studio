#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

struct profiler_time_entry {
	uint64_t time_delta;
	uint64_t count;
};

EXPORT void profile_register_root(const char *name,
		uint64_t expected_time_between_calls);

EXPORT void profile_start(const char *name);
EXPORT void profile_end(const char *name);

EXPORT void profile_print(void);
EXPORT void profile_print_overhead(void);
EXPORT void profile_print_time_between_calls(void);

EXPORT void profile_free(void);

#ifndef _MSC_VER
#define PRINTFATTR(f, a) __attribute__((__format__(__printf__, f, a)))
#else
#define PRINTFATTR(f, a)
#endif

PRINTFATTR(1, 2)
EXPORT const char *profile_store_name(const char *format, ...);
EXPORT void profile_free_names(void);

#undef PRINTFATTR

#ifdef __cplusplus
}

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
