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
    /* In a partitioned table, offsets[b]..offsets[b+1] belongs only to bucket b. */
    size_t bucket_count;
    size_t *bucket_offsets;
} PairHashMap;

typedef int (*PairMapVisitor)(uint64_t key, uint32_t count, void *context);

/* 无序商品对统一编码为 min(a,b) 在高 32 位、max(a,b) 在低 32 位。 */
uint64_t encode_pair(uint32_t product_a, uint32_t product_b);
void decode_pair(uint64_t key, uint32_t *product_a, uint32_t *product_b);

int pair_map_init(PairHashMap *map, size_t initial_capacity);

/*
 * Build one contiguous table from per-bucket entry upper bounds. Bucket slot
 * ranges never overlap, so different OpenMP workers can write without locks.
 */
int pair_map_init_partitioned(PairHashMap *map,
                              const size_t *bucket_expected_sizes,
                              size_t bucket_count);
void pair_map_free(PairHashMap *map);

/* Stable bucket routing shared by all local maps and the destination table. */
size_t pair_map_bucket_index(uint64_t key, size_t bucket_count);

/* 预留 expected_size 个键所需容量，避免批量归并时反复扩容。 */
int pair_map_reserve(PairHashMap *map, size_t expected_size);

/* 累加成功返回 0；内存不足或 uint32_t 计数溢出返回非 0。 */
int pair_map_increment(PairHashMap *map, uint64_t key, uint32_t delta);

/* 找到 key 返回 1，未找到返回 0。count 可以为 NULL。 */
int pair_map_get(const PairHashMap *map, uint64_t key, uint32_t *count);

int pair_map_merge(PairHashMap *destination, const PairHashMap *source);

/*
 * Merge a local map that contains only keys for bucket into that destination
 * bucket. The caller must give each bucket to at most one worker at a time.
 */
int pair_map_merge_into_bucket(PairHashMap *destination, size_t bucket,
                               const PairHashMap *source,
                               size_t *inserted_entries);

/* 访问器返回非 0 时提前停止，并把该值返回给调用方。 */
int pair_map_foreach(const PairHashMap *map,
                     PairMapVisitor visitor, void *context);

/* 与槽位遍历顺序无关的稳定校验和，用于串并行快速回归。 */
uint64_t pair_map_checksum(const PairHashMap *map);

#endif
