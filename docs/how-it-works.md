# SBParry 工作原理

[English](how-it-works.en.md) | **中文**

本文面向想读代码、移植到新版本游戏或做类似工具的人。偏移和特征码对应 2026-08 的 Steam 版（UE4.26）；源码里的定义（`src/game.h`、`src/hooks.asm`）以代码为准。文中的实测数据除特别说明外都来自渡鸦 (Raven) Boss。

## 总体结构

```
 ┌──────────────────────── SB-Win64-Shipping.exe（游戏进程）───────────────────────┐
 │                                                                                 │
 │  UE4SS + SBParryBridge (Lua)                  3 个代码钩子（code cave）          │
 │   ├─ SkillActiveStepTable ─┐                   ├─ IsJustActionActive 入口       │
 │   ├─ Effect / Projectile  ─┤                   ├─ 按下：写 inst+BC 处           │
 │   │  / TargetFilter 表     │                   └─ 步骤切换：mov [r13+B4],ecx    │
 │   ├─ PlayerController、    │                          │ lock xadd               │
 │   │  血条、QTE 控件、      │                          ▼                         │
 │   │  Groggy 偏移           │                   环形缓冲区（256 × 0x30）         │
 │   ├─ SBProjectile 对象池   │                                                    │
 │   └─ InputSettings 键位    │                  手柄导入表槽位                    │
 │                             │                   XInputGetState / scePadReadState │
 │                             │                          │ → 小桩：叠加模拟按键     │
 │                             │                          ▼                         │
 │                             │                   PadCtrl 控制块（在 cave 里）      │
 └─────────────────────────────┼──────────────────────────┬────────────────────────┘
                               │ 文件                     │ ReadProcessMemory /
                               ▼                          │ WriteProcessMemory
       ue4ss/Mods/SBParryBridge/                          │
         steps.tsv  live.txt  projectiles.txt  keys.txt   │
                               │                          │
                               ▼                          ▼
 ┌──────────────────────────────── sbparry.exe（独立进程）────────────────────────┐
 │  tracker：读事件、结算判定、按步骤表推算音符、校准（calib.tsv）、               │
 │           未应对 / 掉血记录、一击必杀                                           │
 │  projectiles：每帧读飞行道具位置，算到达时刻                                    │
 │  live：玩家 / 敌人 / 相机、血条、可惩戒状态、过场 QTE                           │
 │  auto：弹反 / 闪避 / 跳跃 / 惩戒 / 连打（键盘 SendInput 或写 PadCtrl）          │
 │  overlay：分层透明窗口（鼠标穿透）画判定条 / 统计面板 / 提示                    │
 │  托盘图标、全局快捷键、sbparry.ini、日志 sbparry.log                            │
 └─────────────────────────────────────────────────────────────────────────────────┘
```

- sbparry.exe 是外部进程，不往游戏里注入 DLL（UE4SS 本身除外）。
- 游戏里只有三段很短的钩子代码，往环形缓冲区追加事件；以及两个手柄导入表槽位的重定向。其余全部靠读内存；唯一写游戏数据的地方是一击必杀（改堆上的步骤表，见下文）。
- UE4SS 只用来跑 Bridge：技能步骤表、效果表、道具表、键位、各种对象地址和字段偏移这些只能方便地通过 UE 反射拿到的数据由它导出成文本文件。
- sbparry.exe 是窗口子系统程序，没有控制台。日志写在程序目录下的 `sbparry.log`，不论界面语言一律英文（`Log(...)` 的参数在 `EnglishScope` 里求值，其中的 `TR` 都取英文）；托盘菜单的“打开日志文件”用系统默认的文本编辑器打开它（打不开就用记事本）。`sbparry.exe --quit` 通知正在运行的实例还原游戏代码后退出。
- 游戏窗口不在前台时判定条和统计面板隐藏，不盖在别的程序上。Ctrl+Alt 快捷键有注册失败的（被别的程序占了），启动时弹提示列出。

| 源文件 | 职责 |
|---|---|
| `game.cpp` + `hooks.asm` | 附加游戏进程、安装 / 还原钩子和手柄桩 |
| `tracker.cpp` | 步骤表 → 音符预测；事件 → 完美判定、蓝紫光判定；未应对、掉血记录；一击必杀；`timing.csv` |
| `projectiles.cpp` | 飞行道具实时追踪 |
| `live.cpp` | 顺着 PlayerController 读伊芙 / 敌人 / 相机；血条、可惩戒状态、过场 QTE 控件、时间流速 |
| `autoplay.cpp` | 自动弹反 / 闪避 / 跳跃 / 蓝紫光 / 惩戒 / 连打与 QTE |
| `overlay.cpp` | 绘制 |
| `main.cpp` | 主循环（每个 DWM 合成帧一次）、快捷键、托盘 |

## 钩子

| 名称 | 位置 | 被替换的指令 | 记录（kind） |
|---|---|---|---|
| 判定 | `IsJustActionActive(inst, attacker)` 函数入口 | `mov [rsp+8],rbx`（5 字节） | 1：inst、attacker、inst+B8 剩余、inst+BC 总长、inst+58 技能行 |
| 按下 | 玩家按格挡/闪避后设置完美窗口、写 `inst+BC` 的地方（`movss [r14+BC],xmm0`） | 9 字节 | 2：inst、+B8、+BC、+58 |
| 步骤切换 | 技能实例进入新步骤时写步骤时长：`mov [r13+B4],ecx` | 7 字节 | 3：inst、+B0 上一步已过时间、新步骤时长、+60 步骤行、+68 技能组件 |

