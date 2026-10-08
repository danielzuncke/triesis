/* TODO: this file was originally intended to collect all the data, but that
 * changed with using: https://github.com/viktorleis/perfevent
 * Kept as is and ignore what isn't needed.  Remove if time + when clear what
 * is useful.
 */

#if !defined (DZ_LOAD_H)
#define DZ_LOAD_H


// define noreturn keyword depending on compiler
// Requires C11 if build with C compiler
#if defined(__cplusplus)
#define DZ_NORETURN [[noreturn]]
#elif defined(__STDC_VERSION__)
#define DZ_NORETURN _Noreturn
#else
#define DZ_NORETURN
#endif


#if defined(__cplusplus)
extern "C" {

/* load data for insert from:
 *
 *   /dev/shm/database.bin        u8[]  data
 *   /dev/shm/database_meta.bin  u64[]  end offset of each record
 *
 * 0 for null bytes:
 * abc0def0a0 =>    abc0 def0 a0     => 3 strings
 * 1234... (index)       4    8  10  => 3 offset
 *
 * Try to minimize impact from other sources than the measured trie: assume
 * data is given from in-memory data structure and we set /dev/shm to use
 * hugepages (huge=always) (default size).
 * Map file to array.
 * Data is read only during our benchmark.
 *
 * NOTE:
 * This file acts as a template and is reused to implement the tests for the
 * different data structures.  Features may grow once a ds is encountered that
 * can handle more things, until then some things may just be planned / noted
 * for implementation later.
 *
 * has a main function to debug what was read, compile for testing:
 * $ gcc -x c dz_load.h -DTEST_DECODER && ./a.out ...
 *
 * NOTE:
 * Some memory and time helpers are here too.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef uint8_t   u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef uint8_t   i8;
typedef uint16_t i16;
typedef uint32_t i32;
typedef uint64_t i64;


/* (runtime or) os will do clean up; used for all irrecoverable states instead
 * of managing errors.
 */
DZ_NORETURN static inline void dz_fatal(const char *errf, ...)
{
    if (errf == NULL)
    {
        fprintf(stderr, "dz_fatal: undisclosed error occured, stopping\n");
        exit(1);
    }
    va_list args;
    va_start(args, errf);
    vfprintf(stderr, errf, args);
    va_end(args);
    exit(1);
}

#if !defined(DZ_NR_GROUPS)
#define DZ_NR_GROUPS 16
#endif

static inline u64 dz_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64) t.tv_sec * 1000ULL * 1000ULL * 1000ULL + (u64) t.tv_nsec;
}

// Collection of time values that are measured.  In nanoseconds.
typedef struct {
    u64 total;
    u64 total_insert;
    // The first insert value contains the creation initialization time of the
    // datastructure as well. If the data structure has a finalization function
    // to call after which only lookups can happen, the final entry of insert
    // is measured after finalization has happened (for such data structures
    // this array may be unhelpful anyways and one should refer to
    // `total_insert` which also includes the finalization duration).
    u64 insert[DZ_NR_GROUPS];
    // For correct fractions if lines / DZ_NR_GROUPS has a remainder.
    u64 insert_keys[DZ_NR_GROUPS];
    u64 total_lookup;
    u64 total_teardo;
} DzDuration;


/* Collection of memory measurements.  Since freeing memory might not actually
 * return it (runtime heap manager implementation defined), we don't bother
 * tracking it aside of how the memory grows during inserts.
 *
 * The insert values are snapshots after all inserts or after each group of
 * inserts, to get the numbers for just the data structure, one might need to
 * subtract the last snapshot from before the data structure is being used
 * (`inserts_and_lookups_mmapped_kib`).
 */
typedef struct {
    u64 initial_kib; // before loading any data
    u64 inserts_mmapped_kib; // before any data structure operation was done
    u64 inserts_and_lookups_mmapped_kib; // before any data structure operation was done
    // Final value is when inserts have concluded; if a data structure needs to
    // insert all values before doing some finalization after which only
    // lookups can happen, then the final entry is measured after the
    // finalization has happened.
    u64 insert_kib[DZ_NR_GROUPS];
    // For correct fractions if lines / DZ_NR_GROUPS has a remainder.
    u64 insert_keys[DZ_NR_GROUPS];
} DzMemoryKiB;

static inline double dz_ops_per_sec(u64 ops, u64 ns) {
    return ns ? (double) ops * 1e9 / (double) ns : 0.0;
}
static inline double dz_ns_to_ms(u64 ns) { return (double) ns / 1e6; }

