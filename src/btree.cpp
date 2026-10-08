/* Build:
 *

g++ src/btree.cpp -o bin/btree -std=c++17 -O2 -DNDEBUG -march=native -pthread \
  -Isubmodules/tlx \
  -DDZ_STEPS_INSERT=1 -DDZ_STEPS_QUERY=1 -DDZ_THREADS=$(nproc)

 * output not necessarily aligned; command just works for perfevent part (and
 * is left aligned):

./bin/tlx <filename> | head -4 | column -t -s ','

 * -DDZ_THREADS=...
 *  >1 => -pthread; sets DZ_STEPS to 1
 *  default: 1
 *
 * -DDZ_STEPS_QUERY=... to lookup / scan to perfevent every 1/... of total ops
 *  default: 10
 *
 * -DDZ_STEPS_INSERT=... to insert to perfevent + RSS every 1/... of total ops
 *  default: 10
 */

#include "dz_load2.hpp"

// file shows a lot of warnings if lsp has no help to resolve include, but
// works when building from project root with -Isubmodules/tlx (see top)
#include "../submodules/tlx/tlx/container/btree_map.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#if DZ_THREADS > 1
  #define DZ_STEPS_QUERY 1
  #include <thread>
#else
  #define DZ_THREADS 1
  #if !defined(DZ_STEPS_QUERY)
    #define DZ_STEPS_QUERY 10
  #endif
#endif

#if !defined(DZ_STEPS_INSERT)
  #define DZ_STEPS_INSERT 10
#endif

// B+-tree with default node sizes; key: string_view into the loaded keys (not
// copied), value: index of the key
typedef tlx::btree_map<std::string_view, u64> Ds;


static inline std::string_view key_view(const u8 *key, u64 length)
{
    return std::string_view(reinterpret_cast<const char *>(key), length);
}


/* build ds in file order (no bulk load, it requires sorted input); key is a
 * string_view WITHOUT the \0 => -1 (the view is just ptr + length and by
 * definition doesn't contain it), value is the index of the key
 */
static void insert(PerfEvent &perf_event, const std::vector<const u8 *> &keys,
                   const std::vector<u64> &lengths,
                   std::optional<Ds> &ds, u64 *rss_after_insert)
{
    const u64 nr_keys = keys.size();
    perf_event.setParam("phase", "insert");
    for (u64 step = 0; step < DZ_STEPS_INSERT; step++)
    {
        const u64 step_from = nr_keys * step / DZ_STEPS_INSERT;
        const u64 step_to = nr_keys * (step + 1) / DZ_STEPS_INSERT;
        const u64 step_keys = step_to - step_from;
        perf_event.setParam("step", step);
        perf_event.setParam("keys_cum", step_to);
        perf_event.setParam("keys_step", step_keys);
        {
            DzPerfBlock perf_block(perf_event, step_keys);

            if (step == 0)
                ds.emplace();
            for (u64 i = step_from; i < step_to; i++)
                ds->insert2(key_view(keys[i], lengths[i] - 1), i);
        }
        // outside of perf block: reads /proc
        rss_after_insert[step] = dz_mem_usage_kb();
    }
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


// extra func to make multi thread nicer
static u64 lookup_impl(const Ds &ds,
                       const std::vector<const u8 *> &lookup_keys,
                       const std::vector<u64> &lookup_lengths,
                       u64 from, u64 to)
{
    u64 received = 0;
    for (u64 i = from; i < to; i++)
        received ^= (u64) &ds.find(
            key_view(lookup_keys[i], lookup_lengths[i] - 1))->second;
    return received;
}


static void lookup(const std::vector<const u8 *> &keys,
                   const std::vector<u64> &lengths)
{
    const u64 nr_keys = keys.size();

    // generate shuffled vec to query lookup in random order (but forcing
    // random mem access to get next str to lookup)
    std::vector<const u8 *> lookup_keys(keys);
    std::vector<u64> lookup_lengths(lengths);
    shuffle_keys(lookup_keys, lookup_lengths, NULL);


    PerfEvent perf_event;
    perf_event.setParam("ds", "tlx");
    perf_event.setParam("threads", DZ_THREADS);

    u64 rss_baseline = dz_mem_usage_kb();
    u64 rss_after_insert[DZ_STEPS_INSERT] = {0};

    // final entry for accumulation after syncing multi thread
    u64 received[DZ_THREADS + 1] = {0};

    std::optional<Ds> ds; // datastructure

    // INSERT
    insert(perf_event, keys, lengths, ds, rss_after_insert);


    // LOOKUP
    perf_event.setParam("phase", "lookup");
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

#if DZ_THREADS > 1
            std::thread threads[DZ_THREADS];
            for (u32 t = 0; t < DZ_THREADS; t++)
            {
                const u64 from = step_from + step_keys * t / DZ_THREADS;
                const u64 to = step_from + step_keys * (t + 1) / DZ_THREADS;
                threads[t] = std::thread([&, t, from, to]() {
                    received[t] ^= lookup_impl(
                        *ds, lookup_keys, lookup_lengths, from, to);
                });
            }
            for (u32 t = 0; t < DZ_THREADS; t++)
                threads[t].join();
#else
            received[0] ^= lookup_impl(
                *ds, lookup_keys, lookup_lengths, step_from, step_to);
#endif
        for (u32 t = 0; t < DZ_THREADS; t++)
            received[DZ_THREADS] ^= received[t];
        }
    }

    // DESTROY
    destroy(perf_event, ds, nr_keys);

    // BENCHMARK END
    print_rss(rss_baseline, rss_after_insert, DZ_STEPS_INSERT);
    sink_null(received[DZ_THREADS]);
}


static void scan(const std::vector<const u8 *> &keys,
                 const std::vector<u64> &lengths)
{
    const u64 nr_keys = keys.size();

    PerfEvent perf_event;
    perf_event.setParam("ds", "tlx");
    perf_event.setParam("threads", 1); // scan is single threaded

    u64 rss_baseline = dz_mem_usage_kb();
    u64 rss_after_insert[DZ_STEPS_INSERT] = {0};

    u64 received = 0;

    std::optional<Ds> ds; // datastructure

    // INSERT
    insert(perf_event, keys, lengths, ds, rss_after_insert);


    // SCAN
    // iterates all keys in sorted order along the leaves; the iterator returns
    // the stored pair, value is the index (like in lookup)
    perf_event.setParam("phase", "  scan");
    {
        Ds::const_iterator iter = ds->begin();
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
                received ^= (u64) &iter->second;
                ++iter;
            }
        }
    }

    // DESTROY
    destroy(perf_event, ds, nr_keys);

    // BENCHMARK END
    print_rss(rss_baseline, rss_after_insert, DZ_STEPS_INSERT);
    sink_null(received);
    if (received == 0)
        std::cout << "received scan was 0 can be due to symmetry of input, but should check, was: "
            << received << "\n";
}


int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3)
        return 1;

    std::vector<const u8 *> keys;
    std::vector<u64> lengths;
    dz_load(argv[1], keys, lengths);

    if (argc == 2)
        lookup(keys, lengths);
    else
        scan(keys, lengths);

    return 0;
}
