-- Tuantuan: a 240x240 companion for the ARCS-MINI Lua miniapp runtime (API 5).
-- ADB entry: /miniapp/desk_pet.lua. The stable path also identifies its save.
assert(app.api_version >= 5 and screen and screen.circle and screen.arc,
       "Tuantuan needs miniapp API 5 with extended graphics")
assert(app.width == 240 and app.height == 240, "Tuantuan needs a 240x240 screen")

local C = {
    bg=0xF8F4EC, ink=0x473B36, muted=0x938C7D, room=0xE9EFE3,
    floor=0xD4DFCB, cream=0xFFE0A6, light=0xFFF0D1, pink=0xF2A49B,
    green=0x739C79, orange=0xD99A59, blue=0x8EAFC0, white=0xFFFDF6,
    night=0xDCE3E9, nightfloor=0xC9D4DD,
}
local food, happy, energy, affection = 78, 86, 80, 0
local sleeping, sound = false, true
local elapsed, redraw_ms, care_ms, sleep_ms = 0, 0, 0, 0
local save_ms, clock_ms, click_ms, clicks = 10000, 1000, 0, 0
local mode, mode_until, bubble, bubble_until = "idle", 0, "你好呀，我是团团", 6000
local dirty, restore_pending = false, false
local clock_text = "--:--"

local function round(value) return math.floor(value + 0.5) end
local function clamp(value, fallback)
    if type(value) ~= "number" or value ~= value then return fallback end
    return math.max(0, math.min(100, value))
end
local function rect(x, y, w, h, color, radius)
    screen.rect(round(x), round(y), w, h, color, {radius=radius or 0})
end
local function circle(x, y, r, color)
    screen.circle(round(x), round(y), r, color)
end
local function line(x1, y1, x2, y2, color, width)
    screen.line(round(x1), round(y1), round(x2), round(y2), color,
                {width=width or 2, rounded=true})
end
local function text(value, x, y, color, width, align)
    screen.text(value, x, y, color or C.ink,
                {width=width or 216, align=align or "left"})
end
local function say(message, duration)
    bubble, bubble_until = message, elapsed + (duration or 3500)
end
local function chirp(hz)
    if sound and buzzer and not sleeping then buzzer.play(hz, 65) end
end

local function restore()
    if not storage then return end
    if clock and not clock.now() then restore_pending = true; return end
    restore_pending = false
    local saved = storage.load()
    if type(saved) ~= "table" or saved.pet_v ~= 1 then return end
    food = clamp(saved.food, food)
    happy = clamp(saved.happy, happy)
    energy = clamp(saved.energy, energy)
    if type(saved.affection) == "number" and saved.affection == saved.affection then
        affection = math.max(0, math.min(99999, math.floor(saved.affection)))
    end
    sleeping = saved.sleeping == true
    sound = saved.sound ~= false
    say(sleeping and "嘘，团团还在做梦" or "你回来啦，想你了", 5000)
end

local function save()
    if not storage or not dirty or save_ms < 10000 then return end
    -- Use the documented 30-day TTL; never write on every animation frame.
    local ok = storage.save({pet_v=1, food=round(food), happy=round(happy),
        energy=round(energy), affection=affection, sleeping=sleeping, sound=sound}, 2592000)
    save_ms = 0
    if ok then dirty = false end
end

local function state_name()
    if sleeping then return "正在睡觉" end
    if elapsed < mode_until then
        if mode == "feed" then return "正在吃小鱼" end
        if mode == "pet" then return "正在享受摸摸" end
        if mode == "play" then return "正在玩毛线球" end
    end
    if food < 30 then return "肚子有点饿" end
    if energy < 25 then return "有点困了" end
    if happy < 35 then return "想要陪伴" end
    return "开心地陪着你"
end

local function care(action)
    if action == "wake" then
        sleeping = false
        mode, mode_until = "pet", elapsed + 2000
        say("早安！又能一起玩啦")
    elseif action == "sleep" then
        sleeping = true
        mode, mode_until = "idle", 0
        say("晚安，梦里也有你", 5000)
    elseif sleeping then
        return false, "团团正在睡觉，可以先说“叫醒团团”"
    elseif action == "feed" then
        if food >= 98 then say("肚肚饱饱，先留着吧"); return false, "团团已经吃饱啦" end
        food, happy = math.min(100, food + 20), math.min(100, happy + 5)
        mode, mode_until = "feed", elapsed + 3000
        say("啊呜，小鱼真好吃！")
        chirp(1046)
    elseif action == "pet" then
        happy = math.min(100, happy + 10)
        mode, mode_until = "pet", elapsed + 2600
        say("呼噜呼噜，最喜欢你")
        chirp(880)
    elseif action == "play" then
        if energy < 15 then say("有点累，先睡一会儿"); return false, "精力不足，先让团团睡一会儿吧" end
        energy, food = math.max(0, energy - 8), math.max(0, food - 3)
        happy = math.min(100, happy + 15)
        mode, mode_until = "play", elapsed + 6000
        say("抓到啦！再来一次！", 6000)
        chirp(1175)
    else
        return false, "团团还没学会这个操作"
    end
    restore_pending = false
    affection = math.min(99999, affection + 1)
    dirty = true
    save()
    return true, state_name()
