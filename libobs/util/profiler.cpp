#include <algorithm>
#include "profiler.h"


extern "C" void sort_times(profiler_time_entry_t *first,
		profiler_time_entry_t *last)
{
	auto Compare = [](const profiler_time_entry_t &a,
			const profiler_time_entry_t &b)
	{
		return a.time_delta > b.time_delta;
	};

	std::sort(first, last, Compare);
}