特征码（旧实现中的写法，供参考）：

```
判定  48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC A0 00 00 00 80 B9 B4 01 00 00 00
按下  F3 41 0F 11 86 BC 00 00 00 85 FF 74
步骤  F3 41 0F 11 85 B0 00 00 00 8B 48 18 41 89 8D B4 00 00 00 74   （钩在 +0xC）
```

步骤钩子后面紧跟一个 `je`，所以钩子代码要 `pushfq/popfq` 保存标志位。

每个特征码都必须唯一命中，否则日志里报 “Signatures not found” 并放弃挂钩，不会乱写。小版本更新通常不影响这些字节。

### 判定是怎么结算的

- 敌人攻击判定框和 Eve 接触期间，游戏**每帧**调用一次 `IsJustActionActive`（一次攻击通常 2–6 帧）。
- 游戏按**最后一次**调用结算：那一帧 Eve 技能实例的完美窗口剩余时间 `+B8 > 0` 就是完美。第一次调用并不是结算点。
- SBParry 按攻击者把连续的判定事件归成一组（连按格挡时旧技能实例也会收到接触，但游戏按最新一次按键结算），这一组结束后取最后一条：
  - 剩余 > 0 → **PERFECT**
  - 否则 → **早了**，早的量 = 按下到结算的时间 − 窗口长度，换算成帧显示。
- 窗口长度是 SkillTable 行里的 `JustActionTime`（剑形态格挡/闪避在困难难度下为 0.23 s）。游戏每帧扣一次 dt，且要求结算帧剩余 > 0，所以 60 fps 下有效的只有 13 帧 = 217 ms。显示用的“帧”按 60 fps 换算；游戏本身按真实 dt 扣时间。

## 环形缓冲区

在远程分配的 cave 里。头部 4 字节是写计数，钩子用 `lock xadd` 取号，`& 0xFF` 得到槽位，槽位从 `+0x10` 开始，共 256 个。sbparry.exe 记住自己读到的序号，轮询追上。

取了号不等于写完了：钩子取号后先把条目的 `seq` 写成 `~序号`，其余字段写完，最后一步才把 `seq` 翻成序号（x86 的写入按顺序可见）。sbparry.exe 只读 `seq` 等于自己读序号的条目；不等说明游戏线程还在写，留到下一帧再读。空槽的 `seq` 初始化成不会等于任何读序号的值。

`LogEntry`（`src/game.h`，`#pragma pack(1)`，0x30 字节）：

| 偏移 | 类型 | 字段 | 判定 / 按下 | 步骤切换 |
|---|---|---|---|---|
| +00 | u64 | tsc | `rdtsc` | 同左 |
| +08 | u64 | inst | 技能实例 | 技能实例 |
| +10 | f32 | remain | inst+B8 完美窗口剩余 | inst+B0 上一步已过时间 |
| +14 | f32 | total | inst+BC 完美窗口总长 | 新步骤时长 |
| +18 | u32 | kind | 1 判定 / 2 按下 | 3 |
| +1C | u32 | seq | 写入序号（见上） | 同左 |
| +20 | u64 | row | inst+58 技能表行 | inst+60 步骤表行 |
| +28 | u64 | attacker | 判定：攻击者；按下：0 | inst+68 技能组件 |

时间戳是 TSC，程序启动时测出 TSC 频率换算成秒。

## 技能实例

技能实例在一个全局池里，Eve 和敌人共用同一个结构：

| 偏移 | 类型 | 含义 |
|---|---|---|
| +58 | ptr | SkillTable 行（`JustActionTime` @0x108，`JustActionTime_StoryMode` @0x10C） |
| +60 | ptr | 当前步骤在 SkillActiveStepTable 里的行 |
| +68 | ptr | 技能组件 |
| +B0 | f32 | 本步骤已经过的时间 |
| +B4 | f32 | 本步骤时长 |
| +B8 | f32 | 完美窗口剩余时间 |
| +BC | f32 | 完美窗口总长 |

## 音符预测

