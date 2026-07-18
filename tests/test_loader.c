#include "csv_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", message); \
            dataset_free(&dataset); \
            return 1; \
        } \
    } while (0)

static int find_basket(const Dataset *dataset, uint32_t order_id)
{
    for (uint32_t i = 0; i < dataset->baskets.basket_count; ++i) {
        if (dataset->baskets.order_ids[i] == order_id) {
            return (int)i;
        }
    }
    return -1;
}

static int check_user_history(const Dataset *dataset, uint32_t user_id,
                              const uint32_t *products,
                              const uint16_t *frequencies, size_t count)
{
    uint64_t begin = dataset->history.offsets[user_id];
    uint64_t end = dataset->history.offsets[user_id + 1];
    if (end - begin != count) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (dataset->history.product_ids[begin + i] != products[i] ||
            dataset->history.frequencies[begin + i] != frequencies[i]) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = argc == 2 ? argv[1] : "data/toy";
    char error[512];
    Dataset dataset;
    int basket_10;
    int basket_13;
    const uint32_t user1_products[] = {1, 2, 3};
    const uint16_t user1_frequencies[] = {2, 2, 1};
    const uint32_t user2_products[] = {2};
    const uint16_t user2_frequencies[] = {1};

    if (dataset_load(path, &dataset, error, sizeof(error)) != 0) {
        fprintf(stderr, "FAIL: dataset_load: %s\n", error);
        return 1;
    }
    if (strstr(path, "small") != NULL) {
        CHECK(dataset.order_count == 2995, "small order count");
        CHECK(dataset.prior_row_count == 26575, "small prior row count");
        CHECK(dataset.train_row_count == 1327, "small train row count");
        CHECK(dataset.product_count == 6170, "small product count");
        CHECK(dataset.baskets.product_count == dataset.prior_row_count,
              "small prior rows represented exactly once");
        CHECK(dataset_validate(&dataset, error, sizeof(error)) == 0,
              "small structural validation");
        dataset_free(&dataset);
        puts("PASS: loader small structural assertions");
        return 0;
    }
    CHECK(dataset.order_count == 7, "toy order count");
    CHECK(dataset.prior_row_count == 6, "toy prior row count");
    CHECK(dataset.train_row_count == 4, "toy train row count");
    CHECK(dataset.product_count == 4 && dataset.max_product_id == 4,
          "toy product dimensions");
    CHECK(dataset.max_user_id == 3, "toy max user id");
    CHECK(dataset.baskets.basket_count == 4, "all prior orders become baskets");
    CHECK(dataset.baskets.product_count == 6, "basket product count");

    basket_10 = find_basket(&dataset, 10);
    basket_13 = find_basket(&dataset, 13);
    CHECK(basket_10 >= 0, "order 10 basket exists");
    CHECK(dataset.baskets.offsets[basket_10 + 1] -
          dataset.baskets.offsets[basket_10] == 3, "order 10 has three products");
    CHECK(basket_13 >= 0, "empty prior order 13 basket exists");
    CHECK(dataset.baskets.offsets[basket_13 + 1] ==
          dataset.baskets.offsets[basket_13], "order 13 is empty");

    CHECK(check_user_history(&dataset, 1, user1_products,
                             user1_frequencies, 3), "user 1 history");
    CHECK(check_user_history(&dataset, 2, user2_products,
                             user2_frequencies, 1), "user 2 history");
    CHECK(dataset.history.offsets[3] == dataset.history.offsets[4],
          "user 3 empty history");

    CHECK(dataset.truth.offsets[1 + 1] - dataset.truth.offsets[1] == 2,
          "user 1 truth size");
    CHECK(dataset.truth.product_ids[dataset.truth.offsets[1]] == 2 &&
          dataset.truth.product_ids[dataset.truth.offsets[1] + 1] == 4,
          "user 1 truth values");

    dataset_free(&dataset);

    /* 重复加载和释放用于暴露生命周期错误和非确定性问题。 */
    for (int repeat = 0; repeat < 20; ++repeat) {
        if (dataset_load(path, &dataset, error, sizeof(error)) != 0) {
            fprintf(stderr, "FAIL: repeated load %d: %s\n", repeat, error);
            return 1;
        }
        if (dataset_validate(&dataset, error, sizeof(error)) != 0) {
            fprintf(stderr, "FAIL: repeated validate %d: %s\n", repeat, error);
            dataset_free(&dataset);
            return 1;
        }
        dataset_free(&dataset);
    }
    CHECK(dataset_load("data/path-that-does-not-exist", &dataset,
                       error, sizeof(error)) != 0,
          "missing data directory must fail");
    CHECK(error[0] != '\0', "missing directory must return an error message");
    puts("PASS: loader toy assertions");
    return 0;
}
