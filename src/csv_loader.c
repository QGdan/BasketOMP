#include "csv_loader.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSV_LINE_CAPACITY 8192U
#define INVALID_BASKET_INDEX UINT32_MAX

enum {
    EVAL_NONE = 0,
    EVAL_PRIOR = 1,
    EVAL_TRAIN = 2,
    EVAL_TEST = 3
};

typedef struct {
    size_t capacity;
    uint32_t *users;
    uint32_t *basket_indices;
    unsigned char *evals;
} OrderIndex;

typedef struct {
    size_t size;
    size_t capacity;
    uint32_t *order_ids;
    uint32_t *users;
} BasketList;

typedef struct {
    uint32_t user_id;
    uint32_t product_id;
} UserProductRecord;

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

static int build_path(char *output, size_t capacity,
                      const char *directory, const char *filename)
{
    int written = snprintf(output, capacity, "%s/%s", directory, filename);
    return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static void trim_newline(char *line)
{
    size_t length = strlen(line);
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }
}

/* 返回 1 表示读到一行，0 表示 EOF，-1 表示行过长或读取失败。 */
static int read_csv_line(FILE *file, char *line, size_t capacity)
{
    size_t length;
    if (fgets(line, (int)capacity, file) == NULL) {
        return ferror(file) ? -1 : 0;
    }
    length = strlen(line);
    if (length > 0 && line[length - 1] != '\n' && !feof(file)) {
        int character;
        while ((character = fgetc(file)) != '\n' && character != EOF) {
        }
        return -1;
    }
    trim_newline(line);
    return 1;
}

static int check_header(FILE *file, const char *path, const char *expected,
                        char *line, char *error, size_t error_capacity)
{
    int status = read_csv_line(file, line, CSV_LINE_CAPACITY);
    if (status != 1) {
        set_error(error, error_capacity, "%s: cannot read CSV header", path);
        return -1;
    }
    if (strcmp(line, expected) != 0) {
        set_error(error, error_capacity, "%s: unexpected header: %s", path, line);
        return -1;
    }
    return 0;
}

static int parse_u32_field(const char **cursor, uint32_t *value, char delimiter)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(*cursor, &end, 10);
    if (errno != 0 || end == *cursor || parsed > UINT32_MAX || *end != delimiter) {
        return -1;
    }
    *value = (uint32_t)parsed;
    *cursor = end + 1;
    return 0;
}

static int parse_order_line(const char *line, uint32_t *order_id,
                            uint32_t *user_id, unsigned char *eval,
                            uint32_t *order_number)
{
    const char *cursor = line;
    if (parse_u32_field(&cursor, order_id, ',') != 0 ||
        parse_u32_field(&cursor, user_id, ',') != 0) {
        return -1;
    }

    if (strncmp(cursor, "prior,", 6) == 0) {
        *eval = EVAL_PRIOR;
        cursor += 6;
    } else if (strncmp(cursor, "train,", 6) == 0) {
        *eval = EVAL_TRAIN;
        cursor += 6;
    } else if (strncmp(cursor, "test,", 5) == 0) {
        *eval = EVAL_TEST;
        cursor += 5;
    } else {
        return -1;
    }
    return parse_u32_field(&cursor, order_number, ',');
}

static int parse_product_row(const char *line, uint32_t *order_id,
                             uint32_t *product_id)
{
    const char *cursor = line;
    return parse_u32_field(&cursor, order_id, ',') == 0 &&
           parse_u32_field(&cursor, product_id, ',') == 0 ? 0 : -1;
}

static int parse_product_id(const char *line, uint32_t *product_id)
{
    const char *cursor = line;
    return parse_u32_field(&cursor, product_id, ',');
}

static void order_index_free(OrderIndex *index)
{
    free(index->users);
    free(index->basket_indices);
    free(index->evals);
    memset(index, 0, sizeof(*index));
}

