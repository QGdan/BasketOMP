#include "cooccurrence.h"

#include <math.h>
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

void cooccur_result_free(CooccurResult *result)
{
    if (result == NULL) {
        return;
    }
    pair_map_free(&result->pairs);
    free(result->popularity);
    memset(result, 0, sizeof(*result));
}

int build_cooccur_serial(const Dataset *dataset, CooccurResult *result,
                         char *error, size_t error_capacity)
{
    double start;
    if (dataset == NULL || result == NULL) {
        set_error(error, error_capacity, "dataset and result are required");
        return -1;
    }
    memset(result, 0, sizeof(*result));
    result->product_count = dataset->max_product_id + 1;
    result->popularity = calloc(result->product_count, sizeof(*result->popularity));
    if (result->popularity == NULL || pair_map_init(&result->pairs, 1024) != 0) {
        set_error(error, error_capacity, "out of memory for serial cooccurrence");
        cooccur_result_free(result);
        return -1;
    }

    start = omp_get_wtime();
    for (uint32_t basket = 0; basket < dataset->baskets.basket_count; ++basket) {
        uint64_t begin = dataset->baskets.offsets[basket];
        uint64_t end = dataset->baskets.offsets[basket + 1];
        for (uint64_t i = begin; i < end; ++i) {
            ++result->popularity[dataset->baskets.products[i]];
        }
        /* 一个长度为 m 的购物篮枚举 m(m-1)/2 个无序位置对。 */
        for (uint64_t i = begin; i < end; ++i) {
            for (uint64_t j = i + 1; j < end; ++j) {
                uint32_t a = dataset->baskets.products[i];
                uint32_t b = dataset->baskets.products[j];
                if (a == b) {
                    continue;
                }
                if (pair_map_increment(&result->pairs, encode_pair(a, b), 1) != 0) {
                    set_error(error, error_capacity,
                              "serial pair map allocation or count overflow failed");
                    cooccur_result_free(result);
                    return -1;
                }
                ++result->total_pair_events;
            }
        }
    }
    result->compute_seconds = omp_get_wtime() - start;
    return 0;
}

typedef struct {
    const PairHashMap *right;
    int equal;
} PairCompareContext;

static int compare_pair_entry(uint64_t key, uint32_t count, void *opaque)
{
    PairCompareContext *context = opaque;
    uint32_t right_count;
    if (!pair_map_get(context->right, key, &right_count) || right_count != count) {
        context->equal = 0;
        return 1;
    }
    return 0;
}

int cooccur_results_equal(const CooccurResult *left,
                          const CooccurResult *right)
{
    if (left == NULL || right == NULL ||
        left->product_count != right->product_count ||
        left->total_pair_events != right->total_pair_events ||
        left->pairs.size != right->pairs.size) {
        return 0;
    }
    for (uint32_t product = 0; product < left->product_count; ++product) {
        if (left->popularity[product] != right->popularity[product]) {
            return 0;
        }
    }
    {
        PairCompareContext context = {&right->pairs, 1};
        pair_map_foreach(&left->pairs, compare_pair_entry, &context);
        return context.equal;
    }
}

uint64_t cooccur_result_checksum(const CooccurResult *result)
{
    uint64_t checksum = pair_map_checksum(&result->pairs) ^ result->total_pair_events;
    for (uint32_t product = 0; product < result->product_count; ++product) {
        checksum ^= ((uint64_t)result->popularity[product] + UINT64_C(0x9e3779b9)) *
                    ((uint64_t)product + UINT64_C(0x85ebca6b));
    }
    return checksum;
}

static int compare_neighbors(const void *left, const void *right)
{
    const CooccurNeighbor *a = left;
    const CooccurNeighbor *b = right;
    if (a->product_id != b->product_id) {
        return a->product_id < b->product_id ? -1 : 1;
    }
    if (a->weight != b->weight) {
        return a->weight < b->weight ? -1 : 1;
    }
    return 0;
}

static int compare_neighbors_by_weight(const void *left, const void *right)
{
    const CooccurNeighbor *a = left;
    const CooccurNeighbor *b = right;
    if (a->weight != b->weight) {
        return a->weight > b->weight ? -1 : 1;
    }
    if (a->product_id != b->product_id) {
        return a->product_id < b->product_id ? -1 : 1;
    }
    return 0;
}

void cooccur_graph_free(CooccurGraph *graph)
{
    if (graph == NULL) {
        return;
    }
    free(graph->offsets);
    free(graph->neighbors);
    memset(graph, 0, sizeof(*graph));
}

