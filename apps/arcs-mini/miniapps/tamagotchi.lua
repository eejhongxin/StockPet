-- A monochrome, pixel-based virtual pet for the Mini3 miniapp runtime.
-- Upload to /miniapp/tamagotchi.lua; this path owns a separate save slot.
assert(app.api_version >= 5 and screen and app.width == 240 and app.height == 240,
       "Pocket pet needs miniapp API 5 and a 240x240 screen")

local C = {case=0xB8C19B, lcd=0xD0D7B2, ink=0x28382D, dim=0xA6B28C, faint=0xBBC69E}
local HATCH, CHILD, ADULT = 20, 320, 1820
-- Monochrome LCD sprites: '.' transparent, '#' ink, 'o' body fill.
-- Pet sprites fit within 24 x 21 pixels; draw at 4x scale.
local SPRITES = {
    egg = {
        "......####......",
        "....##oooo##....",
        "...#oooooooo#...",
        "..#oooooooooo#..",
        "..#oo##oooooo#..",
        ".#ooo##ooooooo#.",
        ".#oooooooooooo#.",
        ".#ooooooo##ooo#.",
        "#oooooooo##oooo#",
        "#oooooooooooooo#",
        "#ooo##ooooooooo#",
        "#ooo##ooooooooo#",
        "#oooooooooooooo#",
        ".#oooooooooooo#.",
        ".#ooooooo##ooo#.",
        "..#oooooo##oo#..",
        "...##oooooo##...",
        ".....######.....",
    },
    baby = {
        ".....######.....",
        "...##oooooo##...",
        "..#oooooooooo#..",
        ".#oooooooooooo#.",
        ".#ooo##oo##ooo#.",
        "#oooo##oo##oooo#",
        "#oooooooooooooo#",
        "#oooooo##oooooo#",
        "#oooooooooooooo#",
        ".#oooooooooooo#.",
        "..#oooooooooo#..",
        "...##########...",
        "...###....###...",
        "..####....####..",
    },
    child = {
        "....##........##....",
        "...#oo#......#oo#...",
        "...#ooo######ooo#...",
        "..#oooooooooooooo#..",
        ".#oooooooooooooooo#.",
        ".#oooo##oooo##oooo#.",
        "#ooooo##oooo##ooooo#",
        "#oooooooooooooooooo#",
        "#oooooooo##oooooooo#",
        "#oooooooooooooooooo#",
        "#oooooooooooooooooo#",
        ".#oooooooooooooooo#.",
        ".#oooooooooooooooo#.",
        "..#oooooooooooooo#..",
        "...##oooooooooo##...",
        ".....##########.....",
        "....###......###....",
        "...####......####...",
    },
    adult = {
        ".....#..........#.....",
        "....###........###....",
        "...#####......#####...",
        "...#####oooooo#####...",
        "..#oooooooooooooooo#..",
        ".#oooooooooooooooooo#.",
        ".#oooooooooooooooooo#.",
        "#ooooo##oooooo##ooooo#",
        "#ooooo##oooooo##ooooo#",
        "#oooooooooooooooooooo#",
        "#ooooooooo##ooooooooo#",
        "#oooooooooooooooooooo#",
        ".#oooooooooooooooooo#.",
        ".#oooooooooooooooooo#.",
        "..#oooooooooooooooo#..",
        "..#oooooooooooooooo#..",
        "...##oooooooooooo##...",
        ".....############.....",
        "....###........###....",
        "...####........####...",
    },
    adult_round = {
        "....####......####....",
        "...#oooo#....#oooo#...",
        "..#oooooo####oooooo#..",
        "..#oooooooooooooooo#..",
        ".#oooooooooooooooooo#.",
        ".#oooooooooooooooooo#.",
        "#ooooo##oooooo##ooooo#",
        "#ooooo##oooooo##ooooo#",
        "#oooooooooooooooooooo#",
        "#ooooooooo##ooooooooo#",
        "#oooooooooooooooooooo#",
        ".#oooooooooooooooooo#.",
        "..#oooooooooooooooo#..",
        "...#oooooooooooooo#...",
        "....#oooooooooooo#....",
        "...#oooooooooooooo#...",
        "..#oooo#oooooo#oooo#..",
        "...####oooooooo####...",
        ".......########.......",
        ".....###......###.....",
        "....####......####....",
    },
    food = {
        "...##...",
        "..#oo#..",
        ".#oooo#.",
        ".#oooo#.",
        "########",
        "#oooooo#",
        ".#oooo#.",
        "..####..",
    },
    poop = {
        "...##...",
        "..#oo#..",
        "..#oo#..",
        ".#oooo#.",
        ".#o##o#.",
        "#oooooo#",
        "#oooooo#",
        ".######.",
    },
}

