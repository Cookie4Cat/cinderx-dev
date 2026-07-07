#!/usr/bin/env bash
# PGO 三相构建的正式训练负载（M10 训练集扩容轮定稿）。
#
# 16 基准多形态混合（计算/调用密集原六 + 解析/模板/微负载组）+
# 语义冒烟，全程 JIT-on（auto=2）。实验矩阵结论（详见
# docs/dryrun/M10-pgo-trainset-log.md）：
# - 宽集较窄六项训练面板几何 +2.9pp、零回退（收益来自 JIT 侧
#   helper/IC/编译器在多形态下的画像改善）；
# - 解释器纯本底（auto=0 口径）对训练内容不敏感（0.857→0.858），
#   勿为 ceval 加解释态训练遍；
# - 混合双态训练（附加 auto=0 二遍）稀释 JIT 态共享代码画像，
#   实测净负，禁用。
#
# 用法：TRAIN_CMD="bash ci_pipeline/scripts/train_full_311.sh" 供
# build_pgo_lto_311.sh 相二调用。路径按预演容器缺省，可经环境覆盖。

PP=${TRAIN_PP:-/tmp/m9sc:/src/scratch/lib.linux-aarch64-cpython-311:/src/cinderx/PythonLib:/tmp/ppdeps}
PY=${TRAIN_PY:-/opt/python/cp311-cp311/bin/python3.11}
BM=${TRAIN_BM:-/tmp/pp113/pyperformance/data-files/benchmarks}
SMOKE=${TRAIN_SMOKE:-/src/docs/dryrun/smoke}
cd /tmp

run() { # $1=bench dir  $2=extra args
  PYTHONPATH=$PP PYTHONJITAUTO=2 $PY $BM/$1/run_benchmark.py \
    --inherit-environ PYTHONPATH,PYTHONJITAUTO -p1 -w0 -n1 $2 \
    -o /tmp/trf_$1.json >/dev/null 2>&1 || true
}

for b in bm_richards bm_deltablue bm_raytrace bm_go bm_generators \
         bm_sqlglot_v2 bm_django_template bm_pprint bm_mako bm_pathlib \
         bm_xml_etree bm_hexiom bm_deepcopy bm_docutils bm_coroutines; do
  run $b
done
run bm_pickle "--pure-python pickle"

for s in smoke_laggards smoke_ic_round smoke_frame_inline smoke_entry_guard smoke_m10_fixes; do
  PYTHONPATH=$PP PYTHONJITAUTO=2 $PY $SMOKE/$s.py >/dev/null 2>&1 || true
done
