#include "cooccurrence.h"
#include "csv_loader.h"
#include "evaluator.h"
#include "recommender.h"

#include <errno.h>
#include <limits.h>
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    MODE_SERIAL = 0,
    MODE_OPENMP = 1
} RunMode;

typedef struct {
    const char *data_path;
    const char *output_path;
    RunMode mode;
    int threads;
    uint32_t k;
    uint32_t max_neighbors;
    uint32_t sample_users;
    OmpSchedule cooccur_schedule;
    int cooccur_chunk;
    OmpSchedule recommend_schedule;
    int recommend_chunk;
} Options;

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--data DIR] [--mode serial|openmp] "
            "[--threads N] [--top-k K] [--max-neighbors N] "
            "[--cooccur-schedule static|dynamic|guided] [--cooccur-chunk N] "
            "[--recommend-schedule static|dynamic|guided] [--recommend-chunk N] "
            "[--samples N] [--output FILE]\n",
            program);
}

static int parse_nonnegative_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_schedule(const char *text, OmpSchedule *schedule)
{
    if (strcmp(text, "static") == 0) {
        *schedule = OMP_SCHEDULE_STATIC;
    } else if (strcmp(text, "dynamic") == 0) {
        *schedule = OMP_SCHEDULE_DYNAMIC;
    } else if (strcmp(text, "guided") == 0) {
        *schedule = OMP_SCHEDULE_GUIDED;
    } else {
        return -1;
    }
    return 0;
}

static const char *schedule_name(OmpSchedule schedule)
{
    return schedule == OMP_SCHEDULE_STATIC ? "static" :
           schedule == OMP_SCHEDULE_DYNAMIC ? "dynamic" : "guided";
}

