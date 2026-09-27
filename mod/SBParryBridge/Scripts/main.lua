-- SBParryBridge：把 SBParry.exe 需要、但只能通过 UE 反射拿到的数据导出成文本文件
--   steps.tsv  步骤表（SkillActiveStepTable），按 RowMap 顺序
--   live.txt   PlayerController、慢动作、可惩戒标记、血条、过场 QTE 控件的地址 / 偏移（见 exportLive）
--   projectiles.txt  飞行道具实例（对象池）的地址、能否完美弹反/闪避、速度
--   keys.txt   当前键位（格挡 / 闪避 / 轻攻击 / 移动），自动操作按玩家自己的键位按
-- 全部写在本 mod 目录（ue4ss/Mods/SBParryBridge/）

local DIR = "ue4ss/Mods/SBParryBridge/"
local STEP_TABLE = "/Game/Local/Data/SkillActiveStepTable.SkillActiveStepTable"
local EFFECT_TABLE = "/Game/Local/Data/EffectTable.EffectTable"
local PROJECTILE_TABLE = "/Game/Local/Data/ProjectileTable.ProjectileTable"

-- 整个文件先写到 .tmp 再改名：SBParry 随时在读，不能让它读到写了一半的文件。
-- Windows 上 rename 不能覆盖，先删旧的；SBParry 正开着旧文件时删 / 改名会失败，返回 false 下次再写
local function writeFile(name, text)
    local f = io.open(DIR .. name .. ".tmp", "w")
    if not f then return false end
    f:write(text)
    f:close()
    os.remove(DIR .. name)
    return os.rename(DIR .. name .. ".tmp", DIR .. name) ~= nil
end

-- 蓝光（闪到身后）/ 紫光（后撤）机会效果：默认持续时间和触发距离从效果表读
local function chanceInfo()
    local info = {}
    local et = StaticFindObject(EFFECT_TABLE)
    if not et or not et:IsValid() then return info end
    et:ForEachRow(function(name, row)
        name = tostring(name)
        local kind = name:find("^Chance_BehindSkill") and 1 or name:find("^Chance_MoveBackSkill") and 2 or nil
        if kind then
            -- ActiveTargetFilterAlias 形如 Enemy_3DArc_450_120_200：距离 450cm
            local range = tonumber(row.ActiveTargetFilterAlias:ToString():match("_(%d+)_%d+_%d+$") or "0") / 100
            info[name] = {kind = kind, life = row.LifeTime, range = range}
        end
    end)
    return info
end

-- 步骤开始时挂的效果是 JSON 数组字符串，例如
-- [{"Alias":"Chance_BehindSkill", "Time":0.633, "startDelayTime":0.9}, ...]
local function findChance(json, info)
    for obj in json:gmatch("{[^{}]*}") do
        local alias = obj:match('"Alias"%s*:%s*"([%w_]+)"')
        local ci = alias and info[alias]
        if ci then
            local t = tonumber(obj:match('"Time"%s*:%s*([%d%.]+)') or "") or ci.life
            local d = tonumber(obj:match('"startDelayTime"%s*:%s*([%d%.]+)') or "") or 0
            return ci.kind, d, t, ci.range
        end
    end
    return 0, 0, 0, 0
end

-- 飞行道具（剑气等）：伤害来自道具本身，可否完美弹反/闪避看道具表，不看发射它的 Hit 步骤
local function projectileInfo()
    local info = {}
    local pt = StaticFindObject(PROJECTILE_TABLE)
    if not pt or not pt:IsValid() then return info end
    pt:ForEachRow(function(name, row)
        -- 实际速度会被夹在 [MinSpeed, MaxSpeed] 里（渡鸦剑气 Speed=2000 但 Min=Max=2700，实际 27m/s）；
        -- 有加速度且还没到上限时按初速和上限的平均估计
        local v, lo, hi = row.Speed, row.MinSpeed, row.MaxSpeed
        if lo > 0 and v < lo then v = lo end
        if hi > 0 and v > hi then v = hi end
        if row.Accelation > 0 and hi > v then v = (v + hi) / 2 end
        info[tostring(name)] = {jp = row.AvailableJustParry, ja = row.AvailableJustAction, speed = v}
    end)
    return info
end