/* Tries to align, but is just best guess; parser (if there is one) has to
 * strip etc / reader pay attention.
 */
static void dz_print_stats(u64 n, const DzDuration *d, const DzMemoryKiB *m)
{
    // time totals
    printf("-- time totals --\n");
    printf("%-10s %14s %16s\n", "phase", "ms", "ops/s");
    printf("%-10s %14.3f %16.1f\n", "insert",
           dz_ns_to_ms(d->total_insert), dz_ops_per_sec(n, d->total_insert));
    printf("%-10s %14.3f %16.1f\n", "lookup",
           dz_ns_to_ms(d->total_lookup), dz_ops_per_sec(n, d->total_lookup));
    printf("%-10s %14.3f %16.1f\n", "teardown",
           dz_ns_to_ms(d->total_teardo), dz_ops_per_sec(n, d->total_teardo));
    printf("%-10s %14.3f %16.1f\n\n", "total",
           dz_ns_to_ms(d->total), dz_ops_per_sec(n, d->total));

    // memory totals
    u64 baseline_kib = m->inserts_and_lookups_mmapped_kib;
    u64 total_kib = m->insert_kib[DZ_NR_GROUPS - 1];
    i64 ds_kib = (i64) total_kib - (i64) baseline_kib;
    double growth = baseline_kib > 0
        ? (double) total_kib / (double) baseline_kib
        : 0.0;
    printf("-- memory totals --\n");
    printf("%-20s %12" PRIu64 " KiB\n", "initial",          m->initial_kib);
    printf("%-20s %12" PRIu64 " KiB\n", "mmapped_insert",   m->inserts_mmapped_kib);
    printf("%-20s %12" PRIu64 " KiB\n", "mmapped_baseline", baseline_kib);
    printf("%-20s %12" PRIu64 " KiB\n", "final_rss",        total_kib);
    printf("%-20s %12" PRIi64 " KiB\n", "index_only",       ds_kib);
    printf("%-20s %12.3f x\n",          "growth_factor",    growth);
    printf("%-20s %12.1f B\n\n",        "bytes_per_key",
           n > 0 ? (double) ds_kib * 1024.0 / (double) n : 0.0);

    // stepwise inserts
    printf("-- stepwise inserts (%d groups) --\n", DZ_NR_GROUPS);
    printf("%3s %11s %9s %10s %14s %7s %10s %9s %9s %7s %13s %13s\n",
           "grp", "keys_sum", "keys_grp", "ms", "ops/s", "%g0", "rss_KiB",
           "idx_KiB", "d_KiB", "B/key", "ops/s/MiB", "ops/s/Mkeys");
    u64 sum_keys = 0;
    double rate0 = 0.0;
    i64 prev_ds_kib = 0;
    for (int i = 0; i < DZ_NR_GROUPS; i++)
    {
        u64 group_keys = d->insert_keys[i];
        sum_keys += group_keys;
        double rate = dz_ops_per_sec(group_keys, d->insert[i]);
        if (i == 0)
            rate0 = rate;
        double percent = rate0 > 0.0 ? rate / rate0 * 100.0 : 0.0;
        total_kib = m->insert_kib[i];
        ds_kib = (i64) total_kib - (i64) baseline_kib;
        i64 ds_growth_in_kib = ds_kib - prev_ds_kib;

        printf("%3d %11" PRIu64 " %9" PRIu64 " %10.3f %14.1f %7.1f "
               "%10" PRIu64 " %9" PRIi64 " %+9" PRIi64 " %7.1f %13.1f %13.1f\n",
               i, sum_keys, group_keys, dz_ns_to_ms(d->insert[i]), rate, percent,
               total_kib, ds_kib, ds_growth_in_kib,
               sum_keys ? (double) ds_kib * 1024.0 / (double) sum_keys : 0.0,
               ds_kib > 0 ? rate / ((double) ds_kib / 1024.0) : 0.0,
               sum_keys ? rate / ((double) sum_keys / 1e6) : 0.0);

        prev_ds_kib = ds_kib;
    }
    printf("\n");
}


/* gets RSS of the process in KiB
 * (resident set size; physical RAM backing the process)
 */
static u64 dz_mem_usage_kb(void)
{
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (f == NULL) dz_fatal("failed to open /proc/self/smaps_rollup\n");

    char line[256];
    u64 rss_kb = UINT64_MAX;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, "Rss:", 4) == 0)
        {
            if (sscanf(line + 4, "%" SCNu64, &rss_kb) != 1)
                dz_fatal(
                    "failed to read the RSS (resident set size) from /proc/self/smaps_rollup\n");
        }
    }
    fclose(f);
    if (rss_kb == UINT64_MAX)
        dz_fatal(
            "did not find resident set size entry in /proc/self/smaps_rollup\n");
    return rss_kb;
}


