#include "recommender.h"
#include "recommender_internal.h"

#include <math.h>
#include <omp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

int recommendation_result_allocate(uint32_t user_count, uint32_t k,
                                   RecommendationResult *result)
{
    size_t slot_count;
    if (result == NULL || k == 0 ||
        user_count > SIZE_MAX / k) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    slot_count = (size_t)user_count * k;
    result->lengths = calloc(user_count, sizeof(*result->lengths));
    result->candidate_counts = calloc(user_count, sizeof(*result->candidate_counts));
    result->product_ids = calloc(slot_count, sizeof(*result->product_ids));
    result->scores = calloc(slot_count, sizeof(*result->scores));
    if ((user_count > 0 &&
         (result->lengths == NULL || result->candidate_counts == NULL)) ||
        (slot_count > 0 && (result->product_ids == NULL || result->scores == NULL))) {
        recommendation_result_free(result);
        return -1;
    }
    result->user_count = user_count;
    result->k = k;
    return 0;
}

void recommendation_result_free(RecommendationResult *result)
{
    if (result == NULL) {
        return;
    }
    free(result->lengths);
    free(result->candidate_counts);
    free(result->product_ids);
    free(result->scores);
    memset(result, 0, sizeof(*result));
}

void recommendation_result_finalize_stats(RecommendationResult *result)
{
    result->total_candidates = 0;
    result->max_candidates = 0;
    result->active_users = 0;
    result->candidate_shortage_users = 0;
    result->empty_candidate_users = 0;
    for (uint32_t user = 0; user < result->user_count; ++user) {
        uint32_t count = result->candidate_counts[user];
        result->total_candidates += count;
        if (count > result->max_candidates) {
            result->max_candidates = count;
        }
        if (count == 0) {
            ++result->empty_candidate_users;
        } else {
            ++result->active_users;
            if (count < result->k) {
                ++result->candidate_shortage_users;
            }
        }
    }
}

int recommendation_workspace_init(uint32_t product_count,
                                  RecommendationWorkspace *workspace)
{
    if (workspace == NULL) {
        return -1;
    }
    memset(workspace, 0, sizeof(*workspace));
    workspace->marks = calloc(product_count, sizeof(*workspace->marks));
    workspace->candidates = malloc((size_t)product_count *
                                   sizeof(*workspace->candidates));
    workspace->scores = malloc((size_t)product_count * sizeof(*workspace->scores));
    if (workspace->marks == NULL || workspace->candidates == NULL ||
        workspace->scores == NULL) {
        recommendation_workspace_free(workspace);
        return -1;
    }
    workspace->product_count = product_count;
    return 0;
}

void recommendation_workspace_free(RecommendationWorkspace *workspace)
{
    if (workspace == NULL) {
        return;
    }
    free(workspace->marks);
    free(workspace->candidates);
    free(workspace->scores);
    memset(workspace, 0, sizeof(*workspace));
}

void recommendation_workspace_begin_user(RecommendationWorkspace *workspace)
{
    ++workspace->generation;
    if (workspace->generation == 0) {
        memset(workspace->marks, 0,
               (size_t)workspace->product_count * sizeof(*workspace->marks));
        workspace->generation = 1;
    }
    workspace->candidate_count = 0;
}

static void add_candidate_score(RecommendationWorkspace *workspace,
                                uint32_t product_id, double delta)
{
    if (workspace->marks[product_id] != workspace->generation) {
        workspace->marks[product_id] = workspace->generation;
        workspace->scores[product_id] = 0.0;
        workspace->candidates[workspace->candidate_count++] = product_id;
    }
    workspace->scores[product_id] += delta;
}

static int ranking_is_better(double left_score, uint32_t left_product,
                             double right_score, uint32_t right_product)
{
    return left_score > right_score ||
           (left_score == right_score && left_product < right_product);
}

static void insert_top_k(uint32_t product_id, double score, uint32_t k,
                         uint32_t *length, uint32_t *products, double *scores)
{
    uint32_t position;
    if (*length < k) {
        position = (*length)++;
    } else {
        position = k - 1;
        if (!ranking_is_better(score, product_id, scores[position], products[position])) {
            return;
        }
    }
    products[position] = product_id;
    scores[position] = score;
    while (position > 0 &&
           ranking_is_better(scores[position], products[position],
                             scores[position - 1], products[position - 1])) {
        uint32_t product_temp = products[position - 1];
        double score_temp = scores[position - 1];
        products[position - 1] = products[position];
        scores[position - 1] = scores[position];
        products[position] = product_temp;
        scores[position] = score_temp;
        --position;
    }
}

