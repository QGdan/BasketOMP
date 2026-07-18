#!/usr/bin/env bash
#
# run_correctness.sh — OpenMP 购物篮推荐正确性回归脚本（Linux/macOS 版）
#
# 用法:
#   bash scripts/run_correctness.sh              # 使用 data/toy
#   bash scripts/run_correctness.sh data/small   # 使用 data/small

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
DATA_DIR="${1:-$PROJECT_ROOT/data/toy}"

# OpenMP 线程绑定
export OMP_PROC_BIND=spread
export OMP_PLACES=cores

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass_count=0
fail_count=0

_check() {
    local desc="$1"
    shift
    if "$@" > /dev/null 2>&1; then
        echo -e "  ${GREEN}✓${NC} $desc"
        ((pass_count++))
    else
        echo -e "  ${RED}✗${NC} $desc"
        ((fail_count++))
    fi
}

echo "── 编译测试 ───────────────────────────────"
cd "$PROJECT_ROOT"
bash "$SCRIPT_DIR/build.sh" all

echo ""
echo "── 单元测试 ───────────────────────────────"
_check "loader (toy)"       "$BUILD_DIR/test_loader"      "$PROJECT_ROOT/data/toy"
_check "loader (small)"     "$BUILD_DIR/test_loader"      "$PROJECT_ROOT/data/small"
_check "pair_hash"          "$BUILD_DIR/test_pair_hash"
_check "cooccurrence (toy)" "$BUILD_DIR/test_cooccurrence" "$PROJECT_ROOT/data/toy"
_check "cooccurrence (small)" "$BUILD_DIR/test_cooccurrence" "$PROJECT_ROOT/data/small"
_check "recommender (toy)"  "$BUILD_DIR/test_recommender" "$PROJECT_ROOT/data/toy"
_check "recommender (small)" "$BUILD_DIR/test_recommender" "$PROJECT_ROOT/data/small"
_check "evaluator"          "$BUILD_DIR/test_evaluator"
_check "integration (toy)"  "$BUILD_DIR/test_integration" "$PROJECT_ROOT/data/toy"
_check "integration (small)" "$BUILD_DIR/test_integration" "$PROJECT_ROOT/data/small"

echo ""
echo "── 端到端校验和测试 ───────────────────────"

SERIAL_OUT=$("$BUILD_DIR/basket_recommender_debug" --data "$PROJECT_ROOT/data/toy" --mode serial --top-k 10 2>&1)
PARALLEL_OUT=$("$BUILD_DIR/basket_recommender_debug" --data "$PROJECT_ROOT/data/toy" --mode openmp --threads 4 --top-k 10 2>&1)

SERIAL_CHECKSUM=$(echo "$SERIAL_OUT" | grep '^recommendation_checksum=' | cut -d= -f2)
PARALLEL_CHECKSUM=$(echo "$PARALLEL_OUT" | grep '^recommendation_checksum=' | cut -d= -f2)

if [ "$SERIAL_CHECKSUM" = "$PARALLEL_CHECKSUM" ] && [ -n "$SERIAL_CHECKSUM" ]; then
    echo -e "  ${GREEN}✓${NC} 串/并行校验和一致: $SERIAL_CHECKSUM"
    ((pass_count++))
else
    echo -e "  ${RED}✗${NC} 校验和不一致: serial=$SERIAL_CHECKSUM parallel=$PARALLEL_CHECKSUM"
    ((fail_count++))
fi

# 硬编码校验和：仅警告
EXPECTED_CHECKSUM="6286369242441534757"
if [ "$SERIAL_CHECKSUM" != "$EXPECTED_CHECKSUM" ]; then
    echo -e "  ${YELLOW}⚠${NC} toy 基线校验和已变更"
    echo -e "     预期: $EXPECTED_CHECKSUM"
    echo -e "     实际: $SERIAL_CHECKSUM"
    echo -e "     如果确认算法改动正确，请更新此脚本中的 EXPECTED_CHECKSUM"
fi

# Top-N 一致性检查
LIMITED_SERIAL=$("$BUILD_DIR/basket_recommender_debug" --data "$PROJECT_ROOT/data/toy" --mode serial --top-k 10 --max-neighbors 1 2>&1)
LIMITED_PARALLEL=$("$BUILD_DIR/basket_recommender_debug" --data "$PROJECT_ROOT/data/toy" --mode openmp --threads 4 --top-k 10 --max-neighbors 1 --cooccur-schedule guided --cooccur-chunk 4 --recommend-schedule guided --recommend-chunk 2 2>&1)
LIMITED_SERIAL_CS=$(echo "$LIMITED_SERIAL" | grep '^recommendation_checksum=' | cut -d= -f2)
LIMITED_PARALLEL_CS=$(echo "$LIMITED_PARALLEL" | grep '^recommendation_checksum=' | cut -d= -f2)

if [ "$LIMITED_SERIAL_CS" = "$LIMITED_PARALLEL_CS" ] && [ -n "$LIMITED_SERIAL_CS" ]; then
    echo -e "  ${GREEN}✓${NC} Top-N 串/并行校验和一致: $LIMITED_SERIAL_CS"
    ((pass_count++))
else
    echo -e "  ${RED}✗${NC} Top-N 校验和不一致"
    ((fail_count++))
fi

# 无效参数应报错
if ! "$BUILD_DIR/basket_recommender_debug" --data "$PROJECT_ROOT/data/toy" --mode openmp --threads 2 --recommend-chunk 0 >/dev/null 2>&1; then
    echo -e "  ${GREEN}✓${NC} 无效参数正确报错"
    ((pass_count++))
else
    echo -e "  ${RED}✗${NC} 无效参数未报错"
    ((fail_count++))
fi

echo ""
echo "──────────────────────────────────────────"
echo -e "  通过: ${GREEN}$pass_count${NC}  失败: ${RED}$fail_count${NC}"
if [ "$fail_count" -eq 0 ]; then
    echo -e "  ${GREEN}PASS: 正确性回归全部通过${NC}"
    exit 0
else
    echo -e "  ${RED}FAIL: $fail_count 项未通过${NC}"
    exit 1
fi