typedef struct {
    const CooccurResult *result;
    uint64_t *degrees;
    int failed;
} DegreeContext;

static int count_graph_degrees(uint64_t key, uint32_t count, void *opaque)
{
    DegreeContext *context = opaque;
    uint32_t a;
    uint32_t b;
    (void)count;
    decode_pair(key, &a, &b);
    if (a >= context->result->product_count ||
        b >= context->result->product_count || a == b) {
        context->failed = 1;
        return 1;
    }
    ++context->degrees[a];
    ++context->degrees[b];
    return 0;
}

typedef struct {
    const CooccurResult *result;
    CooccurGraph *graph;
    uint64_t *cursor;
} GraphFillContext;

static int fill_graph_neighbors(uint64_t key, uint32_t weight, void *opaque)
{
    GraphFillContext *context = opaque;
    uint32_t a;
    uint32_t b;
    double denominator;
    decode_pair(key, &a, &b);
    denominator = sqrt((double)context->result->popularity[a] *
                       (double)context->result->popularity[b]);
    context->graph->neighbors[context->cursor[a]++] =
        (CooccurNeighbor){b, weight, denominator};
    context->graph->neighbors[context->cursor[b]++] =
        (CooccurNeighbor){a, weight, denominator};
    return 0;
}

int cooccur_graph_build(const CooccurResult *result, uint32_t max_neighbors,
                        CooccurGraph *graph,
                        char *error, size_t error_capacity)
{
    uint64_t *degrees = NULL;
    uint64_t *cursor = NULL;
    uint64_t *limited_offsets = NULL;
    if (result == NULL || graph == NULL) {
        set_error(error, error_capacity, "cooccurrence result and graph are required");
        return -1;
    }
    memset(graph, 0, sizeof(*graph));
    graph->product_count = result->product_count;
    graph->max_neighbors = max_neighbors;
    degrees = calloc(result->product_count, sizeof(*degrees));
    if (degrees == NULL) {
        set_error(error, error_capacity, "out of memory for graph degrees");
        return -1;
    }
    {
        DegreeContext context = {result, degrees, 0};
        pair_map_foreach(&result->pairs, count_graph_degrees, &context);
        if (context.failed) {
            free(degrees);
            set_error(error, error_capacity, "invalid pair key while building graph");
            return -1;
        }
    }
    graph->offsets = calloc((size_t)result->product_count + 1,
                            sizeof(*graph->offsets));
    if (graph->offsets == NULL) {
        free(degrees);
        set_error(error, error_capacity, "out of memory for graph offsets");
        return -1;
    }
    for (uint32_t product = 0; product < result->product_count; ++product) {
        graph->offsets[product + 1] = graph->offsets[product] + degrees[product];
    }
    graph->edge_entry_count = graph->offsets[result->product_count];
    graph->neighbors = malloc((size_t)graph->edge_entry_count * sizeof(*graph->neighbors));
    cursor = malloc((size_t)result->product_count * sizeof(*cursor));
    if ((graph->edge_entry_count > 0 && graph->neighbors == NULL) || cursor == NULL) {
        free(degrees);
        free(cursor);
        cooccur_graph_free(graph);
        set_error(error, error_capacity, "out of memory for graph neighbors");
        return -1;
    }
    memcpy(cursor, graph->offsets, (size_t)result->product_count * sizeof(*cursor));
    {
        double prepare_start = omp_get_wtime();
        GraphFillContext context = {result, graph, cursor};
        pair_map_foreach(&result->pairs, fill_graph_neighbors, &context);
        graph->edge_prepare_seconds = omp_get_wtime() - prepare_start;
    }
    if (max_neighbors > 0) {
        uint64_t write_cursor = 0;
        double truncate_start = omp_get_wtime();
        limited_offsets = calloc((size_t)result->product_count + 1,
                                 sizeof(*limited_offsets));
        if (limited_offsets == NULL) {
            free(degrees);
            free(cursor);
            cooccur_graph_free(graph);
            set_error(error, error_capacity,
                      "out of memory for truncated graph offsets");
            return -1;
        }
        for (uint32_t product = 0; product < result->product_count; ++product) {
            uint64_t begin = graph->offsets[product];
            uint64_t count = graph->offsets[product + 1] - begin;
            uint64_t keep = count < max_neighbors ? count : max_neighbors;
            limited_offsets[product] = write_cursor;
            if (count > 1) {
                qsort(graph->neighbors + begin, (size_t)count,
                      sizeof(*graph->neighbors), compare_neighbors_by_weight);
            }
            if (keep > 0 && write_cursor != begin) {
                memmove(graph->neighbors + write_cursor,
                        graph->neighbors + begin,
                        (size_t)keep * sizeof(*graph->neighbors));
            }
            if (keep > 1) {
                qsort(graph->neighbors + write_cursor, (size_t)keep,
                      sizeof(*graph->neighbors), compare_neighbors);
            }
            write_cursor += keep;
            if (keep > graph->max_degree) {
                graph->max_degree = (uint32_t)keep;
            }
        }
        limited_offsets[result->product_count] = write_cursor;
        free(graph->offsets);
        graph->offsets = limited_offsets;
        graph->edge_entry_count = write_cursor;
        graph->truncate_seconds = omp_get_wtime() - truncate_start;
    } else {
        for (uint32_t product = 0; product < result->product_count; ++product) {
            uint64_t begin = graph->offsets[product];
            uint64_t count = graph->offsets[product + 1] - begin;
            if (count > 1) {
                qsort(graph->neighbors + begin, (size_t)count,
                      sizeof(*graph->neighbors), compare_neighbors);
            }
            if (count > graph->max_degree) {
                graph->max_degree = (uint32_t)count;
            }
        }
    }
    free(degrees);
    free(cursor);
    return 0;
}