1. Bridge 用 `ForEachRow` 按 RowMap 顺序导出 SkillActiveStepTable 到 `steps.tsv`（列见文末“Bridge 导出的文件”）。第一行 `#table=0x…` 是表对象地址，sbparry.exe 用它在内存里把序号对上真实的行地址（UE4SS Lua 返回的行是拷贝，不能直接用地址），并抽查几行的时长确认顺序一致。
2. 步骤切换钩子让 sbparry.exe 知道有哪些敌方技能实例在跑。每帧读它们的 `+60`（当前步骤）、`+B0`、`+B4`；`+B0` 超过 300 ms 没变就当技能已结束、实例闲置。当前步骤的开始时刻优先用步骤事件的 TSC（精确），对不上时才用 `现在 − B0` 倒推。
3. 从当前步骤沿 `NextStepAlias` 往后走（最多 10 步、1.5 s）：当前步剩余 = `B4 − B0`，后面的步骤累加时长；每遇到一个 Hit 步骤，结算时刻 = 该步开始时刻 + 这一招的结算偏移（见下一节）。
4. 只有“真打”的 Hit 步骤出音符。Bridge 导出的 `real` 列为 1 的条件是下面三者之一：
   - 有攻击碰撞组（`AttackCollisionGroupArray`）；
   - 发射飞行道具（`UsableNonTargetProjectileAliasArray` / `UsableTargetProjectileAliasArray`）或生成冲击波环；
   - 有范围判定 `OverrideTargetFilterAlias`（例如 `BurstAreaSlash_Hit1` 是 12 m 的圆柱）；
   - 在自身位置生成对他人生效的伤害效果（`CreateEffectSelfPosition` 里的效果有 `LoopTargetFilterAlias`，或 `ActiveTargetFilterAlias` 不是 `Self`）。例如红莲分身的斩击 `DummyScarlet_Slash_Hit1` 本身既没有碰撞也没有范围判定，伤害来自它生成的 `HitZoneArea3`（16 m 范围、持续 0.1 秒），按范围攻击处理：步骤开始就结算，可否完美闪避或上效果的 `AvailableJustEvade`。

   以上都没有的 Hit 步骤是脚本演出（拼刀、抓取成功后的连段等），不出音符。
5. 判定条显示未来 1 秒以内的音符。颜色由 Hit 步骤的标志决定：可弹反 → 青色；只可完美闪避 → 琥珀色；都不行 → 红色；冲击波环 → 薄荷绿（跳跃）。发射飞行道具的步骤，可弹反 / 可完美闪避以道具表为准（伤害来自道具；红莲 `PhaseChange2_AttackRange` 的步骤标着可弹反，道具却不可格挡、只能完美闪避，按格挡照样挨打）；生成冲击波环的步骤，可完美闪避还要或上效果表的 `AvailableJustEvade`。

### 顿帧与时间流速

完美弹反/闪避之后，敌人的步骤时间会走得比真实时间慢（顿帧，甚至停住）。音符的 `t` 是游戏时间，sbparry.exe 对每个实例用同一步骤内 `+B0` 的增量除以真实时间测出流速（夹在 [0, 1.5]），自动操作在流速 < 0.8 时把剩余游戏时间按流速换算成真实时间再比较。

这个流速**只在完美结果后的 0.5 s 内**生效。其他时候步骤时间停住多半是在等条件（比如后跳要等落地），当成顿帧会把后面的音符推得很远。

### 步骤切换那一帧

游戏线程正在切步骤时，外部读到的可能是“新步骤 + 旧的已过时间/时长”（半更新），推算会偏早半秒以上。所以实例的步骤和上一帧不同（或 `B0 > B4`）的那一帧不重算，而是沿用这个实例上一帧的音符（减去经过的时间，前提是上一帧在 0.1 s 以内）。早先的做法是这一帧直接不出音符，结果判定条上的绿区会闪。

### 锁定目标丢失

距离和方向用的敌人是 `SBCharacter.LockOnCharacter`（`+0x1438`），没有锁定就用 `CameraLookAtTarget`（`+0x1460`）。Boss 的爆发招会解除锁定，这时退回最近一次锁定过的敌人（`g_live.lastEnemy`，只要它还能读到位置、PlayerController 没变），飞行时间估计、触发距离判断和惩戒距离都不会断。

## 结算时间校准

Hit 步骤开始后，游戏并不是马上结算：判定框有 `DelayTime`，还要等判定框和 Eve 的最后一帧接触。实测不同招式在 Hit 步骤开始后 35–145 ms 不等，但同一招很稳定。

- 每次结算都记下“Hit 步骤开始 → 结算”的秒数，每个步骤保留最近 9 次，取中位数。远程攻击记录的是扣掉飞行时间后的部分。
- 没有数据的招式按类型估计：

  | 招式 | 默认偏移（Hit 步骤开始后） |
  |---|---|
  | 近战（有碰撞组） | 第一个判定框 `DelayTime` + 80 ms |
  | 范围判定（没有碰撞组，`delay = -1`） | +0.01 s，步骤一开始就结算 |
  | 飞行道具 | +0.03 s + 飞行时间 |
  | 冲击波环 | −0.03 s + 扩散时间 |

- 数据存在 exe 旁边的 `calib.tsv`（每行：步骤名 + 若干秒数），下次启动继续用。发布包附带 `calib_default.tsv`，是渡鸦 Boss 的默认校准；本机学到的 `calib.tsv` 优先。

## 飞行道具

剑气之类的远程攻击，伤害来自道具本身，发射它的 Hit 步骤很短（渡鸦后跳剑气的 Hit 步骤只有 0.1 s），离得远时步骤早已切走、剑气还在飞。所以远程攻击分两段处理：道具出现前按步骤估计，出现后按实时位置追踪。

**道具从哪来。** 飞行道具是 `SBProjectile` actor，按类型建对象池、常驻关卡。Bridge 在每个关卡（PlayerController 变化时）`FindAllOf("SBProjectile")` 全量列一次，之后新建的池对象由 `NotifyOnNewObject` 回调记下（回调只排队），Bridge 每 200 ms 的循环把它们追加到 `projectiles.txt`。每行带上 ProjectileTable 对应行（类名去掉 `_C`）的 `AvailableJustParry` / `AvailableJustAction` 和速度；伊芙自己的道具（`P_` 开头）跳过。速度会被游戏夹在 `[MinSpeed, MaxSpeed]` 里，Bridge 导出前先夹好：渡鸦剑气 `Speed` = 2000，但 `MinSpeed` = `MaxSpeed` = 2700，实际 27 m/s。