// huge pages not part of RSS
static u64 dz_mem_hugetlb_kb(void)
{
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (f == NULL) dz_fatal("failed to open /proc/self/smaps_rollup\n");

    char line[256];
    u64 hugetlb_kb = 0;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, "Private_Hugetlb:", 16) == 0)
            if (sscanf(line + 16, "%" SCNu64, &hugetlb_kb) != 1)
                dz_fatal(
                    "failed to read Private_Hugetlb from /proc/self/smaps_rollup\n");
    }
    fclose(f);
    return hugetlb_kb;
}


static const void *map_file(const char *path, size_t *out_filesize)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) dz_fatal("open %s: %s\n", path, strerror(errno));

    struct stat st = {0};
    if (fstat(fd, &st) != 0) dz_fatal("fstat %s: %s\n", path, strerror(errno));
    if (st.st_size == 0) dz_fatal("%s is empty\n", path);

    void *p = NULL;
    p = mmap(NULL, (size_t) st.st_size, PROT_READ,
             MAP_SHARED | MAP_POPULATE, fd, 0);
    close(fd); // mmap keeps file open regardless of fd
    if (p == MAP_FAILED) dz_fatal("mmap %s: %s\n", path, strerror(errno));

    *out_filesize = (size_t) st.st_size;
    return p;
}

typedef enum {
    utf8  = 1,
    utf16 = 2,
    utf32 = 4,
} CodeUnitBytes;

static inline void enforce_valid_cub(CodeUnitBytes cub)
{
    if (cub != utf8 && cub != utf16 && cub != utf32)
        dz_fatal(
            "bad code unit bytes selected, options are (1, 2, 4), was: %d\n",
            cub);
}

/* Reminder: branchless from calc (per godbolt the ternary op version is
 * detected and produces the same output, but still):
 *
 * Branchless:
 * from = i - (i != 0);
 *
 * Same in -O2 and -O3:
 * from = i == 0 ? 0 : i - 1;
 */
typedef struct
{
    // Storing this instead of span helps with errors.
    CodeUnitBytes code_unit_bytes;

    // Use any when munmap; data_length needs to be adjusted for code unit
    // width.
    union {
        const void   *data; // for convenience when passing it around
        const u8    *data8;
        const u16  *data16;
        const u32  *data32;
    };
    // nr of code units in file
    size_t data_length;

    // entries in meta array (the endings of the strings)
    const u64 *meta;
    size_t     meta_length; // nr of strings == length of meta array
} StringDataset;

static void load_data(StringDataset *ds, const char *data_path,
                      const char *meta_path, CodeUnitBytes code_unit_bytes)
{
    memset(ds, 0, sizeof(*ds));

    u32 cub = code_unit_bytes; // alias
    enforce_valid_cub(code_unit_bytes);
    ds->code_unit_bytes = code_unit_bytes;

    ds->data = map_file(data_path, &ds->data_length);
    if (!ds->data || !ds->data_length || ds->data_length % cub != 0)
        dz_fatal(
            "dataset could not be loaded or it's length is not a multiple of the code unit bytes\n");
    ds->data_length = ds->data_length / cub;

    size_t meta_bytes;
    ds->meta = (const u64 *) map_file(meta_path, &meta_bytes);
    if (!ds->meta)
        dz_fatal("dataset_meta file could not be loaded\n");
    if (meta_bytes % sizeof(size_t) != 0)
        dz_fatal(
            "meta file %s is erroneous: does not contain multiple of 8 bytes: %zu\n",
            meta_path, meta_bytes);
    ds->meta_length = meta_bytes / sizeof(u64);

    if (ds->meta[ds->meta_length - 1] != ds->data_length)
        dz_fatal(
            "bad nr of bytes in dataset %s: last offset %" PRIu64 " != data size %zu\n",
            data_path, ds->meta[ds->meta_length - 1], ds->data_length);
}

static void close_data(StringDataset *ds)
{
    size_t data_bytes = ds->data_length * ds->code_unit_bytes;
    size_t meta_bytes = ds->meta_length * sizeof(u64);
    if (ds->data) munmap((void *) ds->data, data_bytes);
    if (ds->meta) munmap((void *) ds->meta, meta_bytes);
    memset(ds, 0, sizeof(*ds));
}

