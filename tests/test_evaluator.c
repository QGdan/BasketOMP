#include "cooccurrence.h"
#include "csv_loader.h"
#include "evaluator.h"
#include "recommender.h"

#include <math.h>
#include <stdio.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", message); \
            goto fail; \
        } \
    } while (0)

int main(void)
{
    char error[512] = {0};
    Dataset dataset = {0};
    CooccurResult model = {0};
    CooccurGraph graph = {0};
    RecommendationResult recommendations = {0};
    Metrics metrics;

    CHECK(dataset_load("data/toy", &dataset, error, sizeof(error)) == 0, error);
    CHECK(build_cooccur_serial(&dataset, &model, error, sizeof(error)) == 0, error);
    CHECK(cooccur_graph_build(&model, 0, &graph, error, sizeof(error)) == 0, error);
    CHECK(recommend_serial(&dataset, &model, &graph, 10, &recommendations,
                           error, sizeof(error)) == 0, error);
    CHECK(evaluate_recommendations(&recommendations, &dataset.truth,
                                   &metrics, error, sizeof(error)) == 0, error);
    CHECK(metrics.evaluated_users == 3, "toy evaluated users");
    CHECK(metrics.hit_users == 2, "toy hit users");
    CHECK(metrics.truth_items == 4, "toy truth item count");
    CHECK(metrics.hit_items == 2, "toy hit item count");
    CHECK(metrics.predicted_items == 6, "toy predicted item count");
    CHECK(fabs(metrics.hit_rate - 2.0 / 3.0) < 1e-12, "toy Hit Rate@10");
    CHECK(fabs(metrics.precision - 2.0 / 9.0) < 1e-12,
          "toy macro Precision@10");
    CHECK(fabs(metrics.recall - 0.5) < 1e-12, "toy macro Recall@10");
    CHECK(fabs(metrics.f1 - 0.3) < 1e-12, "toy macro F1@10");
    CHECK(fabs(metrics.mrr - 1.0 / 3.0) < 1e-12, "toy MRR@10");
    CHECK(metrics.ndcg > 0.33 && metrics.ndcg < 0.35, "toy NDCG@10");
    CHECK(fabs(metrics.micro_precision - 1.0 / 3.0) < 1e-12,
          "toy micro precision");
    CHECK(fabs(metrics.micro_recall - 0.5) < 1e-12,
          "toy micro recall");

    puts("PASS: evaluator toy assertions");
    recommendation_result_free(&recommendations);
    cooccur_graph_free(&graph);
    cooccur_result_free(&model);
    dataset_free(&dataset);
    return 0;

fail:
    if (error[0] != '\0') {
        fprintf(stderr, "detail: %s\n", error);
    }
    recommendation_result_free(&recommendations);
    cooccur_graph_free(&graph);
    cooccur_result_free(&model);
    dataset_free(&dataset);
    return 1;
}
