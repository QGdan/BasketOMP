#include <errno.h>
#include <limits.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_thread_count(const char *text)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > INT_MAX) {
        return -1;
    }
    return (int)value;
}

int main(int argc, char **argv)
{
    int requested = 4;
    int observed = 0;

    if (argc == 2) {
        requested = parse_thread_count(argv[1]);
        if (requested < 1) {
            fprintf(stderr, "invalid thread count: %s\n", argv[1]);
            return 2;
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [threads]\n", argv[0]);
        return 2;
    }

    omp_set_dynamic(0);
    omp_set_num_threads(requested);

    /* single 保证只有一个线程写 observed，避免数据竞争。 */
    #pragma omp parallel
    {
        #pragma omp single
        observed = omp_get_num_threads();
    }

    printf("requested=%d observed=%d max_threads=%d\n",
           requested, observed, omp_get_max_threads());

    if (observed != requested) {
        fprintf(stderr, "OpenMP thread-count validation failed\n");
        return 1;
    }
    return 0;
}