static int parse_positive_int(const char *text, int *value)
{
    char *end = NULL;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < 1 || parsed > INT_MAX) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int parse_options(int argc, char **argv, Options *options)
{
    options->data_path = "data/toy";
    options->output_path = NULL;
    options->mode = MODE_SERIAL;
    options->threads = omp_get_max_threads();
    options->k = 10;
    options->max_neighbors = 0;
    options->sample_users = 0;
    options->cooccur_schedule = OMP_SCHEDULE_DYNAMIC;
    options->cooccur_chunk = 64;
    options->recommend_schedule = OMP_SCHEDULE_DYNAMIC;
    options->recommend_chunk = 16;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            options->data_path = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "serial") == 0) {
                options->mode = MODE_SERIAL;
            } else if (strcmp(mode, "openmp") == 0) {
                options->mode = MODE_OPENMP;
            } else {
                return -1;
            }
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            if (parse_positive_int(argv[++i], &options->threads) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--top-k") == 0 && i + 1 < argc) {
            int value;
            if (parse_positive_int(argv[++i], &value) != 0) {
                return -1;
            }
            options->k = (uint32_t)value;
        } else if (strcmp(argv[i], "--max-neighbors") == 0 && i + 1 < argc) {
            if (parse_nonnegative_u32(argv[++i], &options->max_neighbors) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            int value;
            if (parse_positive_int(argv[++i], &value) != 0) {
                return -1;
            }
            options->sample_users = (uint32_t)value;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            options->output_path = argv[++i];
        } else if ((strcmp(argv[i], "--schedule") == 0 ||
                    strcmp(argv[i], "--cooccur-schedule") == 0) &&
                   i + 1 < argc) {
            if (parse_schedule(argv[++i], &options->cooccur_schedule) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--cooccur-chunk") == 0 && i + 1 < argc) {
            if (parse_positive_int(argv[++i], &options->cooccur_chunk) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--recommend-schedule") == 0 &&
                   i + 1 < argc) {
            if (parse_schedule(argv[++i], &options->recommend_schedule) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--recommend-chunk") == 0 && i + 1 < argc) {
            if (parse_positive_int(argv[++i], &options->recommend_chunk) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    Options options;
    char error[512] = {0};
    Dataset dataset = {0};
    CooccurResult model = {0};
    CooccurGraph graph = {0};
    RecommendationResult recommendations = {0};
    Metrics metrics = {0};
    double end_to_end_start;
    double load_seconds;
    double graph_seconds;
    double evaluation_seconds;
    int exit_code = 1;

    if (parse_options(argc, argv, &options) != 0) {
        print_usage(argv[0]);
        return 2;
    }
    if (options.output_path != NULL &&
        freopen(options.output_path, "wb", stdout) == NULL) {
        fprintf(stderr, "cannot open output file: %s\n", options.output_path);
        return 2;
    }
    end_to_end_start = omp_get_wtime();
    load_seconds = omp_get_wtime();
    if (dataset_load(options.data_path, &dataset, error, sizeof(error)) != 0) {
        fprintf(stderr, "data load failed: %s\n", error);
        goto cleanup;
    }
    load_seconds = omp_get_wtime() - load_seconds;

    if (options.mode == MODE_SERIAL) {
        if (build_cooccur_serial(&dataset, &model, error, sizeof(error)) != 0) {
            fprintf(stderr, "serial cooccurrence failed: %s\n", error);
            goto cleanup;
        }
    } else if (build_cooccur_openmp(&dataset, options.threads,
                                    options.cooccur_schedule,
                                    options.cooccur_chunk,
                                    &model, error, sizeof(error)) != 0) {
        fprintf(stderr, "OpenMP cooccurrence failed: %s\n", error);
        goto cleanup;
    }

    graph_seconds = omp_get_wtime();
    if ((options.mode == MODE_SERIAL
             ? cooccur_graph_build(&model, options.max_neighbors, &graph,
                                   error, sizeof(error))
             : cooccur_graph_build_openmp(&model, options.max_neighbors,
                                          options.threads, &graph,
                                          error, sizeof(error))) != 0) {
        fprintf(stderr, "cooccurrence graph failed: %s\n", error);
        goto cleanup;
    }
    graph_seconds = omp_get_wtime() - graph_seconds;
#ifndef NDEBUG
    /* 完整结构验证属于 Debug 审效测试，不进入 Release 性能计时。 */
    if (cooccur_graph_validate(&model, &graph, error, sizeof(error)) != 0) {
        fprintf(stderr, "cooccurrence graph validation failed: %s\n", error);
        goto cleanup;
    }
#endif

    if (options.mode == MODE_SERIAL) {
        if (recommend_serial(&dataset, &model, &graph, options.k,
                             &recommendations, error, sizeof(error)) != 0) {
            fprintf(stderr, "serial recommendation failed: %s\n", error);
            goto cleanup;
        }
    } else if (recommend_openmp(&dataset, &model, &graph, options.k,
                                options.threads, options.recommend_schedule,
                                options.recommend_chunk, &recommendations,
                                error, sizeof(error)) != 0) {
        fprintf(stderr, "OpenMP recommendation failed: %s\n", error);
        goto cleanup;
    }

    evaluation_seconds = omp_get_wtime();
    if (evaluate_recommendations(&recommendations, &dataset.truth,
                                 &metrics, error, sizeof(error)) != 0) {
        fprintf(stderr, "evaluation failed: %s\n", error);
        goto cleanup;
    }
    evaluation_seconds = omp_get_wtime() - evaluation_seconds;

    printf("dataset=%s\n", options.data_path);
    printf("mode=%s\n", options.mode == MODE_SERIAL ? "serial" : "openmp");
    if (options.max_neighbors == 0) {
        printf("algorithm_profile=fast-normalization-full\n");
    } else {
        printf("algorithm_profile=fast-normalization-top%u\n",
               options.max_neighbors);
    }
    printf("threads=%d\n", options.mode == MODE_SERIAL ? 1 : options.threads);
    printf("schedule=%s\n", schedule_name(options.cooccur_schedule));
    printf("cooccur_schedule=%s\n", schedule_name(options.cooccur_schedule));
    printf("cooccur_chunk=%d\n", options.cooccur_chunk);
    printf("recommend_schedule=%s\n", schedule_name(options.recommend_schedule));
    printf("recommend_chunk=%d\n", options.recommend_chunk);
    printf("top_k=%u\n", options.k);
    printf("max_neighbors=%u\n", options.max_neighbors);
    printf("popular_fallback=0\n");
    printf("orders=%llu\n", (unsigned long long)dataset.order_count);
    printf("prior_rows=%llu\n", (unsigned long long)dataset.prior_row_count);
    printf("train_rows=%llu\n", (unsigned long long)dataset.train_row_count);
    printf("products=%u\n", dataset.product_count);
    printf("users=%u\n", dataset.history.user_count);
    printf("hardware_threads=%d\n", omp_get_num_procs());
    printf("omp_max_threads=%d\n", omp_get_max_threads());
    printf("prior_baskets=%u\n", dataset.baskets.basket_count);
    printf("unique_pairs=%zu\n", model.pairs.size);
    printf("pair_events=%llu\n", (unsigned long long)model.total_pair_events);
    printf("graph_edge_entries=%llu\n",
           (unsigned long long)graph.edge_entry_count);
    printf("max_degree=%u\n", graph.max_degree);
    printf("active_users=%u\n", recommendations.active_users);
    printf("candidate_shortage_users=%u\n",
           recommendations.candidate_shortage_users);
    printf("empty_candidate_users=%u\n",
           recommendations.empty_candidate_users);
    printf("total_candidates=%llu\n",
           (unsigned long long)recommendations.total_candidates);
    printf("max_candidates=%u\n", recommendations.max_candidates);
    printf("load_ms=%.3f\n", load_seconds * 1000.0);
    printf("cooccur_compute_ms=%.3f\n", model.compute_seconds * 1000.0);
    printf("merge_ms=%.3f\n", model.merge_seconds * 1000.0);
    printf("normalization_ms=%.3f\n", graph.edge_prepare_seconds * 1000.0);
    printf("adjacency_ms=%.3f\n", graph_seconds * 1000.0);
    printf("truncate_ms=%.3f\n", graph.truncate_seconds * 1000.0);
    printf("recommend_ms=%.3f\n", recommendations.compute_seconds * 1000.0);
    printf("evaluate_ms=%.3f\n", evaluation_seconds * 1000.0);
    printf("algorithm_ms=%.3f\n",
           (model.compute_seconds + model.merge_seconds + graph_seconds +
            recommendations.compute_seconds) * 1000.0);
    printf("end_to_end_ms=%.3f\n", (omp_get_wtime() - end_to_end_start) * 1000.0);
    printf("hit_rate_at_%u=%.12f\n", options.k, metrics.hit_rate);
    printf("precision_at_%u=%.12f\n", options.k, metrics.precision);
    printf("recall_at_%u=%.12f\n", options.k, metrics.recall);
    printf("f1_at_%u=%.12f\n", options.k, metrics.f1);
    printf("ndcg_at_%u=%.12f\n", options.k, metrics.ndcg);
    printf("mrr_at_%u=%.12f\n", options.k, metrics.mrr);
    printf("hit_rate=%.12f\n", metrics.hit_rate);
    printf("precision=%.12f\n", metrics.precision);
    printf("recall=%.12f\n", metrics.recall);
    printf("f1=%.12f\n", metrics.f1);
    printf("ndcg=%.12f\n", metrics.ndcg);
    printf("mrr=%.12f\n", metrics.mrr);
    printf("micro_precision=%.12f\n", metrics.micro_precision);
    printf("micro_recall=%.12f\n", metrics.micro_recall);
    printf("cooccur_checksum=%llu\n",
           (unsigned long long)cooccur_result_checksum(&model));
    printf("recommendation_checksum=%llu\n",
           (unsigned long long)recommendation_result_checksum(&recommendations));
    if (options.sample_users > 0) {
        uint32_t printed = 0;
        for (uint32_t user = 0;
             user < recommendations.user_count && printed < options.sample_users;
             ++user) {
            if (recommendations.lengths[user] == 0) {
                continue;
            }
            printf("sample_user_%u=", user);
            for (uint32_t rank = 0; rank < recommendations.lengths[user]; ++rank) {
                printf("%s%u", rank == 0 ? "" : ",",
                       recommendations.product_ids[(size_t)user * options.k + rank]);
            }
            putchar('\n');
            ++printed;
        }
    }
    exit_code = 0;

cleanup:
    recommendation_result_free(&recommendations);
    cooccur_graph_free(&graph);
    cooccur_result_free(&model);
    dataset_free(&dataset);
    return exit_code;
}
