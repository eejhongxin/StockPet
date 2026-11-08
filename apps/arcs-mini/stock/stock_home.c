#include "stock_home.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "home_presenter.h"
#include "lisa_ui_fonts.h"
#include "lisa_ui_llm_primary.h"
#include "model_voice.h"
#include "service_stock.h"
#include "service_stock_chart.h"
#include "voice_msg.h"

#define STOCK_AUTO_MS 8000u
#define GRAPH_LEFT 3
#define GRAPH_RIGHT 212
#define GRAPH_TOP 3
#define GRAPH_BOTTOM 68

/* The button callback never touches LVGL. UI state is published by the timer. */
static struct {
    bool visible;
    bool automatic;
    unsigned count;
    unsigned next;
    unsigned toggle;
    char symbol[STOCK_SYMBOL_SIZE];
} s_controls;

typedef struct {
    lv_obj_t *panel, *heading, *name, *code, *price, *change, *stamp, *source;
    lv_obj_t *pet, *graph, *axis_start, *axis_end, *quote_stamp, *chart_note;
    struct home_nav_scr_data *home;
    stock_chart_snapshot_t series;
    char selected[STOCK_SYMBOL_SIZE];
    char name_cache[112];
    uint32_t blink, revision, page_started, chart_sequence;
    unsigned page;
    bool automatic;
    bool graph_initialized;
} stock_home_t;

/* Same 生生 sprite as the native voice desktop. */
static const char *const pet_sprite[] = {
    ".....#..........#.....", "....###........###....",
    "...#####......#####...", "...#####oooooo#####...",
    "..#oooooooooooooooo#..", ".#oooooooooooooooooo#.",
    ".#oooooooooooooooooo#.", "#ooooo##oooooo##ooooo#",
    "#ooooo##oooooo##ooooo#", "#oooooooooooooooooooo#",
    "#ooooooooo##ooooooooo#", "#oooooooooooooooooooo#",
    ".#oooooooooooooooooo#.", ".#oooooooooooooooooo#.",
    "..#oooooooooooooooo#..", "..#oooooooooooooooo#..",
    "...##oooooooooooo##...", ".....############.....",
    "....###........###....", "...####........####...",
};

static bool voice_busy(void)
{
    return model_voice_cloud_is_running() || model_voice_tts_is_pending() ||
           model_voice_tts_is_playing() || model_voice_music_is_playing();
}

bool stock_home_handle_button(uint8_t event)
{
    if ((event != VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK &&
         event != VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK) || voice_busy()) return false;
    taskENTER_CRITICAL();
    bool consume = s_controls.visible;
    if (consume) {
        if (event == VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK) ++s_controls.next;
        else s_controls.toggle ^= 1u;
    }
    taskEXIT_CRITICAL();
    return consume;
}

void stock_home_status(char *text, size_t size)
{
    if (!text || !size) return;
    char symbol[STOCK_SYMBOL_SIZE];
    bool visible, automatic;
    unsigned count;
    taskENTER_CRITICAL();
    memcpy(symbol, s_controls.symbol, sizeof(symbol));
    visible = s_controls.visible;
    automatic = s_controls.automatic;
    count = s_controls.count;
    taskEXIT_CRITICAL();
    snprintf(text, size, "selected=%s mode=%s visible=%u count=%u", symbol[0] ? symbol : "none",
             automatic ? "auto" : "manual", visible, count);
}