**实时追踪**（`projectiles.cpp`）：

- 每帧读每个道具根组件的位置。池里的道具不飞时停在原点或上次消失的地方，位置在变（60 ms 内动过）才算在飞；停了超过 0.2 s 再动算新的一发，旧样本作废。
- 方向用实测位移。我们读内存的时刻和游戏帧不对齐，两次读之间可能隔 0、1 或 2 帧，相邻样本算出的速度忽大忽小；所以保留最近约 0.15 s 的位置样本，用最老和最新的两点算（跨度 ≥ 80 ms）。
- 速度大小夹到道具表的 [初速, 最高速]（没有加速度的道具两者相同）：刚发射时取样窗口里混着还没动起来的几帧，实测速度偏低。
- 从上次取样外推到现在，解“道具中心第一次进入伊芙 1.05 m 以内”的时刻作为命中时刻（实测剑气的完美闪避在中心离伊芙 1.07 m 时结算）。最近距离在 1.05–1.6 m 之间的按最近点时刻算；超过 1.6 m 的当打不到；只看 2 s 以内会到的。
- 实测结果：整个飞行过程中，实时预测的到达时刻前后只差 1–2 ms。

**道具出现之前**，按步骤估计：结算 = Hit 步骤开始 + 偏移 + `(距离 − 5.2 m) / 速度`。道具不是从敌人身上出发的：渡鸦的剑气在她前方约 4.2 m 处生成，而命中又在离伊芙约 1.05 m 时发生，所以飞行道具一律扣固定的 5.2 m（`kProjAhead`），道具出现后就换成实时追踪。

**两段怎么接上。** 实时追踪到的音符归到最近一个发射飞行道具的敌方 Hit 步骤上（3 s 以内），和按步骤估计的音符用同一个实例 + 步骤标识；有道具在飞时，那一步的估计音符就不再出。找不到归属步骤时用道具地址当标识。一轮连发的几发会归到同一个步骤上，所以每个实时音符另外带上道具地址（`Note.proj`），显示平滑和自动操作的去重都按单发区分，第 2、3 发也会照常弹反。

**显示平滑。** 估计值换成实时值时预测会修正，判定条上的点就会跳。远程音符（飞行道具和冲击波环）另有一个显示用的 `tShow`：显示的到达时刻每秒最多修正 1 s，点只是变快或变慢，不会跳，很快收敛到真实值；上一帧没有这个音符（断了 0.2 s 以上）或差距超过 0.6 s（已经是下一发）时直接跳到新值。自动操作用的是未平滑的原始预测。近战不平滑，它们本来就稳，弹反后的顿帧停顿也要照实显示。

**完美闪避的结果。** 飞行道具的完美判定不走 `IsJustActionActive`，判定钩子看不到。伊芙完美闪避成功时会进入 `JustEvade…_Cast1` 步骤，前后 0.3 s 内没有近战接触记录就把它记成一次飞行道具的完美闪避，并用它校准最近的远程 Hit 步骤。

## 冲击波环

贴地扩散的冲击波环（如渡鸦后跳连段落地砸出的环）既不是碰撞组也不是道具：Hit 步骤在自身位置生成一个效果（`CreateEffectSelfPosition`），效果的 `LoopTargetFilterAlias` 指向一个开了 `bDynamicShapeScale` 的目标过滤。命中范围是半径 `FarDistance × scale` 的圆柱，`scale` 在效果的 `LifeTime` 内从 `MinShapeScale` 变到 `MaxShapeScale`。

SBParry 把它当成一个从初始半径出发、匀速扩散的飞行道具：初始半径 = `FarDistance × Min`（写进 `steps.tsv` 第 16 列 `ahead`），速度 = `FarDistance × (Max − Min) / LifeTime`。渡鸦的环：`FarDistance` 200，scale 1.4 → 10.2，1.0 s，即 2.8 m → 20.4 m，17.6 m/s，高约 40 cm。实测 14.0 m 处 630 ms、17.7 m 处 835 ms 掉血，比这个模型早约 30 ms，所以默认偏移取 −30 ms。

闪避躲不掉它：从没打出过完美闪避，时机合适的闪避也照样挨打。所以环的音符是**跳跃**音符，自动模式在到达前约 0.25 s + 输入延迟按跳（早 0.2 s 按实测来不及，刚起跳就被打到；跳跃滞空远比这长）。

表里一共找到 21 个这样的环攻击（Crawler、HedgeBoarBrute、ExoSuit、Raven 等），只有渡鸦的实测验证过。

## 蓝光 / 紫光窗口

这是和完美弹反（JustAction）独立的另一套机制。

