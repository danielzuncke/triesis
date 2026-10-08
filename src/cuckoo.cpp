/* Build:
 *

g++ src/cuckoo.cpp libs/libcuckoo_trie.a -o bin/cuckoo -std=c++17 -O2 \
  -DNDEBUG -march=native -pthread -Isubmodules/cuckoo \
  -DDZ_STEPS_INSERT=1 -DDZ_STEPS_QUERY=1 -DDZ_THREADS=$(nproc)

 * build_libs.sh => libs/cuckoo
 * needs reserved 2MB huge pages (vm.nr_hugepages) for the hash table
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

#include "dz_load.h"
#include "dz_load2.hpp"

extern "C" {
#include "../submodules/cuckoo/cuckoo_trie.h"
}

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

// had err: full with 2; that is the expected (soft) upper bound. use 3 so it
// will go through consistently; no big tuning
#if !defined(DZ_CELLS_PER_KEY)
  #define DZ_CELLS_PER_KEY 3
#endif

/* build ds in file order; key is u8[] incl. the \0, value is the ptr to the
 * key
 */
static void insert(PerfEvent &perf_event, const std::vector<const u8 *> &keys,
                   const std::vector<u64> &lengths,
                   cuckoo_trie *&ds, u8 *&kv_buffer,
                   u64 *rss_after_insert)
{
    const u64 nr_keys = keys.size();

    // size of one kv: header + key + value, rounded up to 8 bytes
    auto kv_bytes = [](u64 length) {
        return (sizeof(ct_kv) + length + sizeof(u64) + 7) & ~(u64) 7;
    };
    u64 kv_buffer_bytes = 0;
    for (u64 i = 0; i < nr_keys; i++)
        kv_buffer_bytes += kv_bytes(lengths[i]);
    u8 *kv_next = NULL;

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
            {
                ds = ct_alloc(DZ_CELLS_PER_KEY * nr_keys);
                if (ds == NULL) dz_fatal("ct_alloc failed (huge pages?)\n");
                kv_buffer = (u8 *) malloc(kv_buffer_bytes);
                if (kv_buffer == NULL) dz_fatal("malloc failed\n");
                kv_next = kv_buffer;
            }
            for (u64 i = step_from; i < step_to; i++)
            {
                ct_kv *kv = (ct_kv *) kv_next;
                kv->key_size = (int) lengths[i];
                kv->value_size = sizeof(u64);
                memcpy(kv->bytes, keys[i], lengths[i]);
                memcpy(kv->bytes + lengths[i], &keys[i], sizeof(u64));
                kv_next += kv_bytes(lengths[i]);
                i32 ret = ct_insert(ds, kv);
                if (ret != S_OK)
                    dz_fatal("ct_insert failed for key %" PRIu64 " with %d (1 = already in, 2 = full, 3 = key too long)\n", i, ret);
            }
        }
        // outside of perf block: reads /proc
        rss_after_insert[step] = dz_mem_usage_kb();
    }
}


static void destroy(PerfEvent &perf_event, cuckoo_trie *&ds, u8 *&kv_buffer,
                    u64 nr_keys)
{
    perf_event.setParam("phase", "destro");
    perf_event.setParam("step", 0);
    perf_event.setParam("keys_cum", nr_keys);
    perf_event.setParam("keys_step", nr_keys);
    DzPerfBlock perf_block(perf_event, nr_keys);
    ct_free(ds);
    free(kv_buffer);
    ds = NULL;
    kv_buffer = NULL;
}


// extra func to make multi thread nicer; lib is built without MULTITHREADING,
// lookups only read
static u64 lookup_impl(cuckoo_trie *ds,
                       const std::vector<const u8 *> &lookup_keys,
                       const std::vector<u64> &lookup_lengths,
                       u64 from, u64 to)
{
    u64 received = 0;
    for (u64 i = from; i < to; i++)
    {
        ct_kv *kv = ct_lookup(
            ds, lookup_lengths[i], (uint8_t *) lookup_keys[i]);
        received ^= (u64) kv;
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
    perf_event.setParam("ds", "cuckoo");
    perf_event.setParam("threads", DZ_THREADS);

    u64 rss_baseline = dz_mem_usage_kb();
    u64 rss_after_insert[DZ_STEPS_INSERT] = {0};
    u64 hugepage_mem_kib = 0;

    // final entry for accumulation after syncing multi thread
    u64 received[DZ_THREADS + 1] = {0};

    cuckoo_trie *ds = NULL; // datastructure
    u8 *kv_buffer = NULL; // kv objects the trie points to

    // INSERT
    insert(perf_event, keys, lengths, ds, kv_buffer, rss_after_insert);
    hugepage_mem_kib = dz_mem_hugetlb_kb();


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
            received[0] ^= lookup_impl(ds, lookup_keys, lookup_lengths, step_from, step_to);
#endif
        for (u32 t = 0; t < DZ_THREADS; t++)
            received[DZ_THREADS] ^= received[t];
        }
    }

    // DESTROY
    destroy(perf_event, ds, kv_buffer, nr_keys);

    // BENCHMARK END
    print_rss(rss_baseline, rss_after_insert, DZ_STEPS_INSERT);
    std::cout << "memory used by hugepages: " << hugepage_mem_kib <<" kiB\n";
    sink_null(received[DZ_THREADS]);
}


static void scan(const std::vector<const u8 *> &keys,
                 const std::vector<u64> &lengths)
{
    const u64 nr_keys = keys.size();

    PerfEvent perf_event;
    perf_event.setParam("ds", "cuckoo");
    perf_event.setParam("threads", 1); // scan is single threaded

    u64 rss_baseline = dz_mem_usage_kb();
    u64 rss_after_insert[DZ_STEPS_INSERT] = {0};
    u64 hugepage_mem_kib = 0;

    u64 received = 0;

    cuckoo_trie *ds = NULL; // datastructure
    u8 *kv_buffer = NULL; // kv objects the trie points to

    // INSERT
    insert(perf_event, keys, lengths, ds, kv_buffer, rss_after_insert);
    hugepage_mem_kib = dz_mem_hugetlb_kb();


    // SCAN
    perf_event.setParam("phase", "  scan");
    {
        // lib has no ct_iter_free; alloc == malloc => free()
        ct_iter *iter = ct_iter_alloc(ds);
        if (iter == NULL) dz_fatal("ct_iter_alloc failed\n");
        // size 0 => position before the smallest key
        u8 empty_key = 0;
        ct_iter_goto(iter, 0, &empty_key);
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
                ct_kv *kv = ct_iter_next(iter);
                received ^= (u64) kv;
            }
        }
        free(iter);
    }

    // DESTROY
    destroy(perf_event, ds, kv_buffer, nr_keys);

    // BENCHMARK END
    print_rss(rss_baseline, rss_after_insert, DZ_STEPS_INSERT);
    std::cout << "memory used by hugepages: " << hugepage_mem_kib <<" kiB\n";
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