-- 自身位置生成的效果（CreateEffectSelfPosition）里会打人的两种：
-- 扩散的冲击波环（如渡鸦后跳连段落地砸出的环）：Hit 步骤在自身位置生成效果，
-- 效果每隔一小段时间用 LoopTargetFilterAlias 的范围检测，范围按 bDynamicShapeScale 在 LifeTime 内从 MinShapeScale 放大到 MaxShapeScale。
-- 当成从 外径×Min 处出发、速度 外径×(Max-Min)/LifeTime 的飞行道具。高度只有几十厘米，要跳（实测闪避躲不掉）。
-- 原地的伤害区域（效果对范围内的其他目标生效，如红莲分身斩击 HitZoneArea3）：步骤开始就结算，按范围攻击处理
local TARGET_FILTER_TABLE = "/Game/Local/Data/TargetFilterTable.TargetFilterTable"
local function waveInfo()
    local filters, waves = {}, {}
    local ft = StaticFindObject(TARGET_FILTER_TABLE)
    local et = StaticFindObject(EFFECT_TABLE)
    if not ft or not ft:IsValid() or not et or not et:IsValid() then return waves end
    ft:ForEachRow(function(name, row)
        if row.bDynamicShapeScale and row.MaxShapeScale > row.MinShapeScale then
            filters[tostring(name)] = {far = row.FarDistance, lo = row.MinShapeScale, hi = row.MaxShapeScale}
        end
    end)
    et:ForEachRow(function(name, row)
        local fl = filters[row.LoopTargetFilterAlias:ToString()]
        if fl and row.LifeTime > 0 then
            waves[tostring(name)] = {speed = fl.far * (fl.hi - fl.lo) / row.LifeTime / 100, ahead = fl.far * fl.lo / 100,
                                     ja = row.AvailableJustEvade}
        else
            -- 不扩散的伤害区域：效果生效时对范围内的目标（不是自己）结算，比如红莲分身的斩击 HitZoneArea3（16m，0.1 秒）
            local target = row.ActiveTargetFilterAlias:ToString()
            if row.LoopTargetFilterAlias:ToString() ~= "None" or (target ~= "None" and target ~= "Self") then
                waves[tostring(name)] = {zone = true, ja = row.AvailableJustEvade}
            end
        end
    end)
    return waves
end