-- Zero-based sprite coordinates. Clear each eye with body fill, then draw
-- one horizontal ink row at eyey + 1 to show closed eyes.
local FACE = {
    baby = {leftx = 5, rightx = 9, eyey = 4, width = 2, height = 2},
    child = {leftx = 6, rightx = 12, eyey = 5, width = 2, height = 2},
    adult = {leftx = 6, rightx = 14, eyey = 7, width = 2, height = 2},
    adult_round = {leftx = 6, rightx = 14, eyey = 6, width = 2, height = 2},
}

local function fresh(generation)
    return {age=0, food=80, happy=80, clean=100, energy=90, health=100,
            poop=0, waste=0, ill=0, sick=false, sleeping=false, dead=false,
            form=0, care=0, meals=0, generation=generation or 1, sound=true}
end
local p = fresh()
local elapsed, frame_ms, second_ms, clock_ms, save_ms = 0, 0, 0, 1000, 10000
local selected, food_selected, clicks, click_ms = 1, 1, 0, 0
local page, mode, mode_until = "pet", "idle", 0
local message, message_until = "小生命，正在等你", 5000
local dirty, restore_pending = false, false
local time_text, game_score, jump_at, medicine_at = "--:--", 0, -1000, -30000

local function integer(value) return math.floor(value + 0.5) end
local function bounded(value, fallback, lo, hi)
    if type(value) ~= "number" or value ~= value or value == math.huge or value == -math.huge then
        return fallback
    end
    return math.max(lo, math.min(hi, value))
end
local function rect(x, y, w, h, color)
    screen.rect(integer(x), integer(y), w, h, color or C.ink)
end
local function text(value, x, y, width, align, color)
    screen.text(value, x, y, color or C.ink, {width=width, align=align or "left"})
end
local function say(value, ms)
    message, message_until = value, elapsed + (ms or 3500)
end
local function beep(hz)
    if p.sound and buzzer then buzzer.play(hz, 60) end
end
local function stage()
    if p.age < HATCH then return 0 end
    if p.age < CHILD then return 1 end
    if p.age < ADULT then return 2 end
    return 3
end
local function stage_name()
    if p.dead then return "纪念" end
    return ({"蛋", "宝宝", "儿童", "成年"})[stage() + 1]
end
local function sprite_name()
    local s = stage()
    if s == 0 then return "egg" end
    if s == 1 then return "baby" end
    if s == 2 then return "child" end
    return p.form == 2 and "adult_round" or "adult"
end
local function save()
    if not storage or restore_pending or not dirty or save_ms < 10000 then return end
    local data = {tama_v=1}
    for key, value in pairs(p) do
        data[key] = type(value) == "number" and math.floor(value * 100 + 0.5) / 100 or value
    end
    local ok = storage.save(data, 2592000)
    save_ms = 0
    if ok then dirty = false end
end
local function restore()
    if not storage then return end
    if clock and not clock.now() then restore_pending = true; return end
    restore_pending = false
    local data = storage.load()
    if type(data) ~= "table" or data.tama_v ~= 1 then return end
    for _, key in ipairs({"food", "happy", "clean", "energy", "health"}) do
        p[key] = bounded(data[key], p[key], 0, 100)
    end
    p.age = math.floor(bounded(data.age, 0, 0, 315360000))
    p.poop = math.floor(bounded(data.poop, 0, 0, 3))
    p.waste = bounded(data.waste, 0, 0, 479)
    p.ill = bounded(data.ill, 0, 0, 60)
    p.form = math.floor(bounded(data.form, 0, 0, 2))
    p.care = math.floor(bounded(data.care, 0, 0, 99999))
    p.meals = math.floor(bounded(data.meals, 0, 0, 9999))
    p.generation = math.floor(bounded(data.generation, 1, 1, 999))
    p.sick, p.sound = data.sick == true, data.sound ~= false
    p.dead = data.dead == true or p.health == 0
    p.sleeping = data.sleeping == true and stage() > 0 and not p.dead
    if stage() == 3 and p.form == 0 then p.form = 1 end
    if p.dead then p.health = 0 end
    say("生生的成长记录已回来", 5000)
end