- 敌人的某些 Cast 步骤在开始时给自己挂效果（`StartSelfEffect`，JSON 数组），其中 `Chance_BehindSkill*` 是蓝光，`Chance_MoveBackSkill*` 是紫光。
- 窗口从步骤开始后 `startDelayTime` 秒打开，持续 `Time` 秒（没写时用效果表里的 `LifeTime`，默认 1.0 s）。
- 触发距离由效果决定：

  | 效果名后缀 | 距离 |
  |---|---|
  | （无） | 4.5 m |
  | `_700` | 6.5 m |
  | `_900` | 9 m |
  | `_1500` | 15 m |

  Bridge 从效果行的 `ActiveTargetFilterAlias`（形如 `Enemy_3DArc_450_120_200`，450 cm）解析距离。
- Eve 这边对应 `FlashBehindAttack`（蓝）和 `MoveBackAttack`（紫）两个指令：闪避键（command 24）+ 移动输入按住至少 0.1 s，方向在 315°–45°（朝敌人）为蓝，135°–225°（背离敌人）为紫。
- 判定条上显示为一段长条。按下时判断：成功 / 早了 / 晚了 / 方向或距离不对。
  检测方法：步骤切换钩子能看到伊芙每次进入新步骤。窗口按敌人步骤切换事件的时间 + `startDelayTime` 记下；伊芙进入 `FlashBehindAttack*_Cast1` / `MoveBackAttack*_Cast1` 即成功；进入普通闪避（含完美闪避）的第一步时，按它相对窗口的位置给出早/晚几帧，落在窗口内却没触发就是方向或距离不对。伊芙的步骤比按键晚 1~2 帧开始，所以贴着窗口边缘的结果可能差一帧。

## 未应对与受到伤害

### 未应对

敌方每进入一个 Hit 步骤（`real` = 1、非连打挣脱步骤），就登记一条待查记录，期限 = 预测的结算时刻 + 0.35 s。到期时满足下面全部条件就记一条“未应对”：

- 从步骤开始前 0.7 s 到期限，伊芙没有进入任何名字含 `Guard` / `Evade` / `JustParry` / `FlashBehind` / `MoveBack` / `Parry` / `Jump` 的步骤；
- 这段时间里也没有判定钩子的接触记录；
- 敌人够得着：近战要在攻击范围 + 1 m 以内（范围来自步骤的 `ActionAssistTargetFilter`，如 `ActionAssist_3DArc_500_120` = 5 m；表里没有就按 3.5 m），飞行道具和冲击波环不限距离。

“未应对”不代表一定挨打了（可能本来就打不中），真正掉血看血条。

### 受到伤害（血条）

伊芙的血量在原生代码里，不好直接找；Bridge 改为导出 HUD 上的血条控件（`WB_MainHUD_PlayerInfo.WidgetTree.ProgressBar_HP`，一个 `ProgressBar`）的地址和 `Percent` 字段偏移，sbparry.exe 每帧读这个 0~1 的值。

血条掉血有动画，会分好几帧降下去。连续的下降合并成一次事件：第一帧下降的时刻约等于挨打时刻，此时把它归到最近开始的敌方 Hit 步骤上（3 s 以内，远程攻击可能在步骤结束后才打到），停止下降 0.3 s 后记一条总量，形如 “Took damage -12.3% (某步骤, 630ms after it started)”。上面冲击波环的实测数据就是这样得到的。

## 惩戒

敌人被打崩倒地后按 Y（重攻击键）可以惩戒。伊芙的技能 `P_Eve_Sword_Normal_LinkAttack1_1` 的目标过滤要求敌人处在 `ActorState_Groggy`（`ESBActorState` = 7）、3 m 以内、在正前方。

`ActorState` 在原生代码里，反射拿不到。通过内存对比找到两个同步变化的标志：

- 部分 Boss 蓝图有 `IsGroggy` 布尔（渡鸦等），偏移每个类不同。Bridge 遍历 `SBCharacter` 实例，按类导出 `类地址:偏移`；
- `SBCharacter.bActiveWeakPointCollision` 在同一时刻翻转，是没有 `IsGroggy` 的敌人的通用后备。

sbparry.exe 读目标敌人的类指针（`+0x10`），查到 `IsGroggy` 偏移就用它，否则用 `bActiveWeakPointCollision`，取最低位。实测两者在倒地可惩戒时同时 0→1，开始惩戒时变回 0，所以不会重复触发。

问题在于标志在敌人刚被打崩时就置位，而惩戒要等倒地动画走到一半才真正可用（渡鸦约 1–1.2 s），太早按 Y 就变成一次重攻击。因此自动模式：

- 标志上升后等 `g_execDelay`（初始 1.2 s）再按；
- 按下后伊芙进入的步骤名含 `StrongAttack`（变成了重攻击）就把 `g_execDelay` 加 0.2 s（最多 3 s，本次运行记住），并等 0.9 s 让重攻击打完再试；
- 距离要 ≤ 2.8 m（游戏要求 3 m，留点余量；正前方的要求靠伊芙出招时自己转向满足），不够就等着。

## 连打与过场 QTE

