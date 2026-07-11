# M10 第二十九轮：attr 内联桩进程级共享

日期：2026-07-11　分支：`dryrun/m10-shared-stubs`　基线：!77。
动机：中间带体检量化的指令缓存压力——单基准 500+ 编译函数、JIT 码
2.9MB、相对字节码膨胀 ×21.7，而 la/lm/sa 三套内联桩每函数各一份
（2-3KB），桩逻辑以 x0=cache 全参数化本与函数无关；950 级 64KB L1I
上宽热面负载的对症项。

## 一、实现（三步）

1. **压力槽迁移（共享前置）**：slow 尾的 IC 压力计数地址由发射期
   烘焙（&extra->ic_slow_pressure）迁为 cache 字段，LIR 生成器在
   allocate*Cache 时注入（unit 测试直构 HIR 的空 code 护栏保留），
   桩经 x0 取址、槽空跳过——桩体自此零 per-code 引用；
2. **共享发射与登记**：首个发射完整桩体的函数在 finalizeCode 后把
   三桩落定地址按家族 CAS 发布到 Context（部分家族未发射时各槽独立
   发布）；代码页由 bump 分配器进程级常驻，裸地址终身有效，无需钉
   宿主；
3. **跳板复用**：其后函数的桩 label 处只发射 mov+br 绝对跳转（沿用
   slow 尾 reg_scratch_br 惯例）。`jit-shared-attr-stubs` /
   `PYTHONJITSHAREDATTRSTUBS` 门控，默认开。

## 二、判据

- **码量（同二进制 env 双态）：总 JIT 码 4064KB→2777KB（−32%），
  中位函数 3488B→1720B（−51%），膨胀 ×21.8→×14.9**；
- 墙钟本机近中性偏正：sqlalchemy_imperative 三发全序 −1.5%，
  sqlglot/sympy/richards 组持平；暖机墙钟持平（发射非编译瓶颈）；
- 全集 v21（存档 full113-ab-v21-summary.json）：86 项零失败，几何
  1.062（v20 1.067，**逐项中位移动 +0.02% 零系统税**；档面差为
  async 噪声组回吐，asyncio_tcp/deltablue/richards 三项带外复测
  1.178/1.629/2.855 全部回带偏上）；attr 最重的目标组全部续涨：
  sqlalchemy_imperative 1.131→**1.142**、sqlglot_v2 1.063、
  sympy 1.013；
- 门禁：PGO 链、冒烟 14 件、diffgate 940 案 0 新增、libtest 26/46
  仅既档、RCM 六组双模全等。

## 三、950 复测建议

共享化的收益假设在弱核指令缓存（64KB L1I vs 本机 192KB）——建议
重点复测宽热面用例：sqlglot 族、sympy、async 族、bpe_tokeniser、
sqlalchemy 双例；若兑现显著，跳板可进一步升级为 b 直跳（±128MB
范围内省去 mov，需分配器相邻性保证，已记档）。

## 四、遗留

- 跳板为 mov+br（5 指令）；同代码区可退化为单条 b（范围保证待查）；
- lm 桩的 ia_ 实例方法位仍留 per-cache 字段读（本就 cache 内，无碍）；
- 剩余大项维持：被调方行内压栈、启动税专项、M6 恢复仪式。
