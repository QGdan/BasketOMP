#include "cooccurrence.h"
#include "csv_loader.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", message); \
            goto fail; \
        } \
    } while (0)

static int check_pair(const CooccurResult *result, uint32_t a, uint32_t b,
                      uint32_t expected)
{
    uint32_t actual = 0;
    return pair_map_get(&result->pairs, encode_pair(a, b), &actual) &&
           actual == expected;
}

static int graphs_equal(const CooccurGraph *left, const CooccurGraph *right)
{
    if (left->product_count != right->product_count ||
        left->max_neighbors != right->max_neighbors ||
        left->max_degree != right->max_degree ||
        left->edge_entry_count != right->edge_entry_count) {
        return 0;
    }
    if (memcmp(left->offsets, right->offsets,
               ((size_t)left->product_count + 1) * sizeof(*left->offsets)) != 0) {
        return 0;
    }
    return memcmp(left->neighbors, right->neighbors,
                  (size_t)left->edge_entry_count * sizeof(*left->neighbors)) == 0;
}

int main(int argc, char **argv)
{
    const char *path = argc == 2 ? argv[1] : "data/toy";
    char error[512];
    Dataset dataset = {0};
    CooccurResult serial = {0};
    CooccurResult parallel = {0};
    CooccurGraph graph = {0};
    CooccurGraph parallel_graph = {0};
    int is_toy = strstr(path, "toy") != NULL;
    const int thread_counts[] = {1, 2, 4};
    const int merge_bucket_counts[] = {0, 7, 64};

    CHECK(dataset_load(path, &dataset, error, sizeof(error)) == 0, error);
    CHECK(build_cooccur_serial(&dataset, &serial, error, sizeof(error)) == 0, error);

    if (is_toy) {
        CHECK(serial.total_pair_events == 4, "toy total pair events");
        CHECK(serial.pairs.size == 3, "toy unique pair count");
        CHECK(serial.popularity[1] == 2 && serial.popularity[2] == 3 &&
              serial.popularity[3] == 1 && serial.popularity[4] == 0,
              "toy popularity");
        CHECK(check_pair(&serial, 1, 2, 2), "toy pair (1,2)");
        CHECK(check_pair(&serial, 1, 3, 1), "toy pair (1,3)");
        CHECK(check_pair(&serial, 2, 3, 1), "toy pair (2,3)");
    }

    for (size_t t = 0; t < sizeof(thread_counts) / sizeof(thread_counts[0]); ++t) {
        for (int schedule = OMP_SCHEDULE_STATIC;
             schedule <= OMP_SCHEDULE_GUIDED; ++schedule) {
            for (size_t b = 0;
                 b < sizeof(merge_bucket_counts) / sizeof(merge_bucket_counts[0]);
                 ++b) {
                int repetitions = is_toy ? 5 : 1;
                for (int repeat = 0; repeat < repetitions; ++repeat) {
                    CHECK(build_cooccur_openmp(&dataset, thread_counts[t],
                          (OmpSchedule)schedule, 16, merge_bucket_counts[b],
                          &parallel, error, sizeof(error)) == 0, error);
                    CHECK(cooccur_results_equal(&serial, &parallel),
                          "serial/bucket-OpenMP cooccurrence mismatch");
                    CHECK(cooccur_result_checksum(&serial) ==
                          cooccur_result_checksum(&parallel), "checksum mismatch");
                    CHECK(parallel.merge_bucket_count > 0,
                          "parallel merge must report actual bucket count");
                    cooccur_result_free(&parallel);
                }
            }
        }
    }

    CHECK(cooccur_graph_build(&serial, 0, &graph, error, sizeof(error)) == 0, error);
    CHECK(cooccur_graph_validate(&serial, &graph, error, sizeof(error)) == 0, error);
    CHECK(graph.edge_entry_count == serial.pairs.size * 2,
          "graph degree sum is twice unique edges");
    for (size_t t = 0; t < sizeof(thread_counts) / sizeof(thread_counts[0]); ++t) {
        CHECK(cooccur_graph_build_openmp(&serial, 0, thread_counts[t],
                                        &parallel_graph,
                                        error, sizeof(error)) == 0, error);
        CHECK(cooccur_graph_validate(&serial, &parallel_graph,
                                     error, sizeof(error)) == 0, error);
        CHECK(graphs_equal(&graph, &parallel_graph),
              "serial/OpenMP full graph mismatch");
        cooccur_graph_free(&parallel_graph);
    }
    cooccur_graph_free(&graph);

    CHECK(cooccur_graph_build(&serial, is_toy ? 1U : 50U, &graph,
                              error, sizeof(error)) == 0, error);
    CHECK(cooccur_graph_validate(&serial, &graph, error, sizeof(error)) == 0,
          error);
    CHECK(graph.max_degree <= (is_toy ? 1U : 50U),
          "truncated graph degree limit");
    if (is_toy) {
        CHECK(graph.offsets[1 + 1] - graph.offsets[1] == 1,
              "toy top-1 degree for product 1");
        CHECK(graph.neighbors[graph.offsets[1]].product_id == 2,
              "toy top-1 picks strongest neighbor");
        CHECK(graph.neighbors[graph.offsets[3]].product_id == 1,
              "toy equal-weight top-1 uses smaller product id");
    }
    for (size_t t = 0; t < sizeof(thread_counts) / sizeof(thread_counts[0]); ++t) {
        CHECK(cooccur_graph_build_openmp(&serial, is_toy ? 1U : 50U,
                                        thread_counts[t], &parallel_graph,
                                        error, sizeof(error)) == 0, error);
        CHECK(cooccur_graph_validate(&serial, &parallel_graph,
                                     error, sizeof(error)) == 0, error);
        CHECK(graphs_equal(&graph, &parallel_graph),
              "serial/OpenMP truncated graph mismatch");
        cooccur_graph_free(&parallel_graph);
    }

    printf("PASS: cooccurrence %s pairs=%zu events=%llu checksum=%llu\n",
           path, serial.pairs.size,
           (unsigned long long)serial.total_pair_events,
           (unsigned long long)cooccur_result_checksum(&serial));
    cooccur_graph_free(&graph);
    cooccur_graph_free(&parallel_graph);
    cooccur_result_free(&parallel);
    cooccur_result_free(&serial);
    dataset_free(&dataset);
    return 0;

fail:
    if (error[0] != '\0') {
        fprintf(stderr, "detail: %s\n", error);
    }
    cooccur_graph_free(&graph);
    cooccur_graph_free(&parallel_graph);
    cooccur_result_free(&parallel);
    cooccur_result_free(&serial);
    dataset_free(&dataset);
    return 1;
}
