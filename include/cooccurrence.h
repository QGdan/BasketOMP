#ifndef BASKET_COOCCURRENCE_H
#define BASKET_COOCCURRENCE_H

#include "model.h"
#include "pair_hash.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OMP_SCHEDULE_STATIC = 0,
    OMP_SCHEDULE_DYNAMIC = 1,
    OMP_SCHEDULE_GUIDED = 2
} OmpSchedule;

typedef struct {
    PairHashMap pairs;
    uint32_t *popularity;
    uint32_t product_count;
    uint64_t total_pair_events;
    double compute_seconds;
    double merge_seconds;
} CooccurResult;

int build_cooccur_serial(const Dataset *dataset, CooccurResult *result,
                         char *error, size_t error_capacity);

int build_cooccur_openmp(const Dataset *dataset, int threads,
                         OmpSchedule schedule, int chunk,
                         CooccurResult *result,
                         char *error, size_t error_capacity);

void cooccur_result_free(CooccurResult *result);

/* 完整比较热度和每个商品对计数，用于正确性质量门。 */
int cooccur_results_equal(const CooccurResult *left,
                          const CooccurResult *right);

uint64_t cooccur_result_checksum(const CooccurResult *result);

int cooccur_graph_build(const CooccurResult *result, uint32_t max_neighbors,
                        CooccurGraph *graph,
                        char *error, size_t error_capacity);

/*
 * OpenMP 邻接图构建：按哈希槽分区统计度数和填充边，并按商品并行排序/
 * Top-N 截断。串行接口保持独立，便于进行公平的串并行性能比较。
 */
int cooccur_graph_build_openmp(const CooccurResult *result,
                               uint32_t max_neighbors, int threads,
                               CooccurGraph *graph,
                               char *error, size_t error_capacity);
int cooccur_graph_validate(const CooccurResult *result,
                           const CooccurGraph *graph,
                           char *error, size_t error_capacity);
void cooccur_graph_free(CooccurGraph *graph);

#endif
