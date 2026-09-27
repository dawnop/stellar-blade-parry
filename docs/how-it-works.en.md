# How SBParry works

**English** | [中文](how-it-works.md)

For people reading the code, porting to a new game build, or building something similar. Offsets and patterns are for the Steam build of 2026-08 (UE4.26); the definitions in the source (`src/game.h`, `src/hooks.asm`) are authoritative. Unless noted otherwise, measurements in this document were taken against the Raven boss.

## Architecture

```
 ┌───────────────────────── SB-Win64-Shipping.exe (game) ─────────────────────────┐
 │                                                                                 │
 │  UE4SS + SBParryBridge (Lua)                  3 code-cave hooks                 │
 │   ├─ SkillActiveStepTable ─┐                   ├─ IsJustActionActive entry      │
 │   ├─ Effect / Projectile  ─┤                   ├─ press: writes inst+BC         │
 │   │  / TargetFilter tables │                   └─ step change: mov [r13+B4],ecx │
 │   ├─ PlayerController, HP  │                          │ lock xadd               │
 │   │  bar, QTE widget,      │                          ▼                         │
 │   │  groggy offsets        │                   ring buffer (256 × 0x30)         │
 │   ├─ SBProjectile pools    │                                                    │
 │   └─ InputSettings bindings│                   gamepad import slots             │
 │                            │                    XInputGetState / scePadReadState│
 │                            │                          │ → stub ORs in buttons   │
 │                            │                          ▼                         │
 │                            │                   PadCtrl block (inside the cave)  │
 └────────────────────────────┼──────────────────────────┬────────────────────────┘
                              │ files                    │ ReadProcessMemory /
                              ▼                          │ WriteProcessMemory
       ue4ss/Mods/SBParryBridge/                         │
         steps.tsv  live.txt  projectiles.txt  keys.txt  │
                              │                          │
                              ▼                          ▼
 ┌────────────────────────────── sbparry.exe (separate process) ──────────────────┐
 │  tracker: read events, settle verdicts, predict notes from the step table,     │
 │           calibration (calib.tsv), unhandled/damage log, one-hit kill          │
 │  projectiles: read projectile positions every frame, solve arrival time        │
 │  live:    Eve / enemy / camera, HP bar, groggy state, cutscene QTE             │
 │  auto:    parry / dodge / jump / finisher / mash (SendInput or PadCtrl)        │
 │  overlay: click-through layered window — bar / stats panel / toasts            │
 │  tray icon, global hotkeys, sbparry.ini, log sbparry.log                       │
 └─────────────────────────────────────────────────────────────────────────────────┘
```

- sbparry.exe is an external process. No DLL is injected besides UE4SS itself.
- Inside the game there are only three short hook stubs that append events to a ring buffer, plus two redirected gamepad import slots. Everything else is memory reads; the only game data ever written is the step table, by the one-hit-kill toggle (see below).
- UE4SS only runs the bridge, which exports what is easy to get through UE reflection (step, effect and projectile tables, key bindings, assorted object addresses and field offsets) as text files.
- sbparry.exe is a Windows-subsystem app with no console. The log goes to `sbparry.log` in the program folder and is always in English whatever the UI language (the arguments of `Log(...)` are evaluated inside an `EnglishScope`, so every `TR` in them picks English); the tray menu's "Open log file" item opens it in the default text editor (falling back to Notepad). `sbparry.exe --quit` tells the running instance to restore the game code and exit.
- The bar and stats panel are hidden while the game window isn't in the foreground, so they don't cover other programs. If some Ctrl+Alt hotkeys fail to register (taken by another program), a toast at startup lists them.

| Source | Role |
|---|---|
| `game.cpp` + `hooks.asm` | attach to the game, install / restore hooks and pad stubs |
| `tracker.cpp` | step table → note prediction; events → perfect verdicts, blue/violet verdicts; unhandled and damage log; one-hit kill; `timing.csv` |
| `projectiles.cpp` | live projectile tracking |
| `live.cpp` | Eve / enemy / camera via the PlayerController; HP bar, groggy state, cutscene QTE widget, time dilation |
| `autoplay.cpp` | auto parry / dodge / jump / blue-violet / finisher / mash and QTE |
| `overlay.cpp` | drawing |
| `main.cpp` | main loop (once per DWM composition frame), hotkeys, tray |

## Hooks

| Name | Site | Replaced instruction | Logged (kind) |
|---|---|---|---|
| Judge | entry of `IsJustActionActive(inst, attacker)` | `mov [rsp+8],rbx` (5 bytes) | 1: inst, attacker, inst+B8 remaining, inst+BC total, inst+58 skill row |
| Press | where the player's guard/dodge sets up the just-action window and writes `inst+BC` (`movss [r14+BC],xmm0`) | 9 bytes | 2: inst, +B8, +BC, +58 |
| Step | a skill instance entering a new step writes the step duration: `mov [r13+B4],ecx` | 7 bytes | 3: inst, +B0 elapsed in previous step, new step duration, +60 step row, +68 skill component |

