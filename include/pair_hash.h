#ifndef BASKET_PAIR_HASH_H
#define BASKET_PAIR_HASH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t key;
    uint32_t count;
    unsigned char used;
} PairEntry;

typedef struct {
    PairEntry *entries;
    size_t capacity;
    size_t size;
} PairHashMap;

typedef int (*PairMapVisitor)(uint64_t key, uint32_t count, void *context);

/* 无序商品对统一编码为 min(a,b) 在高 32 位、max(a,b) 在低 32 位。 */
uint64_t encode_pair(uint32_t product_a, uint32_t product_b);
void decode_pair(uint64_t key, uint32_t *product_a, uint32_t *product_b);

int pair_map_init(PairHashMap *map, size_t initial_capacity);
void pair_map_free(PairHashMap *map);

/* 预留 expected_size 个键所需容量，避免批量归并时反复扩容。 */
int pair_map_reserve(PairHashMap *map, size_t expected_size);

/* 累加成功返回 0；内存不足或 uint32_t 计数溢出返回非 0。 */
int pair_map_increment(PairHashMap *map, uint64_t key, uint32_t delta);

/* 找到 key 返回 1，未找到返回 0。count 可以为 NULL。 */
int pair_map_get(const PairHashMap *map, uint64_t key, uint32_t *count);

int pair_map_merge(PairHashMap *destination, const PairHashMap *source);

/* 访问器返回非 0 时提前停止，并把该值返回给调用方。 */
int pair_map_foreach(const PairHashMap *map,
                     PairMapVisitor visitor, void *context);

/* 与槽位遍历顺序无关的稳定校验和，用于串并行快速回归。 */
uint64_t pair_map_checksum(const PairHashMap *map);

#endif
