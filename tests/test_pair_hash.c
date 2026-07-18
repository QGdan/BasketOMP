#include "pair_hash.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s\n", message); \
            pair_map_free(&map); \
            return 1; \
        } \
    } while (0)

static uint32_t next_random(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static int count_entries(uint64_t key, uint32_t count, void *context)
{
    size_t *visited = context;
    (void)key;
    (void)count;
    ++*visited;
    return 0;
}

int main(void)
{
    PairHashMap map;
    PairHashMap other;
    uint32_t count = 0;
    uint32_t a;
    uint32_t b;
    uint32_t state = UINT32_C(123456789);
    uint32_t expected[51][51] = {{0}};
    uint64_t checksum_before;

    CHECK(pair_map_init(&map, 1) == 0, "initialize map");
    CHECK(map.capacity >= 16, "minimum capacity");
    CHECK(!pair_map_get(&map, encode_pair(1, 2), &count), "empty lookup");
    CHECK(encode_pair(1, 2) == encode_pair(2, 1), "canonical pair encoding");
    decode_pair(encode_pair(9, 3), &a, &b);
    CHECK(a == 3 && b == 9, "pair decoding");

    CHECK(pair_map_increment(&map, encode_pair(1, 2), 1) == 0, "first insert");
    CHECK(pair_map_increment(&map, encode_pair(2, 1), 4) == 0, "same key increment");
    CHECK(pair_map_get(&map, encode_pair(1, 2), &count) && count == 5,
          "accumulated value");

    for (int i = 0; i < 5000; ++i) {
        uint32_t left = next_random(&state) % 50 + 1;
        uint32_t right = next_random(&state) % 50 + 1;
        if (left == right) {
            right = right % 50 + 1;
        }
        a = left < right ? left : right;
        b = left < right ? right : left;
        ++expected[a][b];
        CHECK(pair_map_increment(&map, encode_pair(a, b), 1) == 0,
              "random insertion");
    }
    CHECK(map.capacity >= 64, "multiple growth operations");
    {
        size_t capacity_before_reserve = map.capacity;
        CHECK(pair_map_reserve(&map, 10000) == 0, "reserve bulk capacity");
        CHECK(map.capacity > capacity_before_reserve, "reserve grows capacity");
    }
    for (a = 1; a <= 50; ++a) {
        for (b = a + 1; b <= 50; ++b) {
            if (expected[a][b] > 0) {
                CHECK(pair_map_get(&map, encode_pair(a, b), &count),
                      "random oracle key exists");
                CHECK(count == expected[a][b] + (a == 1 && b == 2 ? 5U : 0U),
                      "random oracle count");
            }
        }
    }

    checksum_before = pair_map_checksum(&map);
    CHECK(pair_map_init(&other, 2) == 0, "initialize merge source");
    CHECK(pair_map_increment(&other, encode_pair(1, 2), 7) == 0,
          "merge source existing key");
    CHECK(pair_map_increment(&other, encode_pair(60, 61), 3) == 0,
          "merge source new key");
    CHECK(pair_map_merge(&map, &other) == 0, "merge maps");
    CHECK(pair_map_get(&map, encode_pair(60, 61), &count) && count == 3,
          "merged new key");
    CHECK(pair_map_checksum(&map) != checksum_before, "checksum changes with contents");
    pair_map_free(&other);

    /* A non-power-of-two bucket count must support stable routing and lookup. */
    {
        size_t bucket_sizes[7] = {256, 256, 256, 256, 256, 256, 256};
        size_t visited = 0;
        CHECK(pair_map_init_partitioned(&other, bucket_sizes, 7) == 0,
              "initialize partitioned map");
        CHECK(other.bucket_count == 7, "partitioned bucket count");
        for (uint32_t product = 1; product <= 300; ++product) {
            CHECK(pair_map_increment(&other,
                                     encode_pair(product, product + 1000), 1) == 0,
                  "partitioned insertion");
        }
        CHECK(pair_map_increment(&other, encode_pair(7, 1007), 4) == 0,
              "partitioned existing-key increment");
        CHECK(pair_map_get(&other, encode_pair(7, 1007), &count) && count == 5,
              "partitioned lookup");
        CHECK(pair_map_foreach(&other, count_entries, &visited) == 0 &&
              visited == other.size && other.size == 300,
              "partitioned foreach and size");
        CHECK(pair_map_reserve(&other, 1000) != 0,
              "partitioned map rejects whole-table rehash");
        pair_map_free(&other);
    }

    /* 已存在键的计数溢出必须被拒绝且不能破坏原值。 */
    CHECK(pair_map_increment(&map, encode_pair(70, 71), UINT32_MAX) == 0,
          "insert max count");
    CHECK(pair_map_increment(&map, encode_pair(70, 71), 1) != 0,
          "reject count overflow");
    CHECK(pair_map_get(&map, encode_pair(70, 71), &count) && count == UINT32_MAX,
          "overflow keeps original value");
    {
        size_t visited = 0;
        CHECK(pair_map_foreach(&map, count_entries, &visited) == 0,
              "foreach succeeds");
        CHECK(visited == map.size, "foreach visits every used entry");
    }

    pair_map_free(&map);
    puts("PASS: pair hash assertions");
    return 0;
}
