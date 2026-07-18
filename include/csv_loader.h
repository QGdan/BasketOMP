#ifndef BASKET_CSV_LOADER_H
#define BASKET_CSV_LOADER_H

#include "model.h"

#include <stddef.h>

/*
 * 从 data_dir 加载四个 CSV。成功返回 0，失败返回非 0 并写入 error。
 * dataset_load 成功后，调用方必须调用 dataset_free。
 */
int dataset_load(const char *data_dir, Dataset *dataset,
                 char *error, size_t error_capacity);

/* 检查 CSR 区间、ID 范围和明细数量等结构不变量。 */
int dataset_validate(const Dataset *dataset,
                     char *error, size_t error_capacity);

void dataset_free(Dataset *dataset);

#endif
