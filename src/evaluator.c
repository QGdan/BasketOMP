#include "evaluator.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
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

int evaluate_recommendations(const RecommendationResult *predictions,
                             const GroundTruth *truth,
                             Metrics *metrics,
                             char *error, size_t error_capacity)
{
    double precision_sum = 0.0;
    double recall_sum = 0.0;
    double f1_sum = 0.0;
    double ndcg_sum = 0.0;
    double mrr_sum = 0.0;
    if (predictions == NULL || truth == NULL || metrics == NULL ||
        predictions->user_count != truth->user_count || predictions->k == 0) {
        set_error(error, error_capacity, "prediction and truth dimensions mismatch");
        return -1;
    }
    memset(metrics, 0, sizeof(*metrics));
    for (uint32_t user = 0; user < truth->user_count; ++user) {
        uint64_t truth_begin = truth->offsets[user];
        uint64_t truth_end = truth->offsets[user + 1];
        uint64_t truth_count = truth_end - truth_begin;
        uint64_t user_hits = 0;
        uint32_t prediction_count;
        if (truth_count == 0) {
            continue;
        }
        ++metrics->evaluated_users;
        metrics->truth_items += truth_count;
        prediction_count = predictions->lengths[user];
        metrics->predicted_items += prediction_count;
        for (uint64_t item = truth_begin; item < truth_end; ++item) {
            uint32_t expected = truth->product_ids[item];
            for (uint32_t rank = 0; rank < predictions->lengths[user]; ++rank) {
                if (predictions->product_ids[(size_t)user * predictions->k + rank] ==
                    expected) {
                    ++user_hits;
                    break;
                }
            }
        }
        if (user_hits > 0) {
            ++metrics->hit_users;
        }
        metrics->hit_items += user_hits;
        {
            double user_precision = prediction_count > 0
                ? (double)user_hits / (double)prediction_count : 0.0;
            double user_recall = (double)user_hits / (double)truth_count;
            precision_sum += user_precision;
            recall_sum += user_recall;
            if (user_precision + user_recall > 0.0) {
                f1_sum += 2.0 * user_precision * user_recall /
                          (user_precision + user_recall);
            }
        }
        {
            double dcg = 0.0;
            double ideal_dcg = 0.0;
            uint64_t ideal_hits = truth_count < prediction_count
                ? truth_count : prediction_count;
            int first_hit_seen = 0;
            for (uint32_t rank = 0; rank < prediction_count; ++rank) {
                uint32_t predicted =
                    predictions->product_ids[(size_t)user * predictions->k + rank];
                int relevant = 0;
                for (uint64_t item = truth_begin; item < truth_end; ++item) {
                    if (truth->product_ids[item] == predicted) {
                        relevant = 1;
                        break;
                    }
                }
                if (relevant) {
                    dcg += 1.0 / log2((double)rank + 2.0);
                    if (!first_hit_seen) {
                        mrr_sum += 1.0 / ((double)rank + 1.0);
                        first_hit_seen = 1;
                    }
                }
            }
            for (uint64_t rank = 0; rank < ideal_hits; ++rank) {
                ideal_dcg += 1.0 / log2((double)rank + 2.0);
            }
            if (ideal_dcg > 0.0) {
                ndcg_sum += dcg / ideal_dcg;
            }
        }
    }
    if (metrics->evaluated_users > 0) {
        metrics->hit_rate = (double)metrics->hit_users /
                            (double)metrics->evaluated_users;
        metrics->precision = precision_sum / (double)metrics->evaluated_users;
        metrics->recall = recall_sum / (double)metrics->evaluated_users;
        metrics->f1 = f1_sum / (double)metrics->evaluated_users;
        metrics->ndcg = ndcg_sum / (double)metrics->evaluated_users;
        metrics->mrr = mrr_sum / (double)metrics->evaluated_users;
    }
    if (metrics->predicted_items > 0) {
        metrics->micro_precision = (double)metrics->hit_items /
                                   (double)metrics->predicted_items;
    }
    if (metrics->truth_items > 0) {
        metrics->micro_recall = (double)metrics->hit_items /
                                (double)metrics->truth_items;
    }
    return 0;
}
