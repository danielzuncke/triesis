/* dz_load.h was originally intended to do all the loading and utility.
 * Since it turns out everything is cpp anyways and I now benchmark with
 * perfevent, create a dz_load for cpp as well (this file).
 */

#include "dz_load.h"

#include "../submodules/perfevent/PerfEvent.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <random>
#include <sstream>


/* Dynamically calculated col's in output here so the perfevent blocks are a
 * bit shorter.
 */
struct DzPerfBlock
{
	PerfEvent &perf_event;
	u64 ops; // operations / second

	DzPerfBlock(PerfEvent &perf_event, u64 ops)
	: perf_event(perf_event), ops(ops)
	{
		perf_event.startCounters();
	}

	~DzPerfBlock()
	{
		perf_event.stopCounters();
		double seconds = perf_event.getDuration();
		std::stringstream header;
		std::stringstream data;

		perf_event.printParams(header, data);
		PerfEvent::printCounter(header, data, "time", seconds);
		PerfEvent::printCounter(header, data, "op/s",
			  seconds > 0 ? (double) ops / seconds : 0.0);
		PerfEvent::printCounter(header, data, "ns/key",
			  ops > 0 ? seconds * 1e9 / (double) ops : 0.0);

		perf_event.printReport(header, data, ops);

		if (perf_event.printHeader)
		{
			std::cout << header.str() << "\n";
			perf_event.printHeader = false;
		}
		std::cout << data.str() << "\n";
	}
};


/* Load file (\n sep) into vec of \0 term strings (remove \n) and vec
 * containing all the lengths.
 * Strings are contiguous in memory afterwards:
 * [ s1...\0   s2...\0    s3...\0    etc]
 *
 * o_keys contains ptrs to s1, s2, ...
 *
 * o_lengths contains lengths of s1, s2, ... INCLUDING the \0: used as byte arr
 * and doesn't get interpreted as string by data structures
 *
 * if free explicitly and not auto on prog exit: free(o_keys[0]);
 */
static void dz_load(const char *path, std::vector<const u8 *> &o_keys,
                    std::vector<u64> &o_lengths)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) dz_fatal("open %s: %s\n", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long file_bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_bytes <= 0) dz_fatal("%s is empty\n", path);

    // +1 in case file isn't \n terminated
    u8 *data = (u8 *) malloc((size_t) file_bytes + 1);
    if (data == NULL) dz_fatal("malloc failed\n");
    if (fread(data, 1, (size_t) file_bytes, f) != (size_t) file_bytes)
        dz_fatal("read %s failed\n", path);
    fclose(f);

    // If file isn't \n terminated add into extra allocated byte and inc
    // data_bytes. Loop below iterates by char until
    u64 data_bytes = (u64) file_bytes;
    if (data[data_bytes - 1] != '\n')
        data[data_bytes++] = '\n';

    // exact sizes, single alloc
    u64 nr_keys = (u64) std::count(data, data + data_bytes, (u8) '\n');
    o_keys.clear();
    o_lengths.clear();
    o_keys.reserve(nr_keys);
    o_lengths.reserve(nr_keys);

    u64 from = 0;
    for (u64 i = 0; i < data_bytes; i++)
    {
        if (data[i] != '\n')
            continue;
        data[i] = 0; // \0 terminate the strings.
        o_keys.push_back(data + from);
        o_lengths.push_back(i - from + 1); // +1 to include the \0 byte.
        from = i + 1;
    }
}


/* Shuffles keys and lengths in place with the same permutation: the same
 * seed for both, so the order doesn't have to be stored.
 * Optionally seed the shuffle.
 */
static void shuffle_keys(
    std::vector<const u8 *> &keys, std::vector<u64> &lengths,
    i32 *seedVal)
{
    const auto seed = seedVal
        ? static_cast<std::default_random_engine::result_type>(*seedVal)
        : std::random_device {}();
    // https://stackoverflow.com/a/6926473/12446997
    auto rng = std::default_random_engine {seed};
    std::shuffle(std::begin(keys), std::end(keys), rng);
    rng = std::default_random_engine {seed};
    std::shuffle(std::begin(lengths), std::end(lengths), rng);
}


// Function to do smth that cannot be optimized away. (fs interaction)
static void sink_null(u64 x)
{
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull == NULL) dz_fatal("open /dev/null failed\n");
    fprintf(devnull, "%" PRIu64, x);
    fclose(devnull);
}


static void print_rss(u64 baseline, u64 *values, u64 length)
{
    std::cout << "RSS in kiB:\n  baseline after loading file: " << baseline;
    for (u64 i = 0; i < length; i++)
        std::cout << "\n  after insert step " << i << ": " << values[i];
    std::cout << "\n  total without baseline: "
        << values[length - 1] - baseline << "\n";
}
