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

.PHONY: all debug release smoke clean

all: debug

debug: build/basket_recommender_debug.exe

release: build/basket_recommender.exe

smoke: build/omp_smoke.exe
	./build/omp_smoke.exe 4

build/basket_recommender_debug.exe: $(APP_SOURCES)
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LDLIBS)

build/basket_recommender.exe: $(APP_SOURCES)
	$(CC) $(CFLAGS_RELEASE) $^ -o $@ $(LDLIBS)

build/omp_smoke.exe: tests/omp_smoke.c
	$(CC) $(CFLAGS_DEBUG) $^ -o $@ $(LDLIBS)

clean:
	-del /Q build\*.exe 2>NUL || exit 0
