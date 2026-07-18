#ifndef BASKET_MODEL_H
#define BASKET_MODEL_H

#include <stddef.h>
#include <stdint.h>

/* 第 i 个购物篮占用 products[offsets[i] ... offsets[i + 1] - 1]。 */
typedef struct {
    uint32_t basket_count;
    uint64_t product_count;
    uint64_t *offsets;
    uint32_t *products;
    uint32_t *users;
    uint32_t *order_ids;
} BasketTable;

/* user_count 使用 max_user_id + 1，用户 ID 可直接作为 offsets 下标。 */
typedef struct {
    uint32_t user_count;
    uint64_t entry_count;
    uint64_t *offsets;
    uint32_t *product_ids;
    uint16_t *frequencies;
} UserHistory;

/* train 真值按用户采用 CSR 存储。 */
typedef struct {
    uint32_t user_count;
    uint64_t entry_count;
    uint64_t *offsets;
    uint32_t *product_ids;
} GroundTruth;

typedef struct {
    uint32_t product_id;
    uint32_t weight;
    /* 按原评分公式预计算，推荐阶段保持 frequency*weight/denominator 次序。 */
    double normalization_denominator;
} CooccurNeighbor;

/* 商品 ID 直接作为 offsets 下标；第 p 个商品的邻居区间为 [p, p+1)。 */
typedef struct {
    uint32_t product_count;
    uint32_t max_neighbors;
    uint32_t max_degree;
    uint64_t edge_entry_count;
    uint64_t *offsets;
    CooccurNeighbor *neighbors;
    double edge_prepare_seconds;
    double truncate_seconds;
} CooccurGraph;

typedef struct {
    uint32_t max_product_id;
    uint32_t product_count;
    unsigned char *valid_products;

    uint32_t max_user_id;
    uint64_t order_count;
    uint64_t prior_row_count;
    uint64_t train_row_count;

    BasketTable baskets;
    UserHistory history;
    GroundTruth truth;
} Dataset;

#endif
