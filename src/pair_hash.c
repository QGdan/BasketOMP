#include "pair_hash.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PAIR_MAP_MIN_CAPACITY 16U
#define PAIR_MAP_LOAD_NUMERATOR 7U
#define PAIR_MAP_LOAD_DENOMINATOR 10U

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

uint64_t encode_pair(uint32_t product_a, uint32_t product_b)
{
    uint32_t lower = product_a < product_b ? product_a : product_b;
    uint32_t upper = product_a < product_b ? product_b : product_a;
    return ((uint64_t)lower << 32) | (uint64_t)upper;
}

void decode_pair(uint64_t key, uint32_t *product_a, uint32_t *product_b)
{
    if (product_a != NULL) {
        *product_a = (uint32_t)(key >> 32);
    }
    if (product_b != NULL) {
        *product_b = (uint32_t)key;
    }
}

static size_t next_power_of_two(size_t requested)
{
    size_t capacity = PAIR_MAP_MIN_CAPACITY;
    while (capacity < requested) {
        if (capacity > SIZE_MAX / 2) {
            return 0;
        }
        capacity *= 2;
    }
    return capacity;
}

int pair_map_init(PairHashMap *map, size_t initial_capacity)
{
    size_t capacity;
    if (map == NULL) {
        return -1;
    }
    memset(map, 0, sizeof(*map));
    capacity = next_power_of_two(initial_capacity);
    if (capacity == 0 || capacity > SIZE_MAX / sizeof(*map->entries)) {
        return -1;
    }
    map->entries = calloc(capacity, sizeof(*map->entries));
    if (map->entries == NULL) {
        return -1;
    }
    map->capacity = capacity;
    return 0;
}

void pair_map_free(PairHashMap *map)
{
    if (map == NULL) {
        return;
    }
    free(map->entries);
    memset(map, 0, sizeof(*map));
}

static size_t find_slot(const PairEntry *entries, size_t capacity,
                        uint64_t key, int *found)
{
    size_t index = (size_t)(mix64(key) & (uint64_t)(capacity - 1));
    for (;;) {
        if (!entries[index].used) {
            *found = 0;
            return index;
        }
        if (entries[index].key == key) {
            *found = 1;
            return index;
        }
        index = (index + 1) & (capacity - 1);
    }
}

static int pair_map_rehash(PairHashMap *map, size_t new_capacity)
{
    PairEntry *new_entries;
    size_t i;
    if (new_capacity < map->capacity ||
        new_capacity > SIZE_MAX / sizeof(*new_entries)) {
        return -1;
    }
    new_entries = calloc(new_capacity, sizeof(*new_entries));
    if (new_entries == NULL) {
        return -1;
    }
    for (i = 0; i < map->capacity; ++i) {
        if (map->entries[i].used) {
            int found;
            size_t slot = find_slot(new_entries, new_capacity,
                                    map->entries[i].key, &found);
            (void)found;
            new_entries[slot] = map->entries[i];
        }
    }
    free(map->entries);
    map->entries = new_entries;
    map->capacity = new_capacity;
    return 0;
}

int pair_map_reserve(PairHashMap *map, size_t expected_size)
{
    size_t required_slots;
    size_t capacity;
    if (map == NULL || map->entries == NULL) {
        return -1;
    }
    if (expected_size > (SIZE_MAX - (PAIR_MAP_LOAD_NUMERATOR - 1)) /
                        PAIR_MAP_LOAD_DENOMINATOR) {
        return -1;
    }
    required_slots = (expected_size * PAIR_MAP_LOAD_DENOMINATOR +
                      PAIR_MAP_LOAD_NUMERATOR - 1) /
                     PAIR_MAP_LOAD_NUMERATOR;
    capacity = next_power_of_two(required_slots);
    if (capacity == 0) {
        return -1;
    }
    if (capacity <= map->capacity) {
        return 0;
    }
    return pair_map_rehash(map, capacity);
}

int pair_map_increment(PairHashMap *map, uint64_t key, uint32_t delta)
{
    size_t slot;
    int found;
    if (map == NULL || map->entries == NULL || map->capacity == 0) {
        return -1;
    }
    slot = find_slot(map->entries, map->capacity, key, &found);
    if (found) {
        if (UINT32_MAX - map->entries[slot].count < delta) {
            return -1;
        }
        map->entries[slot].count += delta;
        return 0;
    }

    /* 插入新键前维持不超过 0.70 的负载因子，缩短线性探测链。 */
    if ((map->size + 1) * PAIR_MAP_LOAD_DENOMINATOR >=
        map->capacity * PAIR_MAP_LOAD_NUMERATOR) {
        if (map->capacity > SIZE_MAX / 2 ||
            pair_map_rehash(map, map->capacity * 2) != 0) {
            return -1;
        }
        slot = find_slot(map->entries, map->capacity, key, &found);
    }
    map->entries[slot].key = key;
    map->entries[slot].count = delta;
    map->entries[slot].used = 1;
    ++map->size;
    return 0;
}

int pair_map_get(const PairHashMap *map, uint64_t key, uint32_t *count)
{
    size_t slot;
    int found;
    if (map == NULL || map->entries == NULL || map->capacity == 0) {
        return 0;
    }
    slot = find_slot(map->entries, map->capacity, key, &found);
    if (found && count != NULL) {
        *count = map->entries[slot].count;
    }
    return found;
}

int pair_map_merge(PairHashMap *destination, const PairHashMap *source)
{
    if (destination == NULL || source == NULL) {
        return -1;
    }
    for (size_t i = 0; i < source->capacity; ++i) {
        if (source->entries[i].used &&
            pair_map_increment(destination, source->entries[i].key,
                               source->entries[i].count) != 0) {
            return -1;
        }
    }
    return 0;
}

int pair_map_foreach(const PairHashMap *map,
                     PairMapVisitor visitor, void *context)
{
    if (map == NULL || visitor == NULL) {
        return -1;
    }
    for (size_t i = 0; i < map->capacity; ++i) {
        if (map->entries[i].used) {
            int status = visitor(map->entries[i].key,
                                 map->entries[i].count, context);
            if (status != 0) {
                return status;
            }
        }
    }
    return 0;
}

uint64_t pair_map_checksum(const PairHashMap *map)
{
    uint64_t sum = mix64((uint64_t)map->size);
    uint64_t xor_value = 0;
    for (size_t i = 0; i < map->capacity; ++i) {
        if (map->entries[i].used) {
            uint64_t item = mix64(map->entries[i].key ^
                                  ((uint64_t)map->entries[i].count << 1));
            sum += item;
            xor_value ^= item;
        }
    }
    return mix64(sum) ^ xor_value;
}