void recommend_one_user(uint32_t user_id,
                        const Dataset *dataset,
                        const CooccurResult *model,
                        const CooccurGraph *graph,
                        uint32_t k,
                        RecommendationWorkspace *workspace,
                        RecommendationResult *result)
{
    uint64_t history_begin = dataset->history.offsets[user_id];
    uint64_t history_end = dataset->history.offsets[user_id + 1];
    uint32_t *output_products = result->product_ids + (size_t)user_id * k;
    double *output_scores = result->scores + (size_t)user_id * k;
    uint32_t length = 0;

    recommendation_workspace_begin_user(workspace);
    for (uint64_t h = history_begin; h < history_end; ++h) {
        uint32_t source = dataset->history.product_ids[h];
        double frequency = dataset->history.frequencies[h];
        add_candidate_score(workspace, source, frequency);

        for (uint64_t edge = graph->offsets[source];
             edge < graph->offsets[source + 1]; ++edge) {
            uint32_t candidate = graph->neighbors[edge].product_id;
            double denominator =
                graph->neighbors[edge].normalization_denominator;
            if (denominator > 0.0) {
                double co_score = frequency * graph->neighbors[edge].weight /
                                  denominator;
                add_candidate_score(workspace, candidate, 0.8 * co_score);
            }
        }
    }

    for (uint32_t i = 0; i < workspace->candidate_count; ++i) {
        uint32_t product = workspace->candidates[i];
        double score = workspace->scores[product] +
                       0.2 * log1p((double)model->popularity[product]);
        insert_top_k(product, score, k, &length, output_products, output_scores);
    }
    result->lengths[user_id] = length;
    result->candidate_counts[user_id] = workspace->candidate_count;
}

int recommend_serial(const Dataset *dataset,
                     const CooccurResult *model,
                     const CooccurGraph *graph,
                     uint32_t k,
                     RecommendationResult *result,
                     char *error, size_t error_capacity)
{
    RecommendationWorkspace workspace;
    double start;
    if (dataset == NULL || model == NULL || graph == NULL || result == NULL || k == 0) {
        set_error(error, error_capacity, "recommendation inputs and positive K are required");
        return -1;
    }
    if (model->product_count != graph->product_count ||
        recommendation_result_allocate(dataset->history.user_count, k, result) != 0 ||
        recommendation_workspace_init(model->product_count, &workspace) != 0) {
        set_error(error, error_capacity, "out of memory or incompatible recommendation model");
        recommendation_result_free(result);
        return -1;
    }
    start = omp_get_wtime();
    for (uint32_t user = 0; user < dataset->history.user_count; ++user) {
        recommend_one_user(user, dataset, model, graph, k, &workspace, result);
    }
    result->compute_seconds = omp_get_wtime() - start;
    recommendation_result_finalize_stats(result);
    recommendation_workspace_free(&workspace);
    return 0;
}

int recommendation_results_equal(const RecommendationResult *left,
                                  const RecommendationResult *right,
                                  double tolerance)
{
    if (left == NULL || right == NULL || left->user_count != right->user_count ||
        left->k != right->k) {
        return 0;
    }
    for (uint32_t user = 0; user < left->user_count; ++user) {
        if (left->lengths[user] != right->lengths[user]) {
            return 0;
        }
        for (uint32_t rank = 0; rank < left->lengths[user]; ++rank) {
            size_t position = (size_t)user * left->k + rank;
            if (left->product_ids[position] != right->product_ids[position] ||
                fabs(left->scores[position] - right->scores[position]) > tolerance) {
                return 0;
            }
        }
    }
    return 1;
}

uint64_t recommendation_result_checksum(const RecommendationResult *result)
{
    uint64_t checksum = UINT64_C(1469598103934665603);
    for (uint32_t user = 0; user < result->user_count; ++user) {
        checksum ^= result->lengths[user];
        checksum *= UINT64_C(1099511628211);
        for (uint32_t rank = 0; rank < result->lengths[user]; ++rank) {
            checksum ^= result->product_ids[(size_t)user * result->k + rank];
            checksum *= UINT64_C(1099511628211);
        }
    }
    return checksum;
}