end

local function heart(x, y, size)
    circle(x - size // 2, y, size // 2 + 1, C.pink)
    circle(x + size // 2, y, size // 2 + 1, C.pink)
    line(x - size + 1, y + 1, x, y + size, C.pink, 3)
    line(x + size - 1, y + 1, x, y + size, C.pink, 3)
end

local function fish(x, y)
    rect(x, y, 24, 13, C.blue, 6)
    line(x + 24, y + 6, x + 31, y + 1, C.blue, 4)
    line(x + 24, y + 6, x + 31, y + 11, C.blue, 4)
    circle(x + 6, y + 4, 1, C.ink)
end

local function draw_pet()
    local active = elapsed < mode_until
    local bob = sleeping and round(math.sin(elapsed / 950)) or round(math.sin(elapsed / 650) * 2)
    local dx = active and mode == "play" and round(math.sin(elapsed / 250) * 10) or 0
    local dy = bob
    if active and mode == "play" then dy = dy - round(math.abs(math.sin(elapsed / 250)) * 7) end
    local x, y = 120 + dx, 113 + dy
    -- Tail, body and paws sit behind the head. Every frame stays below 100 shapes.
    screen.arc(x + 30, y + 27, 16, 280, 125, C.ink, {width=10, rounded=true})
    screen.arc(x + 30, y + 27, 16, 280, 125, C.cream, {width=6, rounded=true})
    screen.rect(x - 28, y + 6, 56, 40, C.cream,
                {radius=20, border_width=3, border_color=C.ink})
    rect(x - 15, y + 18, 30, 22, C.light, 12)
    screen.rect(x - 32, y + 34, 27, 14, C.cream,
                {radius=7, border_width=2, border_color=C.ink})
    screen.rect(x + 5, y + 34, 27, 14, C.cream,
                {radius=7, border_width=2, border_color=C.ink})
    -- Rounded ears are easier to read than fine detail on a 240px display.
    for _, ear in ipairs({-28, 28}) do
        screen.rect(x + ear - 12, y - 53, 24, 38, C.cream,
                    {radius=9, border_width=3, border_color=C.ink})
        rect(x + ear - 5, y - 45, 10, 19, C.pink, 4)
    end
    screen.rect(x - 44, y - 36, 88, 67, C.cream,
                {radius=27, border_width=3, border_color=C.ink})
    rect(x - 7, y - 33, 14, 12, C.orange, 5)
    rect(x - 21, y - 30, 8, 9, C.orange, 4)
    rect(x + 13, y - 30, 8, 9, C.orange, 4)
    circle(x - 28, y + 7, 7, C.pink)
    circle(x + 28, y + 7, 7, C.pink)
    local closed = sleeping or (active and mode == "pet") or elapsed % 4300 < 180
    local look = round(math.sin(elapsed / 1700) * 2)
    for _, eye in ipairs({-18, 18}) do
        if closed then
            screen.arc(x + eye, y - 4, 7, sleeping and 20 or 200,
                       sleeping and 160 or 340, C.ink, {width=3, rounded=true})
        else
            rect(x + eye - 4 + look, y - 13, 8, 15, C.ink, 4)
            circle(x + eye - 1 + look, y - 10, 2, C.white)
        end
    end
    circle(x, y + 3, 3, C.ink)
    screen.arc(x - 4, y + 7, 4, 0, 160, C.ink, {width=2})
    screen.arc(x + 4, y + 7, 4, 20, 180, C.ink, {width=2})
    if sleeping then
        text("z", 166, 92 - round((elapsed % 2500) / 350), C.muted, 16)
        text("Z", 179, 72 - round((elapsed % 2500) / 450), C.muted, 16)
    elseif active and mode == "pet" then
        heart(65, 84 - round((elapsed % 1200) / 100), 6)
        heart(176, 102 - round((elapsed % 1500) / 100), 5)
    elseif active and mode == "feed" then
        fish(x - 13, y + 18 + round(math.sin(elapsed / 140) * 2))
    elseif active and mode == "play" then
        local bx = 174 + round(math.sin(elapsed / 280) * 17)
        circle(bx, 149, 12, C.pink)
        screen.arc(bx, 149, 8, 40, 290, C.white, {width=2})
        line(bx - 9, 146, bx + 9, 152, C.white, 2)
        line(bx + 9, 156, 213, 160, C.pink, 2)
    end
end

local function meter(x, label, value, color)
    rect(x, 176, 68, 22, C.white, 8)
    text(label, x + 5, 179, C.muted, 18)
    rect(x + 25, 184, 36, 6, C.room, 3)
    local fill = round(36 * value / 100)
    if fill > 0 then rect(x + 25, 184, fill, 6, color, 3) end
end

local function draw()
    screen.begin(C.bg, {ordered=true})
    circle(16, 19, 3, sleeping and C.blue or C.green)
    text("团团", 25, 10, C.ink, 40)
    text("Lv." .. (1 + affection // 10), 79, 12, C.muted, 55)
    text(clock_text, 167, 12, C.muted, 59, "right")
    rect(12, 38, 216, 132, sleeping and C.night or C.room, 18)
    -- A small window and warm floor make the pet feel at home.
    rect(179, 48, 34, 34, C.white, 8)
    if sleeping then
        circle(195, 64, 9, C.blue)
        circle(199, 60, 8, C.white)
    else
        circle(196, 63, 8, C.orange)
        line(181, 73, 210, 73, C.room, 2)
        line(195, 50, 195, 79, C.room, 2)
    end
    rect(20, 144, 200, 19, sleeping and C.nightfloor or C.floor, 8)
    rect(77, 151, 86, 10, sleeping and C.nightfloor or 0xBDCCB5, 5)
    -- A sprout on the left keeps the scene balanced without large assets.
    rect(30, 137, 18, 18, C.orange, 5)
    line(39, 139, 39, 118, C.green, 3)
    circle(34, 125, 5, C.green)
    circle(44, 120, 5, C.green)
    draw_pet()
    meter(12, "饱", food, C.orange)
    meter(86, "乐", happy, C.pink)
    meter(160, "力", energy, C.green)
    local message = bubble
    if elapsed >= bubble_until then
        if sleeping then message = "呼噜……醒来再陪你"
        elseif food < 30 then message = "肚子咕咕叫，想吃小鱼"
        elseif energy < 25 then message = "眼皮打架啦，想睡一觉"
        elseif happy < 35 then message = "能不能陪我玩一会儿"
        else
            local phrases = {"你忙你的，我在这里", "今天也要好好生活呀", "晒晒太阳，发发呆", "有你在，普通一天也好"}
            message = phrases[1 + (elapsed // 9000) % #phrases]
        end
    end
    text(message, 12, 203, C.ink, 216, "center")
    text("单摸 · 双喂 · 三睡", 12, 224, C.muted, 216, "center")
    screen.present()
end

function on_start()
    restore()
    if clock then
        local t = clock.localtime()
        if t then clock_text = string.format("%02d:%02d", t.hour, t.min) end
    end
    draw()
end

function on_tick(dt_ms)
    elapsed, redraw_ms = elapsed + dt_ms, redraw_ms + dt_ms
    care_ms, save_ms, clock_ms = care_ms + dt_ms, save_ms + dt_ms, clock_ms + dt_ms
    if clicks > 0 then
        click_ms = click_ms + dt_ms
        if click_ms >= 330 then
            local count = clicks
            clicks, click_ms = 0, 0
            if count >= 3 then care(sleeping and "wake" or "sleep")
            elseif sleeping then care("wake")
            elseif count == 2 then care("feed")
            else care("pet") end
        end
    end
    if care_ms >= 1000 then
        local seconds = math.floor(care_ms / 1000)
        care_ms = care_ms % 1000
        if sleeping then
            energy = math.min(100, energy + seconds / 2)
            food = math.max(0, food - seconds / 120)
            sleep_ms = sleep_ms + seconds * 1000
            if sleep_ms >= 60000 then happy = math.min(100, happy + 1); sleep_ms = 0 end
        else
            energy = math.max(0, energy - seconds / 90)
            food = math.max(0, food - seconds / 60)
            happy = math.max(0, happy - seconds / 120)
            sleep_ms = 0
        end
        dirty = true
    end
    if clock_ms >= 1000 then
        clock_ms = 0
        if restore_pending then restore() end
        if clock then
            local t = clock.localtime()
            if t then clock_text = string.format("%02d:%02d", t.hour, t.min) end
        end
    end
    if save_ms >= 30000 then save() end
    if redraw_ms >= 100 then redraw_ms = 0; draw() end
end

function on_button_click(button_id)
    if button_id ~= "function" then return end
    clicks, click_ms = math.min(3, clicks + 1), 0
end

function get_state()
    return string.format("桌宠团团，%s。饱食度%d，心情%d，精力%d，亲密等级%d。单击摸摸、双击喂食、三击睡觉或叫醒，长按三秒退出。",
        state_name(), round(food), round(happy), round(energy), 1 + affection // 10)
end

function get_actions()
    return {
        pet={description="摸摸桌宠团团，增加心情和亲密度", parameters={}},
        feed={description="给桌宠团团喂小鱼，增加饱食度", parameters={}},
        play={description="陪桌宠团团玩毛线球，增加心情并消耗精力", parameters={}},
        sleep={description="让桌宠团团睡觉，持续恢复精力", parameters={}},
        wake={description="叫醒桌宠团团", parameters={}},
        set_sound={description="打开或关闭桌宠按键提示音", parameters={
            enabled={type="boolean", description="true打开提示音，false静音", required=true}}},
    }
end

function on_action(name, args)
    local ok, message
    if name == "set_sound" then
        sound, dirty, restore_pending = args.enabled, true, false
        say(sound and "听，团团的小声音" or "好，我会安安静静的")
        save()
        ok, message = true, sound and "桌宠提示音已打开" or "桌宠提示音已关闭"
    else
        ok, message = care(name)
    end
    draw()
    return {ok=ok, message=message}
end

function on_exit() save() end