Patterns (as in the earlier implementation, for reference):

```
judge  48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC A0 00 00 00 80 B9 B4 01 00 00 00
press  F3 41 0F 11 86 BC 00 00 00 85 FF 74
step   F3 41 0F 11 85 B0 00 00 00 8B 48 18 41 89 8D B4 00 00 00 74   (hooked at +0xC)
```

The step site is followed by a `je`, so its stub saves flags with `pushfq/popfq`.

Every pattern must match exactly once; otherwise the log reports the mismatch and nothing is patched. Minor game patches usually leave these bytes alone.

### How the verdict is settled

- While an enemy attack collision touches Eve, the game calls `IsJustActionActive` **every frame** (typically 2–6 frames per attack).
- The game settles on the **last** call: perfect iff Eve's skill instance still has just-action time left (`+B8 > 0`) on that frame. The first call is not the settlement.
- SBParry groups consecutive judge events by attacker (while mashing guard, older skill instances also receive contacts, but the game settles on the newest press) and, once the group ends, takes the last one:
  - remaining > 0 → **PERFECT**
  - otherwise → **EARLY**, by (press-to-settle time − window), shown in frames.
- The window is `JustActionTime` in the SkillTable row (sword-form parry/dodge on Hard: 0.23 s). The game subtracts dt per frame and needs remaining > 0 on the settling frame, so at 60 fps only 13 frames = 217 ms are usable. Frames in the UI are a 60 fps conversion; the game itself decrements by real dt.

## Ring buffer

Lives in the remotely allocated cave. The first 4 bytes are a write counter; a stub grabs a slot with `lock xadd`, masks with `& 0xFF`, and writes at `+0x10 + slot × 0x30` (256 slots). sbparry.exe keeps its own read index and polls to catch up.

Grabbing a slot doesn't mean it has been written: after taking its number the stub first sets the entry's `seq` to `~number`, writes the other fields, and flips `seq` to the number as the very last store (x86 stores become visible in order). sbparry.exe only consumes an entry whose `seq` equals its read index; otherwise the game thread is still writing it and it waits for the next frame. Empty slots are initialised with a `seq` that never equals a read index.

`LogEntry` (`src/game.h`, `#pragma pack(1)`, 0x30 bytes):

| Offset | Type | Field | Judge / press | Step change |
|---|---|---|---|---|
| +00 | u64 | tsc | `rdtsc` | same |
| +08 | u64 | inst | skill instance | skill instance |
| +10 | f32 | remain | inst+B8 just-action remaining | inst+B0 elapsed in previous step |
| +14 | f32 | total | inst+BC just-action total | new step duration |
| +18 | u32 | kind | 1 judge / 2 press | 3 |
| +1C | u32 | seq | write sequence (see above) | same |
| +20 | u64 | row | inst+58 skill row | inst+60 step row |
| +28 | u64 | attacker | judge: attacker; press: 0 | inst+68 skill component |

Timestamps are TSC; the TSC frequency is measured at startup.

## Skill instance

Skill instances live in one global pool shared by Eve and enemies:

| Offset | Type | Meaning |
|---|---|---|
| +58 | ptr | SkillTable row (`JustActionTime` @0x108, `JustActionTime_StoryMode` @0x10C) |
| +60 | ptr | current step's row in SkillActiveStepTable |
| +68 | ptr | skill component |
| +B0 | f32 | time elapsed in the current step |
| +B4 | f32 | current step duration |
| +B8 | f32 | just-action time remaining |
| +BC | f32 | just-action window total |

## Note prediction

1. The bridge dumps SkillActiveStepTable with `ForEachRow` (RowMap order) to `steps.tsv` (columns are listed under "Files exported by the bridge"). The first line `#table=0x…` is the table object address, which sbparry.exe uses to map indices to real row addresses in memory (UE4SS Lua hands out row *copies*, so their addresses are useless); it also spot-checks a few durations to make sure the order matches.
2. The step hook tells sbparry.exe which enemy skill instances are active. Each frame it reads their `+60` (current step), `+B0` and `+B4`; an instance whose `+B0` hasn't moved for 300 ms is treated as idle. The current step's start time comes from the step event's TSC when it is consistent (exact), and is otherwise back-computed as `now − B0`.
3. From the current step it walks the `NextStepAlias` chain (up to 10 steps or 1.5 s): remaining in the current step = `B4 − B0`, then the durations of the following steps. For each Hit step, settle time = step start + that attack's settle offset (next section).
4. Only Hit steps that actually hit produce notes. The bridge sets the `real` column when the step has any of:
   - an attack collision group (`AttackCollisionGroupArray`);
   - a projectile (`UsableNonTargetProjectileAliasArray` / `UsableTargetProjectileAliasArray`) or a shockwave ring;
   - an area target filter, `OverrideTargetFilterAlias` (e.g. `BurstAreaSlash_Hit1` is a 12 m cylinder);
   - a damaging effect spawned at its own position (an effect in `CreateEffectSelfPosition` with a `LoopTargetFilterAlias`, or an `ActiveTargetFilterAlias` other than `Self`). For example Scarlet's clone slash `DummyScarlet_Slash_Hit1` has neither collision nor an area filter; the damage comes from the `HitZoneArea3` effect it spawns (16 m, lasts 0.1 s). It is treated as an area attack: settles at step start, with the dodge flag OR'ed with the effect's `AvailableJustEvade`.

   Hit steps with none of these are scripted follow-ups (after a clash or a successful grab) and produce no note.