/* 把哈希槽均匀切成固定分区，分区与实际执行它的 OpenMP 线程无关。 */
static void graph_slot_range(size_t capacity, int worker, int worker_count,
                             size_t *begin, size_t *end)
{
    size_t base = capacity / (size_t)worker_count;
    size_t extra = capacity % (size_t)worker_count;
    size_t before = (size_t)worker < extra ? (size_t)worker : extra;
    *begin = (size_t)worker * base + before;
    *end = *begin + base + ((size_t)worker < extra ? 1U : 0U);
}

int cooccur_graph_build_openmp(const CooccurResult *result,
                               uint32_t max_neighbors, int threads,
                               CooccurGraph *graph,
                               char *error, size_t error_capacity)
{
    uint64_t *local_degrees = NULL;
    uint64_t *thread_cursors = NULL;
    unsigned char *invalid_partition = NULL;
    uint64_t *limited_offsets = NULL;
    CooccurNeighbor *limited_neighbors = NULL;
    size_t local_entry_count;
    uint32_t max_degree = 0;

    if (result == NULL || graph == NULL || threads < 1) {
        set_error(error, error_capacity,
                  "cooccurrence result, graph and positive threads are required");
        return -1;
    }
    memset(graph, 0, sizeof(*graph));
    graph->product_count = result->product_count;
    graph->max_neighbors = max_neighbors;

    if ((size_t)threads > SIZE_MAX / result->product_count) {
        set_error(error, error_capacity, "parallel graph workspace size overflow");
        return -1;
    }
    local_entry_count = (size_t)threads * result->product_count;
    local_degrees = calloc(local_entry_count, sizeof(*local_degrees));
    thread_cursors = malloc(local_entry_count * sizeof(*thread_cursors));
    invalid_partition = calloc((size_t)threads, sizeof(*invalid_partition));
    graph->offsets = calloc((size_t)result->product_count + 1,
                            sizeof(*graph->offsets));
    if (local_degrees == NULL || thread_cursors == NULL ||
        invalid_partition == NULL || graph->offsets == NULL) {
        set_error(error, error_capacity,
                  "out of memory for parallel graph workspace");
        goto fail;
    }

    /*
     * 每个逻辑分区只写自己的度数行，不使用 atomic。固定槽位分区也保证
     * 后续填充时每个分区拥有预先计算好的独立写入区间。
     */
    #pragma omp parallel for num_threads(threads) schedule(static)
    for (int worker = 0; worker < threads; ++worker) {
        size_t begin;
        size_t end;
        uint64_t *degrees = local_degrees +
                            (size_t)worker * result->product_count;
        graph_slot_range(result->pairs.capacity, worker, threads, &begin, &end);
        for (size_t slot = begin; slot < end; ++slot) {
            const PairEntry *entry = &result->pairs.entries[slot];
            uint32_t a;
            uint32_t b;
            if (!entry->used) {
                continue;
            }
            decode_pair(entry->key, &a, &b);
            if (a >= result->product_count || b >= result->product_count ||
                a == b) {
                invalid_partition[worker] = 1;
                continue;
            }
            ++degrees[a];
            ++degrees[b];
        }
    }
    for (int worker = 0; worker < threads; ++worker) {
        if (invalid_partition[worker]) {
            set_error(error, error_capacity,
                      "invalid pair key while building parallel graph");
            goto fail;
        }
    }

    /* 商品数约五万，按商品归并线程局部度数，写入互不冲突。 */
    #pragma omp parallel for num_threads(threads) schedule(static)
    for (uint32_t product = 0; product < result->product_count; ++product) {
        uint64_t total = 0;
        for (int worker = 0; worker < threads; ++worker) {
            total += local_degrees[(size_t)worker * result->product_count +
                                   product];
        }
        graph->offsets[product + 1] = total;
    }
    for (uint32_t product = 0; product < result->product_count; ++product) {
        graph->offsets[product + 1] += graph->offsets[product];
    }
    graph->edge_entry_count = graph->offsets[result->product_count];
    if (graph->edge_entry_count > SIZE_MAX / sizeof(*graph->neighbors)) {
        set_error(error, error_capacity, "parallel graph edge count overflow");
        goto fail;
    }
    graph->neighbors = malloc((size_t)graph->edge_entry_count *
                              sizeof(*graph->neighbors));
    if (graph->edge_entry_count > 0 && graph->neighbors == NULL) {
        set_error(error, error_capacity,
                  "out of memory for parallel graph neighbors");
        goto fail;
    }

    /* 为每个“分区 × 商品”分配不重叠的连续写入区间。 */
    #pragma omp parallel for num_threads(threads) schedule(static)
    for (uint32_t product = 0; product < result->product_count; ++product) {
        uint64_t cursor = graph->offsets[product];
        for (int worker = 0; worker < threads; ++worker) {
            size_t index = (size_t)worker * result->product_count + product;
            thread_cursors[index] = cursor;
            cursor += local_degrees[index];
        }
    }

    {
        double prepare_start = omp_get_wtime();
        #pragma omp parallel for num_threads(threads) schedule(static)
        for (int worker = 0; worker < threads; ++worker) {
            size_t begin;
            size_t end;
            uint64_t *cursors = thread_cursors +
                                (size_t)worker * result->product_count;
            graph_slot_range(result->pairs.capacity, worker, threads,
                             &begin, &end);
            for (size_t slot = begin; slot < end; ++slot) {
                const PairEntry *entry = &result->pairs.entries[slot];
                uint32_t a;
                uint32_t b;
                double denominator;
                if (!entry->used) {
                    continue;
                }
                decode_pair(entry->key, &a, &b);
                denominator = sqrt((double)result->popularity[a] *
                                   (double)result->popularity[b]);
                graph->neighbors[cursors[a]++] =
                    (CooccurNeighbor){b, entry->count, denominator};
                graph->neighbors[cursors[b]++] =
                    (CooccurNeighbor){a, entry->count, denominator};
            }
        }
        graph->edge_prepare_seconds = omp_get_wtime() - prepare_start;
    }

    if (max_neighbors > 0) {
        double truncate_start = omp_get_wtime();
        limited_offsets = calloc((size_t)result->product_count + 1,
                                 sizeof(*limited_offsets));
        if (limited_offsets == NULL) {
            set_error(error, error_capacity,
                      "out of memory for parallel truncated offsets");
            goto fail;
        }
        for (uint32_t product = 0; product < result->product_count; ++product) {
            uint64_t count = graph->offsets[product + 1] -
                             graph->offsets[product];
            uint64_t keep = count < max_neighbors ? count : max_neighbors;
            limited_offsets[product + 1] = limited_offsets[product] + keep;
        }
        if (limited_offsets[result->product_count] >
            SIZE_MAX / sizeof(*limited_neighbors)) {
            set_error(error, error_capacity,
                      "parallel truncated graph size overflow");
            goto fail;
        }
        limited_neighbors = malloc(
            (size_t)limited_offsets[result->product_count] *
            sizeof(*limited_neighbors));
        if (limited_offsets[result->product_count] > 0 &&
            limited_neighbors == NULL) {
            set_error(error, error_capacity,
                      "out of memory for parallel truncated neighbors");
            goto fail;
        }

        /* 每个商品的排序和 Top-N 复制相互独立，动态调度平衡热门商品。 */
        #pragma omp parallel for num_threads(threads) schedule(dynamic, 32) \
                                 reduction(max:max_degree)
        for (uint32_t product = 0; product < result->product_count; ++product) {
            uint64_t begin = graph->offsets[product];
            uint64_t count = graph->offsets[product + 1] - begin;
            uint64_t destination = limited_offsets[product];
            uint64_t keep = limited_offsets[product + 1] - destination;
            if (count > 1) {
                qsort(graph->neighbors + begin, (size_t)count,
                      sizeof(*graph->neighbors), compare_neighbors_by_weight);
            }
            if (keep > 0) {
                memcpy(limited_neighbors + destination,
                       graph->neighbors + begin,
                       (size_t)keep * sizeof(*limited_neighbors));
            }
            if (keep > 1) {
                qsort(limited_neighbors + destination, (size_t)keep,
                      sizeof(*limited_neighbors), compare_neighbors);
            }
            if (keep > max_degree) {
                max_degree = (uint32_t)keep;
            }
        }
        free(graph->offsets);
        free(graph->neighbors);
        graph->offsets = limited_offsets;
        graph->neighbors = limited_neighbors;
        graph->edge_entry_count = graph->offsets[result->product_count];
        graph->max_degree = max_degree;
        graph->truncate_seconds = omp_get_wtime() - truncate_start;
        limited_offsets = NULL;
        limited_neighbors = NULL;
    } else {
        #pragma omp parallel for num_threads(threads) schedule(dynamic, 32) \
                                 reduction(max:max_degree)
        for (uint32_t product = 0; product < result->product_count; ++product) {
            uint64_t begin = graph->offsets[product];
            uint64_t count = graph->offsets[product + 1] - begin;
            if (count > 1) {
                qsort(graph->neighbors + begin, (size_t)count,
                      sizeof(*graph->neighbors), compare_neighbors);
            }
            if (count > max_degree) {
                max_degree = (uint32_t)count;
            }
        }
        graph->max_degree = max_degree;
    }

    free(local_degrees);
    free(thread_cursors);
    free(invalid_partition);
    return 0;

fail:
    free(local_degrees);
    free(thread_cursors);
    free(invalid_partition);
    free(limited_offsets);
    free(limited_neighbors);
    cooccur_graph_free(graph);
    return -1;
}