- **挣脱连打**：带 `NextStepAliasWhenLinkBreak` 的敌方步骤是拼刀 / 被抓（渡鸦、锯鲨、巨兽、Scarlet 等都有），Bridge 导出为 `mash` 列。敌人处在这种步骤时，自动模式以约 14 Hz 连打轻攻击（每 70 ms 按一次、按住 35 ms，时长按伊芙的时间流速拉长）。
- **过场 QTE**：过场动画里的 QTE 是控件 `SBSequencerQTEWidget`（第一次播过场时才创建）。Bridge 导出它的地址和 `Visibility`、`InputType`、`InputAction`、`UIInputAction`、`bBindInput` 的偏移。控件可见（`ESlateVisibility` 为 0/3/4）且绑定了输入时，读出 `InputAction`（为 0 则用 `UIInputAction`）的 FName 比较序号，和 `keys.txt` 里 `@names` 导出的动作名序号对照，按对应的动作；对不上时按轻攻击。

## 自动操作

默认关闭（Ctrl+Alt+A）。开启后按判定条的原始预测（不是平滑后的 `tShow`）替玩家按键：

| 音符 | 动作 | 时机 |
|---|---|---|
| 可弹反（青） | 格挡 | 完美窗口正中略靠后：结算前 `窗口/2 + 输入延迟 − 25 ms − autoAimMs` |
| 只可完美闪避（琥珀） | 闪避 | 同上 |
| 都不行（红） | 普通闪避（靠无敌帧躲开） | 同上 |
| 冲击波环（绿） | 跳跃 | 到达前 0.25 s + 输入延迟 |
| 蓝 / 紫窗口 | 推方向 0.12 s 后闪避 | 窗口内、敌人进入触发距离 − 0.3 m 时；快到窗口末尾就直接按；紫光在窗口末段出手（`autoChance`） |
| 敌人可惩戒 | 重攻击 | 见“惩戒”（`autoQte`，Ctrl+Alt+X） |
| 拼刀 / 被抓 / 过场 QTE | 连打轻攻击 / QTE 要求的动作 | 见上节（挣脱连打不受 `autoQte` 影响） |

每帧的优先级是：音符 > 惩戒 > 连打。同一个音符每帧都会重新算出来，按过的按“实例 + 步骤 + 时刻”去重（命中音符容差 0.45 s，蓝紫窗口 0.6 s）；飞行道具还要比道具地址，连发的每一发各按一次。

- **输入延迟**：自动按下到按下钩子收到事件的实测中位数（最近 15 次），没有数据时按 70 ms（实测 3–5 帧）。
- **补按**：格挡/闪避按下 0.14 s 后伊芙还没有任何步骤切换，说明输入没被游戏接受；离命中还有 30 ms 以上就补按一次（只补一次）。
- **完美闪避后的无敌期暂缓按键**：完美闪避成功后伊芙有一段专属动作（JustEvade Cast1+Cast2，游戏时间 0.6 s，加上慢动作实测约 0.88 s），全程无敌但不能再闪避。这期间按下的闪避会被游戏缓存到动作结束才打出来，变成一个时机错掉的普通闪避，还占掉下一下的闪避（红莲 BackDashSpaceCut 第 5、6 下只隔 0.6 s，就是这样连着挨打）。所以 JustEvade 开始后 0.85 s 内，预测落在这段里的攻击先不按（`timing.csv` 里记 `skip-invuln`）。这只是暂缓，不记成已处理：远程招的预测可能会变（红莲 `PhaseChange2_AttackRange_Hit3` 预测在无敌期内，实际晚了 0.5 s 才到），无敌期过了攻击还没到的话照常按。
- **暂停**：游戏不在前台，或玩家按着 Ctrl / Alt（比如刚按完快捷键，免得拼成组合键）时不发键，并松开所有模拟按键。
- **按住时长**：游戏按游戏时间判断按键（闪避要按住 0.02 s、蓝紫方向要保持 0.1 s）。伊芙被放慢时（Boss 爆发招的慢动作等）真实按住时间按伊芙的时间流速（`WorldSettings.TimeDilation` × 伊芙的 `CustomTimeDilation`，`+0xB0`）拉长。

按键来自 Bridge 导出的 `keys.txt`（从 `Default__InputSettings` 读 Guard / Evade / AttackLight / AttackStrong / Jump / Interaction_Key 动作映射和 MoveForward / MoveRight 轴映射；带修饰键的组合跳过）。

输入设备跟随玩家最近一次**真实**操作的那种，或用 `autoDevice` 固定：

- 键鼠：用 Raw Input（`RIDEV_INPUTSINK`）监听。`SendInput` 注入的事件 `hDevice` 为 0，据此排除本程序自己的按键；按着 Ctrl/Alt 的按键和鼠标的轻微抖动也不算。
- 手柄：游戏内的手柄桩在玩家有按键或摇杆推出死区时记下 `xiLastReal / psLastReal`（在叠加模拟按键之前判断）。
- 还没见过任何真实操作时（刚启动就挂机），接了 XInput 手柄就用手柄注入，否则用键盘。只在没有动作进行中时切换设备。

各设备的注入方式：

- **键盘**：`SendInput`，因此游戏必须在前台。
- **XInput / DualSense**：不装虚拟手柄驱动。把游戏导入表里 `XInputGetState` 和 libScePad 的 `scePadReadState` 两个槽位改成 cave 里的小桩：桩先调用原函数，再在 `PadCtrl.xiActive / psActive` 非 0 时把 `xiButtons / psButtons` OR 进返回的状态，需要时覆盖摇杆值。
- 延迟加载的导入可能在之后才被解析、覆盖掉改过的槽位，所以每秒检查并补挂（`RefreshPadHooks`）。