static int order_index_grow(OrderIndex *index, uint32_t order_id)
{
    size_t new_capacity = index->capacity == 0 ? 1024 : index->capacity;
    uint32_t *new_users;
    uint32_t *new_baskets;
    unsigned char *new_evals;
    size_t i;

    while (new_capacity <= order_id) {
        if (new_capacity > SIZE_MAX / 2) {
            return -1;
        }
        new_capacity *= 2;
    }
    if (new_capacity == index->capacity) {
        return 0;
    }

    new_users = calloc(new_capacity, sizeof(*new_users));
    new_baskets = malloc(new_capacity * sizeof(*new_baskets));
    new_evals = calloc(new_capacity, sizeof(*new_evals));
    if (new_users == NULL || new_baskets == NULL || new_evals == NULL) {
        free(new_users);
        free(new_baskets);
        free(new_evals);
        return -1;
    }
    for (i = 0; i < new_capacity; ++i) {
        new_baskets[i] = INVALID_BASKET_INDEX;
    }
    if (index->capacity > 0) {
        memcpy(new_users, index->users, index->capacity * sizeof(*new_users));
        memcpy(new_baskets, index->basket_indices,
               index->capacity * sizeof(*new_baskets));
        memcpy(new_evals, index->evals, index->capacity * sizeof(*new_evals));
    }
    free(index->users);
    free(index->basket_indices);
    free(index->evals);
    index->users = new_users;
    index->basket_indices = new_baskets;
    index->evals = new_evals;
    index->capacity = new_capacity;
    return 0;
}

static void basket_list_free(BasketList *list)
{
    free(list->order_ids);
    free(list->users);
    memset(list, 0, sizeof(*list));
}

static int basket_list_append(BasketList *list, uint32_t order_id,
                              uint32_t user_id)
{
    if (list->size == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 1024 : list->capacity * 2;
        uint32_t *new_orders = malloc(new_capacity * sizeof(*new_orders));
        uint32_t *new_users = malloc(new_capacity * sizeof(*new_users));
        if (new_orders == NULL || new_users == NULL) {
            free(new_orders);
            free(new_users);
            return -1;
        }
        if (list->size > 0) {
            memcpy(new_orders, list->order_ids, list->size * sizeof(*new_orders));
            memcpy(new_users, list->users, list->size * sizeof(*new_users));
        }
        free(list->order_ids);
        free(list->users);
        list->order_ids = new_orders;
        list->users = new_users;
        list->capacity = new_capacity;
    }
    list->order_ids[list->size] = order_id;
    list->users[list->size] = user_id;
    ++list->size;
    return 0;
}

static int grow_valid_products(unsigned char **valid, size_t *capacity,
                               uint32_t product_id)
{
    size_t new_capacity = *capacity == 0 ? 1024 : *capacity;
    unsigned char *new_valid;
    while (new_capacity <= product_id) {
        if (new_capacity > SIZE_MAX / 2) {
            return -1;
        }
        new_capacity *= 2;
    }
    if (new_capacity == *capacity) {
        return 0;
    }
    new_valid = calloc(new_capacity, sizeof(*new_valid));
    if (new_valid == NULL) {
        return -1;
    }
    if (*capacity > 0) {
        memcpy(new_valid, *valid, *capacity * sizeof(*new_valid));
    }
    free(*valid);
    *valid = new_valid;
    *capacity = new_capacity;
    return 0;
}