5. Anything within the next second goes onto the bar. Colour comes from the Hit step's flags: parry-able → cyan; perfect-dodge only → amber; neither → red; shockwave ring → mint (jump). For projectile steps the parry/dodge flags come from the projectile table, since the damage comes from the projectile (Scarlet's `PhaseChange2_AttackRange` step is marked parry-able, but its projectile can't be guarded and is perfect-dodge only; guarding still gets you hit); for ring steps, the dodge flag is also OR'ed with the effect's `AvailableJustEvade`.

### Hitstop and time rate

After a perfect parry or dodge the enemy's step time runs slower than real time (hitstop, sometimes a full stop). Note times are in game time, so sbparry.exe measures a per-instance rate from how far `+B0` advances within the same step per unit of real time (clamped to [0, 1.5]). When the rate is below 0.8, auto mode converts the remaining game time into real time before comparing.

The rate is **only applied within 0.5 s after a perfect result**. At any other time a stalled step usually means the enemy is waiting on a condition (e.g. a back-jump waiting to land), and reading that as hitstop would push every later note far out.

### The frame a step changes

While the game thread is switching steps, an outside reader can see the new step together with the old elapsed time / duration (a half-updated instance), which makes the prediction half a second or more too early. So on a frame where an instance's step differs from the previous frame (or `B0 > B4`), its notes are not recomputed; the previous frame's notes are reused, shifted by the elapsed time, as long as they are less than 0.1 s old. An earlier version dropped the notes for that frame instead, which made the green zone on the bar flicker.

### Losing the lock-on

