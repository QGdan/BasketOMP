#ifndef BASKET_EVALUATOR_H
#define BASKET_EVALUATOR_H

#include "model.h"
#include "recommender.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t evaluated_users;
    uint64_t hit_users;
    uint64_t truth_items;
    uint64_t hit_items;
    uint64_t predicted_items;
    double hit_rate;
    double precision;
    double recall;
    double f1;
    double ndcg;
    double mrr;
    double micro_precision;
    double micro_recall;
} Metrics;

/*
 * Hit Rate 为至少命中一个商品的用户比例。
 * Precision/Recall/F1/NDCG/MRR 均为逐用户宏平均，
 * 仅统计存在 train 真值的用户；另输出 micro 汇总指标。
 */
int evaluate_recommendations(const RecommendationResult *predictions,
                             const GroundTruth *truth,
                             Metrics *metrics,
                             char *error, size_t error_capacity);

#endif
