/* Build:
 *

g++ src/wormhole.cpp libs/libwh.a -o bin/wormhole -std=c++17 \
  -O2 -DNDEBUG -march=native -pthread -DNDEBUG \
  -DDZ_STEPS_INSERT=1 -DDZ_STEPS_QUERY=1 -DDZ_THREADS=$(nproc)

 * build_libs.sh => libs/wormhole/lib/libwh.a
 *
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

#include "../submodules/wormhole/lib.h"
#include "../submodules/wormhole/kv.h"
#include "../submodules/wormhole/wh.h" // must come last

#include <cstdio>
#include <cstdlib>
#include <cstring>
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


/* build ds in file order with the unsafe (no locks) api; key is u8[] incl. the
 * \0, value is the ptr to the key (8 bytes, copied into the kv).
 */
static void insert(PerfEvent &perf_event, const std::vector<const u8 *> &keys,
                   const std::vector<u64> &lengths,
                   struct wormhole *&ds, u64 *rss_after_insert)
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
                ds = whunsafe_create(&kvmap_mm_ndf);
            for (u64 i = step_from; i < step_to; i++)
                whunsafe_put(ds, kv_create(
                    keys[i], (u32) lengths[i], &keys[i], sizeof(keys[i])));
        }
        // outside of perf block: reads /proc
        rss_after_insert[step] = dz_mem_usage_kb();
    }
}


static void destroy(PerfEvent &perf_event, struct wormhole *&ds,
                    u64 nr_keys)
{
    perf_event.setParam("phase", "destro");
    perf_event.setParam("step", 0);
    perf_event.setParam("keys_cum", nr_keys);
    perf_event.setParam("keys_step", nr_keys);
    DzPerfBlock perf_block(perf_event, nr_keys);
    wormhole_destroy(ds);
    ds = NULL;
}


static u64 lookup_impl(struct wormhole *ds,
                       const std::vector<const u8 *> &lookup_keys,
                       const std::vector<u64> &lookup_lengths,
                       u64 from, u64 to)
{
    u64 received = 0;
    for (u64 i = from; i < to; i++)
    {
        struct kref key;
        kref_ref_hash32(&key, lookup_keys[i], (u32) lookup_lengths[i]);
        whunsafe_inp(ds, &key, [](struct kv *const curr, void *const acc) {
            *(u64 *) acc ^= (u64) curr;
        }, &received);
    }
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
    perf_event.setParam("ds", "wormhole");
    perf_event.setParam("threads", DZ_THREADS);

    u64 rss_baseline = dz_mem_usage_kb();
    u64 rss_after_insert[DZ_STEPS_INSERT] = {0};

    // final entry for accumulation after syncing multi thread
    u64 received[DZ_THREADS + 1] = {0};

    struct wormhole *ds = NULL; // datastructure

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
                        ds, lookup_keys, lookup_lengths, from, to);
                });
            }
            for (u32 t = 0; t < DZ_THREADS; t++)
                threads[t].join();
#else
            received[0] ^= lookup_impl(
                ds, lookup_keys, lookup_lengths, step_from, step_to);
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
    perf_event.setParam("ds", "wormhole");
    perf_event.setParam("threads", 1); // scan is single threaded

    u64 rss_baseline = dz_mem_usage_kb();
    u64 rss_after_insert[DZ_STEPS_INSERT] = {0};

    u64 received = 0;

    struct wormhole *ds = NULL; // datastructure

    // INSERT
    insert(perf_event, keys, lengths, ds, rss_after_insert);


    // SCAN
    // leaves (nodes of linked list) get sorted (lazy, but here always) on
    // first (and only) iter
    perf_event.setParam("phase", "  scan");
    {
        struct wormhole_iter *iter = whunsafe_iter_create(ds);
        // null key => position at the smallest key
        whunsafe_iter_seek(iter, kref_null());
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
                wormhole_iter_inp(iter, [](struct kv *const curr,
                                           void *const priv) {
                    *(u64 *) priv ^= (u64) curr;
                }, &received);
                whunsafe_iter_skip1(iter);
            }
        }
        whunsafe_iter_destroy(iter);
    }

    // DESTROY
    destroy(perf_event, ds, nr_keys);

    // BENCHMARK END
    print_rss(rss_baseline, rss_after_insert, DZ_STEPS_INSERT);
    sink_null(received);
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
