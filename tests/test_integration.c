#include "cooccurrence.h"
#include "csv_loader.h"
#include "recommender.h"

#include <stdio.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", message); \
            goto fail; \
        } \
    } while (0)

int main(int argc, char **argv)
{
    const char *path = argc == 2 ? argv[1] : "data/toy";
    char error[512] = {0};
    Dataset dataset = {0};
    CooccurResult model = {0};
    CooccurGraph full = {0};
    CooccurGraph unlimited = {0};
    CooccurGraph limited = {0};
    RecommendationResult baseline = {0};
    RecommendationResult equivalent = {0};
    RecommendationResult limited_serial = {0};
    RecommendationResult limited_parallel = {0};

    CHECK(dataset_load(path, &dataset, error, sizeof(error)) == 0, error);
    CHECK(build_cooccur_serial(&dataset, &model, error, sizeof(error)) == 0,
          error);
    CHECK(cooccur_graph_build(&model, 0, &full, error, sizeof(error)) == 0,
          error);
    CHECK(cooccur_graph_build(&model, UINT32_MAX, &unlimited,
                              error, sizeof(error)) == 0, error);
    CHECK(recommend_serial(&dataset, &model, &full, 10, &baseline,
                           error, sizeof(error)) == 0, error);
    CHECK(recommend_serial(&dataset, &model, &unlimited, 10, &equivalent,
                           error, sizeof(error)) == 0, error);
    CHECK(recommendation_results_equal(&baseline, &equivalent, 0.0),
          "max-neighbors above max degree must equal full graph");

    CHECK(cooccur_graph_build(&model, 1, &limited, error, sizeof(error)) == 0,
          error);
    CHECK(cooccur_graph_build_openmp(&model, 1, 0, &unlimited,
                                     error, sizeof(error)) != 0,
          "zero graph thread count must fail");
    CHECK(recommend_serial(&dataset, &model, &limited, 10, &limited_serial,
                           error, sizeof(error)) == 0, error);
    CHECK(recommend_openmp(&dataset, &model, &limited, 10, 4,
                           OMP_SCHEDULE_GUIDED, 2, &limited_parallel,
                           error, sizeof(error)) == 0, error);
    CHECK(recommendation_results_equal(&limited_serial, &limited_parallel,
                                       1e-12),
          "limited graph serial/OpenMP mismatch");
    CHECK(build_cooccur_openmp(&dataset, 2, OMP_SCHEDULE_DYNAMIC, 0,
                               &model, error, sizeof(error)) != 0,
          "zero cooccurrence chunk must fail");

    printf("PASS: optimization integration %s full=%llu limited=%llu\n",
           path, (unsigned long long)full.edge_entry_count,
           (unsigned long long)limited.edge_entry_count);
    recommendation_result_free(&limited_parallel);
    recommendation_result_free(&limited_serial);
    recommendation_result_free(&equivalent);
    recommendation_result_free(&baseline);
    cooccur_graph_free(&limited);
    cooccur_graph_free(&unlimited);
    cooccur_graph_free(&full);
    cooccur_result_free(&model);
    dataset_free(&dataset);
    return 0;

fail:
    if (error[0] != '\0') {
        fprintf(stderr, "detail: %s\n", error);
    }
    recommendation_result_free(&limited_parallel);
    recommendation_result_free(&limited_serial);
    recommendation_result_free(&equivalent);
    recommendation_result_free(&baseline);
    cooccur_graph_free(&limited);
    cooccur_graph_free(&unlimited);
    cooccur_graph_free(&full);
    cooccur_result_free(&model);
    dataset_free(&dataset);
    return 1;
}
