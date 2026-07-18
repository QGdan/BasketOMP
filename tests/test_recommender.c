#include "cooccurrence.h"
#include "csv_loader.h"
#include "recommender.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", message); \
            goto fail; \
        } \
    } while (0)

static int has_duplicates(const RecommendationResult *result, uint32_t user)
{
    for (uint32_t i = 0; i < result->lengths[user]; ++i) {
        for (uint32_t j = i + 1; j < result->lengths[user]; ++j) {
            if (result->product_ids[(size_t)user * result->k + i] ==
                result->product_ids[(size_t)user * result->k + j]) {
                return 1;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc == 2 ? argv[1] : "data/toy";
    int is_toy = strstr(path, "toy") != NULL;
    char error[512] = {0};
    Dataset dataset = {0};
    CooccurResult model = {0};
    CooccurGraph graph = {0};
    RecommendationResult serial = {0};
    RecommendationResult parallel = {0};
    const int thread_counts[] = {1, 2, 4};

    CHECK(dataset_load(path, &dataset, error, sizeof(error)) == 0, error);
    CHECK(build_cooccur_serial(&dataset, &model, error, sizeof(error)) == 0, error);
    CHECK(cooccur_graph_build(&model, 0, &graph, error, sizeof(error)) == 0, error);
    CHECK(recommend_serial(&dataset, &model, &graph, 10, &serial,
                           error, sizeof(error)) == 0, error);

    if (is_toy) {
        size_t user2 = (size_t)2 * serial.k;
        CHECK(serial.lengths[1] == 3, "toy user 1 candidate length");
        CHECK(serial.lengths[2] == 3, "toy user 2 candidate length");
        CHECK(serial.lengths[3] == 0, "toy empty-history user has no recommendations");
        CHECK(serial.product_ids[user2] == 2 &&
              serial.product_ids[user2 + 1] == 1 &&
              serial.product_ids[user2 + 2] == 3,
              "toy user 2 deterministic ranking");
        CHECK(isfinite(serial.scores[user2]), "toy score is finite");
    }
    for (uint32_t user = 0; user < serial.user_count; ++user) {
        CHECK(!has_duplicates(&serial, user), "recommendation contains duplicates");
    }

    for (size_t t = 0; t < sizeof(thread_counts) / sizeof(thread_counts[0]); ++t) {
        for (int schedule = OMP_SCHEDULE_STATIC;
             schedule <= OMP_SCHEDULE_GUIDED; ++schedule) {
            int repetitions = is_toy ? 10 : 1;
            for (int repeat = 0; repeat < repetitions; ++repeat) {
                CHECK(recommend_openmp(&dataset, &model, &graph, 10,
                                       thread_counts[t],
                                       (OmpSchedule)schedule, 16, &parallel,
                                       error, sizeof(error)) == 0, error);
                CHECK(recommendation_results_equal(&serial, &parallel, 1e-12),
                      "serial/OpenMP recommendation mismatch");
                CHECK(recommendation_result_checksum(&serial) ==
                      recommendation_result_checksum(&parallel),
                      "recommendation checksum mismatch");
                recommendation_result_free(&parallel);
            }
        }
    }

    printf("PASS: recommender %s users=%u checksum=%llu\n", path,
           serial.user_count,
           (unsigned long long)recommendation_result_checksum(&serial));
    recommendation_result_free(&parallel);
    recommendation_result_free(&serial);
    cooccur_graph_free(&graph);
    cooccur_result_free(&model);
    dataset_free(&dataset);
    return 0;

fail:
    if (error[0] != '\0') {
        fprintf(stderr, "detail: %s\n", error);
    }
    recommendation_result_free(&parallel);
    recommendation_result_free(&serial);
    cooccur_graph_free(&graph);
    cooccur_result_free(&model);
    dataset_free(&dataset);
    return 1;
}