static void pet_draw(lv_event_t *event)
{
    stock_home_t *ui = lv_event_get_user_data(event);
    lv_area_t bounds;
    lv_obj_get_coords(ui->pet, &bounds);
    lv_draw_rect_dsc_t draw;
    lv_draw_rect_dsc_init(&draw);
    draw.bg_opa = LV_OPA_COVER;
    bool blink = (lv_tick_get() % 5000) < 160;
    for (unsigned y = 0; y < 20; ++y) {
        for (unsigned x = 0; x < 22; ++x) {
            char pixel = pet_sprite[y][x];
            if (pixel == '.') continue;
            if (blink && y == 7 && (x == 6 || x == 7 || x == 14 || x == 15)) pixel = 'o';
            draw.bg_color = lv_color_hex(pixel == '#' ? 0xB8C19B : 0xD0D7B2);
            lv_area_t box = {bounds.x1 + x, bounds.y1 + y,
                             bounds.x1 + x, bounds.y1 + y};
            lv_draw_rect(lv_event_get_draw_ctx(event), &draw, &box);
        }
    }
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, "");
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(obj, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

static void set_text(lv_obj_t *obj, const char *text)
{
    if (strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
}

static void set_color(lv_obj_t *obj, uint32_t color)
{
    if (lv_obj_get_style_text_color(obj, 0).full != lv_color_hex(color).full)
        lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

/* Keep everyday prices large; unusually long values still retain their digits. */
static void fit_number(lv_obj_t *obj, const char *text, bool price)
{
    const lv_font_t *font = price ? &lv_font_montserrat_32 : &lv_font_montserrat_14;
    int width = lv_obj_get_width(obj);
    if (price && lv_txt_get_width(text, strlen(text), font, 0, LV_TEXT_FLAG_NONE) > width)
        font = &lv_font_montserrat_24;
    if (lv_txt_get_width(text, strlen(text), font, 0, LV_TEXT_FLAG_NONE) > width)
        font = price ? &lv_font_montserrat_14 : &lv_font_montserrat_12;
    if (lv_obj_get_style_text_font(obj, 0) != font) lv_obj_set_style_text_font(obj, font, 0);
    if (price) lv_obj_set_y(obj, 67 - (font->line_height - font->base_line));
    set_text(obj, text);
}

static int point_x(uint16_t minute, stock_market_t market)
{
    unsigned end = market == STOCK_MARKET_CN ? 900 : 960;
    return GRAPH_LEFT + (minute - 570u) * (GRAPH_RIGHT - GRAPH_LEFT) / (end - 570u);
}

static int point_y(float price, float minimum, float maximum)
{
    float value = GRAPH_BOTTOM - (price - minimum) * (GRAPH_BOTTOM - GRAPH_TOP) / (maximum - minimum);
    if (value < GRAPH_TOP) value = GRAPH_TOP;
    if (value > GRAPH_BOTTOM) value = GRAPH_BOTTOM;
    return (int)(value + 0.5f);
}

/* Real elapsed exchange-local minutes, leaving lunch and missing intervals blank. */
static bool points_join(const stock_chart_point_t *a, const stock_chart_point_t *b)
{
    return b->minute > a->minute && b->minute - a->minute == 1;
}

static void draw_text(lv_draw_ctx_t *ctx, const lv_area_t *bounds, int x, int y,
                      int width, const char *text, bool small, uint32_t color)
{
    lv_draw_label_dsc_t draw;
    lv_draw_label_dsc_init(&draw);
    draw.font = small ? &lv_font_montserrat_12 : &lv_font_chinese_16;
    draw.color = lv_color_hex(color);
    lv_area_t area = {bounds->x1 + x, bounds->y1 + y,
                      bounds->x1 + x + width - 1, bounds->y1 + y + 18};
    lv_draw_label(ctx, &draw, &area, text, NULL);
}

static void graph_draw(lv_event_t *event)
{
    stock_home_t *ui = lv_event_get_user_data(event);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    lv_area_t bounds;
    lv_obj_get_coords(ui->graph, &bounds);
    stock_chart_t *chart = &ui->series.chart;
    bool valid = chart->valid && chart->count && strcmp(chart->symbol, ui->selected) == 0;
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(0x21303A);
    for (unsigned i = 0; i < 3; ++i) {
        int y = GRAPH_TOP + i * (GRAPH_BOTTOM - GRAPH_TOP) / 2;
        lv_point_t a = {bounds.x1 + GRAPH_LEFT, bounds.y1 + y};
        lv_point_t b = {bounds.x1 + GRAPH_RIGHT, bounds.y1 + y};
        lv_draw_line(ctx, &line, &a, &b);
    }
    if (!valid) {
        const char *text = !ui->selected[0] ? "按键说添加股票" :
                           ui->series.failed ? "分时暂不可用" : "分时载入中";
        draw_text(ctx, &bounds, 35, 27, 178, text, false, 0xA7B5C2);
        return;
    }
    float minimum = chart->previous_close, maximum = chart->previous_close;
    for (unsigned i = 0; i < chart->count; ++i) {
        if (chart->points[i].price < minimum) minimum = chart->points[i].price;
        if (chart->points[i].price > maximum) maximum = chart->points[i].price;
    }
    float padding = (maximum - minimum) * 0.08f;
    if (padding < maximum * 0.0005f) padding = maximum * 0.0005f;
    if (padding < 0.005f) padding = 0.005f;
    minimum -= padding;
    maximum += padding;
    if (chart->market == STOCK_MARKET_CN) {
        lv_draw_rect_dsc_t shade;
        lv_draw_rect_dsc_init(&shade);
        shade.bg_opa = LV_OPA_COVER;
        shade.bg_color = lv_color_hex(0x17212A);
        lv_area_t area = {bounds.x1 + point_x(690, chart->market) + 1, bounds.y1 + GRAPH_TOP,
                          bounds.x1 + point_x(780, chart->market) - 1, bounds.y1 + GRAPH_BOTTOM};
        lv_draw_rect(ctx, &shade, &area);
        draw_text(ctx, &bounds, point_x(690, chart->market) + 10, 27, 40, "午休", false, 0x566775);
    }
    int close_y = point_y(chart->previous_close, minimum, maximum);
    line.color = lv_color_hex(0x52616C);
    line.dash_width = 3;
    line.dash_gap = 3;
    lv_point_t a = {bounds.x1 + GRAPH_LEFT, bounds.y1 + close_y};
    lv_point_t b = {bounds.x1 + GRAPH_RIGHT, bounds.y1 + close_y};
    lv_draw_line(ctx, &line, &a, &b);
    line.dash_width = line.dash_gap = 0;
    line.width = 2;
    line.color = lv_color_hex(chart->points[chart->count - 1].price >= chart->previous_close ? 0xF28B82 : 0x81C995);
    for (unsigned i = 0; i < chart->count; ++i) {
        b.x = bounds.x1 + point_x(chart->points[i].minute, chart->market);
        b.y = bounds.y1 + point_y(chart->points[i].price, minimum, maximum);
        if (i && points_join(&chart->points[i - 1], &chart->points[i])) lv_draw_line(ctx, &line, &a, &b);
        /* Every isolated point is represented, including a one-point opening. */
        if ((!i || !points_join(&chart->points[i - 1], &chart->points[i])) &&
            (i + 1 == chart->count || !points_join(&chart->points[i], &chart->points[i + 1]))) {
            lv_draw_rect_dsc_t dot;
            lv_draw_rect_dsc_init(&dot);
            dot.bg_color = line.color;
            dot.bg_opa = LV_OPA_COVER;
            lv_area_t area = {b.x - 1, b.y - 1, b.x + 1, b.y + 1};
            lv_draw_rect(ctx, &dot, &area);
        }
        a = b;
    }
    char value[24];
    snprintf(value, sizeof(value), "%.2f", (double)(maximum - padding));
    draw_text(ctx, &bounds, 4, 1, 100, value, true, 0x82929F);
    snprintf(value, sizeof(value), "%.2f", (double)(minimum + padding));
    draw_text(ctx, &bounds, 4, GRAPH_BOTTOM - 14, 100, value, true, 0x82929F);
}

static bool home_visible(const stock_home_t *ui)
{
    const struct home_nav_scr_data *home = ui->home;
    return !lv_obj_has_flag(home->view, LV_OBJ_FLAG_HIDDEN) && home->finished &&
           !home->standby_sleep_active && home->work_type == WORK_TYPE_VOICE &&
           !home->img_rec_running && !home->img_rec_in_progress &&
           !home->camera_preview_result_pending && !home->music_text_active &&
           !home->mcp_loading && !home->mcp_emoji_running && !home->oneshot_emoji_running &&
           !voice_busy() && !lisa_ui_llm_primary_img_is_visible(home->view);
}

static void stock_home_update(lv_timer_t *timer)
{
    stock_home_t *ui = timer->user_data;
    bool visible = home_visible(ui);
    unsigned next = 0, toggle = 0;
    taskENTER_CRITICAL();
    s_controls.visible = visible;
    if (visible) {
        next = s_controls.next;
        toggle = s_controls.toggle;
    }
    s_controls.next = s_controls.toggle = 0;
    taskEXIT_CRITICAL();
    if (!visible) {
        lv_obj_add_flag(ui->panel, LV_OBJ_FLAG_HIDDEN);
        ui->page_started = lv_tick_get();
        return;
    }
    lv_obj_clear_flag(ui->panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->panel);
    stock_snapshot_t snapshot;
    service_stock_snapshot(&snapshot);
    uint32_t ticks = lv_tick_get();
    if (snapshot.revision != ui->revision) {
        ui->revision = snapshot.revision;
        unsigned found = snapshot.count;
        for (unsigned i = 0; i < snapshot.count; ++i) {
            if (strcmp(snapshot.instruments[i].symbol, ui->selected) == 0) { found = i; break; }
        }
        ui->page = found < snapshot.count ? found : 0;
        ui->page_started = ticks;
    }
    if (toggle) { ui->automatic = !ui->automatic; ui->page_started = ticks; }
    if (next) {
        ui->automatic = false;
        if (snapshot.count) ui->page = (ui->page + next % snapshot.count) % snapshot.count;
        ui->page_started = ticks;
    } else if (ui->automatic && snapshot.count > 1 && ticks - ui->page_started >= STOCK_AUTO_MS) {
        ui->page = (ui->page + 1) % snapshot.count;
        ui->page_started = ticks;
    }
    if (ui->page >= snapshot.count) ui->page = 0;
    const stock_instrument_t *instrument = snapshot.count ? &snapshot.instruments[ui->page] : NULL;
    const stock_quote_t *quote = snapshot.count ? &snapshot.quotes[ui->page] : NULL;
    const char *symbol = instrument ? instrument->symbol : "";
    bool changed = strcmp(ui->selected, symbol) != 0;
    if (changed) {
        snprintf(ui->selected, sizeof(ui->selected), "%s", symbol);
        service_stock_chart_select(symbol);
    }
    taskENTER_CRITICAL();
    s_controls.automatic = ui->automatic;
    s_controls.count = snapshot.count;
    memcpy(s_controls.symbol, ui->selected, sizeof(s_controls.symbol));
    taskEXIT_CRITICAL();
    char text[112];
    snprintf(text, sizeof(text), "%u/%lu %s", snapshot.count ? ui->page + 1 : 0,
             (unsigned long)snapshot.count, ui->automatic ? "自动" : "手动");
    set_text(ui->heading, text);
    snprintf(text, sizeof(text), "%s", instrument ? instrument->name : "还没有自选股");
    if (strcmp(ui->name_cache, text) != 0) {
        snprintf(ui->name_cache, sizeof(ui->name_cache), "%s", text);
        lv_label_set_text(ui->name, text);
    }
    snprintf(text, sizeof(text), "%s%s%s", instrument ? instrument->symbol + 2 : "",
             instrument ? "  " : "", !instrument ? "" :
             instrument->market == STOCK_MARKET_CN ? "CNY" : "USD");
    set_text(ui->code, text);
    if (quote && quote->valid) {
        snprintf(text, sizeof(text), instrument->market == STOCK_MARKET_CN ? "%.2f" : "$%.2f", quote->price);
        if (lv_txt_get_width(text, strlen(text), &lv_font_montserrat_14, 0, LV_TEXT_FLAG_NONE) > 144)
            snprintf(text, sizeof(text), instrument->market == STOCK_MARKET_CN ? "%.4g" : "$%.4g", quote->price);
        fit_number(ui->price, text, true);
        snprintf(text, sizeof(text), "%+.2f%%", quote->change_pct);
        if (lv_txt_get_width(text, strlen(text), &lv_font_montserrat_12, 0, LV_TEXT_FLAG_NONE) > 68)
            snprintf(text, sizeof(text), "%+.3g%%", quote->change_pct);
        fit_number(ui->change, text, false);
        set_color(ui->change, quote->change > 0 ? 0xF28B82 : quote->change < 0 ? 0x81C995 : 0xCDD6DF);
    } else {
        fit_number(ui->price, "--", true);
        if (lv_obj_get_style_text_font(ui->change, 0) != &lv_font_chinese_16)
            lv_obj_set_style_text_font(ui->change, &lv_font_chinese_16, 0);
        set_text(ui->change, instrument ? "等待报价" : "");
        set_color(ui->change, 0xCDD6DF);
    }
    service_stock_chart_snapshot(&ui->series);
    if (!ui->graph_initialized || changed || ui->chart_sequence != ui->series.sequence) {
        ui->graph_initialized = true;
        ui->chart_sequence = ui->series.sequence;
        lv_obj_invalidate(ui->graph);
    }
    bool chart_valid = ui->series.chart.valid && strcmp(ui->series.chart.symbol, symbol) == 0;
    if (chart_valid) {
        snprintf(text, sizeof(text), "%s %s", ui->series.chart.date,
                 instrument && instrument->market == STOCK_MARKET_CN ? "CST" : "ET");
    } else snprintf(text, sizeof(text), "--");
    set_text(ui->stamp, text);
    set_text(ui->chart_note, chart_valid && ui->series.failed ? "旧分时" : "");
    if (chart_valid && ui->series.failed) lv_obj_clear_flag(ui->chart_note, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui->chart_note, LV_OBJ_FLAG_HIDDEN);
    if (quote && quote->valid) snprintf(text, sizeof(text), "%.16s %s", quote->time_text,
                                       instrument->market == STOCK_MARKET_CN ? "CST" : "ET");
    else snprintf(text, sizeof(text), "--");
    set_text(ui->quote_stamp, text);
    set_text(ui->axis_start, instrument ? "09:30" : "");
    set_text(ui->axis_end, instrument ? (instrument->market == STOCK_MARKET_CN ? "15:00" : "16:00") : "");
    const char *state = !snapshot.network_connected ? "离线" :
                        snapshot.state == STOCK_STATE_ERROR ? "更新失败" :
                        !snapshot.clock_available ? "未校时" :
                        instrument && !snapshot.scheduled[ui->page] ? "休市段" :
                        instrument && snapshot.stale[ui->page] ? "旧报价" : "";
    bool delayed = instrument && instrument->market == STOCK_MARKET_US;
    snprintf(text, sizeof(text), "%s%s%s", delayed ? "可能延时" : "",
             delayed && state[0] ? "·" : "", state);
    set_text(ui->source, text);
    uint32_t blink = (ticks % 5000) < 160;
    if (blink != ui->blink) { ui->blink = blink; lv_obj_invalidate(ui->pet); }
}

void stock_home_init(struct home_nav_scr_data *home)
{
    stock_home_t *ui = lv_mem_alloc(sizeof(*ui));
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
    ui->home = home;
    ui->panel = lv_obj_create(home->view);
    lv_obj_set_size(ui->panel, 240, 204);
    lv_obj_add_flag(ui->panel, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(ui->panel, 0, 0);
    lv_obj_set_style_border_width(ui->panel, 0, 0);
    lv_obj_set_style_radius(ui->panel, 0, 0);
    lv_obj_set_style_bg_color(ui->panel, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(ui->panel, LV_OPA_COVER, 0);
    lv_obj_align(ui->panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui->pet = lv_obj_create(ui->panel);
    lv_obj_remove_style_all(ui->pet);
    lv_obj_set_pos(ui->pet, 206, 0);
    lv_obj_set_size(ui->pet, 22, 20);
    lv_obj_clear_flag(ui->pet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui->pet, pet_draw, LV_EVENT_DRAW_MAIN, ui);
    ui->heading = label(ui->panel, 148, 21, 80, 0x82929F);
    lv_obj_set_style_text_align(ui->heading, LV_TEXT_ALIGN_RIGHT, 0);
    ui->name = label(ui->panel, 12, 0, 184, 0xFFFFFF);
    lv_label_set_long_mode(ui->name, LV_LABEL_LONG_DOT);
    ui->code = label(ui->panel, 12, 22, 130, 0x82929F);
    lv_obj_set_style_text_font(ui->code, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(ui->code, LV_LABEL_LONG_DOT);
    ui->price = label(ui->panel, 12, 38, 144, 0xFFFFFF);
    lv_obj_set_style_text_font(ui->price, &lv_font_montserrat_32, 0);
    ui->change = label(ui->panel, 160, 50, 68, 0xCDD6DF);
    lv_obj_set_style_text_font(ui->change, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(ui->change, LV_TEXT_ALIGN_RIGHT, 0);
    ui->graph = lv_obj_create(ui->panel);
    lv_obj_remove_style_all(ui->graph);
    lv_obj_set_pos(ui->graph, 12, 77);
    lv_obj_set_size(ui->graph, 216, 72);
    lv_obj_clear_flag(ui->graph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui->graph, graph_draw, LV_EVENT_DRAW_MAIN, ui);
    ui->axis_start = label(ui->panel, 12, 151, 40, 0x82929F);
    ui->axis_end = label(ui->panel, 188, 151, 40, 0x82929F);
    lv_obj_set_style_text_font(ui->axis_start, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_font(ui->axis_end, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(ui->axis_end, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *quote_title = label(ui->panel, 12, 168, 36, 0x82929F);
    lv_label_set_text(quote_title, "报价");
    ui->quote_stamp = label(ui->panel, 48, 169, 180, 0x82929F);
    lv_obj_set_style_text_font(ui->quote_stamp, &lv_font_montserrat_14, 0);
    ui->stamp = label(ui->panel, 55, 152, 130, 0x82929F);
    lv_obj_set_style_text_font(ui->stamp, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(ui->stamp, LV_TEXT_ALIGN_CENTER, 0);
    ui->chart_note = label(ui->panel, 164, 77, 64, 0xD0D7B2);
    lv_obj_set_style_text_align(ui->chart_note, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_bg_color(ui->chart_note, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(ui->chart_note, LV_OPA_COVER, 0);
    ui->source = label(ui->panel, 12, 186, 216, 0x82929F);
    home->stock_timer = lv_timer_create(stock_home_update, 100, ui);
    if (!home->stock_timer) { lv_obj_del(ui->panel); lv_mem_free(ui); return; }
    stock_home_update(home->stock_timer);
}

void stock_home_deinit(struct home_nav_scr_data *home)
{
    if (!home->stock_timer) return;
    taskENTER_CRITICAL();
    memset(&s_controls, 0, sizeof(s_controls));
    taskEXIT_CRITICAL();
    stock_home_t *ui = home->stock_timer->user_data;
    lv_timer_del(home->stock_timer);
    home->stock_timer = NULL;
    lv_obj_del(ui->panel);
    lv_mem_free(ui);
}
