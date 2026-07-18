#include "cooccurrence.h"

#include <omp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MERGE_BUCKETS 4096

static void set_error(char *error, size_t capacity, const char *format, ...)
{
    va_list args;
    if (error == NULL || capacity == 0) {
        return;
    }
    va_start(args, format);
    vsnprintf(error, capacity, format, args);
    va_end(args);
}

/*
 * Auto mode uses about four buckets per thread, rounded to a power of two and
 * clamped to 64..1024. A 48-thread run therefore defaults to 256 buckets.
 */
static size_t resolve_merge_bucket_count(int threads, int requested)
{
    size_t target;
    size_t buckets = 1;
    if (requested > 0) {
        return (size_t)requested;
    }
    target = (size_t)threads * 4;
    if (target < 64) {
        target = 64;
    }
    if (target > 1024) {
        target = 1024;
    }
    while (buckets < target) {
        buckets *= 2;
    }
    return buckets;
}

int build_cooccur_openmp(const Dataset *dataset, int threads,
                         OmpSchedule schedule, int chunk,
                         int merge_buckets, CooccurResult *result,
                         char *error, size_t error_capacity)
{
    PairHashMap *local_pairs = NULL;
    uint32_t *local_popularity = NULL;
    uint64_t *local_events = NULL;
    int *local_failures = NULL;
    size_t *bucket_entries = NULL;
    size_t *bucket_inserted = NULL;
    int *bucket_failures = NULL;
    size_t bucket_count;
    size_t local_map_count;
    uint32_t product_count;
    double start;

    if (dataset == NULL || result == NULL || threads < 1 || chunk < 1 ||
        merge_buckets < 0 || merge_buckets > MAX_MERGE_BUCKETS ||
        schedule < OMP_SCHEDULE_STATIC || schedule > OMP_SCHEDULE_GUIDED) {
        set_error(error, error_capacity,
                  "dataset, result, valid schedule and positive parameters required");
        return -1;
    }
    memset(result, 0, sizeof(*result));
    product_count = dataset->max_product_id + 1;
    bucket_count = resolve_merge_bucket_count(threads, merge_buckets);
    if ((size_t)threads > SIZE_MAX / bucket_count ||
        (size_t)threads > SIZE_MAX / product_count) {
        set_error(error, error_capacity, "OpenMP workspace size overflow");
        return -1;
    }
    local_map_count = (size_t)threads * bucket_count;
    local_pairs = calloc(local_map_count, sizeof(*local_pairs));
    local_popularity = calloc((size_t)threads * product_count,
                              sizeof(*local_popularity));
    local_events = calloc((size_t)threads, sizeof(*local_events));
    local_failures = calloc((size_t)threads, sizeof(*local_failures));
    bucket_entries = calloc(bucket_count, sizeof(*bucket_entries));
    bucket_inserted = calloc(bucket_count, sizeof(*bucket_inserted));
    bucket_failures = calloc(bucket_count, sizeof(*bucket_failures));
    result->popularity = calloc(product_count, sizeof(*result->popularity));
    result->product_count = product_count;
    result->merge_bucket_count = (uint32_t)bucket_count;
    if (local_pairs == NULL || local_popularity == NULL ||
        local_events == NULL || local_failures == NULL ||
        bucket_entries == NULL || bucket_inserted == NULL ||
        bucket_failures == NULL || result->popularity == NULL) {
        set_error(error, error_capacity, "out of memory for OpenMP cooccurrence");
        goto fail;
    }
    /*
     * Each worker owns bucket_count small maps. Empty buckets start at only 16
     * slots; hot buckets grow independently under the normal 0.70 load rule.
     */
    for (size_t index = 0; index < local_map_count; ++index) {
        if (pair_map_init(&local_pairs[index], 16) != 0) {
            set_error(error, error_capacity,
                      "out of memory for thread-local bucket map");
            goto fail;
        }
    }

    omp_set_dynamic(0);
    omp_set_num_threads(threads);
    omp_set_schedule(schedule == OMP_SCHEDULE_STATIC ? omp_sched_static :
                     schedule == OMP_SCHEDULE_DYNAMIC ? omp_sched_dynamic :
                     omp_sched_guided, chunk);

    start = omp_get_wtime();
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        PairHashMap *thread_buckets =
            local_pairs + (size_t)tid * bucket_count;
        uint32_t *popularity = local_popularity + (size_t)tid * product_count;
        uint64_t events = 0;
        int failed = 0;

        /* Baskets are read-only; maps, popularity and events are thread-private. */
        #pragma omp for schedule(runtime)
        for (int64_t basket = 0;
             basket < (int64_t)dataset->baskets.basket_count; ++basket) {
            uint64_t begin;
            uint64_t end;
            if (failed) {
                continue;
            }
            begin = dataset->baskets.offsets[basket];
            end = dataset->baskets.offsets[basket + 1];
            for (uint64_t i = begin; i < end; ++i) {
                ++popularity[dataset->baskets.products[i]];
            }
            for (uint64_t i = begin; i < end && !failed; ++i) {
                for (uint64_t j = i + 1; j < end; ++j) {
                    uint32_t a = dataset->baskets.products[i];
                    uint32_t b = dataset->baskets.products[j];
                    uint64_t key;
                    size_t bucket;
                    if (a == b) {
                        continue;
                    }
                    key = encode_pair(a, b);
                    bucket = pair_map_bucket_index(key, bucket_count);
                    if (pair_map_increment(&thread_buckets[bucket], key, 1) != 0) {
                        failed = 1;
                        break;
                    }
                    ++events;
                }
            }
        }
        local_events[tid] = events;
        local_failures[tid] = failed;
    }
    result->compute_seconds = omp_get_wtime() - start;

    for (int thread = 0; thread < threads; ++thread) {
        if (local_failures[thread]) {
            set_error(error, error_capacity,
                      "thread %d failed to update local bucket map", thread);
            goto fail;
        }
    }

    start = omp_get_wtime();
    /* The sum of local sizes is a safe capacity upper bound for each bucket. */
    for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
        size_t maximum_entries = 0;
        for (int thread = 0; thread < threads; ++thread) {
            size_t local_size =
                local_pairs[(size_t)thread * bucket_count + bucket].size;
            if (SIZE_MAX - maximum_entries < local_size) {
                set_error(error, error_capacity,
                          "local bucket size sum overflow at bucket %zu", bucket);
                goto fail;
            }
            maximum_entries += local_size;
        }
        bucket_entries[bucket] = maximum_entries;
    }
    if (pair_map_init_partitioned(&result->pairs, bucket_entries,
                                  bucket_count) != 0) {
        set_error(error, error_capacity,
                  "failed to allocate partitioned global pair map");
        goto fail;
    }

    /*
     * A bucket is the write-ownership boundary. Each iteration owns one
     * non-overlapping slot range, so no lock, critical section or atomic is used.
     */
    #pragma omp parallel for schedule(dynamic, 1) num_threads(threads)
    for (int64_t bucket = 0; bucket < (int64_t)bucket_count; ++bucket) {
        size_t inserted = 0;
        for (int thread = 0; thread < threads; ++thread) {
            PairHashMap *source =
                &local_pairs[(size_t)thread * bucket_count + (size_t)bucket];
            if (pair_map_merge_into_bucket(&result->pairs, (size_t)bucket,
                                           source, &inserted) != 0) {
                bucket_failures[bucket] = 1;
                break;
            }
        }
        bucket_inserted[bucket] = inserted;
    }
    for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
        if (bucket_failures[bucket] ||
            SIZE_MAX - result->pairs.size < bucket_inserted[bucket]) {
            set_error(error, error_capacity,
                      "failed to merge pair bucket %zu", bucket);
            goto fail;
        }
        result->pairs.size += bucket_inserted[bucket];
    }

    /* Reduce popularity in parallel; each iteration writes one product slot. */
    #pragma omp parallel for schedule(static) num_threads(threads)
    for (int64_t product = 0; product < (int64_t)product_count; ++product) {
        uint32_t total = 0;
        for (int thread = 0; thread < threads; ++thread) {
            total += local_popularity[(size_t)thread * product_count +
                                      (size_t)product];
        }
        result->popularity[product] = total;
    }
    for (int thread = 0; thread < threads; ++thread) {
        result->total_pair_events += local_events[thread];
    }
    result->merge_seconds = omp_get_wtime() - start;

    for (size_t index = 0; index < local_map_count; ++index) {
        pair_map_free(&local_pairs[index]);
    }
    free(local_pairs);
    free(local_popularity);
    free(local_events);
    free(local_failures);
    free(bucket_entries);
    free(bucket_inserted);
    free(bucket_failures);
    return 0;

fail:
    if (local_pairs != NULL) {
        for (size_t index = 0; index < local_map_count; ++index) {
            pair_map_free(&local_pairs[index]);
        }
    }
    free(local_pairs);
    free(local_popularity);
    free(local_events);
    free(local_failures);
    free(bucket_entries);
    free(bucket_inserted);
    free(bucket_failures);
    cooccur_result_free(result);
    return -1;
}