local function status()
    if p.dead then return "已离开，可以领养新蛋" end
    if stage() == 0 then return "正在孵化" end
    if p.sleeping then return "睡眠中" end
    if p.sick then return "生病了，需要治疗" end
    if p.food < 25 then return "肚子饿了" end
    if p.poop > 0 or p.clean < 40 then return "需要打扫" end
    if p.energy < 20 then return "困了，想关灯" end
    if p.happy < 35 then return "想玩游戏" end
    return "状态很好"
end

local function grow_second()
    if p.dead then return end
    local before = stage()
    p.age = math.min(315360000, p.age + 1)
    local after = stage()
    if before ~= after then
        if after == 1 then say("破壳啦！你好，我叫生生", 6000)
        elseif after == 2 then say("生生长大啦，进入儿童期", 6000)
        else
            p.form = p.health >= 75 and p.happy >= 60 and p.clean >= 60 and 1 or 2
            say(p.form == 1 and "长成尖耳宝宝啦！" or "长成圆团宝宝啦！", 6000)
        end
        beep(1046)
    end
    dirty = true
    if after == 0 then return end
    p.food = math.max(0, p.food - 1 / (p.sleeping and 180 or 90))
    p.happy = math.max(0, p.happy - 1 / (p.sleeping and 600 or 120))
    p.clean = math.max(0, p.clean - 1 / 180)
    if p.sleeping then p.energy = math.min(100, p.energy + 1 / 5)
    else p.energy = math.max(0, p.energy - 1 / 120) end
    p.waste = p.waste + 1
    if p.waste >= 480 then
        p.waste = 0
        p.poop, p.clean = math.min(3, p.poop + 1), math.max(0, p.clean - 12)
        if not p.sleeping then say("地板脏啦，记得帮我打扫") end
    end
    local danger = p.food < 15 or p.clean < 25 or p.poop >= 3
    p.ill = danger and math.min(60, p.ill + 1) or math.max(0, p.ill - 1)
    if p.ill >= 60 and not p.sick then
        p.sick = true
        say("有点不舒服，请帮我看看", 5000)
    end
    if p.sick then p.health = math.max(0, p.health - 1 / 60)
    elseif p.food >= 25 and p.clean >= 40 then p.health = math.min(100, p.health + 1 / 180) end
    if p.health <= 0 then
        p.dead, p.sleeping, page = true, false, "pet"
        say("谢谢你陪我长大", 6000)
    end
end

local function star_x()
    return 120 + math.floor(math.sin(elapsed / 600) * 20) * 4
end
local function jump()
    if p.dead or p.sleeping or stage() == 0 or p.energy < 8 then
        return false, "现在不能玩，先吃饭或休息吧"
    end
    if elapsed - jump_at < 900 then return false, "等生生落地再跳吧" end
    jump_at, mode, mode_until = elapsed, "jump", elapsed + 800
    local caught = math.abs(star_x() - 120) <= 40
    p.energy, p.food = math.max(0, p.energy - 4), math.max(0, p.food - 2)
    p.happy = math.min(100, p.happy + (caught and 12 or 3))
    if caught then game_score = game_score + 1 end
    p.care, dirty = math.min(99999, p.care + 1), true
    local result = caught and "接到星星啦！" or "差一点，再试一次"
    say(result)
    beep(caught and 1175 or 660)
    save()
    return true, result
end

