#!/usr/bin/env bash
# cinderx 3.11 的 PGO+LTO 三相构建配方（M9 构建配置轮）。
#
# 在既有 CMake 构建树上就地执行三相：
#   ① ENABLE_PGO_GENERATE=ON 全量重编（插桩）；
#   ② 训练负载（调用方经 TRAIN_CMD 注入，建议:代表性基准 + 冒烟，
#      须正常退出——.gcda 于进程退出时落盘）；
#   ③ ENABLE_PGO_USE=ON 全量重编（-fprofile-use -fprofile-correction，
#      GCC 从对象目录就地读取 .gcda）。
# LTO（-flto -fuse-linker-plugin -ffat-lto-objects）全程保持开启。
#
# 用法：
#   BUILD_TREE=<cmake 构建树> TRAIN_CMD=<训练命令> ./build_pgo_lto_311.sh
# 复原为普通构建：
#   cmake -DENABLE_PGO_USE=OFF -DENABLE_PGO_GENERATE=OFF -DENABLE_LTO=OFF <树>
#
# 注意：
# - 三相须同一源码状态（源变更后须整套重跑；增量编译新 TU 缺 .gcda
#   仅降级为无 PGO 并告警，-fprofile-correction 兜底计数不一致）；
# - 训练负载决定优化倾向,建议覆盖:解释器循环(vendored ceval)、JIT
#   编译路径、IC/帧 helper——本轮取 19 基准中的代表 6 项 + 三套冒烟。

set -euo pipefail

BUILD_TREE=${BUILD_TREE:?need cmake build tree}
TRAIN_CMD=${TRAIN_CMD:?need training command}
CMAKE_BIN=${CMAKE_BIN:-cmake}
JOBS=${JOBS:-8}

cd "$BUILD_TREE"

echo "== 相一:插桩构建 =="
"$CMAKE_BIN" -DENABLE_LTO=ON -DENABLE_PGO_GENERATE=ON -DENABLE_PGO_USE=OFF . >/dev/null
make _cinderx -j"$JOBS"

echo "== 相二:训练 =="
bash -c "$TRAIN_CMD"
GCDA=$(find "$BUILD_TREE" -name '*.gcda' | wc -l)
echo "gcda: $GCDA"
[ "$GCDA" -gt 0 ] || { echo "训练未产生 profile"; exit 1; }

echo "== 相三:PGO-use 构建 =="
"$CMAKE_BIN" -DENABLE_PGO_GENERATE=OFF -DENABLE_PGO_USE=ON . >/dev/null
make _cinderx -j"$JOBS"
echo "done: $(md5sum "$BUILD_TREE"/../lib.linux-*/_cinderx.so 2>/dev/null || true)"

echo "== 相四:产物正确性验收 =="
# 历史（M10 PGO 根因专项已收口）：曾出现 profile 依赖的概率性红态
# （JIT 编译码 raise 丢异常，import enum 触发 SystemError），根因为
# deopt 垫片第四实参装配的 3.11 版本门缺陷（generator.cpp，非工具链
# 问题），已修复。本验收步保留为构建体系常设防线：产物必须通过快速
# 正确性探针方可交付；失败保留 gcda 取证并整链报错。
VERIFY_CMD=${VERIFY_CMD:-}
VERIFY_PY=${VERIFY_PY:-/opt/python/cp311-cp311/bin/python3.11}
VERIFY_PP=${VERIFY_PP:-/tmp/m9sc:/src/scratch/lib.linux-aarch64-cpython-311:/src/cinderx/PythonLib}
if [ -z "$VERIFY_CMD" ]; then
  # 快速兜底探针：覆盖 import 期与若干 raise 密集 stdlib 模块。注意：
  # 这是快速兜底，非完整正确性验收——交付构建仍须过完整 diffgate +
  # libtest 快速档（SR3 门禁），相四仅拦最粗红态。探针模块须全部为
  # JIT 基线绿（test_builtin 属既有基线分歧项，不得入探针，否则绿链
  # 被恒判红）。
  VERIFY_CMD="PYTHONPATH=$VERIFY_PP PYTHONJITAUTO=2 $VERIFY_PY -m test \
    test_slice test_exceptions test_raise test_int \
    -q >/dev/null 2>&1 && PYTHONPATH=$VERIFY_PP PYTHONJITAUTO=2 $VERIFY_PY \
    -c 'import enum, re, dataclasses, typing, functools; print(1)'"
fi
if ! bash -c "$VERIFY_CMD" >/dev/null 2>&1; then
  STAMP=$(date +%s)
  tar czf "/tmp/pgo-red-$STAMP.tgz" $(find "$BUILD_TREE" -name '*.gcda') 2>/dev/null || true
  cp "$BUILD_TREE"/../lib.linux-*/_cinderx.so "/tmp/pgo-red-$STAMP.so" 2>/dev/null || true
  echo "相三产物未过正确性探针：红态 gcda 与 .so 已归档 /tmp/pgo-red-$STAMP.*"
  exit 1
fi
echo "verify: OK"