## 一击必杀

用来跳过阶段（Ctrl+Alt+K，不存进 ini，每次启动默认关）。步骤表行 `+0x20` 是 `SkillAttackDamageRate`、`+0x24` 是 `SkillShieldAttackDamageRate`。开启时遍历伊芙的全部步骤（`P_` 开头），两个倍率不全为 0 的行把它们都乘以 1000，直接写游戏内存里的步骤表（堆内存，本来就可写），原值记下来。

关闭、正常退出、Windows 注销 / 关机（`WM_ENDSESSION`）以及 SBParry 自己崩溃（未处理异常过滤器 `SetUnhandledExceptionFilter`）时把原值写回。在任务管理器里强行结束 SBParry 时还不了，倍率会一直保留到游戏重启。读内存失败要重新连接时，游戏还在，先写回再断开；游戏进程已经退出时只丢弃记录（表随进程一起没了）。

## 跳板为什么放在模块的 0xCC 填充里

钩子点只有 5–9 字节，只能放 `jmp rel32`，最远 ±2 GB。游戏跑久了以后，模块附近 ±2 GB 内经常找不到 64 KB 的空闲块，直接“就近分配”会失败。所以：

```
钩子点  E9 rel32  ──►  模块 .text 内一段 ≥16 字节的 0xCC 对齐填充
                        FF 25 00 00 00 00 <abs64>   （14 字节绝对跳转）
                           ──►  cave（VirtualAllocEx，任意地址）
                                 钩子代码 … 执行被替换的原指令 …
                                 FF 25 … 绝对跳回 钩子点+N
```

找填充时要求前一个字节也是 0xCC，避免把指令中间恰好出现的 CC 当成填充。

## 退出还原与崩溃后复用

- **正常退出**（Ctrl+Alt+Q / 托盘退出 / `--quit`，以及注销 / 关机、SBParry 崩溃时）：松开所有模拟按键、还原一击必杀改过的倍率，把三个钩子点写回原始字节，整个 `PadCtrl` 控制块清零（放开注入的手柄按键），导入表槽位还原（槽位仍指向我们的桩时才写回原值）。跳板和 cave 都原样留着：游戏线程可能刚跳进去、正在里面执行，改成 0xCC 或释放都会崩。
- **连接失败**：还没写钩子点就失败时，已分配的 cave 直接释放。连接失败的游戏进程 30 s 后才重试（不是每 2 s）。
- **上次没还原**（sbparry 崩溃或被强关）：再次启动时判定函数入口已经是 `E9 …`。程序顺着 `jmp → 跳板 → cave`，检查 cave 头部（`+0x3F00`）的魔数（`"SBP3"`，也认旧版 `"SBP2"`）以及里面记录的钩子地址是否与当前一致；一致就按头部记录把旧钩子全部还原，再重新安装一套新的。对不上说明是别的东西改过，放弃挂钩。

cave 头部：

```c
struct CaveHeader {
    uint32_t magic /* 'SBP3' */, pad;
    uint64_t judge, press, step, tramp[3];               // 与旧版 SBP2 相同
    uint64_t iatXInput, origXInput, iatScePad, origScePad; // 手柄导入表槽位及原值
};
```

cave 布局（16 KB）：

| 偏移 | 内容 |
|---|---|
| `+0x0000` | 判定钩子块 |
| `+0x0100` | 事件环形缓冲区（序号 + 256 × 0x30） |
| `+0x3200` / `+0x3300` | 按下 / 步骤钩子块 |
| `+0x3400` / `+0x3500` | XInputGetState / scePadReadState 桩 |
| `+0x3E00` | `PadCtrl`（本程序写、桩读） |
| `+0x3F00` | `CaveHeader` |

所有块的源码在 `src/hooks.asm`（ml64 汇编），运行时整块拷进 cave，再替换占位常量（日志 / PadCtrl 地址），每块最后 8 字节填回跳地址或原函数地址。

## Bridge 导出的文件

全部写在 `ue4ss/Mods/SBParryBridge/`。

| 文件 | 频率 | 内容 |
|---|---|---|
| `steps.tsv` | 表加载后一次（每 2 s 重试直到成功） | `#table=0x…`、`#version=7`，每行 17 列，见下 |
| `live.txt` | 每秒检查，内容变了才重写 | 各种对象地址和字段偏移，见下 |
| `projectiles.txt` | 每个关卡全量写一次，之后新建的池对象追加 | 每行 `0x地址 道具表行名 jp ja 初速 最高速` |
| `keys.txt` | 每 3 s 检查，变了才写 | 键位和动作名 FName 序号，见下 |

`steps.tsv`、`live.txt`、`keys.txt` 以及 `projectiles.txt` 的全量重写都是先写 `.tmp` 再改名，sbparry.exe 不会读到写了一半的文件。`live.txt` 和 `projectiles.txt` 修改时间早于游戏进程启动时间的，是上一局游戏留下的，地址全都不对，不读。

### steps.tsv（version 7）