local function act(name, args)
    args = args or {}
    if name == "show_status" then page = page == "status" and "pet" or "status"; return true, status() end
    if name == "set_sound" then
        p.sound, dirty = args.enabled, true
        say(p.sound and "声音已打开" or "声音已关闭")
        save()
        return true, message
    end
    if restore_pending then return false, "等待设备校时，先保留原来的存档" end
    if name == "new_egg" then
        if args.confirm ~= true then return false, "重新领养需要明确确认" end
        p = fresh(math.min(999, p.generation + 1))
        page, selected, mode, dirty = "pet", 1, "idle", true
        game_score, medicine_at, jump_at = 0, -30000, -1000
        say("新的相遇，从一颗蛋开始", 5000)
        save()
        return true, "已领养新蛋"
    end
    if p.dead then return false, "生生已离开，请选择状态菜单领养新蛋" end
    if stage() == 0 then return false, string.format("蛋还需要%d秒孵化", math.max(0, HATCH - p.age)) end
    if name == "wake" then
        p.sleeping, page = false, "pet"
        say("灯亮啦，又是新的一天")
    elseif name == "sleep" then
        p.sleeping, page = true, "pet"
        say("关灯睡觉，精力慢慢恢复")
    elseif p.sleeping then return false, "生生在睡觉，请先打开灯叫醒它"
    elseif name == "feed" or name == "snack" then
        if p.food >= 98 then return false, "已经吃饱啦，不用继续喂" end
        p.food = math.min(100, p.food + (name == "feed" and 22 or 6))
        p.happy = math.min(100, p.happy + (name == "snack" and 12 or 3))
        if name == "snack" then p.health = math.max(1, p.health - 2) end
        p.meals = math.min(9999, p.meals + 1)
        mode, mode_until, page = "feed", elapsed + 2400, "pet"
        say(name == "feed" and "啊呜，米饭吃得饱饱的" or "点心真甜，别吃太多哦")
        beep(988)
    elseif name == "play" then
        if p.energy < 12 then return false, "精力不足，先关灯休息吧" end
        page, game_score = "game", 0
        say("星星到中间，双击跳起", 6000)
    elseif name == "jump" then return jump()
    elseif name == "clean" then
        p.poop, p.clean = 0, 100
        p.happy = math.min(100, p.happy + 3)
        mode, mode_until, page = "clean", elapsed + 2000, "pet"
        say("冲一冲，房间又干净啦")
        beep(784)
    elseif name == "medicine" then
        if not p.sick and p.health >= 95 then return false, "现在很健康，不需要吃药" end
        if elapsed - medicine_at < 30000 then return false, "药刚吃过，先休息一会儿" end
        medicine_at, p.sick, p.ill = elapsed, false, 0
        p.health, p.happy = math.min(100, p.health + 35), math.max(0, p.happy - 3)
        mode, mode_until, page = "medicine", elapsed + 2000, "pet"
        say("吃药好起来，也要保持清洁")
        beep(880)
    else return false, "不支持这个操作" end
    p.care, dirty = math.min(99999, p.care + 1), true
    save()
    return true, status()
end

-- Coalesce every horizontal run: sprites are vector-free pixel rectangles,
-- with an explicit per-frame budget enforced by the miniapp runtime.
local function sprite(name, x, y, scale, body)
    for row, data in ipairs(SPRITES[name]) do
        local column = 1
        while column <= #data do
            local value = data:sub(column, column)
            local finish = column + 1
            while finish <= #data and data:sub(finish, finish) == value do finish = finish + 1 end
            if value ~= "." then
                rect(x + (column - 1) * scale, y + (row - 1) * scale,
                     (finish - column) * scale, scale, value == "#" and C.ink or (body or C.lcd))
            end
            column = finish
        end
    end
end

local function icon(index, x, y, ink)
    local cut = ink == C.ink and C.lcd or C.ink
    if index == 1 then
        rect(x + 5, y + 1, 14, 3, ink); rect(x + 3, y + 6, 18, 3, ink)
        rect(x + 6, y + 10, 12, 4, ink)
    elseif index == 2 then
        rect(x + 1, y + 5, 22, 9, ink); rect(x + 4, y + 2, 16, 3, ink)
        rect(x + 5, y + 7, 6, 2, cut); rect(x + 7, y + 5, 2, 6, cut)
        rect(x + 17, y + 7, 2, 2, cut)
    elseif index == 3 then
        rect(x + 4, y + 7, 14, 7, ink); rect(x + 8, y + 2, 6, 5, ink)
        rect(x + 19, y + 1, 2, 5, ink); rect(x + 17, y + 3, 6, 2, ink)
    elseif index == 4 then
        rect(x + 8, y + 1, 6, 16, ink); rect(x + 3, y + 6, 16, 6, ink)
    elseif index == 5 then
        rect(x + 7, y + 1, 10, 8, ink); rect(x + 10, y + 9, 4, 5, ink)
        rect(x + 6, y + 15, 12, 2, ink)
    else
        for n = 0, 2 do rect(x + 3 + n * 7, y + 10 - n * 4, 4, 7 + n * 4, ink) end
    end
end

local function star(x, y)
    rect(x - 2, y - 8, 4, 16); rect(x - 8, y - 2, 16, 4)
    rect(x - 5, y - 5, 10, 10)
