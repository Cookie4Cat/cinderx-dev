# M9 优化第七轮：go 三件套③（编译态劣化判定）与 values 守卫分支化终评

日期：2026-07-05　分支：`dryrun/m9-probation`　基线：三件套①②轮
（几何均值 0.874x；go 0.549 为唯一净负项）

## 一、方案 A：试用期计时判定（实现→实测否决→降级研究旋钮）

设计：编译完成后前 2K 次调用在入口包装器（3.11 编译入口本就是
`recursionGuardedVectorcall` 共享包装，probation 寄生其中、不触碰
M6 入口身份不变量）内交替走解释/编译两臂并计时，等样本均时对比
裁决。三轮迭代与三个否决性事实：

1. **并发试用交叉污染**：预热期整棵调用树同时试用，调用方的编译臂
   内嵌被调方的解释臂（反之亦然），裁决近随机——richards 被误冻至
   24.6ms（-50%）。全局令牌串行化后 richards 恢复。
2. **亚微秒计时噪声**：clock_gettime 开销与量化噪声支配 100-500ns
   级调用的逐次计时；32 次同臂连续调用成块采样+首块弃权仍不足。
3. **局部贪心 vs 全局混合成本错配（根本性）**：deltablue 全编译
   整体 0.93x，而按函数局部裁决冻结后混合态 0.79x——逐函数最优
   ≠ 整体最优（模式切换税+缓存分裂）；且解释臂在试用期内持续
   特化、越测越快，基线本身漂移。

判决：计时试用在此粒度不可行，保留为默认关的研究旋钮
（CINDERX_AUTOJIT_PROBATION / _MARGIN）。

## 二、方案 B：IC 压力密度冻结（生产判据，默认启用）

依据计数矩阵实测的真判别量：**每调用 helper 慢路径密度**——go 型
（天生物化+类默认值遮蔽）≈11/调用，richards/deltablue ≈1.5-1.7/
调用，量级分离。实现三件：

- **CodeExtra.ic_slow_pressure**：三个内联 stub 的慢尾以 **LSE
  stadd 单指令原子加**直增（发射期烘焙地址；曾用 ldr/add/str 三连，
  miss 密集而密度未超阈的负载白付 ~3-6%，stadd 后归零）；
- **入口包装器采样裁决**：全局节拍 1/16 采样进入策略路径（每调用
  热路径预算 = 一次自增+掩码测试；曾逐调用查 codeExtra，
  call 密集负载 +6-9% 被基线对照抓获），采样计调用×16 折算，每
  折算 4096 次调用对比窗内压力，密度超阈（默认 4，
  CINDERX_AUTOJIT_IC_PRESSURE_RATIO）即经 probationFreeze 卸载
  全部关联函数并置 ROI FROZEN + DecidedCold 位；
- **probationFreeze**：复用 ROI backoff 的冻结原语，jitVectorcall
  既有冻结检查使该 code 长期解释执行。

## 三、values 守卫分支化终评：不予落地（二审）

重落地实测：go 125ms vs 冻结均衡 91ms——**stub 行内物化直读使
miss 不再流经慢尾，密度信号随 deopt 风暴一起消失**，go 型负载失去
全部冻结兜底；deltablue/raytrace 本次收益在方差带内。再评前提写入
代码注释：帧/调用协议税显著下降，或密度信号改从行内命中处采集。
当前 Guard-deopt→ROI backoff 即该形态的正确均衡。

## 四、量化（进程内稳态；当日基线：deltablue 1.76/raytrace 155.5/
richards 15.45——跨构建代码布局漂移使跨日绝对值不可直比）

| 基准 | 无策略 | 密度冻结（终态） |
|---|---|---|
| go | 117.4ms | **90.3ms（-23%，历史最佳）** |
| richards | 15.45ms | 15.28ms（持平） |
| deltablue | 1.76ms | 1.82ms（带内） |
| raytrace | 155.5ms | 158.2ms（带内） |

19 基准 A/B（auto=2/w3/p3v5，19/19 全绿，存档 m9prob-ab-summary.json
+ 复测 m9prob-ab2-rerun.json）：几何均值 0.874→0.863（方差带内；
deltablue/richards/pickle/chaos 首跑离群复测回带 0.883/1.468/
0.628/0.901，richards_super 1.802 与 richards 1.380 互为镜像波动）。
**go A/B 增益被 pyperf 新进程协议稀释**（密度冻结需每 code ~4k 次
真实调用才触发，短测量窗内未充分显效；进程内持续负载 117→90ms
为其真实价值口径）。

## 五、门禁

diffgate 923 全绿；generators 语料/基础 libtest 差分基线原样；
refcount 矩阵五组零漂移；两轮冒烟复跑；3.14 反向编译（ninja）+
attr/method/store/module 冒烟通过（probation/密度均 <0x030C 门内；
stadd 为 stub 发射代码共享文件但受版本门与 ratio 旋钮双重控制）。

## 六、方法论沉淀

- **自适应策略的评审基线必须同日同构建**：跨构建代码布局漂移可达
  ±6%，先量基线再量策略；
- **每调用热路径预算意识**：入口包装器加一次哈希查表即 -6~9%
  （call 密集负载），策略工作必须采样化+静态旗标短路；
- **慢路径插桩用 LSE stadd**（单指令、免 load-use 停顿）；
- **换库前必杀跑动中的 A/B**（本会话第二次踩，教训升级为红线）。

## 七、遗留

- go 终态 0.55→预计 ~0.66（回到冻结均衡）；进一步需帧/调用协议轴
  （generators 轮同源）；
- 密度信号盲区：行内命中不计数（正确——但也是分支化的再评障碍）；
- probation_frozen 统计未接入 get_and_clear 导出（低优先）。
