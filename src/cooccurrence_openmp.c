#include "cooccurrence.h"

#include <omp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int build_cooccur_openmp(const Dataset *dataset, int threads,
                         OmpSchedule schedule, int chunk,
                         CooccurResult *result,
                         char *error, size_t error_capacity)
{
    PairHashMap *local_pairs = NULL;
    uint32_t *local_popularity = NULL;
    uint64_t *local_events = NULL;
    int *local_failures = NULL;
    uint32_t product_count;
    double start;

    if (dataset == NULL || result == NULL || threads < 1 || chunk < 1 ||
        schedule < OMP_SCHEDULE_STATIC || schedule > OMP_SCHEDULE_GUIDED) {
        set_error(error, error_capacity, "dataset, result and positive thread count required");
        return -1;
    }
    memset(result, 0, sizeof(*result));
    product_count = dataset->max_product_id + 1;
    local_pairs = calloc((size_t)threads, sizeof(*local_pairs));
    local_popularity = calloc((size_t)threads * product_count,
                              sizeof(*local_popularity));
    local_events = calloc((size_t)threads, sizeof(*local_events));
    local_failures = calloc((size_t)threads, sizeof(*local_failures));
    result->popularity = calloc(product_count, sizeof(*result->popularity));
    result->product_count = product_count;
    if (local_pairs == NULL || local_popularity == NULL || local_events == NULL ||
        local_failures == NULL || result->popularity == NULL ||
        pair_map_init(&result->pairs, 1024) != 0) {
        set_error(error, error_capacity, "out of memory for OpenMP cooccurrence");
        goto fail;
    }
    for (int thread = 0; thread < threads; ++thread) {
        if (pair_map_init(&local_pairs[thread], 1024) != 0) {
            set_error(error, error_capacity, "out of memory for thread-local pair map");
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
        PairHashMap *pairs = &local_pairs[tid];
        uint32_t *popularity = local_popularity + (size_t)tid * product_count;
        uint64_t events = 0;
        int failed = 0;

        /* 购物篮只读；pairs、popularity、events 均属于当前线程。 */
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
            for (uint64_t i = begin; i < end; ++i) {
                for (uint64_t j = i + 1; j < end; ++j) {
                    uint32_t a = dataset->baskets.products[i];
                    uint32_t b = dataset->baskets.products[j];
                    if (a == b) {
                        continue;
                    }
                    if (pair_map_increment(pairs, encode_pair(a, b), 1) != 0) {
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
                      "thread %d failed to update local pair map", thread);
            goto fail;
        }
    }

    start = omp_get_wtime();
    /* 并行区结束后串行归并，基础版不需要锁，且单独记录归并开销。 */
    {
        size_t maximum_entries = 0;
        for (int thread = 0; thread < threads; ++thread) {
            if (SIZE_MAX - maximum_entries < local_pairs[thread].size) {
                set_error(error, error_capacity, "local pair size sum overflow");
                goto fail;
            }
            maximum_entries += local_pairs[thread].size;
        }
        /*
         * 批量归并前一次性预留上界容量。若从很小的全局表逐步扩容，
         * 按局部哈希槽顺序插入会形成长探测链，1 线程归并尤其明显。
         */
        if (pair_map_reserve(&result->pairs, maximum_entries) != 0) {
            set_error(error, error_capacity, "failed to reserve global pair map");
            goto fail;
        }
    }
    for (int thread = 0; thread < threads; ++thread) {
        if (pair_map_merge(&result->pairs, &local_pairs[thread]) != 0) {
            set_error(error, error_capacity, "failed to merge thread-local pair map");
            goto fail;
        }
        result->total_pair_events += local_events[thread];
        for (uint32_t product = 0; product < product_count; ++product) {
            result->popularity[product] +=
                local_popularity[(size_t)thread * product_count + product];
        }
    }
    result->merge_seconds = omp_get_wtime() - start;
    for (int thread = 0; thread < threads; ++thread) {
        pair_map_free(&local_pairs[thread]);
    }
    free(local_pairs);
    free(local_popularity);
    free(local_events);
    free(local_failures);
    return 0;

fail:
    if (local_pairs != NULL) {
        for (int thread = 0; thread < threads; ++thread) {
            pair_map_free(&local_pairs[thread]);
        }
    }
    free(local_pairs);
    free(local_popularity);
    free(local_events);
    free(local_failures);
    cooccur_result_free(result);
    return -1;
}
