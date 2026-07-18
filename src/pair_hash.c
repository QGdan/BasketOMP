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

size_t pair_map_bucket_index(uint64_t key, size_t bucket_count)
{
    uint64_t bucket_hash;
    if (bucket_count == 0) {
        return 0;
    }
    /*
     * Use the high mixed bits for bucket routing. Hash-table probing uses the
     * low bits, so this separation prevents every key in one bucket from
     * starting at the same small subset of slots.
     */
    bucket_hash = mix64(key) >> 32;
    if ((bucket_count & (bucket_count - 1)) == 0) {
        return (size_t)(bucket_hash & (uint64_t)(bucket_count - 1));
    }
    return (size_t)(bucket_hash % (uint64_t)bucket_count);
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
    free(map->bucket_offsets);
    memset(map, 0, sizeof(*map));
}

int pair_map_init_partitioned(PairHashMap *map,
                              const size_t *bucket_expected_sizes,
                              size_t bucket_count)
{
    size_t total_capacity = 0;
    if (map == NULL || bucket_expected_sizes == NULL || bucket_count == 0 ||
        bucket_count > SIZE_MAX / sizeof(*map->bucket_offsets) - 1) {
        return -1;
    }
    memset(map, 0, sizeof(*map));
    map->bucket_offsets = calloc(bucket_count + 1,
                                 sizeof(*map->bucket_offsets));
    if (map->bucket_offsets == NULL) {
        return -1;
    }
    for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
        size_t expected = bucket_expected_sizes[bucket];
        size_t required_slots;
        size_t bucket_capacity;
        if (expected > (SIZE_MAX - (PAIR_MAP_LOAD_NUMERATOR - 1)) /
                       PAIR_MAP_LOAD_DENOMINATOR) {
            pair_map_free(map);
            return -1;
        }
        required_slots = (expected * PAIR_MAP_LOAD_DENOMINATOR +
                          PAIR_MAP_LOAD_NUMERATOR - 1) /
                         PAIR_MAP_LOAD_NUMERATOR;
        bucket_capacity = next_power_of_two(required_slots);
        if (bucket_capacity == 0 ||
            SIZE_MAX - total_capacity < bucket_capacity) {
            pair_map_free(map);
            return -1;
        }
        map->bucket_offsets[bucket] = total_capacity;
        total_capacity += bucket_capacity;
    }
    map->bucket_offsets[bucket_count] = total_capacity;
    if (total_capacity > SIZE_MAX / sizeof(*map->entries)) {
        pair_map_free(map);
        return -1;
    }
    map->entries = calloc(total_capacity, sizeof(*map->entries));
    if (map->entries == NULL) {
        pair_map_free(map);
        return -1;
    }
    map->capacity = total_capacity;
    map->bucket_count = bucket_count;
    return 0;
}

static size_t find_slot_range(const PairEntry *entries, size_t begin,
                              size_t capacity, uint64_t key, int *found)
{
    size_t relative = (size_t)(mix64(key) & (uint64_t)(capacity - 1));
    for (size_t probe = 0; probe < capacity; ++probe) {
        size_t index = begin + relative;
        if (!entries[index].used) {
            *found = 0;
            return index;
        }
        if (entries[index].key == key) {
            *found = 1;
            return index;
        }
        relative = (relative + 1) & (capacity - 1);
    }
    *found = 0;
    return SIZE_MAX;
}

static size_t find_slot(const PairEntry *entries, size_t capacity,
                        uint64_t key, int *found)
{
    return find_slot_range(entries, 0, capacity, key, found);
}

static int pair_map_rehash(PairHashMap *map, size_t new_capacity)
{
    PairEntry *new_entries;
    size_t i;
    if (map->bucket_count != 0 || new_capacity < map->capacity ||
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
            if (slot == SIZE_MAX) {
                free(new_entries);
                return -1;
            }
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
    if (map == NULL || map->entries == NULL || map->bucket_count != 0) {
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

static int pair_map_increment_bucket(PairHashMap *map, size_t bucket,
                                     uint64_t key, uint32_t delta,
                                     int update_total_size,
                                     size_t *inserted_entries)
{
    size_t begin = map->bucket_offsets[bucket];
    size_t capacity = map->bucket_offsets[bucket + 1] - begin;
    size_t slot;
    int found;

    slot = find_slot_range(map->entries, begin, capacity, key, &found);
    if (slot == SIZE_MAX) {
        return -1;
    }
    if (found) {
        if (UINT32_MAX - map->entries[slot].count < delta) {
            return -1;
        }
        map->entries[slot].count += delta;
        return 0;
    }
    map->entries[slot].key = key;
    map->entries[slot].count = delta;
    map->entries[slot].used = 1;
    if (update_total_size) {
        ++map->size;
    }
    if (inserted_entries != NULL) {
        ++*inserted_entries;
    }
    return 0;
}

int pair_map_increment(PairHashMap *map, uint64_t key, uint32_t delta)
{
    size_t slot;
    int found;
    if (map == NULL || map->entries == NULL || map->capacity == 0) {
        return -1;
    }
    if (map->bucket_count != 0) {
        size_t bucket = pair_map_bucket_index(key, map->bucket_count);
        return pair_map_increment_bucket(map, bucket, key, delta, 1, NULL);
    }
    slot = find_slot(map->entries, map->capacity, key, &found);
    if (slot == SIZE_MAX) {
        return -1;
    }
    if (found) {
        if (UINT32_MAX - map->entries[slot].count < delta) {
            return -1;
        }
        map->entries[slot].count += delta;
        return 0;
    }

    /* Keep the load factor below about 0.70 to shorten linear-probe chains. */
    if ((map->size + 1) * PAIR_MAP_LOAD_DENOMINATOR >=
        map->capacity * PAIR_MAP_LOAD_NUMERATOR) {
        if (map->capacity > SIZE_MAX / 2 ||
            pair_map_rehash(map, map->capacity * 2) != 0) {
            return -1;
        }
        slot = find_slot(map->entries, map->capacity, key, &found);
        if (slot == SIZE_MAX) {
            return -1;
        }
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
    if (map->bucket_count != 0) {
        size_t bucket = pair_map_bucket_index(key, map->bucket_count);
        size_t begin = map->bucket_offsets[bucket];
        size_t capacity = map->bucket_offsets[bucket + 1] - begin;
        slot = find_slot_range(map->entries, begin, capacity, key, &found);
    } else {
        slot = find_slot(map->entries, map->capacity, key, &found);
    }
    if (slot != SIZE_MAX && found && count != NULL) {
        *count = map->entries[slot].count;
    }
    return slot != SIZE_MAX && found;
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

int pair_map_merge_into_bucket(PairHashMap *destination, size_t bucket,
                               const PairHashMap *source,
                               size_t *inserted_entries)
{
    size_t inserted = 0;
    if (destination == NULL || source == NULL ||
        destination->bucket_count == 0 || bucket >= destination->bucket_count) {
        return -1;
    }
    for (size_t i = 0; i < source->capacity; ++i) {
        if (!source->entries[i].used) {
            continue;
        }
        if (pair_map_bucket_index(source->entries[i].key,
                                  destination->bucket_count) != bucket ||
            pair_map_increment_bucket(destination, bucket,
                                      source->entries[i].key,
                                      source->entries[i].count,
                                      0, &inserted) != 0) {
            return -1;
        }
    }
    if (inserted_entries != NULL) {
        *inserted_entries += inserted;
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
