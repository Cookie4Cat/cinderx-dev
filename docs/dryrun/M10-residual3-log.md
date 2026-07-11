# M10 sqla 残项与落后带验尸轮(lm 物化救援 + 异常慢性户冻结)

分支 dryrun/m10-sqla-residual3,四提交。

## 一、lm 桩物化受者救援直判(7fc2fb5dc)

LoadMethod 桩原对已物化受者一律回落 helper(sqlalchemy 天生物化,
lm_helper 占桩流量 59%)。桩内补三段镜像 helper 判据:非管理字典
受者安全回落(preheader 越界解引用防护)、无字典/物化字典键版本
一致直接有效(大头)、版本不等走 ia_hint 遮蔽直判。直方图:
lm_helper 52.4万→30.6万(−41.6%);残余 9.7万=cache 级单 hint
多态抖动(per-entry hint 为后续项)。

## 二、异常慢性户终局冻结(4290d14f0/0fee671a7/853b73432)

argparse_subparsers 编译净负 +23% 验尸链:三态分诊(B0/B4 交错)
→ 全进程 perf 误导(预热期编译器 27% 污染)→ 稳态 attach perf
定罪:B4 稳态解释器份额 32.6%,异常惯用形函数编译后每异常付
deopt 帧重建+解释续跑的两头费。修复:roi 回退记账收窄(仅守卫
失败走渐进轮次)+ UnhandledException 计数过阈(64)且采样率≥5%
时置末轮一次冻结(异常出口无守卫可摘,渐进轮次只会卸载-重编
振荡)+ 率判据防偶发异常累积误冻。

受控验证(长预热稳态):B4 0.01815 vs B0 0.01849,+23% 清零
反超 1.8%。pyperf 单项:0.961→0.985(普通构建);v23 全跑
0.963(单项波动带内,受控收据为准)。

## 三、门禁与全集

冒烟 15/15;libtest 46 模块 0 新增(2 转好);PGO 链验收 OK。
v23(存档 full113-ab-v23-summary.json):86 项零失败,几何
1.038→**1.046**,≥1.0 53→58;sqlalchemy_imperative **1.168
新高**、pickle_pure_python 0.907→0.931(lm 实例方法位受益)、
dulwich_log 0.953→0.993、genshi/coverage/deepcopy/sphinx 全升。

## 四、遗留

lm per-entry hint(9.7万残余);sa kind-6 成员写;kwnames 绑定
缓存;telco/websockets DSO 定罪;pathlib/concurrent_imap 补测;
方法论:全进程 perf 判编译税必须 warmup-attach 稳态窗口。