| 列 | 内容 |
|---|---|
| 0 | 序号（RowMap 顺序） |
| 1 | 步骤名 |
| 2 | 类型：0 = Cast 前摇，1 = Hit 判定 |
| 3 | 时长（s） |
| 4 | `NextStepAlias` 对应的下一步序号，−1 = 无 |
| 5 / 6 | 可完美弹反 / 可完美闪避（`AvailableJustParry` / `AvailableJustAction`；发射道具的步骤以道具表为准，冲击波环和伤害区域或上效果表的 `AvailableJustEvade`） |
| 7 | 第一个攻击判定框的 `DelayTime`，−1 = 没有碰撞组 |
| 8 / 9 / 10 / 11 | 机会窗口：类型（0 / 1 蓝 / 2 紫）、开始、时长（s）、触发距离（m） |
| 12 | 挣脱连打（有 `NextStepAliasWhenLinkBreak` = 1） |
| 13 | 飞行道具 / 冲击波环速度（m/s），0 = 近战 |
| 14 | 攻击范围（m，来自 `ActionAssistTargetFilter`），0 = 未知 |
| 15 | 真实攻击（`real`，见“音符预测”） |
| 16 | `ahead`：冲击波环的初始半径（m），其他一律 −1。sbparry.exe 把 `ahead ≥ 0` 且速度 > 0 的步骤当作冲击波环；飞行道具的估计固定用 `kProjAhead` 5.2 m，道具出现后换成实时追踪 |

`#version` 行 sbparry.exe 并不读。它按列解析，至少有 8 列的行就接受（旧版 Bridge 写的列少），缺的列取默认值。

### live.txt

```
pc=0x…                              PlayerController；sbparry.exe 自己顺着读 Pawn、相机、锁定目标
ws=0x… dil=0x…                      WorldSettings 与 TimeDilation 偏移（慢动作）
groggy=0x类:0x偏移,… weak=0x…        有 IsGroggy 的敌人类及其偏移；SBCharacter.bActiveWeakPointCollision 偏移
hp=0x… pct=0x…                      伊芙血条 ProgressBar 与 Percent 偏移
qte=0x… vis=0x… type=0x… action=0x… uiaction=0x… bind=0x…   过场 QTE 控件与字段偏移
```

`FindFirstOf` / `FindAllOf` 要遍历全部 UObject（约 30 ms），不能频繁搜：

- PlayerController 失效时每 5 s 重搜一次；
- 换了 PlayerController（开局 / 读档 / 换关卡）时全量扫一次血条、QTE 控件和敌人类；
- 之后不做定期全量扫描：血条或 QTE 控件失效 5 s 后、还没找到血条时每 30 s、或新建了 QTE 控件（`NotifyOnNewObject`）时，才重扫血条和 QTE 控件；
- 新出现的敌人类由 `SBCharacter` 的 `NotifyOnNewObject` 记下，在每秒的循环里补算 `IsGroggy` 偏移；
- 内容没变就不写文件。sbparry.exe 每秒重读一次。

### projectiles.txt

首行 `#gen=0x<PlayerController 地址>`，之后每行 `0x地址 行名 jp ja 初速 最高速`，速度单位 cm/s，已按 `MinSpeed` / `MaxSpeed` 夹过；没有加速度的道具最高速 = 初速，最高速 0 = 不限。PlayerController 变化（换关卡）时整个文件重写，换一代 `#gen`；sbparry.exe 每 100 ms 看一次文件大小和修改时间，变了就重读，`#gen` 不同说明换了一代，旧地址全部作废。新对象构造时类名已确定，地址先写上（由 200 ms 的循环追加），位置离开原点才当作在飞。

### keys.txt

```
Guard=E,ThumbMouseButton,Gamepad_LeftShoulder
Evade=…  AttackLight=…  AttackStrong=…  Jump=…  Interaction_Key=…
MoveForward=W:1,S:-1,Gamepad_LeftY:1
MoveRight=…
@names=19454016:AttackLight,…
```

`@names` 是上面各动作名以及 `UI_QTE_<动作>` 的 FName 比较序号，过场 QTE 控件里存的是 FName，用序号对照出要按哪个动作。

## 调试日志 timing.csv

`sbparry.ini` 里 `debugLog=1` 时，程序目录下追加写 `timing.csv`。每行以本地时间 `HH:MM:SS.mmm` 开头，第二列是类型：

| 类型 | 其余字段 |
|---|---|
| `step` | 步骤名、TSC（每次步骤切换，伊芙和敌人都有） |
| `auto` | 动作（guard / evade / jump）、步骤名或 `projectile`、预测 ms、敌人流速、伊芙时间流速、距离 |
| `perfect` / `early` | parry / evade、窗口 ms、提前 ms、预测误差 ms、敌方 Hit 步骤、类型、步骤开始到结算 ms、首判定框延迟 ms |
| `perfect,evade,projectile` | 提前 ms、校准到的远程 Hit 步骤 |
| `chance` | blink / repulse、ok / early / late / missed、窗口 ms、偏差 ms |
| `unhandled` | 步骤名、距离 |
| `damage` | 归因步骤、出招后 ms、距离（掉血第一帧记一条） |
| `proj` | 每帧每个在飞道具：地址、预测到达 ms、速度 m/s、预计最近距离 m、当前距离 m |

`proj` 行用来查判定条上点的漂移；前面“实时预测前后只差 1–2 ms”就是从它看出来的。
