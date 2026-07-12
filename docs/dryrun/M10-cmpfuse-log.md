# M10 COMPARE_OP 融合轮 + 系统工程轮合志

分支 dryrun/m10-syseng,六提交(大页对齐 87b16cbef、COMPARE 融合
d54970d0b/cab0c2a9d/9e0393e8b/e116342df、普查随日志)。

## 一、系统工程轮裁决(五件)

⑤特化形普查:56 形覆盖 24,肥缺口三块(COMPARE_OP_*_JUMP/
PRECALL_NO_KW/STORE_SUBSCR_LIST_INT);③大页:修复自始未生效的
2MiB 对齐坑(匿名 mmap 不对齐,THP 零回填→修后 2048kB 全额;
本机中性归 950 待验;PYTHONJITHUGEPAGES=0 切换整分配器与常驻页
假设不兼容,历史即坏勿用);②mimalloc 证伪(obmalloc 兜底,
genshi 反 +3~5%);①BOLT 转 openEuler 950 项(el8 glibc 挡,
-Wl,--emit-relocs 配方已验);④watchers 立项未动。

## 二、COMPARE_OP 融合:三层断点考古

1. specializedOpcode() 放行白名单只列 3.12+ 名(COMPARE_OP_FLOAT/
   INT/STR),3.11 _JUMP 三形被打回生形;
2. 白名单补丁误嵌 >=3.12 守卫内被预处理吃掉——发射点插桩定罪
   (同 code 同索引 rawunit=0x1c 而 spec=107);
3. 守卫上既判力注释:提前 quickening 轮 raytrace 191.8 vs 124.6
   摘除全族——该判决成立于 despec 诞生同轮,构成受控重测条件。

修通后探针 HIR:GuardType×2 + PrimitiveCompare×2,泛型
RichCompare 全消。despec 武装态重测:raytrace 0.11107 vs 0.11129
风暴未复现(旧判决灾难条件已不成立)。

## 三、v26 定价与 STR 摘除

三形全开 v26:数值族净正(float +3.8%/richards +2.4%),字符串
比较密集族回归(tomli_loads −14%/django_template −5%)——STR 形
UnicodeExact 守卫在 str-vs-非str 混型位点 deopt 抖动,净账为负。
摘除 STR、保留 INT/FLOAT;单项复核回归清零。

## 四、v27 终价(存档 full113-ab-v27-summary.json)

86 项零失败,几何 **1.053→1.054(战役新高)**,≥1.0 62 项,
vs v25 逐项中位 1.0002(零系统税)。float 1.191→**1.259
(+5.7%)**、nbody +1.9%、telco 0.953→0.987;tomli_loads 1.358/
django 1.101 回带。pickle_pure_python 0.881(带外偏低,其历史
±3% 摆动带,950 观察项)。门禁:冒烟 16/16、libtest 0 新增、
PGO 链验收 OK。

## 五、遗留

普查另两缺口(PRECALL_NO_KW 家族/STORE_SUBSCR_LIST_INT)为后续
实现候选;STR 形若做需先解混型位点判据(如按 despec 记账放行);
watchers 回迁立项件;BOLT@950。