static int load_products(const char *data_dir, Dataset *dataset,
                         char *error, size_t error_capacity)
{
    static const char *header = "product_id,product_name,aisle_id,department_id";
    char path[1024];
    char line[CSV_LINE_CAPACITY];
    FILE *file;
    size_t capacity = 0;
    uint64_t line_number = 1;

    if (build_path(path, sizeof(path), data_dir, "products.csv") != 0) {
        set_error(error, error_capacity, "products path is too long");
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_capacity, "cannot open %s", path);
        return -1;
    }
    if (check_header(file, path, header, line, error, error_capacity) != 0) {
        fclose(file);
        return -1;
    }
    for (;;) {
        uint32_t product_id;
        int status = read_csv_line(file, line, sizeof(line));
        if (status == 0) {
            break;
        }
        ++line_number;
        if (status < 0 || parse_product_id(line, &product_id) != 0) {
            set_error(error, error_capacity, "%s:%llu invalid product row", path,
                      (unsigned long long)line_number);
            fclose(file);
            return -1;
        }
        if (grow_valid_products(&dataset->valid_products, &capacity, product_id) != 0) {
            set_error(error, error_capacity, "out of memory while loading products");
            fclose(file);
            return -1;
        }
        if (dataset->valid_products[product_id]) {
            set_error(error, error_capacity, "%s:%llu duplicate product_id %u", path,
                      (unsigned long long)line_number, product_id);
            fclose(file);
            return -1;
        }
        dataset->valid_products[product_id] = 1;
        ++dataset->product_count;
        if (product_id > dataset->max_product_id) {
            dataset->max_product_id = product_id;
        }
    }
    fclose(file);

    /* 收缩到 max_product_id + 1，使后续稠密数组使用同一长度。 */
    if (capacity != (size_t)dataset->max_product_id + 1) {
        size_t final_capacity = (size_t)dataset->max_product_id + 1;
        unsigned char *final_valid = realloc(dataset->valid_products, final_capacity);
        if (final_valid != NULL) {
            dataset->valid_products = final_valid;
        }
    }
    return 0;
}

static int load_orders(const char *data_dir, Dataset *dataset,
                       OrderIndex *index, BasketList *prior_orders,
                       char *error, size_t error_capacity)
{
    static const char *header =
        "order_id,user_id,eval_set,order_number,order_dow,order_hour_of_day,days_since_prior_order";
    char path[1024];
    char line[CSV_LINE_CAPACITY];
    FILE *file;
    uint64_t line_number = 1;

    if (build_path(path, sizeof(path), data_dir, "orders.csv") != 0) {
        set_error(error, error_capacity, "orders path is too long");
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_capacity, "cannot open %s", path);
        return -1;
    }
    if (check_header(file, path, header, line, error, error_capacity) != 0) {
        fclose(file);
        return -1;
    }
    for (;;) {
        uint32_t order_id;
        uint32_t user_id;
        uint32_t order_number;
        unsigned char eval;
        int status = read_csv_line(file, line, sizeof(line));
        if (status == 0) {
            break;
        }
        ++line_number;
        if (status < 0 || parse_order_line(line, &order_id, &user_id,
                                           &eval, &order_number) != 0) {
            set_error(error, error_capacity, "%s:%llu invalid order row", path,
                      (unsigned long long)line_number);
            fclose(file);
            return -1;
        }
        (void)order_number;
        if (order_index_grow(index, order_id) != 0) {
            set_error(error, error_capacity, "out of memory while indexing orders");
            fclose(file);
            return -1;
        }
        if (index->evals[order_id] != EVAL_NONE) {
            set_error(error, error_capacity, "%s:%llu duplicate order_id %u", path,
                      (unsigned long long)line_number, order_id);
            fclose(file);
            return -1;
        }
        index->users[order_id] = user_id;
        index->evals[order_id] = eval;
        ++dataset->order_count;
        if (user_id > dataset->max_user_id) {
            dataset->max_user_id = user_id;
        }
        if (eval == EVAL_PRIOR) {
            if (prior_orders->size > UINT32_MAX ||
                basket_list_append(prior_orders, order_id, user_id) != 0) {
                set_error(error, error_capacity, "out of memory while storing prior orders");
                fclose(file);
                return -1;
            }
            index->basket_indices[order_id] = (uint32_t)(prior_orders->size - 1);
        }
    }
    fclose(file);
    return 0;
}

static int compare_user_product(const void *left, const void *right)
{
    const UserProductRecord *a = left;
    const UserProductRecord *b = right;
    if (a->user_id != b->user_id) {
        return a->user_id < b->user_id ? -1 : 1;
    }
    if (a->product_id != b->product_id) {
        return a->product_id < b->product_id ? -1 : 1;
    }
    return 0;
}

