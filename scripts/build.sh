#!/usr/bin/env bash
#
# build.sh — OpenMP Basket Recommender 跨平台构建脚本
#
# 用法:
#   bash scripts/build.sh [目标]
#
# 目标:
#   debug    (默认)  调试版 basket_recommender_debug
#   release         优化版 basket_recommender (用于性能实验)
#   smoke           OpenMP 运行时冒烟测试
#   test-loader     数据加载测试
#   test-hash       哈希表测试
#   test-cooccur    共现统计测试
#   test-recommender 推荐测试
#   test-evaluator  评估测试
#   test-integration 集成测试
#   all             所有测试 + debug + release
#
# 环境变量:
#   CC              指定编译器 (默认自动检测)
#   CFLAGS_EXTRA    额外编译标志
#
# 示例:
#   bash scripts/build.sh release
#   CC=clang bash scripts/build.sh debug

set -euo pipefail

# ── 项目根目录 ──────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

# ── 编译器检测 ──────────────────────────────────
: "${CC:=}"
if [ -z "$CC" ]; then
    if command -v gcc &>/dev/null; then
        CC=gcc
    elif command -v clang &>/dev/null; then
        CC=clang
    else
        echo "错误: 未找到 gcc 或 clang 编译器" >&2
        exit 1
    fi
fi

echo "编译器: $CC ($($CC --version | head -1))"

# ── 编译标志 ────────────────────────────────────
# 检测是否为 MinGW (Windows)
if $CC -dumpmachine 2>/dev/null | grep -q mingw; then
    MINGW_DEFINE="-D__USE_MINGW_ANSI_STDIO=1"
    EXE_SUFFIX=".exe"
else
    MINGW_DEFINE=""
    EXE_SUFFIX=""
fi

COMMON_FLAGS="-std=c11 -Wall -Wextra -Wpedantic -fopenmp -I$PROJECT_ROOT/include $MINGW_DEFINE ${CFLAGS_EXTRA:-}"
DEBUG_FLAGS="-O0 -g"
RELEASE_FLAGS="-O2 -DNDEBUG"

APP_SOURCES=(
    "$PROJECT_ROOT/src/main.c"
    "$PROJECT_ROOT/src/csv_loader.c"
    "$PROJECT_ROOT/src/pair_hash.c"
    "$PROJECT_ROOT/src/cooccurrence_serial.c"
    "$PROJECT_ROOT/src/cooccurrence_openmp.c"
    "$PROJECT_ROOT/src/recommender_serial.c"
    "$PROJECT_ROOT/src/recommender_openmp.c"
    "$PROJECT_ROOT/src/evaluator.c"
)

# ── 辅助函数 ────────────────────────────────────
_mk_build_dir() {
    mkdir -p "$BUILD_DIR"
}

_compile() {
    local target_name="$1"
    local flags="$2"
    shift 2
    echo "  CC  $target_name"
    $CC $flags "$@" -o "$BUILD_DIR/$target_name$EXE_SUFFIX" -lm
}

# ── 构建目标 ────────────────────────────────────
build_debug() {
    _mk_build_dir
    _compile "basket_recommender_debug" "$COMMON_FLAGS $DEBUG_FLAGS" "${APP_SOURCES[@]}"
}

build_release() {
    _mk_build_dir
    _compile "basket_recommender" "$COMMON_FLAGS $RELEASE_FLAGS" "${APP_SOURCES[@]}"
}

build_smoke() {
    _mk_build_dir
    _compile "omp_smoke" "$COMMON_FLAGS $DEBUG_FLAGS" \
        "$PROJECT_ROOT/tests/omp_smoke.c"
}

build_test_loader() {
    _mk_build_dir
    _compile "test_loader" "$COMMON_FLAGS $DEBUG_FLAGS" \
        "$PROJECT_ROOT/tests/test_loader.c" \
        "$PROJECT_ROOT/src/csv_loader.c"
}

build_test_hash() {
    _mk_build_dir
    _compile "test_pair_hash" "$COMMON_FLAGS $DEBUG_FLAGS" \
        "$PROJECT_ROOT/tests/test_pair_hash.c" \
        "$PROJECT_ROOT/src/pair_hash.c"
}

build_test_cooccur() {
    _mk_build_dir
    _compile "test_cooccurrence" "$COMMON_FLAGS $DEBUG_FLAGS" \
        "$PROJECT_ROOT/tests/test_cooccurrence.c" \
        "$PROJECT_ROOT/src/csv_loader.c" \
        "$PROJECT_ROOT/src/pair_hash.c" \
        "$PROJECT_ROOT/src/cooccurrence_serial.c" \
        "$PROJECT_ROOT/src/cooccurrence_openmp.c"
}

build_test_recommender() {
    _mk_build_dir
    _compile "test_recommender" "$COMMON_FLAGS $DEBUG_FLAGS" \
        "$PROJECT_ROOT/tests/test_recommender.c" \
        "$PROJECT_ROOT/src/csv_loader.c" \
        "$PROJECT_ROOT/src/pair_hash.c" \
        "$PROJECT_ROOT/src/cooccurrence_serial.c" \
        "$PROJECT_ROOT/src/cooccurrence_openmp.c" \
        "$PROJECT_ROOT/src/recommender_serial.c" \
        "$PROJECT_ROOT/src/recommender_openmp.c"
}

build_test_evaluator() {
    _mk_build_dir
    _compile "test_evaluator" "$COMMON_FLAGS $DEBUG_FLAGS" \
        "$PROJECT_ROOT/tests/test_evaluator.c" \
        "$PROJECT_ROOT/src/csv_loader.c" \
        "$PROJECT_ROOT/src/pair_hash.c" \
        "$PROJECT_ROOT/src/cooccurrence_serial.c" \
        "$PROJECT_ROOT/src/recommender_serial.c" \
        "$PROJECT_ROOT/src/evaluator.c"
}

build_test_integration() {
    _mk_build_dir
    _compile "test_integration" "$COMMON_FLAGS $DEBUG_FLAGS" \
        "$PROJECT_ROOT/tests/test_integration.c" \
        "$PROJECT_ROOT/src/csv_loader.c" \
        "$PROJECT_ROOT/src/pair_hash.c" \
        "$PROJECT_ROOT/src/cooccurrence_serial.c" \
        "$PROJECT_ROOT/src/cooccurrence_openmp.c" \
        "$PROJECT_ROOT/src/recommender_serial.c" \
        "$PROJECT_ROOT/src/recommender_openmp.c"
}

# ── 主入口 ──────────────────────────────────────
TARGET="${1:-debug}"

case "$TARGET" in
    debug)
        build_debug
        ;;
    release)
        build_release
        ;;
    smoke)
        build_smoke
        ;;
    test-loader)
        build_test_loader
        ;;
    test-hash)
        build_test_hash
        ;;
    test-cooccur)
        build_test_cooccur
        ;;
    test-recommender)
        build_test_recommender
        ;;
    test-evaluator)
        build_test_evaluator
        ;;
    test-integration)
        build_test_integration
        ;;
    all)
        build_debug
        build_release
        build_smoke
        build_test_loader
        build_test_hash
        build_test_cooccur
        build_test_recommender
        build_test_evaluator
        build_test_integration
        ;;
    *)
        echo "未知目标: $TARGET" >&2
        echo "可用目标: debug release smoke test-loader test-hash test-cooccur test-recommender test-evaluator test-integration all" >&2
        exit 2
        ;;
esac

echo "构建完成: $TARGET"