/* Intended to only read in ascending order, should be fine to reset and to
 * move but requires manual adjustment of `i` and `from`.
 */
typedef struct
{
    const StringDataset *ds;
    // next unread value in meta
    size_t current_eos;
    // start reading from this offset in code units from data array
    u64 from_code_unit;
} DatasetCursor;

static void cursor_init(DatasetCursor *c, const StringDataset *ds)
{
    c->ds = ds;
    c->current_eos = 0;
    c->from_code_unit = 0;
}

/* Returns true (1) while more strings are available.  Reads according to
 * code unit width, but caller has to interpret it himself
 */
static int cursor_next(DatasetCursor *c,
                       const void **out_str, size_t *out_length)
{
    size_t current_eos = c->current_eos;
    u64 from = c->from_code_unit;
    u64 to = c->ds->meta[current_eos];

    if (current_eos >= c->ds->meta_length)
        return 0;

    if (from >= to || to > c->ds->data_length)
        dz_fatal(
            "bad offset at index %zu: from=%" PRIu64 "to=%" PRIu64 "\n",
            current_eos, from, to);

    u32 cub = c->ds->code_unit_bytes; // alias
    switch (c->ds->code_unit_bytes) {
        case utf8:  *out_str = c->ds->data8  + from; break;
        case utf16: *out_str = c->ds->data16 + from; break;
        case utf32: *out_str = c->ds->data32 + from; break;
    }
    *out_length = (size_t) (to - from);

    c->from_code_unit = to;
    c->current_eos++;
    return 1;
}

static u64 dz_max_key_length(StringDataset ds)
{
    u64 max = 0;

    u64 prev = 0;
    for (size_t i = 0; i < ds.meta_length; i++)
    {
        u64 cur = ds.meta[i];
        if (cur - prev > max)
            max = cur - prev;
        prev = cur;
    }

    return max;
}

// dies on error
static inline void dz_load(const char *pdata, const char *pmeta,
                           CodeUnitBytes cub, StringDataset *out_ds)
{
    if (pdata == NULL) { pdata = "/dev/shm/dataset.bin"; }
    if (pmeta == NULL) { pmeta = "/dev/shm/dataset_meta.bin"; }
    enforce_valid_cub(cub);
    load_data(out_ds, pdata, pmeta, cub);
}

#if defined (TEST_DECODER)

static inline void dz_help(void)
{
    fprintf(stderr,
    "takes up to 4 args:\n"
    "\n"
    "- 1. arg: how many entries to print (default all)\n"
    "- 2. arg: code unit width (8 default, 16, 32)\n"
    "- 3. arg: input file dataset (default /dev/shm/dataset.bin)\n"
    "- 4. arg: input file meta (default /dev/shm/dataset_meta.bin)\n"
    "\n"
    "If it's not utf8 consider piping to a hexdump viewer.\n");
}

int main(int argc, char **argv)
{
    if (argc > 5) { dz_help(); return 1; }
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0)
        {
            dz_help();
            return 0;
        }
    }

    const u64 lines = argc > 1 ? strtoull(argv[1], NULL, 0) : UINT64_MAX;
    const CodeUnitBytes width = argc > 2 ? (CodeUnitBytes) atoi(argv[2]) : utf8;
    enforce_valid_cub(width);
    const char *data_path = (argc > 3) ? argv[3] : "/dev/shm/dataset.bin";
    const char *meta_path = (argc > 4) ? argv[4] : "/dev/shm/dataset_meta.bin";

    StringDataset ds;
    load_data(&ds, data_path, meta_path, width);

    size_t n = 0;
    size_t code_unit_bytes = ds.code_unit_bytes;
    const void *str = 0;
    size_t str_len = 0;
    DatasetCursor cur;
    cursor_init(&cur, &ds);

    void *newline;
    if (code_unit_bytes == utf8) newline = (void *) "\n";
    else if (code_unit_bytes == utf16) newline = (void *) "\n\0"; // litte endian
    else newline = (void *) "\n\0\0\0"; // litte endian

    while (cursor_next(&cur, &str, &str_len)) {
        if (n < lines)
        {
            fwrite(str, code_unit_bytes, str_len, stdout);
            fwrite(newline, code_unit_bytes, 1, stdout);
            n++;
        }
    }
    printf("Decoder: Total records: %zu\n", n);

    close_data(&ds);
    return 0;
}
#endif // if defined(TEST_DECODER)

}
#endif // defined(__cplusplus)
#endif // !defined (DZ_LOAD_H)