int cooccur_graph_validate(const CooccurResult *result,
                           const CooccurGraph *graph,
                           char *error, size_t error_capacity)
{
    if (result == NULL || graph == NULL ||
        graph->product_count != result->product_count) {
        set_error(error, error_capacity, "graph dimensions mismatch");
        return -1;
    }
    if (graph->max_neighbors == 0 &&
        graph->edge_entry_count != (uint64_t)result->pairs.size * 2) {
        set_error(error, error_capacity, "graph degree sum mismatch");
        return -1;
    }
    for (uint32_t product = 0; product < graph->product_count; ++product) {
        if (graph->offsets[product] > graph->offsets[product + 1]) {
            set_error(error, error_capacity, "graph offsets not monotonic at %u", product);
            return -1;
        }
        if (graph->max_neighbors > 0 &&
            graph->offsets[product + 1] - graph->offsets[product] >
                graph->max_neighbors) {
            set_error(error, error_capacity,
                      "graph degree exceeds max_neighbors at %u", product);
            return -1;
        }
        for (uint64_t i = graph->offsets[product];
             i < graph->offsets[product + 1]; ++i) {
            uint32_t neighbor = graph->neighbors[i].product_id;
            uint32_t expected_weight;
            double expected_denominator;
            if (neighbor >= graph->product_count || neighbor == product ||
                !pair_map_get(&result->pairs, encode_pair(product, neighbor),
                              &expected_weight) ||
                expected_weight != graph->neighbors[i].weight) {
                set_error(error, error_capacity, "invalid graph edge %u -> %u",
                          product, neighbor);
                return -1;
            }
            expected_denominator = sqrt((double)result->popularity[product] *
                                        (double)result->popularity[neighbor]);
            if (graph->neighbors[i].normalization_denominator !=
                expected_denominator) {
                set_error(error, error_capacity,
                          "invalid graph normalization %u -> %u",
                          product, neighbor);
                return -1;
            }
            if (i > graph->offsets[product] &&
                graph->neighbors[i - 1].product_id >= neighbor) {
                set_error(error, error_capacity, "neighbors not strictly sorted for %u",
                          product);
                return -1;
            }
        }
    }
    return 0;
}
