#include "recommender.h"
#include "recommender_internal.h"

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

int recommend_openmp(const Dataset *dataset,
                     const CooccurResult *model,
                     const CooccurGraph *graph,
                     uint32_t k, int threads,
                     OmpSchedule schedule, int chunk,
                     RecommendationResult *result,
                     char *error, size_t error_capacity)
{
    RecommendationWorkspace *workspaces = NULL;
    double start;
    if (dataset == NULL || model == NULL || graph == NULL || result == NULL ||
        k == 0 || threads < 1 || chunk < 1 ||
        schedule < OMP_SCHEDULE_STATIC || schedule > OMP_SCHEDULE_GUIDED ||
        model->product_count != graph->product_count) {
        set_error(error, error_capacity, "invalid OpenMP recommendation inputs");
        return -1;
    }
    if (recommendation_result_allocate(dataset->history.user_count, k, result) != 0) {
        set_error(error, error_capacity, "out of memory for recommendation result");
        return -1;
    }
    workspaces = calloc((size_t)threads, sizeof(*workspaces));
    if (workspaces == NULL) {
        set_error(error, error_capacity, "out of memory for recommendation workspaces");
        recommendation_result_free(result);
        return -1;
    }
    for (int thread = 0; thread < threads; ++thread) {
        if (recommendation_workspace_init(model->product_count,
                                          &workspaces[thread]) != 0) {
            set_error(error, error_capacity,
                      "out of memory for thread %d recommendation workspace", thread);
            for (int previous = 0; previous < thread; ++previous) {
                recommendation_workspace_free(&workspaces[previous]);
            }
            free(workspaces);
            recommendation_result_free(result);
            return -1;
        }
    }

    omp_set_dynamic(0);
    omp_set_num_threads(threads);
    omp_set_schedule(schedule == OMP_SCHEDULE_STATIC ? omp_sched_static :
                     schedule == OMP_SCHEDULE_DYNAMIC ? omp_sched_dynamic :
                     omp_sched_guided, chunk);
    start = omp_get_wtime();
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        RecommendationWorkspace *workspace = &workspaces[tid];

        /* 模型和历史只读；每个线程使用私有工作区，每个用户写独立结果区间。 */
        #pragma omp for schedule(runtime)
        for (int64_t user = 0;
             user < (int64_t)dataset->history.user_count; ++user) {
            recommend_one_user((uint32_t)user, dataset, model, graph, k,
                               workspace, result);
        }
    }
    result->compute_seconds = omp_get_wtime() - start;
    recommendation_result_finalize_stats(result);

    for (int thread = 0; thread < threads; ++thread) {
        recommendation_workspace_free(&workspaces[thread]);
    }
    free(workspaces);
    return 0;
}