static int build_history(UserProductRecord *records, uint64_t record_count,
                         uint32_t user_count, UserHistory *history,
                         char *error, size_t error_capacity)
{
    uint64_t *unique_per_user;
    uint64_t unique_count = 0;
    uint64_t i;

    qsort(records, (size_t)record_count, sizeof(*records), compare_user_product);
    unique_per_user = calloc(user_count, sizeof(*unique_per_user));
    if (unique_per_user == NULL) {
        set_error(error, error_capacity, "out of memory while counting user history");
        return -1;
    }
    for (i = 0; i < record_count;) {
        uint64_t next = i + 1;
        while (next < record_count &&
               records[next].user_id == records[i].user_id &&
               records[next].product_id == records[i].product_id) {
            ++next;
        }
        ++unique_per_user[records[i].user_id];
        ++unique_count;
        i = next;
    }

    history->user_count = user_count;
    history->entry_count = unique_count;
    history->offsets = calloc((size_t)user_count + 1, sizeof(*history->offsets));
    history->product_ids = malloc((size_t)unique_count * sizeof(*history->product_ids));
    history->frequencies = malloc((size_t)unique_count * sizeof(*history->frequencies));
    if (history->offsets == NULL ||
        (unique_count > 0 && (history->product_ids == NULL || history->frequencies == NULL))) {
        free(unique_per_user);
        set_error(error, error_capacity, "out of memory while building user history");
        return -1;
    }
    for (uint32_t user = 0; user < user_count; ++user) {
        history->offsets[user + 1] = history->offsets[user] + unique_per_user[user];
    }
    unique_count = 0;
    for (i = 0; i < record_count;) {
        uint64_t next = i + 1;
        while (next < record_count &&
               records[next].user_id == records[i].user_id &&
               records[next].product_id == records[i].product_id) {
            ++next;
        }
        if (next - i > UINT16_MAX) {
            free(unique_per_user);
            set_error(error, error_capacity,
                      "user %u product %u frequency exceeds uint16_t",
                      records[i].user_id, records[i].product_id);
            return -1;
        }
        history->product_ids[unique_count] = records[i].product_id;
        history->frequencies[unique_count] = (uint16_t)(next - i);
        ++unique_count;
        i = next;
    }
    free(unique_per_user);
    return 0;
}

