/* Build:
 *

g++ src/scanarr.cpp -o bin/scanarr -std=c++17 -O2 -DNDEBUG -march=native \
  -Isubmodules/tlx \
  -DDZ_STEPS_QUERY=1

 * just single threaded since no lookups, no inserts steps since sort cannot
 * well be split up. kept code from copy paste faster
 *
 * -DDZ_STEPS_QUERY=... to scan to perfevent every 1/... of total ops
 *  default: 10
 */

#include "dz_load2.hpp"

// file shows a lot of warnings if lsp has no help to resolve include, but
// works when building from project root with -Isubmodules/tlx (see top)
#include "../submodules/tlx/tlx/sort/strings.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#if DZ_THREADS > 1
  #define DZ_STEPS_QUERY 1
#else
  #define DZ_THREADS 1
  #if !defined(DZ_STEPS_QUERY)
    #define DZ_STEPS_QUERY 10
  #endif
#endif

// static: built in 1 step from all keys
#define DZ_STEPS_INSERT 1

// array of ptrs to the \0 terminated keys (not copied)
typedef std::vector<const u8 *> Ds;


/* copy the ptrs (input is const) and sort them by the keys they point to;
 * single threaded tlx string sort, keys are read as \0 terminated strings =>
 * lengths not needed
 */
static void insert(PerfEvent &perf_event, const std::vector<const u8 *> &keys,
                   std::optional<Ds> &ds, u64 *rss_after_insert)
{
    const u64 nr_keys = keys.size();
    perf_event.setParam("phase", "insert");
    perf_event.setParam("step", 0);
    perf_event.setParam("keys_cum", nr_keys);
    perf_event.setParam("keys_step", nr_keys);
    {
        DzPerfBlock perf_block(perf_event, nr_keys);

        ds.emplace();
        ds->reserve(nr_keys);
        ds->insert(ds->end(), keys.begin(), keys.end());
        tlx::sort_strings(*ds);
    }
    // outside of perf block: reads /proc
    rss_after_insert[0] = dz_mem_usage_kb();
}


static void destroy(PerfEvent &perf_event,
                    std::optional<Ds> &ds,
                    u64 nr_keys)
{
    perf_event.setParam("phase", "destro");
    perf_event.setParam("step", 0);
    perf_event.setParam("keys_cum", nr_keys);
    perf_event.setParam("keys_step", nr_keys);
    DzPerfBlock perf_block(perf_event, nr_keys);
    ds.reset();
}


static void scan(const std::vector<const u8 *> &keys,
                 const std::vector<u64> &lengths)
{
    const u64 nr_keys = keys.size();
    (void) lengths; // keys are \0 terminated

    PerfEvent perf_event;
    perf_event.setParam("ds", "scanarr");
    perf_event.setParam("threads", 1); // scan is single threaded

    u64 rss_baseline = dz_mem_usage_kb();
    u64 rss_after_insert[DZ_STEPS_INSERT] = {0};

    u64 received = 0;

    std::optional<Ds> ds; // datastructure

    // INSERT
    insert(perf_event, keys, ds, rss_after_insert);


    // SCAN
    // iterates the array in sorted order; value is the position of the entry
    // in the array: &ds[i]
    perf_event.setParam("phase", "scan");
    {
        for (u64 step = 0; step < DZ_STEPS_QUERY; step++)
        {
            const u64 step_from = nr_keys * step / DZ_STEPS_QUERY;
            const u64 step_to = nr_keys * (step + 1) / DZ_STEPS_QUERY;
            const u64 step_keys = step_to - step_from;
            perf_event.setParam("step", step);
            perf_event.setParam("keys_cum", step_to);
            perf_event.setParam("keys_step", step_keys);
            DzPerfBlock perf_block(perf_event, step_keys);

            for (u64 i = step_from; i < step_to; i++)
            {
                // each ptr has to be visited once, but no work
                received ^= (u64) (*ds)[i];
            }
        }
    }

    // DESTROY
    destroy(perf_event, ds, nr_keys);

    // BENCHMARK END
    print_rss(rss_baseline, rss_after_insert, DZ_STEPS_INSERT);
    sink_null(received);
}


int main(int argc, char **argv)
{
    // scan only: 2nd arg (any value) required, same as the other ds; no
    // lookup => fail without it so a wrong script call is noticed
    if (argc != 3)
        return 1;

    std::vector<const u8 *> keys;
    std::vector<u64> lengths;
    dz_load(argv[1], keys, lengths);

    scan(keys, lengths);

    return 0;
}