The enemy used for distance and direction is `SBCharacter.LockOnCharacter` (`+0x1438`), or `CameraLookAtTarget` (`+0x1460`) without a lock. Boss bursts can remove the lock-on; in that case it falls back to the last locked enemy (`g_live.lastEnemy`, kept while its location is still readable and the PlayerController hasn't changed), so flight-time estimates, range checks and the finisher distance keep working.

## Settle-timing calibration

The game doesn't settle at the start of the Hit step: the collision has a `DelayTime`, and the verdict comes on the last contact frame. Measured offsets range from about 35 to 145 ms after the Hit step starts depending on the attack, but are stable for a given attack.

- Every settled hit records "Hit step start → settle" in seconds; the last 9 samples per step are kept and the median is used. For ranged attacks the sample is stored with the flight time subtracted.
- Attacks without samples get a default by type:

  | Attack | Default offset (after Hit step start) |
  |---|---|
  | melee (has a collision group) | first collision `DelayTime` + 80 ms |
  | area hit (no collision group, `delay = -1`) | +0.01 s, settles as the step starts |
  | projectile | +0.03 s + flight time |
  | shockwave ring | −0.03 s + expansion time |

- Samples are saved to `calib.tsv` next to the exe (one line per step: name + samples). The release ships `calib_default.tsv` with default calibration for the Raven boss; the locally learned `calib.tsv` takes precedence.

## Projectiles

For ranged attacks such as sword waves, the damage comes from the projectile itself, and the Hit step that fires it is short (Raven's back-jump sword wave Hit step lasts 0.1 s). At range, the enemy has long moved on to another step while the wave is still in the air. Ranged attacks are therefore handled in two phases: an estimate from the step before the projectile exists, then live tracking once it is flying.

**Finding them.** Projectiles are `SBProjectile` actors, pooled per type and kept in the level. The bridge lists them with `FindAllOf("SBProjectile")` once per level (whenever the PlayerController changes), then new pool objects are picked up by `NotifyOnNewObject` callbacks (which only queue them) and appended to `projectiles.txt` by the bridge's 200 ms loop. Each line carries the matching ProjectileTable row's (class name minus `_C`) `AvailableJustParry` / `AvailableJustAction` flags and speeds; Eve's own projectiles (`P_…`) are skipped. The game clamps speed to `[MinSpeed, MaxSpeed]`, so the bridge clamps it before exporting: Raven's sword waves have `Speed` 2000 but `MinSpeed` = `MaxSpeed` = 2700, i.e. 27 m/s.

**Live tracking** (`projectiles.cpp`):

- Every frame it reads each projectile's root component location. An idle pooled projectile sits at the origin or wherever it last disappeared, so it only counts as flying while its position keeps changing (moved within the last 60 ms); if it moves again after more than 0.2 s at rest it is a new shot and old samples are discarded.
- Direction comes from observed motion. Our reads aren't aligned to game frames — two reads can be 0, 1 or 2 frames apart — so velocities from adjacent samples are noisy. Instead it keeps roughly the last 0.15 s of positions and differences the oldest and newest (≥ 80 ms apart).
- The speed magnitude is clamped to the table's [initial, max] (equal for projectiles without acceleration): right after launch the sample window still contains frames before the projectile started moving, so the measured speed is too low.
- The position is extrapolated to now, and the hit time is the first moment the projectile's centre comes within 1.05 m of Eve (a perfect dodge on a sword wave was measured to register at 1.07 m). If the closest approach is between 1.05 and 1.6 m, the time of closest approach is used; anything passing farther than 1.6 m is ignored, as is anything more than 2 s out.
- Result: over the whole flight, the live predicted arrival time stays constant to within 1–2 ms.

**Before the projectile spawns**, the step-based estimate is: settle = Hit step start + offset + `(distance − 5.2 m) / speed`. Projectiles don't start at the enemy: Raven's waves spawn about 4.2 m in front of her, and the hit lands about 1.05 m from Eve, so projectiles always use the fixed 5.2 m (`kProjAhead`) until live tracking takes over.

**Joining the two.** Live-tracked notes are attributed to the most recent projectile-launching enemy Hit step (within 3 s) and carry the same instance + step identity as the estimate, so they dedupe; while a projectile is flying, the estimated note for that step is dropped. If no owning step is found, the projectile's address is used as the identity. The shots of a volley all map to the same step, so each live note also carries its projectile address (`Note.proj`); display smoothing and auto-mode deduplication tell the shots apart, and the 2nd and 3rd shots get parried too.

**Display smoothing.** When the estimate is replaced by the live value, the prediction shifts and the dot on the bar would jump. Ranged notes (projectiles and rings) therefore get a separate display time `tShow`: the displayed arrival time may move by at most 1 s per second, so the dot just speeds up or slows down and quickly converges. If the note was absent on the previous frame (a gap over 0.2 s) or the difference exceeds 0.6 s (it's the next shot), it snaps to the new value. Auto mode uses the raw, unsmoothed prediction. Melee notes aren't smoothed: they're stable anyway, and hitstop pauses after a parry should be shown as they are.

**Perfect dodges.** A projectile's perfect check doesn't go through `IsJustActionActive`, so the judge hook never sees it. When a perfect dodge succeeds, Eve enters a `JustEvade…_Cast1` step; if there was no melee contact within 0.3 s of it, it is recorded as a perfect dodge of a projectile and used to calibrate the most recent ranged Hit step.

## Shockwave rings

Ground shockwave rings (such as the one Raven slams out at the end of her back-jump combo) are neither collision groups nor projectiles. The Hit step spawns an effect at its own position (`CreateEffectSelfPosition`) whose `LoopTargetFilterAlias` refers to a target filter with `bDynamicShapeScale` set. The hit area is a cylinder of radius `FarDistance × scale`, with `scale` going from `MinShapeScale` to `MaxShapeScale` over the effect's `LifeTime`.

SBParry treats it as a projectile that starts at the initial radius and expands at constant speed: initial radius = `FarDistance × Min` (exported in column 16, `ahead`, of `steps.tsv`), speed = `FarDistance × (Max − Min) / LifeTime`. Raven's ring: `FarDistance` 200, scale 1.4 → 10.2 over 1.0 s, i.e. 2.8 m → 20.4 m at 17.6 m/s, about 40 cm tall. Measured hits at 14.0 m @ 630 ms and 17.7 m @ 835 ms come about 30 ms earlier than this model, hence the −30 ms default offset.

Dodging doesn't avoid it: it never produced a perfect dodge, and well-timed dodges still got hit. Ring notes are therefore JUMP notes, and auto mode presses jump about 0.25 s + input latency before arrival (pressing 0.2 s ahead was measured to be too late — Eve got hit just as she left the ground; the jump's airtime is much longer than this).

The tables contain 21 such ring attacks (Crawler, HedgeBoarBrute, ExoSuit, Raven, …); only Raven's has been verified.

## Blue / violet windows

A separate mechanic from JustAction.

- Some enemy Cast steps apply effects to themselves when they start (`StartSelfEffect`, a JSON array): `Chance_BehindSkill*` is blue, `Chance_MoveBackSkill*` is violet.
- The window opens `startDelayTime` seconds into the step and lasts `Time` seconds (falling back to the effect's `LifeTime`, default 1.0 s).
- Range depends on the effect:

  | Effect suffix | Range |
  |---|---|
  | (none) | 4.5 m |
  | `_700` | 6.5 m |
  | `_900` | 9 m |
  | `_1500` | 15 m |

  The bridge parses it from the effect row's `ActiveTargetFilterAlias` (e.g. `Enemy_3DArc_450_120_200` = 450 cm).
- Eve's side is the `FlashBehindAttack` (blue) and `MoveBackAttack` (violet) commands: the dodge button (command 24) with the move input held for at least 0.1 s at 315°–45° (toward the enemy) for blue or 135°–225° (away) for violet.
- The bar shows these as spans. A press is judged as success / early / late / wrong direction or range.
  Detection: the step-transition hook sees every step Eve enters. Windows are recorded from the enemy's step-transition timestamp + `startDelayTime`. Eve entering `FlashBehindAttack*_Cast1` / `MoveBackAttack*_Cast1` = success; entering the first step of a normal (or perfect) dodge is graded early/late in frames relative to the window, and a dodge inside the window that didn't trigger means wrong direction or out of range. Eve's step starts 1–2 frames after the key press, so results right at a window edge can be off by a frame.

## Unhandled attacks and damage

### Unhandled

Whenever an enemy enters a Hit step (`real` = 1, not a mash step), a pending hit is queued with a deadline of predicted settle + 0.35 s. When the deadline passes, an "Unhandled" result is logged if all of the following hold:

- from 0.7 s before the step started until the deadline, Eve entered no step whose name contains `Guard` / `Evade` / `JustParry` / `FlashBehind` / `MoveBack` / `Parry` / `Jump`;
- the judge hook recorded no contact in that period;
- the enemy could reach her: for melee, within the attack's reach + 1 m (reach comes from the step's `ActionAssistTargetFilter`, e.g. `ActionAssist_3DArc_500_120` = 5 m) or 3.5 m if the table has none; projectiles and rings have no distance limit.

"Unhandled" doesn't necessarily mean Eve got hit (the attack may have missed anyway); actual damage comes from the HP bar.

### Damage (HP bar)

Eve's HP lives in native code and isn't easy to get at, so the bridge exports the HUD's HP bar instead — the `ProgressBar` at `WB_MainHUD_PlayerInfo.WidgetTree.ProgressBar_HP` — together with its `Percent` field offset. sbparry.exe reads this 0–1 value every frame.

The bar drop is animated over several frames. Consecutive drops are merged into one event: the first dropping frame is taken as roughly the hit time, and at that moment the event is attributed to the most recently started enemy Hit step (within 3 s — ranged attacks can land after their step has ended). Once the bar has stopped dropping for 0.3 s, one line with the total is logged, e.g. "Took damage −12.3% (step, 630ms after it started)". The shockwave-ring measurements above were obtained this way.

## Finisher

When an enemy is broken and falls, pressing Y (heavy attack) performs a finisher. The target filter of Eve's skill `P_Eve_Sword_Normal_LinkAttack1_1` requires the enemy to be in `ActorState_Groggy` (`ESBActorState` = 7), within 3 m and in front of her.

`ActorState` is native and not reachable through reflection. A memory diff turned up two flags that change at the same moment:

- some boss blueprints (Raven among them) have an `IsGroggy` bool, at a different offset per class. The bridge walks the `SBCharacter` instances and exports `class address:offset` per class;
- `SBCharacter.bActiveWeakPointCollision` flips at the same time and serves as a generic fallback for enemies without `IsGroggy`.

sbparry.exe reads the target enemy's class pointer (`+0x10`), uses its `IsGroggy` offset if known and `bActiveWeakPointCollision` otherwise, and tests the low bit. Both were observed to go 0→1 when the finisher becomes possible and back to 0 once it starts, so it never fires twice.

The catch: the flag rises the moment the enemy breaks, but the finisher only becomes available partway through the fall animation (about 1–1.2 s for Raven), and pressing Y too early just produces a heavy attack. So auto mode:

- waits `g_execDelay` (initially 1.2 s) after the flag rises;
- if the step Eve enters after the press contains `StrongAttack` (it turned into a heavy attack), adds 0.2 s to `g_execDelay` (max 3 s, remembered for the session) and waits 0.9 s for the heavy attack to finish before trying again;
- requires distance ≤ 2.8 m (the game allows 3 m; the in-front requirement is met by Eve turning as she attacks), otherwise it waits.

## Mashing and cutscene QTEs

- **Break-free mashing**: enemy steps with a `NextStepAliasWhenLinkBreak` are clashes or grabs (Raven, Scarlet and several other bosses have them); the bridge exports this as the `mash` column. While an enemy is in such a step, auto mode mashes light attack at about 14 Hz (a 35 ms press every 70 ms, both stretched by Eve's time dilation).
- **Cutscene QTEs**: QTEs in cutscenes are a `SBSequencerQTEWidget` (created the first time a cutscene plays). The bridge exports its address and the offsets of `Visibility`, `InputType`, `InputAction`, `UIInputAction` and `bBindInput`. When the widget is visible (`ESlateVisibility` 0/3/4) and has an input bound, sbparry.exe reads the FName comparison index in `InputAction` (or `UIInputAction` if that is 0), looks it up in the action-name indices exported under `@names` in `keys.txt`, and presses that action; unknown actions fall back to light attack.

## Auto mode

Off by default (Ctrl+Alt+A). When on, it presses buttons for the player based on the raw prediction (not the smoothed `tShow`):

| Note | Action | Timing |
|---|---|---|
| parry-able (cyan) | guard | slightly after the middle of the perfect window: `window/2 + input latency − 25 ms − autoAimMs` before settle |
| dodge-only (amber) | dodge | same |
| neither (red) | plain dodge (i-frames) | same |
| shockwave ring (mint) | jump | 0.25 s + input latency before arrival |
| blue / violet window | hold direction 0.12 s, then dodge | inside the window once the enemy is within range − 0.3 m, or unconditionally near the end of the window; violet fires late in the window (`autoChance`) |
| enemy groggy | heavy attack | see "Finisher" (`autoQte`, Ctrl+Alt+X) |
| clash / grab / cutscene QTE | mash light attack / the QTE's action | see above (break-free mashing ignores `autoQte`) |

Priority per frame is notes > finisher > mashing. The same note is recomputed every frame, so fired notes are deduplicated by instance + step + time (0.45 s tolerance for hits, 0.6 s for blue/violet windows); projectile notes also compare the projectile address, so each shot of a volley gets its own press.

- **Input latency**: the median (last 15 samples) of the time from an automatic press to the press hook seeing it; 70 ms (measured 3–5 frames) until there's data.
- **Retry**: if Eve shows no step change 0.14 s after a guard/dodge press, the input wasn't accepted; if the hit is still more than 30 ms away, the press is retried once.
- **Presses postponed during the perfect-dodge invulnerability**: after a perfect dodge Eve plays a dedicated sequence (JustEvade Cast1+Cast2, 0.6 s of game time, about 0.88 s real with the slow motion) during which she is invulnerable but cannot dodge again. A dodge pressed during it is buffered and fires when the sequence ends, as a mistimed normal dodge that also uses up the dodge for the next hit (that is how Scarlet's BackDashSpaceCut hits 5 and 6, 0.6 s apart, landed twice in a row). So for 0.85 s after the JustEvade start, hits predicted to land inside that span are not pressed (`skip-invuln` in `timing.csv`). This is only a postponement, not marked handled: ranged predictions can move (Scarlet's `PhaseChange2_AttackRange_Hit3` was predicted inside the invulnerability but arrived 0.5 s later), and if the hit still hasn't landed once the span is over it is pressed as usual.
- **Pause**: nothing is sent while the game isn't in the foreground or while Ctrl / Alt is held (e.g. right after a hotkey, so the keys don't combine), and all simulated input is released.
- **Hold times**: the game checks inputs in game time (a dodge needs 0.02 s held, the blue/violet direction 0.1 s). When Eve is slowed (boss-burst slow motion etc.), real hold times are stretched by Eve's time dilation (`WorldSettings.TimeDilation` × Eve's `CustomTimeDilation` at `+0xB0`).

Keys come from the bridge's `keys.txt` (Guard / Evade / AttackLight / AttackStrong / Jump / Interaction_Key action mappings and MoveForward / MoveRight axis mappings from `Default__InputSettings`; bindings with modifiers are skipped).

The device follows whatever the player last used **for real**, or is pinned with `autoDevice`:

- Keyboard/mouse: watched through Raw Input (`RIDEV_INPUTSINK`). Events injected with `SendInput` arrive with `hDevice` = 0, which is how our own presses are excluded; keys pressed with Ctrl/Alt held and tiny mouse jitter don't count either.
- Pads: the in-game pad stubs record `xiLastReal / psLastReal` whenever the real pad has a button down or a stick outside the deadzone (checked before our buttons are OR'ed in).
- Before any real input has been seen (auto mode running from startup), it uses pad injection if an XInput pad is being polled, otherwise the keyboard. The device only switches while no action is in progress.

Injection per device:

- **Keyboard**: `SendInput`, so the game must be in the foreground.
- **XInput / DualSense**: no virtual controller driver. The game's import slots for `XInputGetState` and libScePad's `scePadReadState` are pointed at small stubs in the cave. A stub calls the original, then, while `PadCtrl.xiActive / psActive` is set, ORs `xiButtons / psButtons` into the returned state and optionally overrides the stick.
- Delay-loaded imports may be resolved later and overwrite the slot, so the slots are checked and re-applied every second (`RefreshPadHooks`).

## One-hit kill

For skipping boss phases (Ctrl+Alt+K; not saved to the ini, always off at startup). Step table rows hold `SkillAttackDamageRate` at `+0x20` and `SkillShieldAttackDamageRate` at `+0x24`. Turning it on walks all of Eve's steps (`P_…`), and for every row where the two rates aren't both zero multiplies them by 1000 directly in the game's in-memory step table (heap memory, already writable), saving the originals.

The originals are written back when it is toggled off, on normal exit, on Windows logoff / shutdown (`WM_ENDSESSION`) and when SBParry itself crashes (an unhandled-exception filter, `SetUnhandledExceptionFilter`). If SBParry is killed from Task Manager they can't be restored and stay until the game is restarted. When a failed read forces a reconnect the game is still running, so they are written back before detaching; if the game process itself exited, the saved values are simply dropped (the table is gone with it).

## Why trampolines live in the module's 0xCC padding

Hook sites are 5–9 bytes, enough only for `jmp rel32` (±2 GB). After the game has been running a while there is often no free 64 KB region within ±2 GB of the module, so a "near" allocation fails. Instead:

```
hook site  E9 rel32  ──►  ≥16 bytes of 0xCC alignment padding in the module's .text
                           FF 25 00 00 00 00 <abs64>   (14-byte absolute jump)
                              ──►  cave (VirtualAllocEx, any address)
                                    stub … replaced original instruction …
                                    FF 25 … absolute jump back to site+N
```

The padding search requires the preceding byte to be 0xCC too, so a stray CC inside an instruction isn't mistaken for padding.

## Restore on exit, reuse after a crash

- **Normal exit** (Ctrl+Alt+Q / tray / `--quit`, as well as logoff / shutdown and a crash of SBParry): all simulated input is released, one-hit-kill rates are restored, the three hook sites get their original bytes back, the whole `PadCtrl` block is zeroed (releasing injected pad buttons), and the import slots are restored (only if they still point at our stubs). The trampolines and the cave are left in place: a game thread may have just jumped in and be executing there, so overwriting them with 0xCC or freeing them could crash it.
- **Failed attach**: if attaching fails before any hook site is written, the allocated cave is freed. A game process that failed to attach is retried every 30 s instead of every 2 s.
- **Previous run didn't restore** (sbparry crashed or was killed): the judge entry already starts with `E9 …`. sbparry follows `jmp → trampoline → cave`, checks the cave header at `+0x3F00` for the magic (`"SBP3"`, or the older `"SBP2"`) and that the recorded hook addresses match; if so it restores everything recorded there and installs a fresh set of hooks. If not, something else patched the function and it gives up.

Cave header:

```c
struct CaveHeader {
    uint32_t magic /* 'SBP3' */, pad;
    uint64_t judge, press, step, tramp[3];               // same as the old SBP2 header
    uint64_t iatXInput, origXInput, iatScePad, origScePad; // gamepad import slots and original values
};
```

Cave layout (16 KB):

| Offset | Content |
|---|---|
| `+0x0000` | judge hook block |
| `+0x0100` | event ring buffer (index + 256 × 0x30) |
| `+0x3200` / `+0x3300` | press / step hook blocks |
| `+0x3400` / `+0x3500` | XInputGetState / scePadReadState stubs |
| `+0x3E00` | `PadCtrl` (written by sbparry, read by the stubs) |
| `+0x3F00` | `CaveHeader` |

All blocks are written in `src/hooks.asm` (ml64). At runtime each block is copied into the cave, its placeholder constants (log / PadCtrl address) are patched, and its last 8 bytes receive the return address or the original function pointer.

## Files exported by the bridge

All files go to `ue4ss/Mods/SBParryBridge/`.

| File | When | Content |
|---|---|---|
| `steps.tsv` | once the table is loaded (retried every 2 s) | `#table=0x…`, `#version=7`, then 17 columns per row, see below |
| `live.txt` | checked every second, rewritten only on change | object addresses and field offsets, see below |
| `projectiles.txt` | full list once per level, new pool objects appended | one line per projectile: `0xaddr row jp ja initial max` |
| `keys.txt` | checked every 3 s, written on change | bindings and action-name FName indices, see below |

`steps.tsv`, `live.txt`, `keys.txt` and full rewrites of `projectiles.txt` are written to a `.tmp` file and renamed, so sbparry.exe never reads a half-written file. A `live.txt` or `projectiles.txt` last modified before the game process started is left over from a previous game session (all its addresses are wrong) and is ignored.

### steps.tsv (version 7)

| Column | Content |
|---|---|
| 0 | index (RowMap order) |
| 1 | step name |
| 2 | type: 0 = Cast wind-up, 1 = Hit |
| 3 | duration (s) |
| 4 | index of the `NextStepAlias` step, −1 = none |
| 5 / 6 | parry-able / perfect-dodgeable (`AvailableJustParry` / `AvailableJustAction`; taken from the projectile table for projectile steps; OR'ed with the effect's `AvailableJustEvade` for rings and damage zones) |
| 7 | first attack collision's `DelayTime`, −1 = no collision group |
| 8 / 9 / 10 / 11 | chance window: kind (0 / 1 blue / 2 violet), start, length (s), range (m) |
| 12 | mash (1 = has `NextStepAliasWhenLinkBreak`) |
| 13 | projectile / ring speed (m/s), 0 = melee |
| 14 | reach (m, from `ActionAssistTargetFilter`), 0 = unknown |
| 15 | real attack (`real`, see "Note prediction") |
| 16 | `ahead`: shockwave ring initial radius (m), −1 for everything else. sbparry.exe treats a step with `ahead ≥ 0` and speed > 0 as a ring; projectile estimates always use the fixed `kProjAhead` 5.2 m until live tracking takes over |

sbparry.exe doesn't read the `#version` line. It parses columns and accepts any row with at least 8 of them (older bridges wrote fewer); missing columns take defaults.

### live.txt

```
pc=0x…                              PlayerController; sbparry.exe follows it to the pawn, camera and lock-on target
ws=0x… dil=0x…                      WorldSettings and the TimeDilation offset (slow motion)
groggy=0xclass:0xoff,… weak=0x…     enemy classes with IsGroggy and its offset; SBCharacter.bActiveWeakPointCollision offset
hp=0x… pct=0x…                      Eve's HP ProgressBar and its Percent offset
qte=0x… vis=0x… type=0x… action=0x… uiaction=0x… bind=0x…   cutscene QTE widget and field offsets
```

`FindFirstOf` / `FindAllOf` walk every UObject (about 30 ms), so searches are rationed:

- the PlayerController is re-searched every 5 s while the cached one is invalid;
- when the PlayerController changes (new game / load / level change), the HP bar, QTE widget and enemy classes are all scanned once;
- after that there is no periodic full scan: the HP bar and QTE widget are rescanned only 5 s after one went stale, every 30 s while no HP bar has been found, or when a QTE widget is created (`NotifyOnNewObject`);
- new enemy classes are picked up by `NotifyOnNewObject` on `SBCharacter`, and their `IsGroggy` offsets are filled in by the once-a-second loop;
- the file is only rewritten when its content changes. sbparry.exe re-reads it every second.

### projectiles.txt

The first line is `#gen=0x<PlayerController address>`, then one line per projectile: `0xaddr row jp ja initial max`, speeds in cm/s, already clamped to `MinSpeed` / `MaxSpeed`; for projectiles without acceleration max = initial, and max 0 means unlimited. The whole file is rewritten with a new `#gen` when the PlayerController changes (new level); sbparry.exe checks the file size and modification time every 100 ms, re-reads it on change, and forgets all old addresses when `#gen` differs. A new object's class is already known at construction, so its address is written right away (appended by the 200 ms loop); it only counts as flying once it leaves the origin.

### keys.txt

```
Guard=E,ThumbMouseButton,Gamepad_LeftShoulder
Evade=…  AttackLight=…  AttackStrong=…  Jump=…  Interaction_Key=…
MoveForward=W:1,S:-1,Gamepad_LeftY:1
MoveRight=…
@names=19454016:AttackLight,…
```

`@names` holds the FName comparison indices of the action names above and of `UI_QTE_<action>`. The cutscene QTE widget stores an FName, and these indices map it back to the action to press.

## Debug log timing.csv

With `debugLog=1` in `sbparry.ini`, `timing.csv` is appended to in the program directory. Each line starts with the local time `HH:MM:SS.mmm`, followed by a type:

| Type | Remaining fields |
|---|---|
| `step` | step name, TSC (every step change, Eve's and enemies') |
| `auto` | action (guard / evade / jump), step name or `projectile`, predicted ms, enemy rate, Eve's time dilation, distance |
| `perfect` / `early` | parry / evade, window ms, lead ms, prediction error ms, enemy Hit step, type, step-start-to-settle ms, first collision delay ms |
| `perfect,evade,projectile` | lead ms, ranged Hit step it calibrated |
| `chance` | blink / repulse, ok / early / late / missed, window ms, offset ms |
| `unhandled` | step name, distance |
| `damage` | attributed step, ms since it started, distance (logged on the first dropping frame) |
| `proj` | every frame, per flying projectile: address, predicted arrival ms, speed m/s, predicted closest distance m, current distance m |

The `proj` lines are for chasing drift of the dots on the bar; the "constant within 1–2 ms" result above came from them.