static int load_prior(const char *data_dir, Dataset *dataset,
                      const OrderIndex *index, const BasketList *prior_orders,
                      char *error, size_t error_capacity)
{
    static const char *header = "order_id,product_id,add_to_cart_order,reordered";
    char path[1024];
    char line[CSV_LINE_CAPACITY];
    FILE *file;
    uint64_t *counts = NULL;
    uint64_t *cursor = NULL;
    UserProductRecord *records = NULL;
    uint64_t row_count = 0;
    uint64_t line_number = 1;

    if (build_path(path, sizeof(path), data_dir, "order_products__prior.csv") != 0) {
        set_error(error, error_capacity, "prior path is too long");
        return -1;
    }
    counts = calloc(prior_orders->size, sizeof(*counts));
    if (prior_orders->size > 0 && counts == NULL) {
        set_error(error, error_capacity, "out of memory for basket counts");
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        free(counts);
        set_error(error, error_capacity, "cannot open %s", path);
        return -1;
    }
    if (check_header(file, path, header, line, error, error_capacity) != 0) {
        fclose(file);
        free(counts);
        return -1;
    }
    for (;;) {
        uint32_t order_id;
        uint32_t product_id;
        uint32_t basket_index;
        int status = read_csv_line(file, line, sizeof(line));
        if (status == 0) {
            break;
        }
        ++line_number;
        if (status < 0 || parse_product_row(line, &order_id, &product_id) != 0 ||
            order_id >= index->capacity || index->evals[order_id] != EVAL_PRIOR ||
            product_id > dataset->max_product_id || !dataset->valid_products[product_id]) {
            set_error(error, error_capacity, "%s:%llu invalid prior row", path,
                      (unsigned long long)line_number);
            fclose(file);
            free(counts);
            return -1;
        }
        basket_index = index->basket_indices[order_id];
        if (basket_index == INVALID_BASKET_INDEX) {
            set_error(error, error_capacity, "%s:%llu prior order has no basket", path,
                      (unsigned long long)line_number);
            fclose(file);
            free(counts);
            return -1;
        }
        ++counts[basket_index];
        ++row_count;
    }
    fclose(file);

    dataset->baskets.basket_count = (uint32_t)prior_orders->size;
    dataset->baskets.product_count = row_count;
    dataset->baskets.offsets = calloc(prior_orders->size + 1,
                                      sizeof(*dataset->baskets.offsets));
    dataset->baskets.products = malloc((size_t)row_count *
                                       sizeof(*dataset->baskets.products));
    dataset->baskets.users = malloc(prior_orders->size *
                                    sizeof(*dataset->baskets.users));
    dataset->baskets.order_ids = malloc(prior_orders->size *
                                        sizeof(*dataset->baskets.order_ids));
    records = malloc((size_t)row_count * sizeof(*records));
    cursor = malloc(prior_orders->size * sizeof(*cursor));
    if (dataset->baskets.offsets == NULL ||
        (row_count > 0 && (dataset->baskets.products == NULL || records == NULL)) ||
        (prior_orders->size > 0 &&
         (dataset->baskets.users == NULL || dataset->baskets.order_ids == NULL || cursor == NULL))) {
        free(counts);
        free(cursor);
        free(records);
        set_error(error, error_capacity, "out of memory while allocating prior data");
        return -1;
    }
    for (size_t i = 0; i < prior_orders->size; ++i) {
        dataset->baskets.offsets[i + 1] = dataset->baskets.offsets[i] + counts[i];
        dataset->baskets.users[i] = prior_orders->users[i];
        dataset->baskets.order_ids[i] = prior_orders->order_ids[i];
        cursor[i] = dataset->baskets.offsets[i];
    }

    file = fopen(path, "rb");
    if (file == NULL ||
        check_header(file, path, header, line, error, error_capacity) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        free(counts);
        free(cursor);
        free(records);
        return -1;
    }
    row_count = 0;
    line_number = 1;
    for (;;) {
        uint32_t order_id;
        uint32_t product_id;
        uint32_t basket_index;
        uint64_t position;
        int status = read_csv_line(file, line, sizeof(line));
        if (status == 0) {
            break;
        }
        ++line_number;
        if (status < 0 || parse_product_row(line, &order_id, &product_id) != 0 ||
            order_id >= index->capacity || index->evals[order_id] != EVAL_PRIOR ||
            product_id > dataset->max_product_id || !dataset->valid_products[product_id]) {
            set_error(error, error_capacity, "%s:%llu invalid prior row on second pass", path,
                      (unsigned long long)line_number);
            fclose(file);
            free(counts);
            free(cursor);
            free(records);
            return -1;
        }
        basket_index = index->basket_indices[order_id];
        position = cursor[basket_index]++;
        dataset->baskets.products[position] = product_id;
        records[row_count].user_id = index->users[order_id];
        records[row_count].product_id = product_id;
        ++row_count;
    }
    fclose(file);
    dataset->prior_row_count = row_count;

    if (build_history(records, row_count, dataset->max_user_id + 1,
                      &dataset->history, error, error_capacity) != 0) {
        free(counts);
        free(cursor);
        free(records);
        return -1;
    }
    free(counts);
    free(cursor);
    free(records);
    return 0;
}