end
local function draw_pet()
    if p.dead then
        rect(104, 84, 32, 48); rect(88, 100, 64, 12)
        text("生生，谢谢你", 22, 139, 196, "center")
        return
    end
    local name, scale = sprite_name(), 4
    local image = SPRITES[name]
    local walk = (p.sleeping or stage() == 0 or page == "game") and 0
        or math.floor(math.sin(elapsed / 1400) * 4) * 4
    local x, y = 120 - #image[1] * scale // 2 + walk, 156 - #image * scale
    if stage() == 0 then
        if p.age >= 15 then x = x + (elapsed // 200 % 2 == 0 and -4 or 4) end
    elseif elapsed < mode_until and mode == "jump" then y = y - 4 end
    local body = p.sleeping and C.dim or C.lcd
    sprite(name, x, y, scale, body)
    local face = FACE[name]
    if face and (p.sleeping or elapsed % 4000 < 180) then
        for _, eye in ipairs({face.leftx, face.rightx}) do
            rect(x + eye * scale, y + face.eyey * scale, 8, 8, body)
            rect(x + eye * scale, y + (face.eyey + 1) * scale, 8, 4)
        end
    end
    for i = 1, math.min(2, p.poop) do sprite("poop", i == 1 and 22 or 202, 140, 2, body) end
    if p.sleeping then text("Z z", 180, 76, 40) end
    if p.sick then
        rect(191, 81, 6, 18); rect(185, 87, 18, 6)
        text("!", 206, 81, 12)
    end
    if stage() == 0 and p.age >= 15 then
        rect(x + 20, y + 32, 12, 4); rect(x + 28, y + 36, 12, 4)
        rect(x + 20, y + 40, 12, 4)
    elseif elapsed < mode_until and mode == "feed" then sprite("food", 180, 111, 3, body)
    elseif elapsed < mode_until and mode == "clean" then
        for n = 0, 2 do rect(28 + n * 66, 92 + (elapsed // 200 % 3) * 4, 12, 4) end
    elseif elapsed < mode_until and mode == "medicine" then
        rect(32, 94, 6, 18); rect(26, 100, 18, 6)
    end
    if page == "game" then
        star(star_x(), 91)
        text("得分 " .. game_score, 18, 69, 100)
    end
end

local function meter(x, label, value)
    text(label, x, 186, 18)
    local filled = math.max(0, math.min(4, math.ceil(value / 25)))
    for i = 0, 3 do rect(x + 22 + i * 11, 190, 9, 8, i < filled and C.ink or C.dim) end
end
local labels = {"喂食", "游戏", "清洁", "医药", "灯光", "状态"}
local function draw()
    screen.begin(C.case, {ordered=true})
    text("生生拓麻", 12, 7, 96)
    text(time_text, 169, 7, 59, "right")
    rect(6, 30, 228, 180)
    rect(8, 32, 224, 176, C.lcd)
    for i = 1, 6 do
        local x = 13 + (i - 1) * 36
        local chosen = i == selected
        rect(x, 35, 32, 25, chosen and C.ink or C.lcd)
        icon(i, x + 4, 39, chosen and C.lcd or C.ink)
    end
    rect(12, 64, 216, 2, C.dim)
    if p.sleeping and page == "pet" then rect(12, 68, 216, 92, C.dim) end
    if page == "status" then
        text("饱食 " .. integer(p.food), 20, 76, 100)
        text("快乐 " .. integer(p.happy), 130, 76, 94)
        text("清洁 " .. integer(p.clean), 20, 101, 100)
        text("健康 " .. integer(p.health), 130, 101, 94)
        text("精力 " .. integer(p.energy), 20, 126, 100)
        text("便便 " .. p.poop, 130, 126, 94)
    elseif page == "food" then
        text("今天吃什么？", 16, 76, 208, "center")
        for i, label in ipairs({"正餐：补充饱食", "点心：增加快乐"}) do
            local y = 106 + (i - 1) * 29
            if i == food_selected then rect(20, y - 2, 200, 23) end
            text(label, 22, y, 196, "center", i == food_selected and C.lcd or C.ink)
        end
    else draw_pet() end
    rect(12, 179, 216, 1, C.dim)
    local caption
    if restore_pending then caption = "等待校时，保留原存档"
    elseif elapsed < message_until then caption = message
    elseif p.dead then caption = "选状态，双击领养新蛋"
    elseif stage() == 0 then caption = string.format("孵化倒计时 %d 秒", HATCH - p.age)
    elseif page == "game" then caption = "星星到中间，双击跳起"
    elseif page == "food" then caption = "单击选食物，双击开饭"
    elseif page == "status" then
        caption = string.format("%s %dm G%d", stage_name(), p.age // 60, p.generation)
    else caption = labels[selected] .. " / " .. stage_name() .. " / " .. status() end
    -- All status strings fit one 16px line, including Chinese glyphs.
    if #caption > 39 then caption = status() end
    text(caption, 14, 161, 212, "center")
    meter(14, "饱", p.food); meter(88, "乐", p.happy); meter(162, "力", p.energy)
    text("单选 · 双确认 · 三返回", 8, 218, 224, "center")
    screen.present()
end

local function confirm()
    if page == "status" then page = "pet"; return end
    local ok, result
    if page == "food" then ok, result = act(food_selected == 1 and "feed" or "snack")
    elseif page == "game" then ok, result = jump()
    elseif selected == 1 then
        if p.dead or stage() == 0 or p.sleeping then ok, result = act("feed")
        else page, food_selected = "food", 1; say("单击选食物，双击开饭"); return end
    elseif selected == 2 then ok, result = act("play")
    elseif selected == 3 then ok, result = act("clean")
    elseif selected == 4 then ok, result = act("medicine")
    elseif selected == 5 then ok, result = act(p.sleeping and "wake" or "sleep")
    elseif p.dead then ok, result = act("new_egg", {confirm=true})
    else ok, result = act("show_status") end
    if ok == false then say(result) end
end

function on_start()
    restore()
    if clock then
        local t = clock.localtime()
        if t then time_text = string.format("%02d:%02d", t.hour, t.min) end
    end
    draw()
end
function on_tick(dt_ms)
    elapsed, frame_ms, second_ms = elapsed + dt_ms, frame_ms + dt_ms, second_ms + dt_ms
    clock_ms, save_ms = clock_ms + dt_ms, save_ms + dt_ms
    if clock_ms >= 1000 then
        clock_ms = 0
        if restore_pending then restore() end
        if clock then
            local t = clock.localtime()
            if t then time_text = string.format("%02d:%02d", t.hour, t.min) end
        end
    end
    local count = 0
    while second_ms >= 1000 and count < 60 do
        second_ms, count = second_ms - 1000, count + 1
        grow_second()
    end
    if clicks > 0 then
        click_ms = click_ms + dt_ms
        if click_ms >= 330 then
            local n = clicks
            clicks, click_ms = 0, 0
            if n >= 3 then page, selected = "pet", 1; say("回来看看生生")
            elseif n == 2 then confirm()
            elseif page == "food" then food_selected = 3 - food_selected
            elseif page == "game" then say("等星星到中间，双击跳起")
            else page, selected = "pet", selected % 6 + 1; say(labels[selected], 1000) end
        end
    end
    if save_ms >= 60000 then save() end
    if frame_ms >= 200 then frame_ms = 0; draw() end
end
function on_button_click(button_id)
    if button_id == "function" then clicks, click_ms = math.min(3, clicks + 1), 0 end
end
function get_state()
    return string.format("拓麻宠物生生，阶段%s，%s。年龄%d秒，第%d代；饱食%d，快乐%d，清洁%d，健康%d，精力%d，便便%d；界面%s，菜单%s。单击选择、双击确认、三击返回，长按三秒退出。",
        stage_name(), status(), p.age, p.generation, integer(p.food), integer(p.happy), integer(p.clean),
        integer(p.health), integer(p.energy), p.poop, page, labels[selected])
end
function get_actions()
    return {
        feed={description="给拓麻宠物生生吃正餐，增加饱食度", parameters={}},
        snack={description="给拓麻宠物吃点心，增加快乐，过量会影响健康", parameters={}},
        play={description="开始接星星小游戏，星星到中央时双击跳起", parameters={}},
        jump={description="在接星星小游戏中跳起一次", parameters={}},
        clean={description="清理拓麻宠物的便便，恢复清洁度", parameters={}},
        medicine={description="给生病的拓麻宠物吃药，恢复健康", parameters={}},
        sleep={description="关灯，让拓麻宠物睡觉恢复精力", parameters={}},
        wake={description="开灯，叫醒拓麻宠物", parameters={}},
        show_status={description="查看或关闭拓麻宠物的成长与健康面板", parameters={}},
        set_sound={description="开关拓麻宠物操作提示音", parameters={
            enabled={type="boolean", required=true, description="true打开，false静音"}}},
        new_egg={description="重新领养一颗蛋，重置当前宠物的养成进度，需要明确确认", parameters={
            confirm={type="boolean", required=true, enum={true}, description="明确确认丢弃本代进度并重新开始"}}},
    }
end
function on_action(name, args)
    local ok, result = act(name, args)
    if not ok then say(result) end
    draw()
    return {ok=ok, message=result}
end
function on_exit() save() end
