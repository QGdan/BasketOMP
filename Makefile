CC := gcc
CFLAGS_COMMON := -std=c11 -Wall -Wextra -Wpedantic -D__USE_MINGW_ANSI_STDIO=1 -fopenmp -Iinclude
CFLAGS_DEBUG := $(CFLAGS_COMMON) -O0 -g
CFLAGS_RELEASE := $(CFLAGS_COMMON) -O2 -DNDEBUG
LDLIBS := -lm
APP_SOURCES := \
	src/main.c \
	src/csv_loader.c \
	src/pair_hash.c \
	src/cooccurrence_serial.c \
	src/cooccurrence_openmp.c \
	src/recommender_serial.c \
	src/recommender_openmp.c \
	src/evaluator.c

TEST_COOCCUR_SOURCES := \
	src/csv_loader.c \
	src/pair_hash.c \
	src/cooccurrence_serial.c \
	src/cooccurrence_openmp.c

TEST_RECOMMEND_SOURCES := \
	$(TEST_COOCCUR_SOURCES) \
	src/recommender_serial.c \
	src/recommender_openmp.c

.PHONY: all debug release smoke clean
.PHONY: test-loader test-hash test-cooccur test-recommender test-evaluator test-integration

all: debug release smoke test-loader test-hash test-cooccur test-recommender test-evaluator test-integration

debug: build/basket_recommender_debug

release: build/basket_recommender

smoke: build/omp_smoke
	./build/omp_smoke 4

test-loader: build/test_loader
test-hash: build/test_pair_hash
test-cooccur: build/test_cooccurrence
test-recommender: build/test_recommender
test-evaluator: build/test_evaluator
test-integration: build/test_integration

# ── 主程序 ──────────────────────────────
build/basket_recommender_debug: $(APP_SOURCES)
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LDLIBS)

build/basket_recommender: $(APP_SOURCES)
	$(CC) $(CFLAGS_RELEASE) $^ -o $@ $(LDLIBS)

# ── 测试程序 ────────────────────────────
build/omp_smoke: tests/omp_smoke.c
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LDLIBS)

build/test_loader: tests/test_loader.c src/csv_loader.c
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LDLIBS)

build/test_pair_hash: tests/test_pair_hash.c src/pair_hash.c
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LDLIBS)

build/test_cooccurrence: tests/test_cooccurrence.c $(TEST_COOCCUR_SOURCES)
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LDLIBS)

build/test_recommender: tests/test_recommender.c $(TEST_RECOMMEND_SOURCES)
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LDLIBS)

build/test_evaluator: tests/test_evaluator.c $(TEST_COOCCUR_SOURCES) src/recommender_serial.c src/evaluator.c
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LDLIBS)

build/test_integration: tests/test_integration.c $(TEST_RECOMMEND_SOURCES)
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LDLIBS)

# ── 跨平台 clean：Git Bash / Linux / macOS 均可用 ──
clean:
	-rm -f build/*.exe build/*.out build/basket_recommender_debug build/basket_recommender build/omp_smoke build/test_* 2>/dev/null
