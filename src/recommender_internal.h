#ifndef BASKET_RECOMMENDER_INTERNAL_H
#define BASKET_RECOMMENDER_INTERNAL_H

#include "recommender.h"

/*
 * C 版本用稠密数组加代际标记模拟候选映射。
 * begin_user 只递增 generation，无需为每个用户清空 product_count 个分数。
 */
typedef struct {
    uint32_t product_count;
    uint32_t generation;
    uint32_t candidate_count;
    uint32_t *marks;
    uint32_t *candidates;
    double *scores;
} RecommendationWorkspace;

int recommendation_result_allocate(uint32_t user_count, uint32_t k,
                                   RecommendationResult *result);
int recommendation_workspace_init(uint32_t product_count,
                                  RecommendationWorkspace *workspace);
void recommendation_workspace_free(RecommendationWorkspace *workspace);
void recommendation_workspace_begin_user(RecommendationWorkspace *workspace);
void recommendation_result_finalize_stats(RecommendationResult *result);

void recommend_one_user(uint32_t user_id,
                        const Dataset *dataset,
                        const CooccurResult *model,
                        const CooccurGraph *graph,
                        uint32_t k,
                        RecommendationWorkspace *workspace,
                        RecommendationResult *result);

#endif