-- 列：序号 名字 类型(0=Cast 1=Hit) 时长 下一步序号 可完美弹反 可完美闪避 首个判定框延迟 机会类型(0/1蓝/2紫) 机会开始 机会时长 机会距离(米)
--     挣脱连打(1=这一步有 NextStepAliasWhenLinkBreak：拼刀/被抓时连打轻攻击挣脱) 飞行道具速度(米/秒，0=近战)
--     攻击范围(米，来自 ActionAssistTargetFilter，如 ActionAssist_3DArc_500_120 = 5m，0=未知)
--     真实攻击(1=有攻击碰撞组、飞行道具或范围判定；0=脚本演出，如拼刀/抓取成功后的连段)
--     出发距离(米，飞行道具生成处离敌人多远；冲击波环 = 初始外径；-1=未知)
local function dumpSteps()
    local dt = StaticFindObject(STEP_TABLE)
    if not dt or not dt:IsValid() then return false end
    local info = chanceInfo()
    local proj = projectileInfo()
    local waves = waveInfo()
    local rows, index = {}, {}
    dt:ForEachRow(function(name, row)
        -- 首个判定框的延迟：从 AttackCollisionGroupArray 的 JSON 字符串取（JsonArray 是游戏载入后才解析填进去的，
        -- 开局导出时可能还空着）。没有碰撞组 = -1
        local cga = row.AttackCollisionGroupArray:ToString()
        local delay = -1
        local first = cga:match("{[^{}]*}")
        if first then delay = tonumber(first:match('"DelayTime"%s*:%s*(-?[%d%.]+)') or "") or 0 end
        local ck, cs, cl, cr = findChance(row.StartSelfEffect:ToString(), info)
        local jp, ja, speed = row.AvailableJustParry, row.AvailableJustAction, 0
        local pa = row.UsableNonTargetProjectileAliasArray
        if #pa == 0 then pa = row.UsableTargetProjectileAliasArray end
        if #pa > 0 then
            local p = proj[pa[1]:ToString()]
            -- 伤害来自道具，能否弹反/闪避以道具表为准：红莲 PhaseChange2_AttackRange 的步骤标着可弹反，
            -- 道具却是不可格挡、只能完美闪避（按格挡照样挨打）
            if p then jp, ja, speed = p.jp, p.ja, p.speed / 100 end
        end
        -- Hit 步骤在自身位置生成的效果：扩散的冲击波环（当作飞行道具，要跳）或原地的伤害区域（步骤开始就结算）
        local ahead, isWave = -1, false
        for alias in row.CreateEffectSelfPosition:ToString():gmatch('"Alias"%s*:%s*"([%w_]+)"') do
            local w = waves[alias]
            if w then
                if not w.zone then speed, ahead = w.speed, w.ahead end
                ja, isWave = ja or w.ja, true
                break
            end
        end
        rows[#rows + 1] = {
            name = tostring(name), type = row.Type, dur = row.Duration, next = row.NextStepAlias:ToString(),
            jp = jp, ja = ja, delay = delay, speed = speed,
            -- 范围判定（OverrideTargetFilterAlias，如 BurstAreaSlash_Hit1 = 12m 圆柱）和自身位置生成的伤害效果也是真打
            real = cga ~= "" or #pa > 0 or row.OverrideTargetFilterAlias:ToString() ~= "None" or isWave,
            ahead = ahead,
            reach = tonumber(row.ActionAssistTargetFilter:ToString():match("^ActionAssist_3D%a+_(%d+)") or "0") / 100,
            ck = ck, cs = cs, cl = cl, cr = cr, mash = row.NextStepAliasWhenLinkBreak:ToString() ~= "None",
        }
        index[tostring(name)] = #rows - 1
    end)
    local out = {string.format("#table=0x%X\n#version=7\n", dt:GetAddress())}
    for i, r in ipairs(rows) do
        out[#out + 1] = string.format("%d\t%s\t%d\t%.4f\t%d\t%d\t%d\t%.4f\t%d\t%.4f\t%.4f\t%.1f\t%d\t%.1f\t%.1f\t%d\t%.1f\n", i - 1, r.name, r.type, r.dur,
            index[r.next] or -1, r.jp and 1 or 0, r.ja and 1 or 0, r.delay, r.ck, r.cs, r.cl, r.cr, r.mash and 1 or 0, r.speed, r.reach, r.real and 1 or 0, r.ahead)
    end
    if not writeFile("steps.tsv", table.concat(out)) then return false end
    print(string.format("[SBParryBridge] exported %d steps\n", #rows))
    return true
end

-- 表可能晚于 mod 加载，重试到成功为止
local stepsDone = false
LoopInGameThreadWithDelay(2000, function()
    if stepsDone then return end
    local ok, r = pcall(dumpSteps)
    if ok and r then stepsDone = true elseif not ok then print("[SBParryBridge] " .. tostring(r) .. "\n") end
end)

-- 实时信息（live.txt）：
--   pc=     PlayerController（SBParry 自己顺着读 Pawn / 相机 / 锁定目标）
--   ws=     WorldSettings 与 TimeDilation 偏移（慢动作）
--   groggy= 可惩戒标记：部分 Boss 蓝图有 IsGroggy（渡鸦等，偏移各类不同），导出“类地址:偏移”；
--           weak= 通用的 SBCharacter.bActiveWeakPointCollision，没有 IsGroggy 的敌人看它
--   hp=     伊芙血条（HUD 进度条，Percent 0~1）：血量在原生代码里，读血条来知道“挨打了”
--   qte=    过场 QTE 控件 SBSequencerQTEWidget 与字段偏移
-- FindFirstOf/FindAllOf 要遍历全部 UObject（约 30ms，卡游戏线程），能不搜就不搜：
--   PlayerController 失效时 5 秒一搜；换了 PlayerController（开局 / 读档 / 换关卡）全量扫一次血条、QTE 控件、敌人类；
--   之后只在有东西失效（5 秒后）、还没找到血条（30 秒一次）、或新建了 QTE 控件时重扫血条和 QTE 控件；
--   新出现的敌人类由 NotifyOnNewObject 记下，在这里补算 IsGroggy 偏移。
-- 内容没变就不写文件
local cachedPC, lastPCSearch = nil, -100
local cachedHp, cachedQte, extraLines, lastScan = nil, nil, "", -100
local qteCreated = false
local groggyCls, newChars = {}, {} -- 类地址 -> IsGroggy 偏移（没有为 false）；新建的敌人，等循环处理

local function propOffset(clsPath, name)
    local off
    local c = StaticFindObject(clsPath)
    while c and c:IsValid() and not off do
        c:ForEachProperty(function(p) if p:GetFName():ToString() == name then off = p:GetOffset_Internal() end end)
        c = c:GetSuperStruct()
    end
    return off or 0
end

-- 记下一个角色的类有没有 IsGroggy；有新类返回 true
local function addCharClass(a)
    if not a or not a:IsValid() then return false end
    local c = a:GetClass()
    local addr = c:GetAddress()
    if groggyCls[addr] ~= nil then return false end
    local off, k = false, c
    while k and k:IsValid() and not off do
        k:ForEachProperty(function(p) if p:GetFName():ToString() == "IsGroggy" then off = p:GetOffset_Internal() end end)
        k = k:GetSuperStruct()
    end
    groggyCls[addr] = off
    return true
end

local function groggyLine()
    local parts = {}
    for addr, off in pairs(groggyCls) do
        if off then parts[#parts + 1] = string.format("0x%X:0x%X", addr, off) end
    end
    table.sort(parts)
    return string.format("groggy=%s weak=0x%X\n", table.concat(parts, ","),
        propOffset("/Script/SB.SBCharacter", "bActiveWeakPointCollision"))
end

local function scanWidgets()
    cachedHp, cachedQte = nil, nil
    for _, w in ipairs(FindAllOf("ProgressBar") or {}) do
        if w:IsValid() and w:GetFullName():find("WB_MainHUD_PlayerInfo%.WidgetTree%.ProgressBar_HP$") then cachedHp = w end
    end
    -- 过场 QTE 控件第一次播过场时才创建
    for _, w in ipairs(FindAllOf("SBSequencerQTEWidget") or {}) do
        if w:IsValid() and w:GetFullName():find("Transient") then cachedQte = w end
    end
end

local function extraText()
    local lines = {groggyLine()}
    if cachedHp and cachedHp:IsValid() then
        lines[#lines + 1] = string.format("hp=0x%X pct=0x%X\n", cachedHp:GetAddress(), propOffset("/Script/UMG.ProgressBar", "Percent"))
    end
    if cachedQte and cachedQte:IsValid() then
        local W = "/Script/SB.SBSequencerQTEWidget"
        lines[#lines + 1] = string.format("qte=0x%X vis=0x%X type=0x%X action=0x%X uiaction=0x%X bind=0x%X\n", cachedQte:GetAddress(),
            propOffset("/Script/UMG.Widget", "Visibility"), propOffset(W, "InputType"), propOffset(W, "InputAction"),
            propOffset(W, "UIInputAction"), propOffset(W, "bBindInput"))
    end
    return table.concat(lines)
end

local liveText = ""
local function exportLive()
    local newPC = false
    if not (cachedPC and cachedPC:IsValid()) and os.clock() - lastPCSearch >= 5 then
        lastPCSearch = os.clock()
        local pc = FindFirstOf("PlayerController")
        cachedPC = pc and pc:IsValid() and pc or nil
        newPC = cachedPC ~= nil
    end
    if not cachedPC then return end
    if newPC then
        for _, a in ipairs(FindAllOf("SBCharacter") or {}) do addCharClass(a) end
        newChars = {}
    end
    for _, a in ipairs(newChars) do addCharClass(a) end
    newChars = {}
    local stale = (cachedHp and not cachedHp:IsValid()) or (cachedQte and not cachedQte:IsValid())
    local wait = stale and 5 or not cachedHp and 30 or nil
    if newPC or qteCreated or (wait and os.clock() - lastScan >= wait) then
        lastScan, qteCreated = os.clock(), false
        scanWidgets()
    end
    local ws = FindFirstOf("WorldSettings")
    local text = string.format("pc=0x%X\n", cachedPC:GetAddress())
    if ws and ws:IsValid() then
        text = text .. string.format("ws=0x%X dil=0x%X\n", ws:GetAddress(), propOffset("/Script/Engine.WorldSettings", "TimeDilation"))
    end
    text = text .. extraText()
    if text ~= liveText and writeFile("live.txt", text) then liveText = text end
end
LoopInGameThreadWithDelay(1000, function() pcall(exportLive) end)
-- 回调可能不在游戏线程：只记下来，由上面的循环处理
pcall(NotifyOnNewObject, "/Script/SB.SBCharacter", function(o) newChars[#newChars + 1] = o end)
pcall(NotifyOnNewObject, "/Script/SB.SBSequencerQTEWidget", function() qteCreated = true end)

-- 飞行道具实例（SBProjectile，按类型建对象池，常驻关卡里）：SBParry 每帧读它们的位置、算速度，预测什么时候打到伊芙。
-- projectiles.txt：首行 "#gen=PlayerController地址"（每次全量重写换一代，SBParry 据此丢掉旧地址），
-- 之后每行：地址 道具表行名 可完美弹反 可完美闪避 初速 最高速（厘米/秒，已按 Min/MaxSpeed 夹过；最高速 0=不限）。
-- 开局 / 换关卡时全量搜一次，之后新建的池对象由循环追加
local projRows, projFile = nil, DIR .. "projectiles.txt"
local function projLine(o)
    if not o or not o:IsValid() then return nil end
    local name = o:GetClass():GetFName():ToString():gsub("_C$", "")
    if name:find("^P_") then return nil end -- 伊芙自己的子弹
    if not projRows then
        projRows = {}
        local pt = StaticFindObject(PROJECTILE_TABLE)
        if pt and pt:IsValid() then
            pt:ForEachRow(function(n, row)
                local v, lo, hi = row.Speed, row.MinSpeed, row.MaxSpeed
                if lo > 0 and v < lo then v = lo end
                if hi > 0 and v > hi then v = hi end
                projRows[tostring(n)] = {row.AvailableJustParry, row.AvailableJustAction, v, row.Accelation > 0 and hi or v}
            end)
        end
    end
    local r = projRows[name]
    if not r then return nil end
    return string.format("0x%X %s %d %d %.0f %.0f\n", o:GetAddress(), name, r[1] and 1 or 0, r[2] and 1 or 0, r[3], r[4])
end
local projPC, newProjs = nil, {}
local function exportProjectiles()
    if not cachedPC then return end
    if projPC ~= cachedPC then
        local lines = {string.format("#gen=0x%X\n", cachedPC:GetAddress())}
        for _, o in ipairs(FindAllOf("SBProjectile") or {}) do
            local l = projLine(o)
            if l then lines[#lines + 1] = l end
        end
        if not writeFile("projectiles.txt", table.concat(lines)) then return end
        projPC, newProjs = cachedPC, {}
        return
    end
    if #newProjs == 0 then return end
    local lines = {}
    for _, o in ipairs(newProjs) do
        local l = projLine(o)
        if l then lines[#lines + 1] = l end
    end
    newProjs = {}
    local f = #lines > 0 and io.open(projFile, "a")
    if f then f:write(table.concat(lines)); f:close() end
end
LoopInGameThreadWithDelay(200, function() pcall(exportProjectiles) end)
-- 构造时类名已确定；地址先写上，SBParry 读到位置不在原点才当作在飞
pcall(NotifyOnNewObject, "/Script/SB.SBProjectile", function(o) newProjs[#newProjs + 1] = o end)

-- 键位：游戏改键后写回 InputSettings 的映射，这里读当前值。内容没变就不写文件
--   Guard=E,ThumbMouseButton,Gamepad_LeftShoulder
--   MoveForward=W:1,S:-1,Gamepad_LeftY:1
--   @names=19454016:AttackLight,...   动作名的 FName 序号（过场 QTE 控件里存的是 FName，用序号对照）
local lastKeys = ""
local function exportKeys()
    local s = StaticFindObject("/Script/Engine.Default__InputSettings")
    if not s or not s:IsValid() then return end
    local lines = {}
    local actions = {"Guard", "Evade", "AttackLight", "AttackStrong", "Jump", "Interaction_Key"}
    for _, a in ipairs(actions) do
        local out, ks = {}, {}
        s:GetActionMappingByName(FName(a), out)
        for i = 1, #out do
            local m = out[i]:get()
            local k = m.Key.KeyName:ToString()
            -- 带修饰键的组合跳过（自动操作只按单键）
            if k ~= "None" and not m.bShift and not m.bCtrl and not m.bAlt and not m.bCmd then ks[#ks + 1] = k end
        end
        lines[#lines + 1] = a .. "=" .. table.concat(ks, ",")
    end
    for _, a in ipairs({"MoveForward", "MoveRight"}) do
        local out, ks = {}, {}
        s:GetAxisMappingByName(FName(a), out)
        for i = 1, #out do
            local m = out[i]:get()
            local k = m.Key.KeyName:ToString()
            if k ~= "None" then ks[#ks + 1] = string.format("%s:%g", k, m.Scale) end
        end
        lines[#lines + 1] = a .. "=" .. table.concat(ks, ",")
    end
    local names = {}
    for _, a in ipairs(actions) do
        for _, n in ipairs({a, "UI_QTE_" .. a}) do
            local ok, idx = pcall(function() return FName(n):GetComparisonIndex() end)
            if ok and idx then names[#names + 1] = string.format("%d:%s", idx, a) end
        end
    end
    lines[#lines + 1] = "@names=" .. table.concat(names, ",")
    local text = table.concat(lines, "\n") .. "\n"
    if text ~= lastKeys and writeFile("keys.txt", text) then lastKeys = text end
end
LoopInGameThreadWithDelay(3000, function() pcall(exportKeys) end)