static int build_truth(UserProductRecord *records, uint64_t record_count,
                       uint32_t user_count, GroundTruth *truth,
                       char *error, size_t error_capacity)
{
    uint64_t *counts;
    uint64_t unique_count = 0;
    uint64_t i;

    qsort(records, (size_t)record_count, sizeof(*records), compare_user_product);
    counts = calloc(user_count, sizeof(*counts));
    if (counts == NULL) {
        set_error(error, error_capacity, "out of memory while counting train truth");
        return -1;
    }
    for (i = 0; i < record_count;) {
        uint64_t next = i + 1;
        while (next < record_count &&
               records[next].user_id == records[i].user_id &&
               records[next].product_id == records[i].product_id) {
            ++next;
        }
        ++counts[records[i].user_id];
        ++unique_count;
        i = next;
    }
    truth->user_count = user_count;
    truth->entry_count = unique_count;
    truth->offsets = calloc((size_t)user_count + 1, sizeof(*truth->offsets));
    truth->product_ids = malloc((size_t)unique_count * sizeof(*truth->product_ids));
    if (truth->offsets == NULL || (unique_count > 0 && truth->product_ids == NULL)) {
        free(counts);
        set_error(error, error_capacity, "out of memory while building train truth");
        return -1;
    }
    for (uint32_t user = 0; user < user_count; ++user) {
        truth->offsets[user + 1] = truth->offsets[user] + counts[user];
    }
    unique_count = 0;
    for (i = 0; i < record_count;) {
        uint64_t next = i + 1;
        while (next < record_count &&
               records[next].user_id == records[i].user_id &&
               records[next].product_id == records[i].product_id) {
            ++next;
        }
        truth->product_ids[unique_count++] = records[i].product_id;
        i = next;
    }
    free(counts);
    return 0;
}

static int load_train(const char *data_dir, Dataset *dataset,
                      const OrderIndex *index,
                      char *error, size_t error_capacity)
{
    static const char *header = "order_id,product_id,add_to_cart_order,reordered";
    char path[1024];
    char line[CSV_LINE_CAPACITY];
    FILE *file;
    UserProductRecord *records;
    uint64_t row_count = 0;
    uint64_t line_number = 1;

    if (build_path(path, sizeof(path), data_dir, "order_products__train.csv") != 0) {
        set_error(error, error_capacity, "train path is too long");
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_capacity, "cannot open %s", path);
        return -1;
    }
    if (check_header(file, path, header, line, error, error_capacity) != 0) {
        fclose(file);
        return -1;
    }
    for (;;) {
        uint32_t order_id;
        uint32_t product_id;
        int status = read_csv_line(file, line, sizeof(line));
        if (status == 0) {
            break;
        }
        ++line_number;
        if (status < 0 || parse_product_row(line, &order_id, &product_id) != 0 ||
            order_id >= index->capacity || index->evals[order_id] != EVAL_TRAIN ||
            product_id > dataset->max_product_id || !dataset->valid_products[product_id]) {
            set_error(error, error_capacity, "%s:%llu invalid train row", path,
                      (unsigned long long)line_number);
            fclose(file);
            return -1;
        }
        ++row_count;
    }
    fclose(file);

    records = malloc((size_t)row_count * sizeof(*records));
    if (row_count > 0 && records == NULL) {
        set_error(error, error_capacity, "out of memory while loading train truth");
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL ||
        check_header(file, path, header, line, error, error_capacity) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        free(records);
        return -1;
    }
    row_count = 0;
    for (;;) {
        uint32_t order_id;
        uint32_t product_id;
        int status = read_csv_line(file, line, sizeof(line));
        if (status == 0) {
            break;
        }
        if (status < 0 || parse_product_row(line, &order_id, &product_id) != 0 ||
            order_id >= index->capacity || index->evals[order_id] != EVAL_TRAIN ||
            product_id > dataset->max_product_id || !dataset->valid_products[product_id]) {
            fclose(file);
            free(records);
            set_error(error, error_capacity, "%s invalid train row on second pass", path);
            return -1;
        }
        records[row_count].user_id = index->users[order_id];
        records[row_count].product_id = product_id;
        ++row_count;
    }
    fclose(file);
    dataset->train_row_count = row_count;
    if (build_truth(records, row_count, dataset->max_user_id + 1,
                    &dataset->truth, error, error_capacity) != 0) {
        free(records);
        return -1;
    }
    free(records);
    return 0;
}

