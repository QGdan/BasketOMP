#ifndef BASKET_RECOMMENDER_H
#define BASKET_RECOMMENDER_H

#include "cooccurrence.h"
#include "model.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t user_count;
    uint32_t k;
    uint32_t *lengths;
    uint32_t *candidate_counts;
    uint32_t *product_ids;
    double *scores;
    uint64_t total_candidates;
    uint32_t max_candidates;
    uint32_t active_users;
    uint32_t candidate_shortage_users;
    uint32_t empty_candidate_users;
    double compute_seconds;
} RecommendationResult;

int recommend_serial(const Dataset *dataset,
                     const CooccurResult *model,
                     const CooccurGraph *graph,
                     uint32_t k,
                     RecommendationResult *result,
                     char *error, size_t error_capacity);

int recommend_openmp(const Dataset *dataset,
                     const CooccurResult *model,
                     const CooccurGraph *graph,
                     uint32_t k, int threads,
                     OmpSchedule schedule, int chunk,
                     RecommendationResult *result,
                     char *error, size_t error_capacity);

void recommendation_result_free(RecommendationResult *result);
int recommendation_results_equal(const RecommendationResult *left,
                                 const RecommendationResult *right,
                                 double tolerance);
uint64_t recommendation_result_checksum(const RecommendationResult *result);

#endif