void dataset_free(Dataset *dataset)
{
    if (dataset == NULL) {
        return;
    }
    free(dataset->valid_products);
    free(dataset->baskets.offsets);
    free(dataset->baskets.products);
    free(dataset->baskets.users);
    free(dataset->baskets.order_ids);
    free(dataset->history.offsets);
    free(dataset->history.product_ids);
    free(dataset->history.frequencies);
    free(dataset->truth.offsets);
    free(dataset->truth.product_ids);
    memset(dataset, 0, sizeof(*dataset));
}

int dataset_validate(const Dataset *dataset,
                     char *error, size_t error_capacity)
{
    uint32_t i;
    if (dataset == NULL || dataset->valid_products == NULL) {
        set_error(error, error_capacity, "dataset or product table is null");
        return -1;
    }
    if (dataset->baskets.offsets == NULL || dataset->history.offsets == NULL ||
        dataset->truth.offsets == NULL) {
        set_error(error, error_capacity, "one or more CSR offset arrays are null");
        return -1;
    }
    for (i = 0; i < dataset->baskets.basket_count; ++i) {
        if (dataset->baskets.offsets[i] > dataset->baskets.offsets[i + 1]) {
            set_error(error, error_capacity, "basket offsets are not monotonic at %u", i);
            return -1;
        }
        if (dataset->baskets.users[i] > dataset->max_user_id) {
            set_error(error, error_capacity, "basket user out of range at %u", i);
            return -1;
        }
    }
    if (dataset->baskets.offsets[dataset->baskets.basket_count] !=
        dataset->baskets.product_count ||
        dataset->baskets.product_count != dataset->prior_row_count) {
        set_error(error, error_capacity, "basket product count mismatch");
        return -1;
    }
    for (uint64_t p = 0; p < dataset->baskets.product_count; ++p) {
        uint32_t product_id = dataset->baskets.products[p];
        if (product_id > dataset->max_product_id || !dataset->valid_products[product_id]) {
            set_error(error, error_capacity, "invalid basket product at %llu",
                      (unsigned long long)p);
            return -1;
        }
    }
    if (dataset->history.user_count != dataset->max_user_id + 1 ||
        dataset->truth.user_count != dataset->max_user_id + 1) {
        set_error(error, error_capacity, "user CSR dimensions do not match max_user_id");
        return -1;
    }
    for (i = 0; i < dataset->history.user_count; ++i) {
        if (dataset->history.offsets[i] > dataset->history.offsets[i + 1] ||
            dataset->truth.offsets[i] > dataset->truth.offsets[i + 1]) {
            set_error(error, error_capacity, "user offsets are not monotonic at %u", i);
            return -1;
        }
    }
    if (dataset->history.offsets[dataset->history.user_count] !=
            dataset->history.entry_count ||
        dataset->truth.offsets[dataset->truth.user_count] !=
            dataset->truth.entry_count) {
        set_error(error, error_capacity, "user CSR entry count mismatch");
        return -1;
    }
    return 0;
}

int dataset_load(const char *data_dir, Dataset *dataset,
                 char *error, size_t error_capacity)
{
    OrderIndex index = {0};
    BasketList prior_orders = {0};
    int result = -1;

    if (data_dir == NULL || dataset == NULL) {
        set_error(error, error_capacity, "data directory and dataset are required");
        return -1;
    }
    memset(dataset, 0, sizeof(*dataset));
    if (error != NULL && error_capacity > 0) {
        error[0] = '\0';
    }

    if (load_products(data_dir, dataset, error, error_capacity) != 0 ||
        load_orders(data_dir, dataset, &index, &prior_orders,
                    error, error_capacity) != 0 ||
        load_prior(data_dir, dataset, &index, &prior_orders,
                   error, error_capacity) != 0 ||
        load_train(data_dir, dataset, &index, error, error_capacity) != 0 ||
        dataset_validate(dataset, error, error_capacity) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    order_index_free(&index);
    basket_list_free(&prior_orders);
    if (result != 0) {
        dataset_free(dataset);
    }
    return result;
}
