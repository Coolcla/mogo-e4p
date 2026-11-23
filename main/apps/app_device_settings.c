/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 用户 2026-11-11 修订：锁屏样式改为六类缩略图及同页选择，图片加载独立于绘图和输入。
 * User revision: lock styles use six thumbnails and inline choices, with image I/O outside drawing/input.
 * 用户实屏反馈：锁屏样式保留小图，加载完成只整屏定稿一次，避免逐张局刷导致文字变灰。
 * Device feedback: retain thumbnails and settle completed batches once to avoid faded text from repeated partial updates.
 * 用户新增：阅读票根提供透明或封面背景，独立记忆选择并在保存失败时保留原值。
 * User addition: tickets offer transparent/cover backgrounds with independent persistence and failed-save retention.
 * 用户 2026-11-11 确认：蓝牙设置采用 WiFi 同款状态卡和分页列表，设备详情管理配对，按键学习留在动作行内。
 * Approved revision: Bluetooth uses WiFi-style cards/paging, peer details and inline learning.
 * 中文：分组设置页；网络短时对时后 PMU 持续走时，开机恢复系统时钟。
 * English: Grouped settings; temporary WiFi sync seeds the PMU, whose RTC restores time at boot.
 * 用户修订：设置列表滑动仅差分刷新内容，滑动过程中不周期插入黑白清屏。
 * User revision: settings lists scroll with content-only differentials and no periodic black/white wipe during swipes.
 * 用户授权新输入法：资料卡和签名共用九宫格/全键盘及离线词语候选，保存仍由设置页负责。
 * Profile and signature share Zhuyin/QWERTY input and offline phrase candidates; this page still owns saving.
 * 用户修订：资料卡与签名输入只画变化区域，不因输入次数触发整屏黑白清屏。
 * User revision: profile/signature input paints changed regions without whole-screen wipes based on typing counts.
 * 用户修订：主页刷新模式只列普通与水波纹，快刷按用户 2026-10-21 要求撤下；两种模式均固定底栏。
 * User revision: the main-page refresh page lists only ordinary and water; fast was withdrawn per the user
 * on 2026-10-21, and both remaining modes retain navigation.
 * 用户修订：联网升级先画反馈再启动任务；进度只追加黑色段，不擦除已完成段，不累计整页清屏。升级前释放不在显示的封面缓存。
 * User revision: present feedback before starting online work; append black progress without erasing completed segments or counting page cleanup. Release off-screen cover caches before updating.
 * 用户确认：字号并入系统字体并预览；选择字体后询问同步阅读字体，设置按连接/显示/个性化/升级分组。
 * Approved revision: size and preview live with the UI face; ask before syncing the reading face, and group connection/display/personalization/maintenance.
 * 用户修订：设置题头与列表周围留白保持纯白，普通主页切入/周期整理复用书架同轮分区输出；题头柔和，内容白像素只向白擦除，灰阶保留。普通列表滑动使用柔和差分；手动全刷、离散滚动和水波纹节奏保持。
 * User revision: keep setting chrome/list whitespace white. Ordinary root entry/periodic cleanup reuse simultaneous shelf regions with soft chrome, directional white content and preserved grays. Scroll ordinary lists with soft differences; retain manual full refresh, discrete gestures and ripple pacing.
 * 用户撤回跟手滚动：三种主页模式统一恢复一次手势推进一段列表并差分刷新的原逻辑，无惯性、浅墨或停稳补刷。
 * User rollback: all main modes return to one discrete list step per gesture with differential updates, without inertia, light motion ink or idle restoration.
 * Fan firmware profile: OTA upgrade controls are removed from Settings; OTA partition metadata stays in the image.
 * 用户新增：个性化功能新增「触摸震动」入口，页内包含开关与强度档位，打开或改档即时震一记作预览。
 * User addition: personal settings gain a Touch vibration entry whose page holds the switch and intensity steps; enabling or changing the step previews one pulse.
 * 用户 2026-10-25 新增：个性化功能在触摸震动下方新增「阅读背景」入口；列表加预览对齐壁纸选择，
 * 解码延迟到 tick，入口自阅读器迁出后旧面板删除。
 * User addition of 2026-10-25: personalization gains a Reading background entry under Touch
 * vibration; list-plus-preview mirrors the wallpaper picker, decoding defers to the tick, and the
 * reader's retired panel is deleted now that the entry lives here.
 * 用户 2026-10-26 新增：设置列表最底部一行居中的版本号与固件构建日期，随列表滚动露出。
 * User addition of 2026-10-26: the settings list ends with a centred version and firmware build
 * date line, revealed by scrolling to the bottom.
 * 用户 2026-11-03 新增：阅读背景下面保留组件库入口，天气隐藏后仍可重新添加。
 * User addition of 2026-11-03: keep Components below Reading background so hidden weather can be enabled again.
 */
#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "app_content_open.h"
#include "app_device_settings_mode.h"
#include "settings_bluetooth.h"
#include "app_components_mode.h"
#include "app_registry.h"
#include "app_transfer_mode.h"
#include "settings.h"
#include "haptics.h"
#include "lock_pin_flow.h"
#include "ui_pinpad.h"
#include "boot_state.h"
#include "ble_page_turner.h"
#include "device_power.h"
#include "frontlight.h"
#include "pmu_protocol.h"
#include "wifi_transfer.h"
#include "esp_log.h"
#include "ttf_font.h"
#include "app_font_context.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_text_input.h"
#include "ui_keyboard.h"
#include "ui_nav.h"
#include "ui_wallpaper.h"
#include "ui_lock_style.h"
#include "ui_reader_background.h"
#include "ui_avatar.h"
#include "ui_builtin_image.h"
#include "book_search.h"
#include "book_store.h"
#include "book_cover.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "display.h"
#include "display_refresh.h"
#include "pmu_selftest.h"
#include "cloudzao_service.h"
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "soc/rtc_cntl_reg.h"
#endif
#include "time_service.h"
#include "network_service.h"
#include "cellular_service.h"
#include "memory_manager.h"
#include "memory_peak.h"
#include "memory_inventory.h"

static void fit_value(char *value, int width);
static void settings_prepare_profile(void);
static void settings_card(uint8_t *fb, EpdRect rect, int radius, uint8_t fill, uint8_t edge);
static char s_notice[96];
// 主页刷新模式只列出普通与水波纹；快刷已撤下，旧设置加载时回落普通。
// Main-page refresh lists ordinary and water only; fast was retired and stored values fall back to ordinary.
static const app_main_refresh_mode_t s_main_refresh_choices[] = {
    APP_MAIN_REFRESH_NORMAL, APP_MAIN_REFRESH_WATER,
};
#define MAIN_REFRESH_CHOICE_COUNT ((int)(sizeof(s_main_refresh_choices) / sizeof(s_main_refresh_choices[0])))
typedef enum { SETTINGS_MAIN, SETTINGS_WIFI, SETTINGS_NETWORK, SETTINGS_TIME,
               SETTINGS_TIME_EDIT, SETTINGS_SHELF_STYLE, SETTINGS_SYSTEM_FONT,
               SETTINGS_SYSTEM_SIZE, SETTINGS_SYSTEM_CONTRAST, SETTINGS_MAIN_REFRESH, SETTINGS_LOCK_STYLE,
               SETTINGS_READING_BG, SETTINGS_READING_BG_PREVIEW,
               SETTINGS_CONFIG, SETTINGS_BOOT,
               SETTINGS_POWER_SLEEP, SETTINGS_AUTO_LOCK, SETTINGS_HAPTIC,
               SETTINGS_POWER_LOG, SETTINGS_BATT_CALIBRATION, SETTINGS_CHARGE_CURRENT,
               SETTINGS_PROFILE, SETTINGS_AVATAR,
               SETTINGS_BLUETOOTH,
               SETTINGS_MEMORY,
               SETTINGS_QUICK_BAR, SETTINGS_SCREEN_OFFSET,
               SETTINGS_LOCK_PIN, SETTINGS_PIN_ENTRY, SETTINGS_TEXT_EDIT } settings_page_t;
static settings_page_t s_page;
static char s_quick_notice[96];
// 快捷栏长按蓝牙或震动圆键的待定子页；on_enter 消费一次后回到主列表。
// Pending sub-page from a quick-bar Bluetooth or haptics hold; on_enter consumes it once and returns to the main list.
static settings_page_t s_enter_page;
// 本次直达是否从其他页面进入（返回键回到来源页）；以及蓝牙子页当前是否仍按这次直达返回。
// Whether this direct visit came from another page (back returns to the origin), and
// whether the Bluetooth sub-page still uses that back target.
static bool s_enter_ble_from_quick;
static bool s_ble_shortcut_back;
// 触摸震动直达有独立返回标记，设置页内重进时仍返回主列表。
// Haptics keeps its own direct-visit back flag; re-entry within Settings still returns to the main list.
static bool s_enter_haptics_from_quick;
static bool s_haptics_shortcut_back;
static bool s_enter_network_from_quick;
static bool s_network_shortcut_back;
static bool s_enter_memory_from_quick;
static bool s_memory_shortcut_back;
static network_status_t s_network_snapshot;
static const char *s_network_notice;
static mm_snapshot_t s_memory_snapshot;
static mm_clean_report_t s_memory_report;
static int64_t s_memory_next_sample_ms;
static int64_t s_memory_last_present_ms;
static bool s_memory_present_pending;
static bool s_memory_dirty;
enum {
    NETWORK_TOGGLE_Y = 272,
    NETWORK_TOGGLE_H = 126,
    NETWORK_ROUTE_SECTION_Y = 434,
    NETWORK_ROUTE_ROW_Y = 472,
    NETWORK_ROW_H = 70,
};

static const char *settings_cellular_status(const network_status_t *network) {
    if (!network->cellular_enabled) return "已關閉";
    if (network->cellular_ready) return "已連線";
    switch (network->cellular_state) {
        case CELLULAR_SERVICE_NO_SIM: return "未檢測到 SIM 卡";
        case CELLULAR_SERVICE_SIM_NOT_READY:
            return network->cellular_detail[0] ? network->cellular_detail : "SIM 卡未就緒";
        case CELLULAR_SERVICE_NO_REGISTRATION: return "網路註冊被拒";
        case CELLULAR_SERVICE_ERROR: return "連線失敗";
        case CELLULAR_SERVICE_PROBING: return "正在檢測模組";
        case CELLULAR_SERVICE_STARTING: return "正在連線";
        default: return "未連線";
    }
}

static bool settings_network_poll(void) {
    network_status_t next;
    network_service_get_status(&next);
    const bool changed = s_page == SETTINGS_NETWORK
        ? memcmp(&next, &s_network_snapshot, sizeof(next)) != 0
        : s_page == SETTINGS_MAIN && next.cellular_capable != s_network_snapshot.cellular_capable;
    if (changed) s_network_snapshot = next;
    return changed;
}

static ui_pinpad_t s_pinpad;
static lock_pin_flow_t s_pinflow;
static bool s_pin_pending;
static unsigned s_pin_failures;
static int64_t s_pin_retry_at;
static void pin_setup_close(void) {
    ui_pinpad_end(&s_pinpad); lock_pin_flow_end(&s_pinflow); s_pin_pending = false;
}
static void pin_setup_open(app_ctx_t *ctx, bool disable) {
    pin_setup_close(); lock_pin_flow_begin(&s_pinflow, disable);
    ui_pinpad_begin(&s_pinpad, ctx->fb, lock_pin_flow_title(&s_pinflow));
    s_page = SETTINGS_PIN_ENTRY;
}
static app_redraw_t pin_paint(app_ctx_t *ctx, EpdRect area) {
    ui_pinpad_paint(ctx->fb, &s_pinpad, area);
    guard_draw_result(ctx->hl, display_refresh_area(ctx->hl, DISPLAY_REFRESH_DETAIL, area, DISPLAY_REFRESH_DEFAULT));
    return APP_REDRAW_DONE;
}
static app_redraw_t pin_setup_tick(app_ctx_t *ctx) {
    if (s_pinpad.blocked && ctx->now_ms >= s_pin_retry_at) {
        ui_pinpad_reset(&s_pinpad, NULL, "請重新輸入");
        return pin_paint(ctx, ui_pinpad_entry_area());
    }
    if (!s_pin_pending) return APP_REDRAW_NONE;
    s_pin_pending = false;
    lock_pin_step_t prior = s_pinflow.step;
    esp_err_t error = lock_pin_flow_input(&s_pinflow, s_pinpad.digits);
    if (error == ESP_OK && s_pinflow.step == PIN_FLOW_DONE) {
        pin_setup_close(); s_page = SETTINGS_LOCK_PIN;
        snprintf(s_notice, sizeof(s_notice), "%s", lock_pin_enabled() ? "鎖屏密碼已儲存" : "鎖屏密碼已關閉");
        return APP_REDRAW_PAGE;
    }
    const char *notice = error == ESP_OK ? "" : error == ESP_ERR_INVALID_RESPONSE ? "兩次輸入不同，請重設" :
        error == ESP_ERR_INVALID_ARG ? "原密碼不正確，請重試" : "儲存失敗，請重試";
    ui_pinpad_reset(&s_pinpad, lock_pin_flow_title(&s_pinflow), notice);
    if (error == ESP_ERR_INVALID_ARG && prior == PIN_FLOW_OLD && ++s_pin_failures >= 3) {
        s_pin_failures = 0; s_pin_retry_at = ctx->now_ms + 10000;
        snprintf(s_pinpad.notice, sizeof(s_pinpad.notice), "請等待 10 秒後重試"); s_pinpad.blocked = true;
    }
    return pin_paint(ctx, (EpdRect){122, 218, 440, 247});
}
static int s_main_scroll;
static bool s_scroll_drag_consumed, s_scroll_present_pending;
static int s_font_page, s_wallpaper_page;
static bool s_sync_pending, s_sync_waiting;
static bool s_font_sync_confirm;
static char s_font_sync_path[TTF_FONT_PATH_MAX];
static bool s_config_confirm;
static bool s_batt_calibration_confirm;
static device_power_batt_calibration_status_t s_batt_calibration_status;
static int64_t s_batt_calibration_next_ms;
// 屏幕修复：点一下就开始。整屏盲刷要几秒，活儿交给 tick——不能在触摸回调里做。
// Panel repair: one tap starts it. The blind full-screen drive takes seconds, so the work goes to the
// tick rather than a touch callback.
static bool s_panel_repair_pending;
static bool s_boot_pending;
bool app_device_settings_navigation_busy(void) {
    return s_boot_pending;
}

static const char *const TAG = "device_settings";
// 主设置始终单行；大字号只改变字形，字体预览单独保留足够空间。
// Main rows never stack; larger glyphs share one centerline, with extra space only in font previews.

static bool settings_stacked(void) { return app_settings_system_font_size() >= 150; }
static int settings_profile_height(void) { return 106; }
static int settings_main_row_height(void) {
    return 82;
}
#define SETTINGS_ROW_H settings_main_row_height()
#define SETTINGS_DISPLAY_ROWS 7
#define SETTINGS_PERSONAL_ROWS 7
// BOOT 刷机入口已按用户要求从设置列表撤下；页面代码保留，恢复入口只需加回一行。
// The BOOT flashing entry is hidden per the user's request; the page stays, re-adding one row restores it.
#define SETTINGS_MAINTENANCE_ROWS 2
#define SETTINGS_WIRELESS_Y (180 + settings_profile_height() + 77)
static int settings_wireless_rows(void) {
    network_status_t network = {0};
    network_service_get_status(&network);
    return network.cellular_capable ? 4 : 3;
}
#define SETTINGS_DISPLAY_Y (SETTINGS_WIRELESS_Y + settings_wireless_rows() * SETTINGS_ROW_H + 78)
#define SETTINGS_PERSONAL_Y (SETTINGS_DISPLAY_Y + SETTINGS_DISPLAY_ROWS * SETTINGS_ROW_H + 78)
#define SETTINGS_MAINTENANCE_Y (SETTINGS_PERSONAL_Y + SETTINGS_PERSONAL_ROWS * SETTINGS_ROW_H + 78)
// 电源管理集中充电档位、本机电量学习及日志。/ Power management groups charge profiles, local battery learning and logging.
#define SETTINGS_POWER_ROWS 3
#define SETTINGS_POWER_Y (SETTINGS_MAINTENANCE_Y + SETTINGS_MAINTENANCE_ROWS * SETTINGS_ROW_H + 78)
// 设置行图标：32 像素盒，在行高 68 里垂直居中；墨色统一，避免一行一个灰度。
// Setting row icons: a 32 px box centred in the 68 px row with one shared ink level.
#define SETTINGS_ICON_PX 32
#define SETTINGS_ICON_INK 0x58
// 版本行排在最后一组下方，是列表的最后一行内容；间距按行距留白，不跟设置组抢位置。
// The version footer follows the last group as the list's final content line, spaced like a group
// gap so it never crowds the settings.
#define SETTINGS_VERSION_GAP 40
static int settings_version_center_y(void) {
    return SETTINGS_POWER_Y + SETTINGS_POWER_ROWS * SETTINGS_ROW_H + SETTINGS_VERSION_GAP
           + ui_text_effective_px(20) / 2;
}
// 主页面滚动上限：把版本行完整推到导航栏之上，多留 12px 余量。
// Main-page scroll limit: bring the whole version line above the nav bar with 12 px to spare.
static int settings_scroll_max(void) {
    return settings_version_center_y() + (ui_text_effective_px(20) + 1) / 2
           - UI_NAV_REFRESH_END + 12;
}
#define SETTINGS_SCROLL_MAX settings_scroll_max()
static char s_editor[96], s_editor_notice[80];
static ui_text_edit_t s_editor_input;
static bool s_editor_signature;
static const char *system_font_label(const char *path) {
    if (!path || !path[0]) return "思源黑體";
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    static char stem[TTF_FONT_NAME_MAX];
    snprintf(stem, sizeof(stem), "%s", name);
    size_t len = strlen(stem);
    if (len >= 4 && !strcasecmp(stem + len - 4, ".ttf")) stem[len - 4] = 0;
    if (!strcasecmp(stem, "Hei")) return "思源黑體（TF 卡）";
    return ttf_font_localized_name(stem);
}
typedef struct {
    EpdRect size, preview;
    int preview_title, font_title, first, row_height, slots;
} system_font_layout_t;
static system_font_layout_t system_font_layout(void) {
    system_font_layout_t l = settings_stacked()
        ? (system_font_layout_t){{36,197,612,183},{36,471,612,216},413,722,785,142,2}
        : (system_font_layout_t){{36,197,612,100},{36,379,612,154},322,560,615,99,4};
    return l;
}
static EpdRect system_font_item_rect(int slot) {
    system_font_layout_t l = system_font_layout();
    return (EpdRect){36,l.first + slot * l.row_height,612,l.row_height - 16};
}
static EpdRect system_font_sync_button(bool sync) { return (EpdRect){sync ? 351 : 72,683,261,74}; }
static void draw_system_font_sync(uint8_t *fb) {
    // 仅一个有界弹窗，不额外打开阅读字体。/ One bounded dialog; never load the reading face here.
    settings_card(fb,(EpdRect){48,389,588,401},28,UI_GRAY_WHITE,0x58);
    ui_text_fixed_vc(fb,342,448,32,"同步到閱讀字型？",EPD_DRAW_ALIGN_CENTER,false);
    char name[80]; snprintf(name,sizeof(name),"已選擇「%s」",system_font_label(s_font_sync_path));
    while (ui_text_fixed_width_px(27,name) > 520 && strlen(name) > 6) {
        size_t n = strlen(name) - 1;
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
        name[n] = 0;
    }
    ui_text_fixed_vc(fb,342,516,27,name,EPD_DRAW_ALIGN_CENTER,false);
    ui_text_fixed_vc(fb,342,570,24,"是否讓閱讀正文也使用這個字型？",EPD_DRAW_ALIGN_CENTER,false);
    ui_text_fixed_vc(fb,342,614,23,s_notice[0]?s_notice:"不同步時，保留原來的閱讀字型。",EPD_DRAW_ALIGN_CENTER,false);
    for (int i=0;i<2;++i) {
        EpdRect r=system_font_sync_button(i);
        settings_card(fb,r,20,i ? UI_GRAY_BLACK : UI_GRAY_WHITE,0x58);
        ui_text_fixed_vc(fb,r.x+r.width/2,r.y+r.height/2,25,i ? "同步閱讀字型" : "僅系統字型",EPD_DRAW_ALIGN_CENTER,i);
    }
}
static int s_year, s_month, s_day, s_hour, s_minute;
#define WALLPAPER_MAX 64
typedef struct { char path[288]; char name[96]; } wallpaper_item_t;
// 壁纸列表放 PSRAM，理由同上。
// The wallpaper list lives in PSRAM for the same reason.
static wallpaper_item_t *s_wallpapers;

static bool wallpapers_alloc(void) {
    if (!s_wallpapers)
        s_wallpapers = heap_caps_calloc(WALLPAPER_MAX, sizeof(wallpaper_item_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_wallpapers != NULL;
}
static int s_wallpaper_count;
static int s_wallpaper_selected;
// 阅读背景选择：预览解码延迟到 tick，ready 只在解码成功后置位，notice 兼作列表页反馈行。
// Reading-background picker: preview decoding defers to the tick, ready is set only after a
// successful decode, and notice doubles as the list page's feedback line.
static bool s_reading_bg_confirm, s_reading_bg_ready, s_reading_bg_pending;
static char s_reading_bg_notice[96];

static void wallpaper_scan_dir(const char *root) {
    if (!wallpapers_alloc()) return;
    DIR *dir = opendir(root);
    if (!dir) return;
    struct dirent *entry;
    while (s_wallpaper_count < WALLPAPER_MAX && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.' || !strncmp(entry->d_name, "._", 2)) continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg") && strcasecmp(ext, ".png"))) continue;
        wallpaper_item_t *item = &s_wallpapers[s_wallpaper_count];
        if (snprintf(item->path, sizeof(item->path), "%s/%s", root, entry->d_name) >= sizeof(item->path)) continue;
        struct stat st;
        if (stat(item->path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > BOOK_IMAGE_FILE_MAX) continue;
        size_t name_len = strnlen(entry->d_name, sizeof(item->name) - 1);
        memcpy(item->name, entry->d_name, name_len);
        item->name[name_len] = 0;
        ++s_wallpaper_count;
    }
    closedir(dir);
}
static void wallpaper_scan(void) {
    s_wallpaper_count = s_wallpaper_page = 0;
    wallpaper_scan_dir("/sdcard/pictures");
    wallpaper_scan_dir("/sdcard/images");
    wallpaper_scan_dir("/sdcard");
}
// 阅读背景的保存只接受 /sdcard/pictures 前缀，列表就不得显示其它目录，否则点了也存不上。
// The reading-background setter accepts only the /sdcard/pictures prefix, so the list must not
// offer other folders; picking one would fail to persist.
static void reading_bg_scan(void) {
    s_wallpaper_count = s_wallpaper_page = 0;
    wallpaper_scan_dir("/sdcard/pictures");
    s_wallpaper_selected = 0;
    for (int i = 0; i < s_wallpaper_count; ++i)
        if (!strcmp(s_wallpapers[i].path, app_settings_book_background_path())) s_wallpaper_selected = i;
}

static void profile_editor_open(bool signature) {
    s_editor_signature = signature;
    snprintf(s_editor, sizeof(s_editor), "%s", signature ? app_settings_status_signature() : app_settings_device_name());
    ui_text_edit_init(&s_editor_input, s_editor, sizeof(s_editor));
    s_editor_notice[0] = 0;
    ui_keyboard_begin(&s_editor_input, false);
    s_page = SETTINGS_TEXT_EDIT;
}

static int days_in_month(int year, int month) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return 29;
    return days[month - 1];
}

static void time_open(void) {
    time_t now = time(NULL);
    if (now < 1704067200) {
        const pmu_snapshot_t *pmu = device_power_get();
        if (pmu && pmu->time_synced && pmu->unix_sec >= 1704067200) now = pmu->unix_sec;
    }
    if (now < 1704067200) {
        char month[4] = "Jan"; int day = 1, year = 2024;
        if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3) {
            strcpy(month, "Jan"); day = 1; year = 2024;
        }
        static const char *const months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        int m = 1; while (m < 12 && strcmp(month, months[m - 1])) ++m;
        s_year = year; s_month = m; s_day = day; s_hour = s_minute = 0;
    } else {
        struct tm date;
        localtime_r(&now, &date);
        s_year = date.tm_year + 1900; s_month = date.tm_mon + 1; s_day = date.tm_mday;
        s_hour = date.tm_hour; s_minute = date.tm_min;
    }
    s_notice[0] = 0;
    s_page = SETTINGS_TIME_EDIT;
}

static void time_adjust(int field, int step) {
    if (field == 0) s_year = s_year + step > 2099 ? 2024 : s_year + step < 2024 ? 2099 : s_year + step;
    if (field == 1) s_month = s_month + step > 12 ? 1 : s_month + step < 1 ? 12 : s_month + step;
    if (field == 2) s_day = s_day + step > days_in_month(s_year, s_month) ? 1 :
                            s_day + step < 1 ? days_in_month(s_year, s_month) : s_day + step;
    if (field == 3) s_hour = (s_hour + step + 24) % 24;
    if (field == 4) s_minute = (s_minute + step + 60) % 60;
    if (s_day > days_in_month(s_year, s_month)) s_day = days_in_month(s_year, s_month);
}

static bool time_save(void) {
    struct tm local = {.tm_year=s_year-1900,.tm_mon=s_month-1,.tm_mday=s_day,
        .tm_hour=s_hour,.tm_min=s_minute,.tm_sec=0,.tm_isdst=-1};
    int64_t epoch = (int64_t)mktime(&local);
    if (epoch < 1704067200 || epoch > 4102444799LL) return false;
    struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    if (settimeofday(&tv, NULL)) return false;
    uint8_t payload[4] = {(uint8_t)epoch, (uint8_t)(epoch >> 8),
                          (uint8_t)(epoch >> 16), (uint8_t)(epoch >> 24)};
    esp_err_t err = device_power_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
    if (err == ESP_OK) err = device_power_cmd(PMU_CMD_TIME_GET, NULL, 0);
    const pmu_snapshot_t *pmu = device_power_get();
    if (err != ESP_OK || !pmu || !pmu->time_synced) {
        snprintf(s_notice, sizeof(s_notice), "系統時間已設定，PMU 同步失敗");
        return false;
    }
    snprintf(s_notice, sizeof(s_notice), "時間已儲存，並同步到電源管理晶片");
    s_page = SETTINGS_TIME;
    return true;
}

static void settings_card(uint8_t *fb, EpdRect rect, int radius, uint8_t fill, uint8_t edge) {
    ui_fill_round_rect(fb, rect, radius, fill);
    ui_draw_control_frame(fb, rect, radius, edge);
}
static void settings_divider(uint8_t *fb, int y, int x, int width) {
    ui_draw_separator(fb, y, x, width, 0x60);
}
static void settings_value(uint8_t *fb,int x,int cy,int px,const char *value) {
    char shown[128]; snprintf(shown,sizeof(shown),"%s",value);
    size_t n=strlen(shown);
    bool arrow=n>=3 && !strcmp(shown+n-3,"›");
    if (arrow) {
        shown[n-=3]=0;
        while (n && shown[n-1]==' ') shown[--n]=0;
        ui_draw_icon(fb,x-4,cy,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
    }
    ui_text_vc(fb,x-(arrow?28:0),cy,px,shown,EPD_DRAW_ALIGN_RIGHT,false);
}
static void row(uint8_t *fb, int y, const char *label, const char *value) {
    ui_text(fb, 54, y + 17, 27, label, EPD_DRAW_ALIGN_LEFT, false);
    settings_value(fb,627,y+20+ui_text_effective_px(21)/2,21,value);
    settings_divider(fb, y + 68, 54, 575);
}
static void section(uint8_t *fb, int y, const char *title) {
    ui_text(fb, 40, y, 22, title, EPD_DRAW_ALIGN_LEFT, false);
}

/* ---- 日期与时间页 / Date and time page ---- */

// 渲染与命中测试共用同一组坐标，避免"畫在一處、點在別處"。
// Painting and hit testing share these coordinates, so a control is never drawn where it cannot be tapped.
enum {
    TIME_ROW_CURRENT_Y = 296,
    TIME_ROW_AUTO_Y = 380,
    TIME_ROW_WIFI_Y = 470,
    TIME_ROW_MANUAL_Y = 560,
    TIME_ROW_HIT_H = 70,
    TIME_LAST_LINE_Y = 660,
    TIME_HINT_LINE_Y = 700,
    TIME_ADVICE_LINE_Y = 740,
    TIME_NOTICE_LINE_Y = 830,
};

// 上次对时按"多久以前"显示比记日期好读；从未对时和时钟无效另说。
// The last sync reads better as an age than as a date; never-synced and an invalid clock differ.
static void time_last_sync_text(char *out, size_t cap) {
    uint32_t last = app_settings_time_sync_last();
    time_t now = time(NULL);
    if (!last) { snprintf(out, cap, "上次對時：從未"); return; }
    if (now < 1704067200) {
        struct tm tm;
        time_t local = (time_t)last;
        if (localtime_r(&local, &tm))
            snprintf(out, cap, "上次對時：%04d-%02d-%02d %02d:%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        else
            snprintf(out, cap, "上次對時：--");
        return;
    }
    int64_t age = (int64_t)now - (int64_t)last;
    if (age < 0) age = 0;
    if (age < 60) snprintf(out, cap, "上次對時：剛剛");
    else if (age < 3600) snprintf(out, cap, "上次對時：%lld 分鐘前", (long long)(age / 60));
    else if (age < 86400) snprintf(out, cap, "上次對時：%lld 小時前", (long long)(age / 3600));
    else snprintf(out, cap, "上次對時：%lld 天前", (long long)(age / 86400));
}

// 自动对时能否生效取决于开关与已保存的网络，缺哪一样就说哪一样。
// Auto-sync needs the switch and a saved network; name whichever one is missing.
static void time_auto_hint(char *out, size_t cap) {
    if (!app_settings_auto_time_sync()) {
        snprintf(out, cap, "自動對時已關閉，只能按上面的按鈕手動對時。");
        return;
    }
    network_status_t network = {0};
    network_service_get_status(&network);
    char ssid[33];
    bool saved_wifi = false;
    (void)wifi_transfer_get_saved_wifi(ssid, &saved_wifi);
    const bool wifi_configured = saved_wifi || network.wifi_enabled || network.wifi_ready;
    const bool cellular_available = network.cellular_capable && network.cellular_enabled;
    if (!wifi_configured && !cellular_available) {
        snprintf(out, cap, "自動對時需要先啟用 Wi-Fi 或 4G 網路。");
        return;
    }
    snprintf(out, cap, "自動對時每 6 小時一次。");
}

static void back_header(uint8_t *fb, const char *title) {
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, title, EPD_DRAW_ALIGN_CENTER, false);
    settings_divider(fb, 174, 36, 612);
}

static EpdRect standby_mode_rect(unsigned row, bool timed) {
    return (EpdRect){36, 300 + (int)row * 154, 612, row == 2 && timed ? 222 : 132};
}

static EpdRect standby_duration_rect(unsigned column) {
    return (EpdRect){64 + (int)column * 188, 738, 172, 66};
}

static void setting_icon(uint8_t *fb, int index, int cx, int cy) {
    // 每个设置项对应一个生成图标；索引顺序由各分组传入的 icons[] 数组决定。
    // One generated icon per setting row; the index order comes from each group's icons[] array.
    static const ui_icon_t icons[] = {
        UI_ICON_WIFI,              // 0 无线连接
        UI_ICON_BLUETOOTH,         // 1 蓝牙
        UI_ICON_TYPE,              // 2 系统字体、阅读操作
        UI_ICON_A_LARGE_SMALL,     // 3 系统字号
        UI_ICON_LIBRARY_BIG,       // 4 书架样式
        UI_ICON_TICKET,            // 5 锁屏样式
        UI_ICON_CLOCK,             // 6 日期与时间
        UI_ICON_CONTRAST,          // 7 系统对比度
        UI_ICON_COPY,              // 8 保存与恢复
        UI_ICON_POWER,             // 9 待机方式
        UI_ICON_TEXT_ALIGN_START,  // 10 状态栏签名
        UI_ICON_REFRESH_CW,        // 11 首页强刷
        UI_ICON_DOWNLOAD,          // 12 系统升级
        UI_ICON_CPU,               // 13 BOOT 刷机
        UI_ICON_EYE_CLOSED,        // 14 自动休眠
        UI_ICON_SLIDERS_HORIZONTAL, // 15 主页刷新模式 / Main refresh mode
        UI_ICON_LOCK,              // 16 锁屏密码 / lock screen password
        UI_ICON_ROTATE_CW,         // 17 屏幕修复 / panel repair
        UI_ICON_VIBRATE,           // 18 触摸震动 / touch vibration
        UI_ICON_BOOK_OPEN,         // 19 阅读背景 / reading background
        UI_ICON_ZAP,               // 20 电源管理 / power management
        UI_ICON_LIBRARY_BIG,       // 21 组件库 / components
        UI_ICON_MOGO_CELLULAR_SIGNAL, // 22 移动网络
    };
    if (index < 0 || index >= (int)(sizeof(icons) / sizeof(icons[0]))) return;
    ui_draw_icon(fb, cx, cy, SETTINGS_ICON_PX, icons[index], SETTINGS_ICON_INK);
}

static void settings_fit_text(char *value, int px, int width) {
    while (value[0] && ui_text_fixed_width_px(ui_text_effective_px(px),value)>width) {
        size_t n=strlen(value)-1;while(n&&((unsigned char)value[n]&0xc0)==0x80)--n;value[n]=0;
    }
}
static void fit_value(char *value, int width) { settings_fit_text(value,20,width); }

static void setting_group(uint8_t *fb, int title_y, const char *title,
                          int card_y, const int icons[], const char *const labels[],
                          const char *const values[], int count) {
    const int row_height = SETTINGS_ROW_H;
    title_y -= s_main_scroll;
    card_y -= s_main_scroll;
    // 屏外的行不测字、不生成字形；保留卡片边界与原滚动坐标。
    // Skip off-screen glyph work while preserving card boundaries and scroll coordinates.
    if (card_y + count * row_height <= 160 || title_y >= UI_NAV_TOP) return;
    if (title_y + ui_text_effective_px(20) > 160 && title_y < UI_NAV_TOP)
        ui_text(fb, 42, title_y, 20, title, EPD_DRAW_ALIGN_LEFT, false);
    settings_card(fb, (EpdRect){36, card_y, 612, count * row_height}, 22,
                  UI_GRAY_WHITE, 0x70);
    for (int i = 0; i < count; ++i) {
        int y = card_y + i * row_height;
        if (y + row_height <= 160 || y >= UI_NAV_TOP) continue;
        if (i) settings_divider(fb, y, 88, 544);
        setting_icon(fb, icons[i], 67, y + row_height / 2);
        char shown[128]; snprintf(shown,sizeof(shown),"%s",values[i]);
        int value_width = 594 - 100 - 20 - ui_text_fixed_width_px(ui_text_effective_px(25),labels[i]);
        if (value_width > 215) value_width = 215;
        if (value_width < 0) value_width = 0;
        fit_value(shown,value_width);
        ui_text_vc(fb,100,y+row_height/2,25,labels[i],EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,594,y+row_height/2,20,shown,EPD_DRAW_ALIGN_RIGHT,false);
        // 导航是图标，不依赖用户字体是否包含 › 字形，也不会被截短掉。
        // Navigation is an icon, independent of the font cmap and value truncation.
        ui_draw_icon(fb,625,y+row_height/2,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
    }
}

static void setting_toggle_track(uint8_t *fb, EpdRect track, bool active) {
    ui_fill_round_rect(fb, track, 19, active ? 0x58 : 0xc4);
    int cx = active ? track.x + 49 : track.x + 20;
    epd_fill_circle(cx, track.y + 19, 16, UI_GRAY_WHITE, fb);
    epd_draw_circle(cx, track.y + 19, 16, UI_GRAY_BLACK, fb);
}

static void setting_toggle(uint8_t *fb, int y, const char *title, const char *detail, bool active) {
    settings_card(fb, (EpdRect){36, y, 612, 126}, 22, UI_GRAY_WHITE, 0x70);
    ui_text(fb, 59, y + 24, 27, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 59, y + 75, 19, detail, EPD_DRAW_ALIGN_LEFT, false);
    setting_toggle_track(fb, (EpdRect){555, y + 43, 69, 39}, active);
}

static EpdRect shelf_style_card(int index) {
    return (EpdRect){36, 263 + index * 300, 612, index == 2 ? 230 : 277};
}
static EpdRect shelf_style_titles_row(EpdRect card) {
    return (EpdRect){card.x, card.y + 228, card.width, card.height - 228};
}
static uint8_t shelf_style_id(int index) { return index == 2 ? 5 : (uint8_t)(index + 1); }

static void draw_style_thumbnail(uint8_t *fb, int style, int top, bool titles) {
    if (top < 242 || top + 170 >= UI_NAV_TOP) return;
    if (style == 5) {
        ui_fill_round_rect(fb, (EpdRect){68,top,548,170}, 12, UI_GRAY_WHITE);
        for (int i=0;i<3;++i) {
            int y=top+9+i*52;
            ui_fill_rect_fast((EpdRect){86,y,31,43},i==1?UI_GRAY_LIGHT:UI_GRAY_BLACK,fb);
            ui_fill_rect_fast((EpdRect){137,y+6,218,3},0,fb);
            ui_fill_rect_fast((EpdRect){137,y+19,84,2},UI_GRAY_BLACK,fb);
            ui_fill_rect_fast((EpdRect){137,y+34,435,4},UI_GRAY_LIGHT,fb);
            ui_fill_rect_fast((EpdRect){137,y+34,120+i*75,4},UI_GRAY_BLACK,fb);
        }
        return;
    }
    const int left = 99;
    for (int i = 0; i < 3; ++i) {
        int x = left + 20 + i * 155;
        EpdRect cover = {x, top, 116, 145};
        ui_fill_rect_fast(cover, i == 1 ? UI_GRAY_BLACK : i == 2 ? UI_GRAY_LIGHT : UI_GRAY_BLACK, fb);
        ui_draw_round_rect(fb, cover, 1, 0x30);
        ui_draw_round_rect(fb, (EpdRect){x + 7, top + 7, 102, 130}, 1, i == 1 ? 0xc0 : 0xe0);
        ui_hairline(fb, top + 47, x + 28, 60, i == 1 ? 0xc0 : 0xe0);
        ui_hairline(fb, top + 54, x + 39, 38, i == 1 ? 0xc0 : 0xe0);
        ui_hairline(fb, top + 111, x + 44, 28, i == 1 ? 0xc0 : 0xe0);
    }
    if (style == 1) {
        ui_fill_rect_fast((EpdRect){left, top + 143, 486, 16}, UI_GRAY_BLACK, fb);
        ui_hairline(fb, top + 143, left, 486, 0x80);
    } else if (style == 2) {
        ui_draw_acrylic_guard(fb, (EpdRect){left, top + 129, 486, 35});

    }
    if (titles) for (int i = 0; i < 3; ++i)
        ui_fill_rect_fast((EpdRect){left + 43 + i * 155, top + 172, 70, 3}, UI_GRAY_BLACK, fb);
}

// 仅 82x82 灰阶头像，不缓存整屏或第二份字库；文件变更/拔卡会使缓存失效。
// One 82x82 gray avatar, never a full frame or second font; file changes/removal invalidate it.
static EXT_RAM_BSS_ATTR uint8_t s_avatar_pixels[82*82];
static char s_avatar_path[288];
static bool s_avatar_ready, s_avatar_pending;
static char s_main_wifi[33];
static bool s_main_wifi_saved;
// 版本行取应用描述里的版本号与 UTC 构建日期，把 __DATE__ 的 "Oct 26 2026" 排成 2026-10-26。
// The footer takes the app description's version and UTC build date, rendering __DATE__'s
// "Oct 26 2026" as 2026-10-26.
static const char *settings_version_text(void) {
    static char text[128];
    const esp_app_desc_t *desc = esp_app_get_description();
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month = 0;
    for (int i = 0; i < 12; ++i)
        if (!strncmp(desc->date, months[i], 3)) { month = i + 1; break; }
    int day = atoi(desc->date + 4), year = atoi(desc->date + 7);
    if (month && day > 0 && year > 0)
        snprintf(text, sizeof(text), "版本 %s · 構建 %04d-%02d-%02d", desc->version, year, month, day);
    else
        snprintf(text, sizeof(text), "版本 %s · 構建 %s", desc->version, desc->date);
    return text;
}
// 系统字号最大 200% 时版本行会顶到两侧白边，按可用宽度收一级字号再画。
// At the 200% system size the footer would reach the white margins, so step the face down to fit.
static void draw_settings_version(uint8_t *fb, int center_y) {
    const char *text = settings_version_text();
    int px = ui_text_effective_px(20);
    while (px > 14 && ui_text_fixed_width_px(px, text) > 600) --px;
    ui_text_fixed_vc(fb, 342, center_y, px, text, EPD_DRAW_ALIGN_CENTER, false);
}

static void memory_size_text(char *out, size_t cap, size_t bytes) {
    if (bytes >= 1024u * 1024u)
        snprintf(out, cap, "%u.%u MB", (unsigned)(bytes / (1024u * 1024u)),
                 (unsigned)((bytes % (1024u * 1024u)) * 10u / (1024u * 1024u)));
    else
        snprintf(out, cap, "%u KB", (unsigned)((bytes + 512u) / 1024u));
}

static const char *memory_clean_state_text(mm_clean_state_t state) {
    switch (state) {
        case MM_CLEAN_REQUESTED: return "等待主迴圈執行";
        case MM_CLEAN_RUNNING: return "正在回收可重建快取";
        case MM_CLEAN_DONE: return "上次清理已完成";
        case MM_CLEAN_PARTIAL: return "部分資源仍在使用";
        default: return "尚未清理";
    }
}

static bool memory_snapshot_changed(const mm_snapshot_t *left, const mm_snapshot_t *right) {
    mm_snapshot_t a = *left;
    mm_snapshot_t b = *right;
    // updated_ms 是采样时间戳，不应单独触发屏幕刷新。
    // updated_ms is a sampling timestamp, not a visible state change.
    a.updated_ms = 0;
    b.updated_ms = 0;
    return memcmp(&a, &b, sizeof(a)) != 0;
}

static void memory_pool_card(uint8_t *fb, int y, const char *title, const mm_pool_snapshot_t *pool) {
    char free_text[24], largest_text[24], minimum_text[24], used_text[24], total_text[24], summary[64];
    memory_size_text(free_text, sizeof(free_text), pool->free_bytes);
    memory_size_text(largest_text, sizeof(largest_text), pool->largest_free);
    memory_size_text(minimum_text, sizeof(minimum_text), pool->minimum_free);
    memory_size_text(used_text, sizeof(used_text), pool->used_bytes);
    memory_size_text(total_text, sizeof(total_text), pool->total);
    snprintf(summary, sizeof(summary), "%s · 已用 %u%%", total_text,
             pool->total ? (unsigned)((uint64_t)pool->used_bytes * 100u / pool->total) : 0);
    settings_card(fb, (EpdRect){36, y, 612, 136}, 22, UI_GRAY_WHITE, 0x70);
    ui_text_fixed_vc(fb, 60, y + 26, 25, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed_vc(fb, 616, y + 26, 17, summary, EPD_DRAW_ALIGN_RIGHT, false);
    ui_text_fixed_vc(fb, 60, y + 76, 19, "可用", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed_vc(fb, 224, y + 76, 19, free_text, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed_vc(fb, 350, y + 76, 19, "最大連續", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed_vc(fb, 610, y + 76, 19, largest_text, EPD_DRAW_ALIGN_RIGHT, false);
    ui_text_fixed_vc(fb, 60, y + 112, 17, "歷史最低", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed_vc(fb, 224, y + 112, 17, minimum_text, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed_vc(fb, 350, y + 112, 17, "已用", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed_vc(fb, 610, y + 112, 17, used_text, EPD_DRAW_ALIGN_RIGHT, false);
}

static display_rect_t memory_area_hint(app_ctx_t *ctx) {
    (void)ctx;
    return display_rect_from_epd((EpdRect){0, 218, UI_LOCK_WIDTH, UI_NAV_REFRESH_END - 218});
}

static void memory_render_content(uint8_t *fb) {
    ui_fill_rect_fast((EpdRect){0, 218, UI_LOCK_WIDTH, UI_NAV_REFRESH_END - 218}, UI_GRAY_WHITE, fb);
    ui_text_fixed_vc(fb, 36, 232, 19, "實時取樣 500ms · 螢幕按變化更新", EPD_DRAW_ALIGN_LEFT, false);
    memory_pool_card(fb, 260, "內部 RAM", &s_memory_snapshot.pool[MM_POOL_INTERNAL]);
    memory_pool_card(fb, 404, "PSRAM", &s_memory_snapshot.pool[MM_POOL_PSRAM]);
    ui_text_fixed_vc(fb, 36, 578, 19, s_memory_snapshot.inventory_partial
        ? "可歸屬資源 · 繁忙項稍後重試" : "可歸屬資源 · 含常駐記憶體與快取", EPD_DRAW_ALIGN_LEFT, false);
    settings_card(fb, (EpdRect){36, 600, 612, 252}, 22, UI_GRAY_WHITE, 0x70);
    for (int i = 0; i < MM_OWNER_COUNT; ++i) {
        const int y = 630 + i * 40;
        char value[24];
        const size_t bytes = s_memory_snapshot.owner[i].internal_bytes + s_memory_snapshot.owner[i].psram_bytes;
        memory_size_text(value, sizeof(value), bytes);
        ui_text_fixed_vc(fb, 64, y, 20, mm_owner_name((mm_owner_t)i), EPD_DRAW_ALIGN_LEFT, false);
        ui_text_fixed_vc(fb, 594, y, 20, value, EPD_DRAW_ALIGN_RIGHT, false);
        if (i + 1 < MM_OWNER_COUNT) settings_divider(fb, y + 20, 64, 530);
    }
    ui_draw_button(fb, (EpdRect){36, 882, 612, 76}, "一鍵清理可重建記憶體",
        s_memory_snapshot.clean_state == MM_CLEAN_RUNNING || s_memory_snapshot.clean_state == MM_CLEAN_REQUESTED);
    char status[96];
    snprintf(status, sizeof(status), "%s · 第 %u 次", memory_clean_state_text(s_memory_snapshot.clean_state),
             (unsigned)s_memory_snapshot.clean_request_id);
    ui_text_fixed_vc(fb, 342, 990, 20, status, EPD_DRAW_ALIGN_CENTER, false);
    char released[24], released_status[96];
    memory_size_text(released, sizeof(released), s_memory_report.released_internal + s_memory_report.released_psram);
    snprintf(released_status, sizeof(released_status), "可用淨增 %s（內部 %u KB · PSRAM %u KB）",
             released, (unsigned)(s_memory_report.released_internal / 1024u),
             (unsigned)(s_memory_report.released_psram / 1024u));
    ui_text_fixed_vc(fb, 342, 1027, 17, released_status, EPD_DRAW_ALIGN_CENTER, false);
    snprintf(status, sizeof(status), "%s · 分配未透過 %u 次",
             s_memory_snapshot.pressure ? "記憶體緊張，自動清理已啟用" : "記憶體正常，自動監測中",
             (unsigned)s_memory_snapshot.allocation_failures);
    ui_text_fixed_vc(fb, 342, 1070, 17, status, EPD_DRAW_ALIGN_CENTER, false);
}

enum { QUICK_SETTINGS_TOP = 300, QUICK_SETTINGS_ROW_H = 76 };
static void quick_settings_render(uint8_t *fb) {
    static const char *const labels[APP_QUICK_ITEM_COUNT] = {
        "WiFi", "藍芽", "全屏重新整理", "前光", "行動網路", "旋轉鎖定", "觸控震動", "記憶體清理", "鎖屏"};
    const app_quick_layout_t *layout = app_settings_quick_layout();
    back_header(fb, "快捷欄管理");
    char summary[64];
    snprintf(summary, sizeof(summary), "已選 %u / %u · 最多兩排，每排四個", layout->count, APP_QUICK_MAX_VISIBLE);
    ui_text_fixed(fb, 36, 214, 22, summary, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed(fb, 36, 252, 20, s_quick_notice[0] ? s_quick_notice : "點按名稱顯示或隱藏，右側箭頭調整位置", EPD_DRAW_ALIGN_LEFT, false);
    for (unsigned i = 0; i < APP_QUICK_ITEM_COUNT; ++i) {
        const int cy = QUICK_SETTINGS_TOP + (int)i * QUICK_SETTINGS_ROW_H + QUICK_SETTINGS_ROW_H / 2;
        const bool visible = i < layout->count;
        settings_card(fb, (EpdRect){36, cy - 34, 612, 68}, 14, visible ? 0xe0 : UI_GRAY_WHITE, 0x90);
        ui_draw_round_rect(fb, (EpdRect){62, cy - 14, 28, 28}, 5, UI_GRAY_BLACK);
        if (visible) ui_draw_icon(fb, 76, cy, 24, UI_ICON_CHECK, UI_GRAY_BLACK);
        ui_text_fixed_vc(fb, 112, cy, 26, labels[layout->order[i]], EPD_DRAW_ALIGN_LEFT, false);
        char position[24];
        if (visible) snprintf(position, sizeof(position), "%u-%u", i / 4 + 1, i % 4 + 1);
        else snprintf(position, sizeof(position), "隱藏");
        ui_text_fixed_vc(fb, 426, cy, 20, position, EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_icon(fb, 516, cy, 30, UI_ICON_CHEVRON_UP, visible && i > 0 ? UI_GRAY_BLACK : 0xc0);
        ui_draw_icon(fb, 596, cy, 30, UI_ICON_CHEVRON_DOWN, visible && i + 1 < layout->count ? UI_GRAY_BLACK : 0xc0);
    }
    settings_card(fb, (EpdRect){36, 1006, 612, 64}, 16, UI_GRAY_WHITE, 0x70);
    ui_text_fixed_vc(fb, 342, 1038, 24, "恢復預設", EPD_DRAW_ALIGN_CENTER, false);
}

static app_redraw_t quick_settings_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    int tab = ui_nav_hit(ev->x0, ev->y0);
    if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    if (ev->y0 < 190) {
        s_page = SETTINGS_MAIN;
        settings_prepare_profile();
        return APP_REDRAW_PAGE;
    }
    app_quick_layout_t layout = *app_settings_quick_layout();
    s_quick_notice[0] = 0;
    if (ui_rect_hit((EpdRect){36, 1006, 612, 64}, ev->x0, ev->y0)) {
        layout = (app_quick_layout_t)APP_QUICK_LAYOUT_DEFAULT;
    } else {
        if (ev->x0 < 36 || ev->x0 >= 648 || ev->y0 < QUICK_SETTINGS_TOP ||
            ev->y0 >= QUICK_SETTINGS_TOP + APP_QUICK_ITEM_COUNT * QUICK_SETTINGS_ROW_H)
            return APP_REDRAW_NONE;
        unsigned index = (ev->y0 - QUICK_SETTINGS_TOP) / QUICK_SETTINGS_ROW_H;
        uint8_t item = layout.order[index];
        if (ev->x0 >= 476) {
            int next = (int)index + (ev->x0 < 556 ? -1 : 1);
            if (index >= layout.count || next < 0 || next >= layout.count) return APP_REDRAW_NONE;
            layout.order[index] = layout.order[next];
            layout.order[next] = item;
        } else if (index < layout.count) {
            memmove(&layout.order[index], &layout.order[index + 1], layout.count - index - 1);
            layout.order[--layout.count] = item;
        } else {
            if (layout.count == APP_QUICK_MAX_VISIBLE) {
                snprintf(s_quick_notice, sizeof(s_quick_notice), "最多顯示八個按鈕，請先隱藏一個");
                return APP_REDRAW_PAGE;
            }
            memmove(&layout.order[layout.count + 1], &layout.order[layout.count], index - layout.count);
            layout.order[layout.count++] = item;
        }
    }
    if (!app_settings_set_quick_layout(&layout))
        snprintf(s_quick_notice, sizeof(s_quick_notice), "儲存失敗，請重試");
    return APP_REDRAW_PAGE;
}

// 校正页坐标固定为正常竖屏；按下局部反馈，完整轻点才移动一次。
// Calibration uses normal portrait coordinates; presses give local feedback and only completed taps move once.
static int s_screen_offset_pressed = -1;
static unsigned s_screen_offset_retries;
static int64_t s_screen_offset_retry_ms;

enum { SCREEN_OFFSET_RETRIES = 2, SCREEN_OFFSET_RETRY_MS = 1000 };
static EpdRect screen_offset_button(int index) {
    const EpdRect buttons[] = {{242, 430, 200, 86}, {62, 550, 200, 86},
        {422, 550, 200, 86}, {242, 670, 200, 86}, {142, 890, 400, 78}};
    return buttons[index];
}
static void screen_offset_guides(uint8_t *fb) {
    // 黑块从真实屏边向内延伸1–32px，数字就是厚度；格间留1px白缝。
    // Blocks extend 1–32px inward from the actual panel edge, labeled by thickness with 1px gaps.
    for (int n = 1; n <= APP_SCREEN_OFFSET_LIMIT; ++n) {
        int x = 70 + (n - 1) * 17, y = 210 + (n - 1) * 24;
        epd_fill_rect((EpdRect){x, 0, 16, n}, UI_GRAY_BLACK, fb);
        epd_fill_rect((EpdRect){x, 1216 - n, 16, n}, UI_GRAY_BLACK, fb);
        epd_fill_rect((EpdRect){0, y, n, 23}, UI_GRAY_BLACK, fb);
        epd_fill_rect((EpdRect){684 - n, y, n, 23}, UI_GRAY_BLACK, fb);
        char label[4]; snprintf(label, sizeof(label), "%d", n);
        ui_text_fixed_vc(fb, x + 8, 52, 13, label, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_fixed_vc(fb, x + 8, 1164, 13, label, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_fixed_vc(fb, 46, y + 11, 16, label, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_fixed_vc(fb, 638, y + 11, 16, label, EPD_DRAW_ALIGN_CENTER, false);
    }
}
static void screen_offset_draw_button(uint8_t *fb, int index) {
    const char *labels[] = {"上移 1px", "左移 1px", "右移 1px", "下移 1px", "恢復居中"};
    EpdRect box = screen_offset_button(index);
    bool pressed = index == s_screen_offset_pressed;
    epd_fill_rect(box, UI_GRAY_WHITE, fb);
    ui_fill_round_rect(fb, box, 16, pressed ? UI_GRAY_BLACK : UI_GRAY_WHITE);
    ui_draw_control_frame(fb, box, 16, UI_GRAY_BLACK);
    ui_text_fixed_vc(fb, box.x + box.width / 2, box.y + box.height / 2,
                     24, labels[index], EPD_DRAW_ALIGN_CENTER, pressed);
}
static app_redraw_t screen_offset_feedback(app_ctx_t *ctx, int previous) {
    int index = s_screen_offset_pressed >= 0 ? s_screen_offset_pressed : previous;
    if (index < 0) return APP_REDRAW_NONE;
    screen_offset_draw_button(ctx->fb, index);
    guard_draw_result(ctx->hl, display_refresh_area(ctx->hl, DISPLAY_REFRESH_FEEDBACK,
        screen_offset_button(index), DISPLAY_REFRESH_DEFAULT));
    return APP_REDRAW_DONE;
}
static void screen_offset_render(uint8_t *fb, bool presenting_target) {
    int x, y, target_x, target_y;
    app_settings_screen_offset(&target_x, &target_y);
    if (presenting_target) {
        x = target_x;
        y = target_y;
    } else {
        display_get_screen_offset(&x, &y);
    }
    const bool pending = x != target_x || y != target_y;
    ui_clear_page(fb);
    back_header(fb, "螢幕偏移修正");
    char value[80];
    snprintf(value, sizeof(value), "水平 %+d px    豎直 %+d px", x, y);
    ui_text_fixed_vc(fb, 342, 282, 27, value, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_fixed_vc(fb, 342, 340, 20, "每次移動 1px · 對照四周黑塊刻度調整", EPD_DRAW_ALIGN_CENTER, false);
    for (int i = 0; i < 5; ++i) screen_offset_draw_button(fb, i);
    epd_draw_hline(298, 593, 88, UI_GRAY_BLACK, fb);
    epd_draw_vline(342, 549, 88, UI_GRAY_BLACK, fb);
    ui_text_fixed_vc(fb, 342, 818, 21, "右移 / 下移為正 · 範圍 ±32px", EPD_DRAW_ALIGN_CENTER, false);
    const char *notice = s_notice[0] ? s_notice : pending ? "偏移已儲存，尚未生效" : "即時生效並儲存到本機";
    ui_text_fixed_vc(fb, 342, 1012, 20, notice, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_fixed_vc(fb, 342, 1054, 19, "僅修正位置偏移，邊緣外的內容不可見", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_fixed_vc(fb, 342, 1100, 19, "刻度數字 = 黑塊向內延伸的畫素", EPD_DRAW_ALIGN_CENTER, false);
    screen_offset_guides(fb);
}

static void screen_offset_present(app_ctx_t *ctx) {
    int x, y, old_x, old_y;
    app_settings_screen_offset(&x, &y);
    display_get_screen_offset(&old_x, &old_y);
    screen_offset_render(ctx->fb, true);
    const enum EpdDrawError result = update_display_screen_offset(ctx->hl, x, y);
    if (result == EPD_DRAW_SUCCESS) {
        if (x != old_x || y != old_y) ctx->screen_touch_cutoff_ms = esp_timer_get_time() / 1000;
        s_screen_offset_retries = 0;
        return;
    }
    // 失败后恢复帧只描述仍生效的坐标；后台尝试不重复落盘。
    // After failure, keep the recovery frame consistent with applied coordinates; retries do not write NVS.
    screen_offset_render(ctx->fb, false);
    s_screen_offset_retry_ms = esp_timer_get_time() / 1000 + SCREEN_OFFSET_RETRY_MS;
    ESP_LOGW(TAG, "screen offset (%d,%d) not presented: %d (%u retries left)",
             x, y, result, s_screen_offset_retries);
}

static bool screen_offset_retry_if_due(app_ctx_t *ctx) {
    if (s_page != SETTINGS_SCREEN_OFFSET || !s_screen_offset_retries ||
        ctx->now_ms < s_screen_offset_retry_ms || ctx->physical_touch_active || s_screen_offset_pressed >= 0) return false;
    --s_screen_offset_retries;
    screen_offset_present(ctx);
    return true;
}

static bool settings_gesture_contact_allowed(const app_ctx_t *ctx) {
    return s_page != SETTINGS_SCREEN_OFFSET || s_screen_offset_pressed < 0 ||
        ui_rect_hit(screen_offset_button(s_screen_offset_pressed), ctx->touch->x, ctx->touch->y);
}
static app_redraw_t screen_offset_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    int previous = s_screen_offset_pressed;
    if (ev->type == UI_GESTURE_PRESS) {
        s_screen_offset_pressed = -1;
        for (int i = 0; i < 5; ++i)
            if (ui_rect_hit(screen_offset_button(i), ev->x0, ev->y0)) s_screen_offset_pressed = i;
        return screen_offset_feedback(ctx, previous);
    }
    if (ev->type == UI_GESTURE_LONG_PRESS) return APP_REDRAW_NONE;
    s_screen_offset_pressed = -1;
    if (ev->type != UI_GESTURE_TAP) return screen_offset_feedback(ctx, previous);
    if (ui_rect_hit((EpdRect){28,80,68,64}, ev->x0, ev->y0)) { s_page = SETTINGS_MAIN; settings_prepare_profile(); return APP_REDRAW_PAGE; }
    int x, y; app_settings_screen_offset(&x, &y);
    const int dx[] = {0, -1, 1, 0}, dy[] = {-1, 0, 0, 1};
    for (int i = 0; i < 5; ++i) {
        if (i != previous || !ui_rect_hit(screen_offset_button(i), ev->x, ev->y)) continue;
        if (i == 4) x = y = 0;
        else { x += dx[i]; y += dy[i]; }
        if (x < -APP_SCREEN_OFFSET_LIMIT || x > APP_SCREEN_OFFSET_LIMIT ||
            y < -APP_SCREEN_OFFSET_LIMIT || y > APP_SCREEN_OFFSET_LIMIT) {
            snprintf(s_notice, sizeof(s_notice), "已到調整邊界");
        } else if (!app_settings_set_screen_offset(x, y)) {
            snprintf(s_notice, sizeof(s_notice), "儲存失敗，請重試");
        } else {
            s_notice[0] = 0;
        }
        return APP_REDRAW_PAGE;
    }
    return screen_offset_feedback(ctx, previous);
}

static const struct {
    device_power_charge_current_t current;
    const char *name;
    const char *rating;
} s_charge_choices[] = {
    {DEVICE_POWER_CHARGE_1A, "日常充電", "1 A · 預設"},
    {DEVICE_POWER_CHARGE_1_5A, "快速充電", "1.5 A"},
};

static EpdRect charge_choice_rect(unsigned index) {
    return (EpdRect){36, 394 + (int)index * 158, 612, 136};
}

static const char *charge_current_label(unsigned current) {
    for (unsigned i = 0; i < sizeof(s_charge_choices) / sizeof(s_charge_choices[0]); ++i) {
        if (current == s_charge_choices[i].current) return s_charge_choices[i].name;
    }
    return "暫不可用";
}

static void charge_current_render(uint8_t *fb) {
    back_header(fb, "充電檔位");
    const unsigned current = device_power_charge_current();

    // 状态区只标档位，不模拟电量或充电进度。/ The status block shows the profile, not charge progress.
    ui_fill_round_rect(fb, (EpdRect){36, 212, 612, 154}, 20, UI_GRAY_BLACK);
    ui_text_box(fb, (EpdRect){64, 231, 440, 38}, 23, "當前檔位", EPD_DRAW_ALIGN_LEFT, true);
    ui_text_box(fb, (EpdRect){64, 268, 440, 83}, 48, charge_current_label(current), EPD_DRAW_ALIGN_LEFT, true);
    epd_draw_circle(574, 289, 38, UI_GRAY_WHITE, fb);
    ui_draw_icon(fb, 574, 289, 43, UI_ICON_ZAP, UI_GRAY_WHITE);

    for (unsigned i = 0; i < sizeof(s_charge_choices) / sizeof(s_charge_choices[0]); ++i) {
        const EpdRect box = charge_choice_rect(i);
        const bool active = current == s_charge_choices[i].current;
        settings_card(fb, box, 20, UI_GRAY_WHITE, UI_GRAY_BLACK);
        ui_text_box(fb, (EpdRect){64, box.y + 16, 475, 52}, 34, s_charge_choices[i].name,
                    EPD_DRAW_ALIGN_LEFT, false);
        ui_text_box(fb, (EpdRect){64, box.y + 76, 552, 42}, 22, s_charge_choices[i].rating,
                    EPD_DRAW_ALIGN_LEFT, false);
        if (active) {
            epd_fill_circle(600, box.y + 43, 19, UI_GRAY_BLACK, fb);
            ui_draw_icon(fb, 600, box.y + 43, 24, UI_ICON_CHECK, UI_GRAY_WHITE);
        } else {
            epd_draw_circle(600, box.y + 43, 18, current ? UI_GRAY_BLACK : UI_GRAY_LIGHT, fb);
        }
    }

    settings_divider(fb, 737, 36, 612);
    const char *notice = !current ? "重啟裝置後再試" :
        s_notice[0] ? s_notice : "點按卡片切換，重啟後保留";
    ui_text_box(fb, (EpdRect){36, 759, 612, 53}, 27, notice, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){36, 829, 612, 49}, 23, "實際速度受充電器、線材和電量影響。",
                EPD_DRAW_ALIGN_LEFT, false);
    ui_nav_draw(fb, 3);
}

static app_redraw_t charge_current_touch(int x, int y) {
    const unsigned current = device_power_charge_current();
    if (!current) return APP_REDRAW_NONE;
    for (unsigned i = 0; i < sizeof(s_charge_choices) / sizeof(s_charge_choices[0]); ++i) {
        if (!ui_rect_hit(charge_choice_rect(i), x, y)) continue;
        if (current == s_charge_choices[i].current) return APP_REDRAW_NONE;
        const esp_err_t err = device_power_set_charge_current(s_charge_choices[i].current);
        snprintf(s_notice, sizeof(s_notice), "%s", err == ESP_OK ? "" : "未能切換，請重試");
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static void batt_calibration_render(uint8_t *fb) {
    device_power_batt_calibration_status_t status;
    device_power_get_batt_calibration(&status);
    back_header(fb, "自動電量校準(beta)");
    setting_toggle(fb, 272, "自動電量校準(beta)", "按本機放電學習", status.enabled);
    section(fb, 438, "學習狀態");
    ui_text_box(fb, (EpdRect){48, 482, 588, 42}, 28, status.applied ? "已應用部分校準" :
            !status.enabled ? "學習已停止" : status.sampled_bins ? "正在學習" : "等待有效樣本", EPD_DRAW_ALIGN_LEFT, false);
    char coverage[96];
    snprintf(coverage, sizeof(coverage), "已有樣本 %u / %u 個區間，可信 %u 個",
             (unsigned)status.sampled_bins, (unsigned)status.total_bins, (unsigned)status.trusted_bins);
    ui_text_box(fb, (EpdRect){48, 530, 588, 40}, 23, coverage, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){48, 578, 588, 40}, 23, "正常使用即可，學習需要多次有效放電", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_box(fb, (EpdRect){48, 620, 588, 40}, 23, "未覆蓋區間繼續使用系統估算", EPD_DRAW_ALIGN_LEFT, false);
    if (!status.enabled)
        ui_text_box(fb, (EpdRect){48, 662, 588, 40}, 23, "停止學習，已有校準繼續生效", EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, (EpdRect){36, 706, 612, 78}, s_batt_calibration_confirm ?
                   "確認重置校準" : "重置校準", false);
    if (s_notice[0] || status.save_error)
        ui_text_box(fb, (EpdRect){48, 836, 588, 52}, 22, status.save_error ? "校準儲存失敗，請重試" : s_notice,
                EPD_DRAW_ALIGN_LEFT, false);
    ui_nav_draw(fb, 3);
    return;
}

static app_redraw_t batt_calibration_touch(int x, int y) {
    if (ui_rect_hit((EpdRect){36, 272, 612, 126}, x, y)) {
        device_power_batt_calibration_status_t status;
        device_power_get_batt_calibration(&status);
        const esp_err_t err = device_power_set_batt_calibration(!status.enabled);
        s_batt_calibration_confirm = false;
        snprintf(s_notice, sizeof(s_notice), "%s", err == ESP_OK ?
                 (status.enabled ? "停止學習，已有校準繼續生效" : "已開啟，等待有效放電樣本") : "開關儲存失敗，請重試");
        device_power_get_batt_calibration(&s_batt_calibration_status);
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit((EpdRect){36, 706, 612, 78}, x, y)) {
        if (!s_batt_calibration_confirm) {
            s_batt_calibration_confirm = true;
            s_notice[0] = 0;
        } else {
            const esp_err_t err = device_power_reset_batt_calibration();
            s_batt_calibration_confirm = false;
            snprintf(s_notice, sizeof(s_notice), "%s", err == ESP_OK ?
                     "已恢復系統曲線，學習開關保持不變" : "重置未完整儲存，請重試");
            device_power_get_batt_calibration(&s_batt_calibration_status);
        }
        return APP_REDRAW_PAGE;
    }
    if (s_batt_calibration_confirm) {
        s_batt_calibration_confirm = false;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (s_page == SETTINGS_PIN_ENTRY) { ui_pinpad_paint(fb, &s_pinpad, ui_pinpad_full()); return; }
    if (s_page == SETTINGS_SCREEN_OFFSET) { screen_offset_render(fb, false); return; }
    ui_clear_page(fb);

    // 保留 ui_clear_page 的白底，灰阶只由头像、卡片与字形生成。
    // Keep ui_clear_page's white background; avatars, cards and glyphs alone generate grays.
    ui_nav_status(fb);
    if (s_page == SETTINGS_QUICK_BAR) {
        quick_settings_render(fb);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_MEMORY) {
        back_header(fb, "記憶體管理");
        memory_render_content(fb);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_LOCK_PIN) {
        back_header(fb, "鎖屏密碼");
        row(fb, 266, "四位數字密碼", !lock_pin_available() ? "暫不可用" : lock_pin_enabled() ? "已開啟" : "已關閉");
        if (!lock_pin_available()) ui_text_vc(fb, 342, 408, 24, "鎖屏設定暫不可用", EPD_DRAW_ALIGN_CENTER, false);
        else if (lock_pin_enabled()) {
            row(fb, 406, "修改密碼", "驗證原密碼  ›");
            row(fb, 512, "關閉密碼", "驗證原密碼  ›");
        } else row(fb, 406, "開啟密碼", "設定四位密碼  ›");
        if (s_notice[0]) ui_text_vc(fb, 342, 680, 23, s_notice, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 752, 21, "修改和關閉密碼需驗證原密碼", EPD_DRAW_ALIGN_CENTER, false);
        ui_nav_draw(fb, 3); return;
    }
    if (s_page == SETTINGS_PROFILE) {
        back_header(fb, "個人資料");
        ui_text(fb, 42, 214, 21, "自定義個人名稱與頭像", EPD_DRAW_ALIGN_LEFT, false);
        row(fb, 266, "個人名稱", app_settings_device_name());
        row(fb, 366, "更換頭像", app_settings_avatar_path()[0] ? "已選擇圖片  ›" : "預設圖示  ›");
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_AVATAR) {
        back_header(fb, "選擇頭像");
        section(fb, 232, "TF 卡 pictures 資料夾中的 JPG / PNG 圖片");
        row(fb, 270, "使用預設圖示", app_settings_avatar_path()[0] ? "選擇  ›" : "當前  ✓");
        if (!s_wallpaper_count) ui_text(fb, 52, 384, 23, "未找到圖片，請放入 pictures 資料夾", EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < 7; ++i) {
            int index = s_wallpaper_page * 7 + i;
            if (index >= s_wallpaper_count) break;
            EpdRect box = {36, 374 + i * 96, 612, 80};
            bool active = !strcmp(s_wallpapers[index].path, app_settings_avatar_path());
            settings_card(fb, box, 17, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[index].name);
            fit_value(name, 490);
            ui_text_vc(fb, 60, box.y + 40, 24, name, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(611, box.y + 40, 7, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_TEXT_EDIT) {
        back_header(fb, s_editor_signature ? "狀態列簽名" : "個人名稱");
        ui_text(fb, 642, 95, 24, "完成", EPD_DRAW_ALIGN_RIGHT, false);
        ui_text(fb, 36, 201, 21, s_editor_signature ? "狀態列中間顯示，留空則隱藏" : "顯示在頭像旁", EPD_DRAW_ALIGN_LEFT, false);
        ui_text_input_draw(fb, &s_editor_input, (EpdRect){36, 243, 612, 82}, 28, false, NULL);
        if (s_editor_notice[0]) ui_text_fixed_vc(fb, 36, 510, 22, s_editor_notice, EPD_DRAW_ALIGN_LEFT, false);
        ui_keyboard_draw(fb, 560);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_SHELF_STYLE) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, "書架樣式", EPD_DRAW_ALIGN_CENTER, false);
        ui_text(fb, 36, 207, 23, "選擇書架外觀與排列方式", EPD_DRAW_ALIGN_LEFT, false);
        static const char *const styles[] = {"深色書軌", "亞克力書架", "列表書架"};
        static const char *const descriptions[] = {"每頁 9 本", "每頁 9 本", "作者與進度 · 每頁 4 本"};
        if (s_notice[0]) ui_text_vc(fb, 36, 169, 21, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < 3; ++i) {
            EpdRect card = shelf_style_card(i);
            const uint8_t style = shelf_style_id(i);
            const bool active = app_settings_shelf_style() == style;
            const bool titles = app_settings_shelf_show_titles(style);
            const int y = card.y;
            settings_card(fb, card, 24, active ? 0xd0 : UI_GRAY_WHITE, active ? 0x58 : 0x70);
            draw_style_thumbnail(fb, style, y + 11, titles);
            int title_px = ui_text_effective_px(23), detail_px = ui_text_effective_px(17);
            if (title_px > 30) title_px = 30;
            if (detail_px > 22) detail_px = 22;
            ui_text_fixed_vc(fb, 68, y + 207, title_px, styles[i], EPD_DRAW_ALIGN_LEFT, false);
            ui_text_fixed_vc(fb, 310, y + 207, detail_px, descriptions[i], EPD_DRAW_ALIGN_LEFT, false);
            epd_draw_circle(609, y + 207, 13, UI_GRAY_BLACK, fb);
            if (active) epd_fill_circle(609, y + 207, 6, UI_GRAY_BLACK, fb);
            if (style != 5) {
                EpdRect row = shelf_style_titles_row(card);
                settings_divider(fb, row.y, 68, 548);
                int switch_px = ui_text_effective_px(22);
                if (switch_px > 32) switch_px = 32;
                ui_text_fixed_vc(fb, 68, row.y + row.height / 2, switch_px, "顯示書名", EPD_DRAW_ALIGN_LEFT, false);
                setting_toggle_track(fb, (EpdRect){546, row.y + 5, 69, 39}, titles);
            }
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_TIME_EDIT) {
        ui_nav_back(fb, 36, 79);
        ui_text(fb, 36, 151, 47, "日期與時間", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 36, 216, 23, "目前地點時間", EPD_DRAW_ALIGN_LEFT, false);
        ui_hairline(fb, 259, 36, 612, UI_GRAY_LIGHT);
        static const char *const labels[] = {"年", "月", "日", "時", "分"};
        int values[] = {s_year, s_month, s_day, s_hour, s_minute};
        for (int i = 0; i < 5; ++i) {
            int x = 36 + i * 122;
            ui_text(fb, x + 52, 300, 24, labels[i], EPD_DRAW_ALIGN_CENTER, false);
            ui_draw_control_frame(fb, (EpdRect){x, 354, 106, 62}, 8, UI_GRAY_BLACK);
            ui_text_vc(fb, x + 53, 385, 30, "+", EPD_DRAW_ALIGN_CENTER, false);
            char value[8]; snprintf(value, sizeof(value), i == 0 ? "%04d" : "%02d", values[i]);
            ui_text(fb, x + 53, 456, i == 0 ? 34 : 39, value, EPD_DRAW_ALIGN_CENTER, false);
            ui_draw_control_frame(fb, (EpdRect){x, 535, 106, 62}, 8, UI_GRAY_BLACK);
            ui_text_vc(fb, x + 53, 566, 30, "−", EPD_DRAW_ALIGN_CENTER, false);
        }
        ui_text(fb, 36, 649, 23, "點上方 + 或下方 − 調整數字", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){36, 767, 612, 78}, "儲存時間", false);
        ui_draw_button(fb, (EpdRect){36, 874, 612, 78}, "取消", false);
        if (s_notice[0]) ui_text(fb, 36, 996, 22, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    if (s_page == SETTINGS_WIFI) {
        back_header(fb, "WiFi");
        section(fb, 253, "已儲存網路");
        row(fb, 296, s_main_wifi_saved ? s_main_wifi : "尚未配置", s_main_wifi_saved ? "已儲存" : "");
        row(fb, 408, "連線或更改網路", "前往傳輸  ›");
        ui_text(fb, 54, 529, 21, "配網後可在日期與時間中進行網路對時。", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_NETWORK) {
        network_status_t network;
        network_service_get_status(&network);
        back_header(fb, "行動網路");
        section(fb, 232, "移動資料");
        if (network.cellular_capable) {
            setting_toggle(fb, NETWORK_TOGGLE_Y, "4G 行動網路", settings_cellular_status(&network),
                           network.cellular_enabled);
            section(fb, NETWORK_ROUTE_SECTION_Y, "業務聯網方式");
            row(fb, NETWORK_ROUTE_ROW_Y, "日期、天氣、網路", network_route_name(network.route));
            char detail[96];
            char signal[12];
            if (network.cellular_signal == 99) snprintf(signal, sizeof(signal), "未知");
            else snprintf(signal, sizeof(signal), "%d/31", network.cellular_signal);
            snprintf(detail, sizeof(detail), "4G：%s · 訊號 %s", settings_cellular_status(&network), signal);
            ui_text(fb, 48, 602, 21, detail, EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 48, 648, 21, s_network_notice ? s_network_notice :
                    "行動網路使用內建 SIM 卡；聯網方式單獨選擇。", EPD_DRAW_ALIGN_LEFT, false);
        } else {
            ui_text(fb, 48, 340, 21, network.cellular_capability_known
                    ? "當前硬體未檢測到行動網路模組。"
                    : "正在檢測行動網路硬體，請稍候。",
                    EPD_DRAW_ALIGN_LEFT, false);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_FONT) {
        back_header(fb, "系統字型");
        system_font_layout_t l = system_font_layout();
        settings_card(fb,l.size,22,UI_GRAY_WHITE,0x70);
        char size[16]; snprintf(size,sizeof(size),"%u%%",app_settings_system_font_size());
        if (settings_stacked()) {
            ui_text_vc(fb,60,247,27,"系統字號",EPD_DRAW_ALIGN_LEFT,false);
            ui_text_vc(fb,589,247,27,size,EPD_DRAW_ALIGN_RIGHT,false);
            ui_text_vc(fb,60,328,18,"輕點切換，最大 200%",EPD_DRAW_ALIGN_LEFT,false);
        } else {
            ui_text_vc(fb,58,227,25,"系統字號",EPD_DRAW_ALIGN_LEFT,false);
            ui_text_vc(fb,58,269,18,"輕點切換，最大 200%",EPD_DRAW_ALIGN_LEFT,false);
            ui_text_vc(fb,589,247,26,size,EPD_DRAW_ALIGN_RIGHT,false);
        }
        ui_draw_icon(fb,619,247,28,UI_ICON_ROTATE_CW,0x38);
        section(fb,l.preview_title,"效果預覽");
        settings_card(fb,l.preview,22,UI_GRAY_WHITE,0x70);
        ui_text_vc(fb,60,l.preview.y+(settings_stacked()?68:51),29,"閱讀，讓時間慢下來",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,60,l.preview.y+(settings_stacked()?151:107),25,"Aa 123",EPD_DRAW_ALIGN_LEFT,false);
        section(fb,l.font_title,"選擇字型");
        const char *selected = app_settings_system_font_path();
        int count = ttf_font_count(), first = s_font_page*l.slots;
        for (int slot=0;slot<l.slots && first+slot<=count;++slot) {
            int index=first+slot;
            const ttf_font_item_t *item=index?ttf_font_item(index-1):NULL;
            bool active=item?!strcmp(selected,item->path):!selected[0];
            EpdRect box=system_font_item_rect(slot);
            settings_card(fb,box,20,active?0xe0:UI_GRAY_WHITE,0x70);
            char label[TTF_FONT_NAME_MAX];
            snprintf(label,sizeof(label),"%s",item?system_font_label(item->path):"思源黑體（內建）");
            settings_fit_text(label,26,box.width-100);
            ui_text_vc(fb,60,box.y+box.height/2,26,label,EPD_DRAW_ALIGN_LEFT,false);
            if (active) epd_fill_circle(613,box.y+box.height/2,6,UI_GRAY_BLACK,fb);
        }
        char pages[24];snprintf(pages,sizeof(pages),"%d / %d",s_font_page+1,(count+l.slots)/l.slots);
        ui_text_fixed_vc(fb,648,1080,18,pages,EPD_DRAW_ALIGN_RIGHT,false);
        if (s_notice[0]) ui_text_fixed_vc(fb,42,1080,18,s_notice,EPD_DRAW_ALIGN_LEFT,false);
        ui_nav_draw(fb,3);
        if (s_font_sync_confirm) draw_system_font_sync(fb);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_SIZE) {
        back_header(fb, "系統字號");
        section(fb, 248, "介面文字大小");
        for (int i = 0; i < 11; ++i) {
            int value = 100 + i * 10;
            EpdRect box = {36 + (i % 2) * 316, 298 + (i / 2) * 122, 296, 102};
            bool active = app_settings_system_font_size() == value;
            settings_card(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            char label[48]; snprintf(label, sizeof(label), "%d%%", value);
            ui_text_vc(fb, box.x + 24, box.y + 51, 29, label, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(box.x + box.width - 25, box.y + 51, 8, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_MAIN_REFRESH) {
        back_header(fb, "主頁重新整理模式");
        section(fb, 248, "首頁 · 書架 · 檔案管理 · 設定");
        const char* names[] = {"普通", "水波紋"};
        const char* details[] = {"保留灰階層次，兼顧畫面清晰度", "以水波紋切換頁面，保留灰階"};
        for (int i = 0; i < MAIN_REFRESH_CHOICE_COUNT; ++i) {
            const EpdRect box = {36, 298 + i * 158, 612, 132};
            const bool active = app_settings_main_refresh_mode() == s_main_refresh_choices[i];
            settings_card(fb, box, 22, active ? 0xd0 : UI_GRAY_WHITE, 0x58);
            ui_text_vc(fb, 64, box.y + 38, 30, names[i], EPD_DRAW_ALIGN_LEFT, false);
            char detail[128]; snprintf(detail, sizeof(detail), "%s", details[i]); fit_value(detail, 498);
            ui_text_vc(fb, 64, box.y + 89, 22, detail, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(607, box.y + 38, 8, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_CONTRAST) {
        back_header(fb, "系統對比度");
        section(fb, 248, "調整系統介面的灰階層次與清晰度");
        static const char *const names[] = {"柔和", "標準", "清晰", "鮮明", "高對比"};
        for (int i = 0; i < 5; ++i) {
            int value = 100 + i * 10;
            EpdRect box = {36, 298 + i * 116, 612, 90};
            bool active = app_settings_system_contrast() == value;
            settings_card(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            ui_text_vc(fb, 64, box.y + 45, 28, names[i], EPD_DRAW_ALIGN_LEFT, false);
            char shown[16]; snprintf(shown, sizeof(shown), "%d%%", value);
            ui_text_vc(fb, 570, box.y + 45, 22, shown, EPD_DRAW_ALIGN_RIGHT, false);
            if (active) epd_fill_circle(605, box.y + 45, 8, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }

    if (s_page == SETTINGS_LOCK_STYLE) {
        ui_lock_style_render(fb);
        return;
    }

    if (s_page == SETTINGS_READING_BG) {
        back_header(fb, "閱讀背景");
        section(fb, 244, "TF 卡 pictures · JPG / PNG · 不超過 50 MB");
        if (!s_wallpaper_count)
            ui_text(fb, 54, 325, 25, "未找到圖片，請放入 TF 卡的 pictures 資料夾", EPD_DRAW_ALIGN_LEFT, false);
        // 7 行/页给底部的关闭按钮留位；壁纸列表的 8 行版式不放按钮。
        // Seven rows per page leave room for the bottom close button; the wallpaper list has
        // no button and keeps eight.
        for (int i = 0; i < 7; ++i) {
            int index = s_wallpaper_page * 7 + i;
            if (index >= s_wallpaper_count) break;
            EpdRect box = {36, 292 + i * 92, 612, 78};
            bool active = !strcmp(s_wallpapers[index].path, app_settings_book_background_path());
            settings_card(fb, box, 18, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[index].name);
            fit_value(name, 490);
            ui_text_vc(fb, 60, box.y + 39, 24, name, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 39, 6, UI_GRAY_BLACK, fb);
        }
        ui_draw_button(fb, (EpdRect){36, 956, 612, 76}, "關閉閱讀背景 · 恢復白底", false);
        if (s_reading_bg_notice[0])
            ui_text_vc(fb, 342, 1062, 21, s_reading_bg_notice, EPD_DRAW_ALIGN_CENTER, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_READING_BG_PREVIEW) {
        back_header(fb, "預覽閱讀背景");
        EpdRect image = {166, 194, 352, 626};
        ui_fill_round_rect(fb, image, 8, UI_GRAY_WHITE);
        if (s_reading_bg_ready) ui_reader_background_paint(fb, image, false);
        ui_draw_round_rect(fb, image, 8, 0x70);
        if (!s_reading_bg_ready)
            ui_text_vc(fb, 342, 507, 23, s_reading_bg_pending ? "正在載入預覽…" :
                (s_reading_bg_notice[0] ? s_reading_bg_notice : "正在載入預覽…"),
                EPD_DRAW_ALIGN_CENTER, false);
        if (s_wallpaper_selected >= 0 && s_wallpaper_selected < s_wallpaper_count) {
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[s_wallpaper_selected].name);
            fit_value(name, 570);
            ui_text_vc(fb, 342, 859, 24, name, EPD_DRAW_ALIGN_CENTER, false);
            char position[40];
            snprintf(position, sizeof(position), "%d / %d · 上下滑動切換", s_wallpaper_selected + 1, s_wallpaper_count);
            ui_text_vc(fb, 342, 908, 20, position, EPD_DRAW_ALIGN_CENTER, false);
        }
        if (s_reading_bg_ready) {
            EpdRect button = {94, 962, 496, 70};
            ui_fill_round_rect(fb, button, 22, 0x30);
            ui_text_vc(fb, 342, 997, 25, "使用閱讀背景", EPD_DRAW_ALIGN_CENTER, true);
        }
        if (s_reading_bg_confirm) {
            EpdRect dialog = {68, 435, 548, 306};
            ui_fill_round_rect(fb, dialog, 26, UI_GRAY_WHITE);
            ui_draw_control_frame(fb, dialog, 26, 0x50);
            ui_text_vc(fb, 342, 503, 31, "使用閱讀背景？", EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 342, 565, 20, "確認後，閱讀正文以它為底，文字自動加深", EPD_DRAW_ALIGN_CENTER, false);
            EpdRect cancel = {94, 643, 232, 68}, confirm = {358, 643, 232, 68};
            ui_fill_round_rect(fb, cancel, 18, 0xe0);
            ui_fill_round_rect(fb, confirm, 18, 0x30);
            ui_text_vc(fb, 210, 677, 24, "取消", EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 474, 677, 24, "確認", EPD_DRAW_ALIGN_CENTER, true);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_TIME) {
        back_header(fb, "日期與時間");
        time_t now = time(NULL);
        char shown[64] = "未設定";
        if (now >= 1704067200) {
            struct tm tm;
            if (localtime_r(&now, &tm)) snprintf(shown, sizeof(shown), "%04d-%02d-%02d  %02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        }
        section(fb, 253, "當前時間 · 目前地點");
        row(fb, TIME_ROW_CURRENT_Y, shown, "");
        row(fb, TIME_ROW_AUTO_Y, "自動對時", app_settings_auto_time_sync() ? "開  ›" : "關  ›");
        row(fb, TIME_ROW_WIFI_Y, "透過網路對時", "立即同步  ›");
        row(fb, TIME_ROW_MANUAL_Y, "手動設定", "編輯  ›");
        char line[96];
        time_last_sync_text(line, sizeof(line));
        ui_text(fb, 54, TIME_LAST_LINE_Y, 21, line, EPD_DRAW_ALIGN_LEFT, false);
        time_auto_hint(line, sizeof(line));
        ui_text(fb, 54, TIME_HINT_LINE_Y, 21, line, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, TIME_ADVICE_LINE_Y, 21, "長時間離線可能產生誤差，建議定期重新對時。", EPD_DRAW_ALIGN_LEFT, false);
        if (s_sync_pending || s_sync_waiting || s_notice[0])
            ui_text(fb, 54, TIME_NOTICE_LINE_Y, 22, s_sync_pending || s_sync_waiting ? "正在獲取網路時間…" : s_notice,
                    EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_AUTO_LOCK) {
        back_header(fb, "自動休眠鎖屏");
        section(fb, 228, "無操作後自動鎖屏");
        const char *labels[] = {"1 分鐘", "5 分鐘", "10 分鐘", "關閉"};
        const uint8_t values[] = {1, 5, 10, 0};
        for (int i = 0; i < 4; ++i) {
            EpdRect box = {36, 272 + i * 112, 612, 92};
            bool active = app_settings_auto_lock_minutes() == values[i];
            settings_card(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            ui_text_vc(fb, 64, box.y + box.height / 2, 30, labels[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + box.height / 2, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 48, 760, 23, "操作後重新計時，傳輸期間暫停", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 810, 23, "待機方式可選僅淺睡、定時關機或立即關機", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_HAPTIC) {
        back_header(fb, "觸控震動");
        section(fb, 228, "單指按下時短震一次");
        setting_toggle(fb, 272, "觸控震動", "點選螢幕時震一下", app_settings_touch_haptics());
        section(fb, 438, "震動強度");
        const char *labels[] = {"弱", "中", "強"};
        const char *details[] = {"最輕，預設", "適中", "最明顯"};
        for (int i = 0; i < 3; ++i) {
            EpdRect box = {36, 482 + i * 112, 612, 92};
            bool active = app_settings_haptic_intensity() == (uint8_t)i;
            settings_card(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            ui_text_vc(fb, 64, box.y + box.height / 2, 30, labels[i], EPD_DRAW_ALIGN_LEFT, false);
            ui_text_vc(fb, 132, box.y + box.height / 2, 21, details[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + box.height / 2, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 48, 830, 23, "選中後立即震一下，可反覆比較", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 880, 23, "關閉後不再震，檔位仍保留", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_BATT_CALIBRATION) { batt_calibration_render(fb); return; }
    if (s_page == SETTINGS_CHARGE_CURRENT) { charge_current_render(fb); return; }
    if (s_page == SETTINGS_POWER_LOG) {
        back_header(fb, "日誌除錯");
        section(fb, 228, "電量校準日誌");
        setting_toggle(fb, 272, "寫入 batt.log", "每分鐘記錄電壓、電流與充電狀態",
                       app_settings_batt_log());
        ui_text(fb, 48, 440, 23, "寫入 TF 卡根目錄 batt.log，同時輸出到串列埠", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 485, 23, "卡不在或正在 USB 讀卡時自動跳過", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 530, 23, "關閉後不再建立檔案，也不寫日誌", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 575, 23, "預設關閉；開關立即生效", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_POWER_SLEEP) {
        back_header(fb, "待機方式");
        section(fb, 248, "選擇鎖屏後的待機方式");
        bool timed = app_settings_shutdown_minutes() != 0;
        const char *labels[] = {"徹底關機", "僅淺睡", "淺睡後關機"};
        const char *details[] = {"鎖屏即徹底斷電；畫面保留，長按電源鍵約 1 秒開機",
                                 "鎖屏畫面保留；不斷電，按鍵返回關機前頁面",
                                 "鎖屏後開始計時；提前喚醒取消本輪計時"};
        for (int i = 0; i < 3; ++i) {
            EpdRect box = standby_mode_rect((unsigned)i, timed);
            bool active = i == 2 ? timed :
                app_settings_shutdown_style() == (app_shutdown_style_t)(APP_SHUTDOWN_STAGED + i);
            settings_card(fb, box, 22, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            ui_text(fb, 64, box.y + 23, 29, labels[i], EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 64, box.y + 79, 20, details[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 66, 8, UI_GRAY_BLACK, fb);
        }
        if (timed) {
            const char *durations[] = {"10 分鐘", "30 分鐘", "60 分鐘"};
            for (unsigned i = 0; i < 3; ++i) {
                bool active = app_settings_shutdown_style() == (app_shutdown_style_t)(APP_SHUTDOWN_LIGHT_10 + i);
                ui_draw_button(fb, standby_duration_rect(i), durations[i], active);
            }
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_BOOT) {
        back_header(fb, "電腦刷機");
        settings_card(fb, (EpdRect){36, 254, 612, 298}, 22, UI_GRAY_WHITE, 0x70);
        ui_text(fb, 60, 288, 29, "進入 BOOT 模式", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 350, 22, "用於官網完整刷機或故障恢復", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){60, 438, 564, 78}, s_boot_pending ? "正在進入" : "進入 BOOT 模式", false);
        ui_text(fb, 48, 620, 22, "請用 USB 連線電腦，再開啟官網刷機頁", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3); return;
    }
    if (s_page == SETTINGS_CONFIG) {
        back_header(fb, "儲存與恢復配置");
        section(fb, 248, "換機或刷機後，快速恢復個性化設定");
        settings_card(fb, (EpdRect){36, 299, 612, 197}, 22, UI_GRAY_WHITE, 0x70);
        ui_text(fb, 60, 326, 26, "TF 卡根目錄", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 377, 23, "配置備份檔案", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 435, 19, "文字排版、裝置資料、閱讀記錄與書籤", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){36, 561, 612, 83}, "儲存當前配置到 TF 卡", false);
        ui_draw_button(fb, (EpdRect){36, 681, 612, 83},
                       s_config_confirm ? "再次點按，確認恢復配置" : "從 TF 卡恢復配置", false);
        if (s_notice[0]) ui_text(fb, 48, 819, 21, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 899, 19, "包含閱讀記錄、使用者偏好和全部已存 WiFi。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 939, 19, "書籍、字型和圖片仍需留在 TF 卡。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 979, 19, "備份含明文 WiFi 密碼，請妥善保管。", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_BLUETOOTH) { settings_bluetooth_render(fb); return; }
    const int profile_y=180-s_main_scroll, profile_h=settings_profile_height();
    if (profile_y+profile_h>160) {
        settings_card(fb,(EpdRect){36,profile_y,612,profile_h},24,UI_GRAY_WHITE,0x70);
        int avatar_y=profile_y+(profile_h-82)/2;
        bool avatar_ok=s_avatar_ready;
        if (avatar_ok) ui_wallpaper_draw_avatar(fb,s_avatar_pixels,(EpdRect){57,avatar_y,82,82},20);
        if (app_settings_main_fast_refresh() && avatar_ok) ui_image_bw_rect(fb,(EpdRect){57,avatar_y,82,82});
        if (!avatar_ok) {
            ui_builtin_avatar_draw(fb,57,avatar_y,82);
        }
        char name[64];snprintf(name,sizeof(name),"%s",app_settings_device_name());
        int width=588-ui_text_fixed_width_px(ui_text_effective_px(20),"編輯")-188;
        while (name[0] && ui_text_fixed_width_px(ui_text_effective_px(32),name)>width) {
            size_t n=strlen(name)-1;while(n&&((unsigned char)name[n]&0xc0)==0x80)--n;name[n]=0;
        }
        ui_text_vc(fb,164,profile_y+53,32,name,EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,588,profile_y+53,20,"編輯",EPD_DRAW_ALIGN_RIGHT,false);
        ui_draw_icon(fb,625,profile_y+profile_h/2,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
    }
    char wifi[56];snprintf(wifi,sizeof(wifi),"%s",s_main_wifi_saved?s_main_wifi:"未配置");
    network_status_t network;
    network_service_get_status(&network);
    const char *connection_labels[4];
    const char *connection_values[4];
    int connection_icons[4];
    int connection_rows = 0;
    connection_labels[connection_rows] = "WiFi";
    connection_values[connection_rows] = wifi;
    connection_icons[connection_rows++] = 0;
    if (network.cellular_capable) {
        connection_labels[connection_rows] = "行動網路";
        connection_values[connection_rows] = network.route == NETWORK_ROUTE_CELLULAR ? "4G" :
            network.route == NETWORK_ROUTE_AUTO ? "自動" : "Wi-Fi";
        connection_icons[connection_rows++] = 22;
    }
    connection_labels[connection_rows] = "藍芽翻頁器";
    connection_values[connection_rows] = app_settings_ble_turner() ? "已開啟" : "已關閉";
    connection_icons[connection_rows++] = 1;
    connection_labels[connection_rows] = "日期與時間";
    connection_values[connection_rows] = "設定";
    connection_icons[connection_rows++] = 6;
    setting_group(fb,SETTINGS_WIRELESS_Y-49,"連線與時間",SETTINGS_WIRELESS_Y,
                  connection_icons,connection_labels,connection_values,connection_rows);
    char font_name[96],contrast[24],idle[24],standby[48];
    snprintf(font_name,sizeof(font_name),"%s · %u%%",system_font_label(app_settings_system_font_path()),app_settings_system_font_size());
    snprintf(contrast,sizeof(contrast),"%u%%",app_settings_system_contrast());
    unsigned minutes=app_settings_auto_lock_minutes();
    if(minutes)snprintf(idle,sizeof(idle),"%u 分鐘",minutes);else strcpy(idle,"關閉");
    unsigned shutdown_minutes=app_settings_shutdown_minutes();
    if(shutdown_minutes)snprintf(standby,sizeof(standby),"淺睡 %u 分鐘後關機",shutdown_minutes);
    else snprintf(standby,sizeof(standby),"%s",app_settings_shutdown_style()==APP_SHUTDOWN_LIGHT?"僅淺睡":"徹底關機");
    const char *display_labels[]={"系統字型","系統對比度","主頁重新整理模式","首頁強刷","自動休眠鎖屏","待機方式","螢幕偏移修正"};
    const char *display_values[]={font_name,contrast,
        app_settings_main_refresh_mode()==APP_MAIN_REFRESH_WATER?"水波紋":"普通",
        app_settings_home_full_refresh()?"開啟":"關閉",idle,
        standby,"1px 微調"};
    const int display_icons[]={2,7,15,11,14,9,17};
    setting_group(fb,SETTINGS_DISPLAY_Y-49,"顯示",SETTINGS_DISPLAY_Y,
                  display_icons,display_labels,display_values,SETTINGS_DISPLAY_ROWS);
    const char *personal_labels[]={"書架樣式","桌布","鎖屏密碼","觸控震動","閱讀背景","元件庫","快捷欄管理"};
    char haptic_value[24];
    if(!haptics_available())snprintf(haptic_value,sizeof(haptic_value),"不支援");
    else if(!app_settings_touch_haptics())snprintf(haptic_value,sizeof(haptic_value),"關閉");
    else snprintf(haptic_value,sizeof(haptic_value),"開啟 · %s",
        app_settings_haptic_intensity()==APP_HAPTIC_STRONG?"強":
        app_settings_haptic_intensity()==APP_HAPTIC_MEDIUM?"中":"弱");
    char background_value[96];
    const char *background_path=app_settings_book_background_path();
    const char *background_base=background_path[0]?strrchr(background_path,'/'):NULL;
    snprintf(background_value,sizeof(background_value),"%s",
        background_path[0]?(background_base?background_base+1:background_path):"無");
    const char *personal_values[]={app_settings_shelf_style()==5?"列表書架":app_settings_shelf_style()==1?"深色書軌":"亞克力書架",
        app_settings_lock_style()==APP_LOCK_STYLE_CLOCK?"簡約時鐘":app_settings_lock_style()==4?"日曆":app_settings_lock_style()==3?"透明書封":app_settings_lock_style()==2?"書架拼貼":app_settings_lock_style()==1?"桌布":"閱讀票根",
        lock_pin_enabled()?"已開啟":"已關閉",haptic_value,background_value,"","顯示與排序"};
    const int personal_icons[]={4,5,16,18,19,21,15};
    setting_group(fb,SETTINGS_PERSONAL_Y-49,"個性化功能",SETTINGS_PERSONAL_Y,
                  personal_icons,personal_labels,personal_values,SETTINGS_PERSONAL_ROWS);
    const char *maintenance_labels[]={"儲存與恢復","螢幕修復"};
    const char *maintenance_values[]={"配置","清除條紋"};
    const int maintenance_icons[]={8,17};
    setting_group(fb,SETTINGS_MAINTENANCE_Y-49,"維護與恢復",SETTINGS_MAINTENANCE_Y,
                  maintenance_icons,maintenance_labels,maintenance_values,SETTINGS_MAINTENANCE_ROWS);
    char batt_log_value[16];
    snprintf(batt_log_value,sizeof(batt_log_value),"%s",app_settings_batt_log()?"開啟":"關閉");
    device_power_batt_calibration_status_t calibration;
    device_power_get_batt_calibration(&calibration);
    const char *power_labels[]={"自動電量校準(beta)", "日誌除錯", "充電檔位"};
    const char *power_values[]={calibration.enabled ? "學習中" : calibration.applied ? "已保留校準" : "關閉", batt_log_value,
        charge_current_label(device_power_charge_current())};
    const int power_icons[]={20,20,20};
    setting_group(fb,SETTINGS_POWER_Y-49,"電源管理",SETTINGS_POWER_Y,
                  power_icons,power_labels,power_values,SETTINGS_POWER_ROWS);
    const int version_half = (ui_text_effective_px(20) + 1) / 2;
    const int version_center_y = settings_version_center_y() - s_main_scroll;
    if (version_center_y - version_half > 160 && version_center_y + version_half < UI_NAV_TOP)
        draw_settings_version(fb, version_center_y);
    // 主列表也要有一行提示位：确认、修复结果都写在这里，不然点了没有任何回音。
    // The main list needs a notice line too: confirmation and the repair result land here, or a tap
    // would have no answer at all.
    ui_fill_rect_fast((EpdRect){0,0,UI_LOCK_WIDTH,160},UI_GRAY_WHITE,fb);
    ui_nav_status(fb);
    ui_text(fb, 36, 91, 52, "設定", EPD_DRAW_ALIGN_LEFT, false);
    if (s_notice[0]) ui_text_fixed_vc(fb, 36, 138, 20, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    ui_nav_draw(fb, 3);
    if(app_settings_main_refresh_mode()==APP_MAIN_REFRESH_NORMAL){
        // 与书架共用逐相输出：状态栏/题头柔和，内容白像素不反向压黑；不影响其他模式。
        // Reuse shelf scan phases: soft status/title and no reverse-blackening on content whites, without changing other modes.
        const EpdRect content={0,160,UI_LOCK_WIDTH,UI_NAV_REFRESH_END-160};
        display_main_transition_gray_bands(&content,1);
    }
}

static void settings_prepare_profile(void) {
    const char *path=app_settings_avatar_path();
    if(pico_boot_recovery()||!path[0]){ui_avatar_request("",60);s_avatar_ready=s_avatar_pending=false;s_avatar_path[0]=0;return;}
    if(strcmp(path,s_avatar_path))s_avatar_ready=false;
    snprintf(s_avatar_path,sizeof(s_avatar_path),"%s",path);
    ui_avatar_request(path,82);
    ui_avatar_state_t state=ui_avatar_poll(path,82,s_avatar_pixels);
    s_avatar_ready=state==UI_AVATAR_READY;s_avatar_pending=state==UI_AVATAR_PENDING;
}
void app_device_settings_request_bluetooth(bool return_to_origin) {
    s_enter_page = SETTINGS_BLUETOOTH;
    s_enter_ble_from_quick = return_to_origin;
    s_enter_haptics_from_quick = false;
}
void app_device_settings_request_haptics(bool return_to_origin) {
    s_enter_page = SETTINGS_HAPTIC;
    s_enter_haptics_from_quick = return_to_origin;
    s_enter_ble_from_quick = false;
}
void app_device_settings_request_network(bool return_to_origin) {
    s_enter_page = SETTINGS_NETWORK;
    s_enter_network_from_quick = return_to_origin;
    s_enter_ble_from_quick = false;
    s_enter_haptics_from_quick = false;
}
void app_device_settings_request_memory(bool return_to_origin) {
    s_enter_page = SETTINGS_MEMORY;
    s_enter_memory_from_quick = return_to_origin;
    s_enter_ble_from_quick = false;
    s_enter_haptics_from_quick = false;
    s_enter_network_from_quick = false;
}
static void on_enter(app_ctx_t *ctx) {
    s_screen_offset_pressed = -1;
    s_screen_offset_retries = 0;
    ui_lock_style_close();
    (void)ctx;
    pin_setup_close();
    s_notice[0] = 0;
    s_page = SETTINGS_MAIN;
    s_main_scroll = s_font_page = s_wallpaper_page = 0;
    s_scroll_drag_consumed = s_scroll_present_pending = false;
    s_wallpaper_selected = -1;

    s_sync_pending = false;
    s_font_sync_confirm = false; s_font_sync_path[0] = 0;
    s_config_confirm = false;
    s_batt_calibration_confirm = false;
    s_batt_calibration_next_ms = 0;
    s_panel_repair_pending = false;
    s_boot_pending = false;
    (void)wifi_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved);
    settings_prepare_profile();
    // 每次进页先清掉上一次直达的返回目标，避免离开时未经过主列表而留下旧值。
    // Clear the previous direct-visit back target on every entry so a visit that
    // left without passing the main list cannot leave a stale target behind.
    s_ble_shortcut_back = false;
    s_haptics_shortcut_back = false;
    s_network_shortcut_back = false;
    s_memory_shortcut_back = false;
    s_memory_present_pending = false;
    s_memory_dirty = false;
    s_memory_last_present_ms = 0;
    s_memory_next_sample_ms = 0;
    network_service_get_status(&s_network_snapshot);
    // 快捷栏长按直达蓝牙子页：初始状态与主列表「蓝牙」行一致，消费后回到主列表。
    // Quick-bar shortcut straight to the Bluetooth sub-page: same initial state as
    // the main list's Bluetooth row, falling back to the main list after one use.
    if (s_enter_page == SETTINGS_BLUETOOTH) {
        s_page = SETTINGS_BLUETOOTH;
        settings_bluetooth_open();
        // 从其他页面直达时，左上角返回回到来源页；设置页内部重进仍回主列表。
        // A direct visit from another page sends the top-left back to that origin;
        // re-entering from inside Settings keeps the main-list back.
        s_ble_shortcut_back = s_enter_ble_from_quick;
    }
    if (s_enter_page == SETTINGS_HAPTIC) {
        s_page = SETTINGS_HAPTIC;
        s_haptics_shortcut_back = s_enter_haptics_from_quick;
    }
    if (s_enter_page == SETTINGS_NETWORK) {
        s_page = SETTINGS_NETWORK;
        s_network_notice = NULL;
        s_network_shortcut_back = s_enter_network_from_quick;
    }
    if (s_enter_page == SETTINGS_MEMORY) {
        s_page = SETTINGS_MEMORY;
        s_memory_shortcut_back = s_enter_memory_from_quick;
        memory_inventory_snapshot(ctx ? ctx->hl : NULL, &s_memory_snapshot);
        mm_get_clean_report(&s_memory_report);
        s_memory_next_sample_ms = ctx ? ctx->now_ms : 0;
        s_memory_last_present_ms = ctx ? ctx->now_ms : 0;
    }
    s_enter_page = SETTINGS_MAIN;
    s_enter_ble_from_quick = false;
    s_enter_haptics_from_quick = false;
    s_enter_network_from_quick = false;
    s_enter_memory_from_quick = false;
}

static app_redraw_t profile_editor_paint(app_ctx_t *ctx, bool field);
static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (screen_offset_retry_if_due(ctx)) return APP_REDRAW_NONE;
    if (s_page == SETTINGS_BATT_CALIBRATION && ctx->now_ms >= s_batt_calibration_next_ms) {
        device_power_batt_calibration_status_t next;
        device_power_get_batt_calibration(&next);
        s_batt_calibration_next_ms = ctx->now_ms + 1000;
        const bool changed = next.enabled != s_batt_calibration_status.enabled ||
            next.applied != s_batt_calibration_status.applied || next.save_error != s_batt_calibration_status.save_error ||
            next.sampled_bins != s_batt_calibration_status.sampled_bins ||
            next.trusted_bins != s_batt_calibration_status.trusted_bins;
        s_batt_calibration_status = next;
        if (changed) return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_MEMORY) {
        if (ctx->now_ms >= s_memory_next_sample_ms) {
            mm_snapshot_t next;
            mm_clean_report_t report;
            memory_inventory_snapshot(ctx->hl, &next);
            mm_get_clean_report(&report);
            const bool state_changed = next.clean_state != s_memory_snapshot.clean_state ||
                next.clean_request_id != s_memory_snapshot.clean_request_id ||
                report.state != s_memory_report.state || report.request_id != s_memory_report.request_id;
            const bool changed = memory_snapshot_changed(&next, &s_memory_snapshot) ||
                memcmp(&report, &s_memory_report, sizeof(report)) != 0;
            s_memory_snapshot = next;
            s_memory_report = report;
            s_memory_next_sample_ms = ctx->now_ms + 500;
            if (changed) s_memory_dirty = true;
            const bool present_due = s_memory_last_present_ms == 0 ||
                ctx->now_ms - s_memory_last_present_ms >= 1000;
            if (s_memory_dirty && (state_changed || present_due)) {
                s_memory_present_pending = true;
                s_memory_dirty = false;
                s_memory_last_present_ms = ctx->now_ms;
                return APP_REDRAW_AREA;
            }
        }
        return APP_REDRAW_NONE;
    }
    // 屏幕修复：先画最终结果，再在 tick 中整屏恢复；成功后不重复推页。
    // Paint the final notice before the tick's full restoration; do not push the page twice on success.
    if (s_panel_repair_pending) {
        s_panel_repair_pending = false;
        if (s_page != SETTINGS_MAIN) return APP_REDRAW_NONE;
        s_scroll_present_pending = false;
        snprintf(s_notice, sizeof(s_notice), "螢幕修復完成；若條紋仍在可再做一次");
        render(ctx, ctx->fb);
        if (display_panel_repair(ctx->hl) != EPD_DRAW_SUCCESS) {
            snprintf(s_notice, sizeof(s_notice), "螢幕輸出未完成，請稍後重試");
            return APP_REDRAW_FULL;
        }
        return APP_REDRAW_DONE;
    }

    if (s_page==SETTINGS_MAIN && s_avatar_pending && !(ctx->touch && ctx->touch->touched)) {
        ui_avatar_state_t state=ui_avatar_poll(s_avatar_path,82,s_avatar_pixels);
        s_avatar_pending=state==UI_AVATAR_PENDING;
        s_avatar_ready=state==UI_AVATAR_READY;
        if (s_avatar_ready && s_main_scroll < 180+settings_profile_height()-160) {
            render(ctx,ctx->fb);
            EpdRect area={57,180-s_main_scroll+(settings_profile_height()-82)/2,82,82};
            if (area.y<160) { area.height-=160-area.y; area.y=160; }
            if(area.height>0)guard_draw_result(ctx->hl,display_refresh_area(ctx->hl, DISPLAY_REFRESH_MAIN_DETAIL, area, DISPLAY_REFRESH_DEFAULT));
            return APP_REDRAW_DONE;
        }
    }
    if (s_page == SETTINGS_PIN_ENTRY) return pin_setup_tick(ctx);
    if (s_page == SETTINGS_LOCK_STYLE && !(ctx->touch && ctx->touch->touched) && ui_lock_style_tick(ctx->now_ms)) {
        return APP_REDRAW_FULL;
    }

    // 阅读背景预览：解码上限 50 MB，只能放 tick 里做，render 只画缓存。
    // Reading-background preview: decoding reads up to 50 MB, so it runs here; render paints
    // the cache only.
    if (s_reading_bg_pending && s_page == SETTINGS_READING_BG_PREVIEW) {
        s_reading_bg_pending = false;
        s_reading_bg_ready = s_wallpaper_selected >= 0 && s_wallpaper_selected < s_wallpaper_count &&
            ui_reader_background_prepare(s_wallpapers[s_wallpaper_selected].path);
        if (!s_reading_bg_ready)
            snprintf(s_reading_bg_notice, sizeof(s_reading_bg_notice), "%s", "圖片無法解碼或記憶體不足，請換一張");
        return APP_REDRAW_PAGE;
    }

    if (s_page == SETTINGS_TEXT_EDIT) {
        if (ctx->consumed) return APP_REDRAW_NONE;
        bool held = !ctx->released && ctx->touch && ctx->touch->touched && ctx->touch->count == 1;
        if (ui_keyboard_hold_tick(held, ctx->touch ? ctx->touch->x : 0,
            ctx->touch ? ctx->touch->y : 0, ctx->now_ms) == UI_KEYBOARD_CHANGED) return profile_editor_paint(ctx, false);
        return ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms) == UI_KEYBOARD_CHANGED
            ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BLUETOOTH && !(ctx->touch && ctx->touch->touched) &&
        settings_bluetooth_tick(ctx->now_ms)) return APP_REDRAW_PAGE;
    if ((s_page == SETTINGS_NETWORK || s_page == SETTINGS_MAIN) &&
        !(ctx->touch && ctx->touch->touched) && settings_network_poll()) return APP_REDRAW_PAGE;

    (void)ctx;
#ifdef CONFIG_MOGO_CLOUDZAO_TRANSFER
    // 等待已离页的接收任务完成 TF 交还；不阻塞设置页轮询。
    // Defer restart until an exiting receiver returns TF handles without blocking settings ticks.
    if (s_boot_pending && !cloudzao_quiesce(0)) return APP_REDRAW_NONE;
#endif
    if (s_boot_pending) {
        s_boot_pending = false;
        pmu_selftest_prepare_powerdown();
        uint8_t req[2] = {0, 0};
        esp_err_t error = device_power_cmd(PMU_CMD_HOST_REQUEST_RESET, req, sizeof(req));
        if (error != ESP_OK) ESP_LOGW(TAG, "BOOT PMU notice: %s", esp_err_to_name(error));
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        // FORCE_DOWNLOAD_BOOT 是 S3 RTC 寄存器；S31 只能靠按住 BOOT 或 esptool 进入下载。
        // FORCE_DOWNLOAD_BOOT is an S3 RTC register; S31 enters download via BOOT/esptool.
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
#endif
        esp_restart(); return APP_REDRAW_NONE;
    }
    if (s_page != SETTINGS_TIME) {
        s_sync_pending = false;
        return APP_REDRAW_NONE;
    }
    app_time_sync_result_t result = {0};
    if (s_sync_pending) {
        s_sync_pending = false;
        result.error = app_time_service_start_manual();
        if (result.error == ESP_OK) {
            s_sync_waiting = true;
            return APP_REDRAW_NONE;
        }
    } else if (!s_sync_waiting || !app_time_service_take_manual_result(&result)) {
        return APP_REDRAW_NONE;
    }
    s_sync_waiting = false;
    esp_err_t err = result.error;
    bool network_time_received = result.network_time_received;
    const char *hint = memory_peak_hint(err);
    const char *notice = err == ESP_OK ? "網路對時完成，內部時鐘已接管" :
        network_time_received ? "已獲取網路時間，內部時鐘寫入失敗" :
        hint ? hint :
        err == ESP_ERR_NO_MEM ? "記憶體不足，請稍後重試" :
        err == ESP_ERR_NOT_FOUND ? "所選網路不可用，請檢查網路設定" :
        err == ESP_ERR_INVALID_STATE ? "網路尚未連線，請稍後再試" :
        err == ESP_ERR_TIMEOUT ? "網路對時超時，請檢查網路後重試" :
        "網路對時失敗，請檢查網路後重試";
    snprintf(s_notice, sizeof(s_notice), "%s", notice);
    return APP_REDRAW_PAGE;
}

static EpdRect s_input_area;
static bool s_input_layout, s_input_settle;
static app_redraw_t profile_editor_paint(app_ctx_t *ctx, bool field) {
    ui_keyboard_update_t update = ui_keyboard_update(ctx->fb, 560, (EpdRect){36, 243, 612, 82}, 28, false, NULL, field);
    s_input_area = update.area; s_input_layout = update.layout; s_input_settle = update.settle;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}
static app_redraw_t profile_editor_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(ev->x0, ev->y0, 560, ctx->now_ms) ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    bool feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        bool page = ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (page || feedback) ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return feedback ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (y < 160) {
        if (x < 160) { ui_keyboard_end(); s_page = s_editor_signature ? SETTINGS_MAIN : SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (x > 510) goto save_text;
    }
    if (ui_text_input_tap(&s_editor_input, (EpdRect){36, 243, 612, 82}, 28,
                          false, NULL, x, y)) return profile_editor_paint(ctx, true);
    ui_keyboard_result_t result = ui_keyboard_tap(x, y, 560, esp_timer_get_time() / 1000);
    if (result == UI_KEYBOARD_DONE) goto save_text;
    return result == UI_KEYBOARD_CHANGED ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
save_text:
    if (ui_keyboard_pending()) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "請先選擇候選字");
        return APP_REDRAW_PAGE;
    }
    if (s_editor_signature) app_settings_set_status_signature(s_editor);
    else if (s_editor[0]) app_settings_set_device_name(s_editor);
    else { snprintf(s_editor_notice, sizeof(s_editor_notice), "個人名稱不能為空"); return APP_REDRAW_PAGE; }
    const char *saved = s_editor_signature ? app_settings_status_signature() : app_settings_device_name();
    if (strcmp(saved, s_editor)) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "儲存失敗，請檢查設定儲存空間");
        return APP_REDRAW_PAGE;
    }
    ui_keyboard_end();
    s_page = s_editor_signature ? SETTINGS_MAIN : SETTINGS_PROFILE;
    return APP_REDRAW_PAGE;
}

// 越过阈值按段推进，一次手势只更新一次，松手不再播放惯性。
// Advance one discrete step after crossing the threshold, with one update per gesture and no release inertia.
static app_redraw_t scroll_gesture(const ui_gesture_event_t *ev, int *offset, int limit, bool *handled) {
    *handled = ev->type != UI_GESTURE_TAP || s_scroll_drag_consumed;
    if (ev->type == UI_GESTURE_PRESS) {
        s_scroll_drag_consumed = false;
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_TAP || ev->type == UI_GESTURE_CANCEL) {
        s_scroll_drag_consumed = false;
        return APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_MOVE && ev->type != UI_GESTURE_SWIPE_U && ev->type != UI_GESTURE_SWIPE_D)
        return APP_REDRAW_NONE;
    if (ev->y0 < (s_page == SETTINGS_MAIN ? 160 : 190) || ev->y0 >= UI_NAV_TOP)
        return APP_REDRAW_NONE;
    if (s_scroll_drag_consumed) {
        if (ev->type != UI_GESTURE_MOVE) s_scroll_drag_consumed = false;
        return APP_REDRAW_NONE;
    }
    int dy = (int)ev->y - ev->y0, dx = (int)ev->x - ev->x0;
    int distance = abs(dy);
    if (ev->type == UI_GESTURE_MOVE && (distance < 108 || distance <= abs(dx))) return APP_REDRAW_NONE;
    int step = distance < 204 ? 204 : distance > 450 ? 450 : distance;
    bool up = ev->type == UI_GESTURE_SWIPE_U || (ev->type == UI_GESTURE_MOVE && dy < 0);
    int next = *offset + (up ? step : -step);
    if (limit < 0) limit = 0;
    if (next < 0) next = 0;
    if (next > limit) next = limit;
    if (ev->type == UI_GESTURE_MOVE) s_scroll_drag_consumed = true;
    if (next == *offset) return APP_REDRAW_NONE;
    *offset = next;
    s_scroll_present_pending = true;
    return APP_REDRAW_AREA;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (s_page == SETTINGS_SCREEN_OFFSET) return screen_offset_gesture(ctx, ev);
    if (s_page == SETTINGS_QUICK_BAR) return quick_settings_gesture(ctx, ev);
    if (s_page == SETTINGS_LOCK_STYLE) {
        if (ev->type==UI_GESTURE_TAP) {
            int tab=ui_nav_hit(ev->x0,ev->y0);
            if(tab>=0){ui_lock_style_close();s_page=SETTINGS_MAIN;ui_nav_request(ctx,tab);return APP_REDRAW_PAGE;}
        }
        if (ev->type==UI_GESTURE_TAP && ev->y0<140 && ev->x0<170) {
            if (ui_lock_style_back()) return APP_REDRAW_PAGE;
            ui_lock_style_close();s_page=SETTINGS_MAIN;settings_prepare_profile();return APP_REDRAW_PAGE;
        }
        return ui_lock_style_gesture(ev)?APP_REDRAW_PAGE:APP_REDRAW_NONE;
    }
    // 快捷栏直达的返回只属于这次访问：回到设置主列表后恢复普通返回。
    // Quick-bar back targets belong to this visit only: once the main list
    // is shown again the ordinary back returns.
    if (s_ble_shortcut_back && s_page == SETTINGS_MAIN) s_ble_shortcut_back = false;
    if (s_haptics_shortcut_back && s_page == SETTINGS_MAIN) s_haptics_shortcut_back = false;
    if (s_page==SETTINGS_SYSTEM_FONT && s_font_sync_confirm) {
        if (ev->type!=UI_GESTURE_TAP) return APP_REDRAW_NONE;
        for (int i=0;i<2;++i) {
            if (!ui_rect_hit(system_font_sync_button(i),ev->x0,ev->y0)) continue;
            if (i) {
                app_settings_set_font_path(s_font_sync_path);
                if (strcmp(app_settings_font_path(),s_font_sync_path)) {
                    snprintf(s_notice,sizeof(s_notice),"儲存失敗，請重試");return APP_REDRAW_PAGE;
                }
            }
            s_notice[0]=0;s_font_sync_confirm=false;return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_PIN_ENTRY) {
        EpdRect dirty; ui_pin_result_t result = ui_pinpad_handle(&s_pinpad, ev, &dirty);
        if (result == UI_PIN_CANCEL) { pin_setup_close(); s_page = SETTINGS_LOCK_PIN; return APP_REDRAW_PAGE; }
        if (result == UI_PIN_COMPLETE) s_pin_pending = true;
        if (result == UI_PIN_NONE) return APP_REDRAW_NONE;
        ui_pinpad_paint_input(ctx->fb, &s_pinpad);
        guard_draw_result(ctx->hl, display_refresh_area(ctx->hl, DISPLAY_REFRESH_FEEDBACK, dirty, DISPLAY_REFRESH_DEFAULT));
        return APP_REDRAW_DONE;
    }
    if (s_page == SETTINGS_LOCK_PIN && ev->type == UI_GESTURE_TAP && lock_pin_available() &&
        (ui_rect_hit((EpdRect){36,406,612,70},ev->x0,ev->y0) ||
         (lock_pin_enabled() && ui_rect_hit((EpdRect){36,512,612,70},ev->x0,ev->y0)))) {
        if (ctx->now_ms < s_pin_retry_at) { snprintf(s_notice, sizeof(s_notice), "請等待後重試"); return APP_REDRAW_PAGE; }
        pin_setup_open(ctx, ev->y0 >= 512); return APP_REDRAW_PAGE;
    }

    if (s_page == SETTINGS_BOOT) {
        if (ev->type != UI_GESTURE_TAP || s_boot_pending) return APP_REDRAW_NONE;
        if (ev->y0 < 190 && ev->x0 < 170) { s_page = SETTINGS_MAIN; (void)wifi_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        if (ui_rect_hit((EpdRect){60, 438, 564, 78}, ev->x0, ev->y0)) { s_boot_pending = true; return APP_REDRAW_PAGE; }
        int tab = ui_nav_hit(ev->x0, ev->y0);
        if (tab >= 0) ui_nav_request(ctx, tab);
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_TEXT_EDIT) return profile_editor_gesture(ctx, ev);
    if (s_page == SETTINGS_BLUETOOTH) {
        if (ev->type == UI_GESTURE_TAP) {
            int tab = ui_nav_hit(ev->x0, ev->y0);
            if (tab >= 0) {
                settings_bluetooth_close(); s_page = SETTINGS_MAIN; s_ble_shortcut_back = false;
                ui_nav_request(ctx, tab); return APP_REDRAW_PAGE;
            }
            if (ev->x0 < 170 && ev->y0 < 140) {
                if (settings_bluetooth_back()) return APP_REDRAW_PAGE;
                settings_bluetooth_close();
                if (s_ble_shortcut_back) { s_ble_shortcut_back = false; ctx->request_return = true; return APP_REDRAW_NONE; }
                s_page = SETTINGS_MAIN; settings_prepare_profile(); return APP_REDRAW_PAGE;
            }
        }
        return settings_bluetooth_gesture(ev) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_MAIN) {
        bool handled;
        app_redraw_t redraw = scroll_gesture(ev, &s_main_scroll, SETTINGS_SCROLL_MAX, &handled);
        if (handled) return redraw;
    }

    if (s_page == SETTINGS_AVATAR && (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int pages = (s_wallpaper_count + 6) / 7;
        int old = s_wallpaper_page;
        if (ev->type == UI_GESTURE_SWIPE_U && s_wallpaper_page + 1 < pages) ++s_wallpaper_page;
        if (ev->type == UI_GESTURE_SWIPE_D && s_wallpaper_page > 0) --s_wallpaper_page;
        if (s_wallpaper_page == old) return APP_REDRAW_NONE;
        s_scroll_present_pending = true;
        return APP_REDRAW_AREA;
    }

    if (s_page == SETTINGS_READING_BG_PREVIEW && !s_reading_bg_confirm &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int next = s_wallpaper_selected + (ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
        if (next < 0 || next >= s_wallpaper_count) return APP_REDRAW_NONE;
        s_wallpaper_selected = next;
        s_wallpaper_page = next / 7;
        // 换图要重新解码：ready 只对上一张有效。
        // A different image needs a fresh decode; ready was only valid for the previous one.
        s_reading_bg_ready = false;
        s_reading_bg_pending = true;
        s_reading_bg_notice[0] = 0;
        return APP_REDRAW_PAGE;
    }
    if ((s_page == SETTINGS_SYSTEM_FONT || s_page == SETTINGS_READING_BG) &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int *page = s_page == SETTINGS_SYSTEM_FONT ? &s_font_page : &s_wallpaper_page;
        int count = s_page == SETTINGS_SYSTEM_FONT ? ttf_font_count() + 1 : s_wallpaper_count;
        int slots = s_page == SETTINGS_SYSTEM_FONT ? system_font_layout().slots : 7;
        int pages = count > 0 ? (count + slots - 1) / slots : 1;
        int old = *page;
        if (ev->type == UI_GESTURE_SWIPE_U && *page + 1 < pages) ++*page;
        if (ev->type == UI_GESTURE_SWIPE_D && *page > 0) --*page;
        if (*page == old) return APP_REDRAW_NONE;
        s_scroll_present_pending = true;
        return APP_REDRAW_AREA;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;

    if (s_page == SETTINGS_TIME_EDIT) {
        if (ev->y0 < 190) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
        if (ev->y0 >= 354 && ev->y0 < 597) {
            int field = (ev->x0 - 36) / 122;
            if (ev->x0 >= 36 && field >= 0 && field < 5 && (ev->x0 - 36) % 122 < 106) {
                if (ev->y0 < 416) time_adjust(field, 1);
                else if (ev->y0 >= 535) time_adjust(field, -1);
                else return APP_REDRAW_NONE;
                return APP_REDRAW_PAGE;
            }
        }
        if (ev->y0 >= 767 && ev->y0 < 845) { (void)time_save(); return APP_REDRAW_PAGE; }
        if (ev->y0 >= 874 && ev->y0 < 952) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_MEMORY) {
        const int tab = ui_nav_hit(ev->x0, ev->y0);
        if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
        const int y = ev->y0;
        if (y < 190) {
            if (s_memory_shortcut_back) {
                s_memory_shortcut_back = false;
                ctx->request_return = true;
                return APP_REDRAW_NONE;
            }
            s_page = SETTINGS_MAIN;
            (void)wifi_transfer_get_saved_wifi(s_main_wifi, &s_main_wifi_saved);
            settings_prepare_profile();
            return APP_REDRAW_PAGE;
        }
        if (y >= 882 && y < 958 && ev->x0 >= 36 && ev->x0 < 648) {
            if (mm_request_clean(MM_CLEAN_MEMORY_PAGE, NULL) == ESP_OK) {
                memory_inventory_snapshot(ctx->hl, &s_memory_snapshot);
                s_memory_present_pending = false;
                s_memory_dirty = false;
                s_memory_last_present_ms = ctx->now_ms;
            }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    int tab = ui_nav_hit(ev->x0, ev->y0);
    if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    int y = ev->y0;
    if (s_page == SETTINGS_PROFILE) {
        if (y < 190) { s_page = SETTINGS_MAIN; (void)wifi_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        if (y >= 266 && y < 350) { profile_editor_open(false); return APP_REDRAW_PAGE; }
        if (y >= 366 && y < 450) { wallpaper_scan(); s_page = SETTINGS_AVATAR; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_AVATAR) {
        if (y < 190) { s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (y >= 270 && y < 354) { app_settings_set_avatar_path(""); s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (y >= 374 && y < 1046) {
            int i = (y - 374) / 96, index = s_wallpaper_page * 7 + i;
            if (index >= 0 && index < s_wallpaper_count && (y - 374) % 96 < 80) {
                app_settings_set_avatar_path(s_wallpapers[index].path);
                s_page = SETTINGS_PROFILE;
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }

    if (s_page == SETTINGS_READING_BG_PREVIEW) {
        if (y < 190) { s_page = SETTINGS_READING_BG; return APP_REDRAW_PAGE; }
        if (s_reading_bg_confirm) {
            if (y >= 643 && y < 711 && ev->x0 >= 358 && ev->x0 < 590) {
                if (app_settings_set_book_background_path(s_wallpapers[s_wallpaper_selected].path))
                    snprintf(s_reading_bg_notice, sizeof(s_reading_bg_notice), "%s", "已設為閱讀背景，下次開啟書籍生效");
            }
            // 其余点按一律视为取消。
            // Any other tap cancels the pending background choice.
            s_reading_bg_confirm = false;
            return APP_REDRAW_PAGE;
        }
        if (s_reading_bg_ready && y >= 194 && y < 1032) {
            s_reading_bg_confirm = true;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_READING_BG) {
        if (y < 190) { s_page = SETTINGS_MAIN; (void)wifi_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        if (y >= 956 && y < 1032) {
            if (!app_settings_set_book_background_path("")) return APP_REDRAW_NONE;
            snprintf(s_reading_bg_notice, sizeof(s_reading_bg_notice), "%s", "已恢復白底");
            return APP_REDRAW_PAGE;
        }
        if (y >= 292 && y < 936) {
            int index = s_wallpaper_page * 7 + (y - 292) / 92;
            if (index >= s_wallpaper_count) return APP_REDRAW_NONE;
            s_wallpaper_selected = index;
            s_reading_bg_confirm = false;
            s_reading_bg_ready = false;
            s_reading_bg_pending = true;
            s_reading_bg_notice[0] = 0;
            s_page = SETTINGS_READING_BG_PREVIEW;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_SHELF_STYLE) {
        if (y < 190) { s_page = SETTINGS_MAIN; s_notice[0] = 0; (void)wifi_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        for (int i = 0; i < 3; ++i) {
            EpdRect card = shelf_style_card(i);
            if (!ui_rect_hit(card, ev->x0, y)) continue;
            const uint8_t style = shelf_style_id(i);
            if (style != 5 && ui_rect_hit(shelf_style_titles_row(card), ev->x0, y)) {
                const bool saved = app_settings_set_shelf_show_titles(style, !app_settings_shelf_show_titles(style));
                snprintf(s_notice, sizeof(s_notice), "%s", saved ? "" : "儲存失敗，請重試");
                return APP_REDRAW_PAGE;
            }
            app_settings_set_shelf_style(style);
            s_notice[0] = 0;
            s_page = SETTINGS_MAIN;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_HAPTIC && s_haptics_shortcut_back && y < 190) {
        // 快捷栏直达子页：左上角返回直接回到来源页；阅读正文由主循环按离开时
        // 的书与全屏状态重开。
        // A direct quick-bar sub-page visit sends the top-left back
        // to the origin page; the loop reopens the reading body with the book and
        // full-screen state captured on the way out.
        s_haptics_shortcut_back = false;
        ctx->request_return = true;
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_NETWORK && s_network_shortcut_back && y < 190) {
        s_network_shortcut_back = false;
        ctx->request_return = true;
        return APP_REDRAW_NONE;
    }

    if (s_page != SETTINGS_MAIN && y < 190) { s_page = SETTINGS_MAIN; (void)wifi_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_LOCK_PIN) return APP_REDRAW_NONE;
    if (s_page == SETTINGS_AUTO_LOCK) {
        const uint8_t values[] = {1, 5, 10, 0};
        for (int i = 0; i < 4; ++i) if (ui_rect_hit((EpdRect){36, 272 + i * 112, 612, 92}, ev->x0, y)) {
            app_settings_set_auto_lock_minutes(values[i]);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_HAPTIC) {
        if (!haptics_available()) return APP_REDRAW_NONE;
        if (ui_rect_hit((EpdRect){36, 272, 612, 126}, ev->x0, y)) {
            // 打开时震一下作预览；关闭不再震。
            // / Enabling previews one pulse; disabling stays silent.
            bool on = !app_settings_touch_haptics();
            app_settings_set_touch_haptics(on);
            if (on) haptics_pulse();
            return APP_REDRAW_PAGE;
        }
        for (int i = 0; i < 3; ++i) if (ui_rect_hit((EpdRect){36, 482 + i * 112, 612, 92}, ev->x0, y)) {
            app_settings_set_haptic_intensity((uint8_t)i);
            haptics_pulse();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_NETWORK) {
        network_status_t network;
        network_service_get_status(&network);
        if (network.cellular_capable && y >= NETWORK_TOGGLE_Y &&
            y < NETWORK_TOGGLE_Y + NETWORK_TOGGLE_H) {
            const esp_err_t err = network_service_set_enabled(NETWORK_BEARER_CELLULAR, !network.cellular_enabled);
            s_network_notice = memory_peak_hint(err);
            if (err != ESP_OK && !s_network_notice) {
                s_network_notice = network.cellular_enabled ? "4G 未能關閉，請稍後重試" :
                    err == ESP_ERR_NO_MEM ? "記憶體不足，無法啟動 4G" : "4G 未能啟動，請稍後重試";
            }
            return APP_REDRAW_PAGE;
        }
        if (network.cellular_capable && y >= NETWORK_ROUTE_ROW_Y &&
            y < NETWORK_ROUTE_ROW_Y + NETWORK_ROW_H) {
            network_route_t next = network.route == NETWORK_ROUTE_WIFI ? NETWORK_ROUTE_CELLULAR :
                network.route == NETWORK_ROUTE_CELLULAR ? NETWORK_ROUTE_AUTO : NETWORK_ROUTE_WIFI;
            if (next != NETWORK_ROUTE_CELLULAR || network.cellular_capable)
                (void)network_service_set_route(next, network.auto_priority);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BATT_CALIBRATION) return batt_calibration_touch(ev->x0, y);
    if (s_page == SETTINGS_CHARGE_CURRENT) return charge_current_touch(ev->x0, y);
    if (s_page == SETTINGS_POWER_LOG) {
        if (ui_rect_hit((EpdRect){36, 272, 612, 126}, ev->x0, y)) {
            app_settings_set_batt_log(!app_settings_batt_log());
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    if (s_page == SETTINGS_WIFI) {
        if (y >= 408 && y < 478) {
            extern const app_desc_t app_transfer;
            app_transfer_request_wifi_setup();
            ctx->request_app = &app_transfer;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_SYSTEM_FONT) {
        system_font_layout_t l=system_font_layout();
        if (ui_rect_hit(l.size,ev->x0,y)) {
            unsigned prior=app_settings_system_font_size();
            unsigned next=prior>=200?100:prior+10;
            app_settings_set_system_font_size(next);
            if (app_settings_system_font_size()!=next) {
                snprintf(s_notice,sizeof(s_notice),"儲存失敗，請重試");return APP_REDRAW_PAGE;
            }
            s_font_page=0;
            if (s_main_scroll>SETTINGS_SCROLL_MAX) s_main_scroll=SETTINGS_SCROLL_MAX;
            ui_text_set_system_scale(true);s_notice[0]=0;return APP_REDRAW_PAGE;
        }
        for (int slot=0;slot<l.slots;++slot) {
            if (!ui_rect_hit(system_font_item_rect(slot),ev->x0,y)) continue;
            int index=s_font_page*l.slots+slot;
            if (index>ttf_font_count()) return APP_REDRAW_NONE;
            const ttf_font_item_t *item=index?ttf_font_item(index-1):NULL;
            if (index && !item) return APP_REDRAW_NONE;
            snprintf(s_font_sync_path,sizeof(s_font_sync_path),"%s",item?item->path:"");
            app_settings_set_system_font_path(s_font_sync_path);
            if (strcmp(app_settings_system_font_path(),s_font_sync_path)) {
                snprintf(s_notice,sizeof(s_notice),"儲存失敗，請重試");return APP_REDRAW_PAGE;
            }
            app_font_activate_system();s_notice[0]=0;
            s_font_sync_confirm=strcmp(app_settings_font_path(),s_font_sync_path)!=0;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_MAIN_REFRESH) {
        for (int i = 0; i < MAIN_REFRESH_CHOICE_COUNT; ++i) {
            if (!ui_rect_hit((EpdRect){36, 298 + i * 158, 612, 132}, ev->x0, y)) continue;
            app_settings_set_main_refresh_mode(s_main_refresh_choices[i]);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_SYSTEM_SIZE) {
        for (int index = 0; index < 11; ++index) {
            EpdRect box = {36 + (index % 2) * 316, 298 + (index / 2) * 122, 296, 102};
            if (!ui_rect_hit(box, ev->x0, y)) continue;
            app_settings_set_system_font_size((uint8_t)(100 + index * 10));
            ui_text_set_system_scale(true);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_SYSTEM_CONTRAST) {
        if (y < 298 || y >= 878) return APP_REDRAW_NONE;
        int index = (y - 298) / 116;
        app_settings_set_system_contrast((uint8_t)(100 + index * 10));
        return APP_REDRAW_PAGE;
    }

    if (s_page == SETTINGS_TIME) {
        if (y >= TIME_ROW_AUTO_Y && y < TIME_ROW_AUTO_Y + TIME_ROW_HIT_H) {
            app_settings_set_auto_time_sync(!app_settings_auto_time_sync());
            s_notice[0] = 0;
            return APP_REDRAW_PAGE;
        }
        if (y >= TIME_ROW_WIFI_Y && y < TIME_ROW_WIFI_Y + TIME_ROW_HIT_H) {
            if (s_sync_pending || s_sync_waiting) return APP_REDRAW_NONE;
            s_notice[0] = 0; s_sync_pending = true;
            return APP_REDRAW_PAGE;
        }
        if (y >= TIME_ROW_MANUAL_Y && y < TIME_ROW_MANUAL_Y + TIME_ROW_HIT_H) {
            if (s_sync_pending || s_sync_waiting) return APP_REDRAW_NONE;
            time_open(); return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_POWER_SLEEP) {
        bool timed = app_settings_shutdown_minutes() != 0;
        if (timed) {
            for (unsigned i = 0; i < 3; ++i) {
                if (!ui_rect_hit(standby_duration_rect(i), ev->x0, y)) continue;
                app_settings_set_shutdown_style((app_shutdown_style_t)(APP_SHUTDOWN_LIGHT_10 + i));
                return APP_REDRAW_PAGE;
            }
        }
        for (int i = 0; i < 3; ++i) {
            EpdRect box = standby_mode_rect((unsigned)i, timed);
            if (!ui_rect_hit(box, ev->x0, y)) continue;
            if (i < 2)
                app_settings_set_shutdown_style((app_shutdown_style_t)(APP_SHUTDOWN_STAGED + i));
            else if (!timed)
                app_settings_set_shutdown_style(APP_SHUTDOWN_LIGHT_10);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_CONFIG) {
        if (y >= 561 && y < 644) {
            s_config_confirm = false;
            esp_err_t err = app_settings_backup_save();
            snprintf(s_notice, sizeof(s_notice), "%s",
                     err == ESP_OK ? "已儲存到 TF 卡根目錄" :
                     err == ESP_ERR_INVALID_STATE ? "未識別到 TF 卡，請插卡後重試" :
                     "儲存失敗，請檢查 TF 卡剩餘空間");
            return APP_REDRAW_PAGE;
        }
        if (y >= 681 && y < 764) {
            if (!s_config_confirm) {
                s_config_confirm = true;
                snprintf(s_notice, sizeof(s_notice), "恢復將覆蓋設定、合併 WiFi，請再次確認");
                return APP_REDRAW_PAGE;
            }
            s_config_confirm = false;
            esp_err_t err = app_settings_backup_restore();
            if (err == ESP_OK) {
                ttf_font_scan();
                app_font_activate_system();
                ui_text_set_system_scale(true);
                if (frontlight_ready())
                    (void)frontlight_apply(app_settings_frontlight_on(), app_settings_frontlight_brightness(),
                                           app_settings_frontlight_cct());
                // 阅读记录恢复后令首页和书架重新取进度、时长与排序。
                // Rebuild home and shelf caches after restoring reading records.
                book_store_notify_changed();
            }
            char saved_ssid[33] = {0};
            bool wifi_saved = false;
            if (err == ESP_OK)
                (void)wifi_transfer_get_saved_wifi(saved_ssid, &wifi_saved);
            snprintf(s_notice, sizeof(s_notice), "%s",
                     err == ESP_OK ? (wifi_saved ? "配置與閱讀記錄已恢復；WiFi 請重新連線" :
                                                   "配置與閱讀記錄已恢復") :
                     err == ESP_ERR_INVALID_STATE ? "未識別到 TF 卡，請插卡後重試" :
                     err == ESP_ERR_NOT_FOUND ? "未找到配置備份檔案" :
                     err == ESP_ERR_INVALID_RESPONSE ? "配置檔案損壞或版本不相容" :
                     err == ESP_ERR_INVALID_SIZE ? "WiFi 合併超過 8 組，請先遺忘部分網路" :
                     "恢復未完成，請重試");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (y<160 || y>=UI_NAV_TOP || ev->x0<36 || ev->x0>=648) return APP_REDRAW_NONE;
    y+=s_main_scroll;
    if (y>=180 && y<180+settings_profile_height()) { s_page=SETTINGS_PROFILE;return APP_REDRAW_PAGE; }
    int row;
    const int wireless_rows = settings_wireless_rows();
    if (y>=SETTINGS_WIRELESS_Y && y<SETTINGS_WIRELESS_Y+wireless_rows*SETTINGS_ROW_H) {
        row=(y-SETTINGS_WIRELESS_Y)/SETTINGS_ROW_H;
        if(row==0) {
            extern const app_desc_t app_transfer;app_transfer_request_wifi_setup();ctx->request_app=&app_transfer;return APP_REDRAW_NONE;
        }
        network_status_t network;
        network_service_get_status(&network);
        if(network.cellular_capable && row==1) {
            s_page=SETTINGS_NETWORK;s_network_notice=NULL;return APP_REDRAW_PAGE;
        }
        const int bluetooth_row = network.cellular_capable ? 2 : 1;
        if(row==bluetooth_row) {
            s_page=SETTINGS_BLUETOOTH;settings_bluetooth_open();return APP_REDRAW_PAGE;
        }
        s_page=SETTINGS_TIME;return APP_REDRAW_PAGE;
    }
    if(y>=SETTINGS_DISPLAY_Y && y<SETTINGS_DISPLAY_Y+SETTINGS_DISPLAY_ROWS*SETTINGS_ROW_H) {
        row=(y-SETTINGS_DISPLAY_Y)/SETTINGS_ROW_H;
        if(row==0) { ttf_font_scan();s_font_page=0;s_notice[0]=0;s_page=SETTINGS_SYSTEM_FONT; }
        else if(row==1)s_page=SETTINGS_SYSTEM_CONTRAST;
        else if(row==2)s_page=SETTINGS_MAIN_REFRESH;
        else if(row==3)app_settings_set_home_full_refresh(!app_settings_home_full_refresh());
        else if(row==4)s_page=SETTINGS_AUTO_LOCK;
        else if(row==5)s_page=SETTINGS_POWER_SLEEP;
        else {s_page=SETTINGS_SCREEN_OFFSET;s_notice[0]=0;}
        return APP_REDRAW_PAGE;
    }
    if(y>=SETTINGS_PERSONAL_Y && y<SETTINGS_PERSONAL_Y+SETTINGS_PERSONAL_ROWS*SETTINGS_ROW_H) {
        row=(y-SETTINGS_PERSONAL_Y)/SETTINGS_ROW_H;
        if(row==0) {s_page=SETTINGS_SHELF_STYLE;s_notice[0]=0;}
        else if(row==1){ui_lock_style_open(ctx->fb);s_page=SETTINGS_LOCK_STYLE;}
        else if(row==2){s_page=SETTINGS_LOCK_PIN;s_notice[0]=0;}
        else if(row==3){
            if(!haptics_available())return APP_REDRAW_NONE;
            s_page=SETTINGS_HAPTIC;
        }
        else if(row==4){
            reading_bg_scan();
            s_reading_bg_confirm=s_reading_bg_ready=s_reading_bg_pending=false;
            s_reading_bg_notice[0]=0;
            s_page=SETTINGS_READING_BG;
        }
        else if(row==5){app_components_open(ctx,false);return APP_REDRAW_NONE;}
        else if(row==6){s_page=SETTINGS_QUICK_BAR;s_quick_notice[0]=0;}
        return APP_REDRAW_PAGE;
    }
    if(y>=SETTINGS_MAINTENANCE_Y && y<SETTINGS_MAINTENANCE_Y+SETTINGS_MAINTENANCE_ROWS*SETTINGS_ROW_H) {
        row=(y-SETTINGS_MAINTENANCE_Y)/SETTINGS_ROW_H;
        if(row==0){s_page=SETTINGS_CONFIG;s_config_confirm=false;s_notice[0]=0;}
        else if(row==1){
            // 屏幕在刷新中途断电会停在半帧，常规差分刷新永远盖不掉那道条纹；点一下就整屏盲刷
            // 一遍，然后自己把当前页面（含底栏）推回面板。
            // A power cut mid-refresh leaves the panel half-driven, and an ordinary differential
            // refresh can never cover that stripe. One tap runs the blind full-screen pass, which
            // then pushes the current page -- navigation bar included -- back onto the panel.
            s_panel_repair_pending=true;
            snprintf(s_notice,sizeof(s_notice),"螢幕修復中…");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_PAGE;
    }
    if(y>=SETTINGS_POWER_Y && y<SETTINGS_POWER_Y+SETTINGS_POWER_ROWS*SETTINGS_ROW_H) {
        row=(y-SETTINGS_POWER_Y)/SETTINGS_ROW_H;
        if(row==0) {
            s_page=SETTINGS_BATT_CALIBRATION;
            s_batt_calibration_confirm=false;
            s_notice[0]=0;
            device_power_get_batt_calibration(&s_batt_calibration_status);
            s_batt_calibration_next_ms=ctx->now_ms+1000;
        }
        else if(row==1) s_page=SETTINGS_POWER_LOG;
        else if(row==2) { s_page=SETTINGS_CHARGE_CURRENT; s_notice[0]=0; }
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    s_batt_calibration_confirm = false;
    s_screen_offset_pressed = -1;
    if (s_ble_shortcut_back && s_page == SETTINGS_MAIN) s_ble_shortcut_back = false;
    if (s_haptics_shortcut_back && s_page == SETTINGS_MAIN) s_haptics_shortcut_back = false;
    if(s_font_sync_confirm) {
        s_font_sync_confirm=false;s_notice[0]=0;
        if(key!=1)return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_PIN_ENTRY) { pin_setup_close(); s_page = SETTINGS_LOCK_PIN; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_BLUETOOTH) {
        if (key != 1 && settings_bluetooth_back()) return APP_REDRAW_PAGE;
        settings_bluetooth_close();
        if (key != 1 && s_ble_shortcut_back) { s_ble_shortcut_back = false; ctx->request_return = true; return APP_REDRAW_NONE; }
    }
    if (s_page==SETTINGS_LOCK_STYLE) {
        if(key!=1 && ui_lock_style_back())return APP_REDRAW_PAGE;
        ui_lock_style_close();s_page=SETTINGS_MAIN;
        if(key==1){ui_nav_request(ctx,0);return APP_REDRAW_NONE;}
        settings_prepare_profile();return APP_REDRAW_PAGE;
    }
    if (key == 1) { ui_nav_request(ctx, 0); return APP_REDRAW_NONE; }
    if (s_page == SETTINGS_HAPTIC && s_haptics_shortcut_back) {
        s_haptics_shortcut_back = false;
        ctx->request_return = true;
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_MEMORY && s_memory_shortcut_back) {
        s_memory_shortcut_back = false;
        ctx->request_return = true;
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_MEMORY) {
        s_page = SETTINGS_MAIN;
        (void)wifi_transfer_get_saved_wifi(s_main_wifi, &s_main_wifi_saved);
        settings_prepare_profile();
        return APP_REDRAW_PAGE;
    }

    if (s_page == SETTINGS_READING_BG_PREVIEW) { s_reading_bg_confirm = false; s_page = SETTINGS_READING_BG; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_READING_BG) { s_page = SETTINGS_MAIN; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_AVATAR) { s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_TEXT_EDIT) { ui_keyboard_end(); s_page = s_editor_signature ? SETTINGS_MAIN : SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_PROFILE) { s_page = SETTINGS_MAIN; (void)wifi_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_TIME_EDIT) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
    if (s_page != SETTINGS_MAIN) { s_page = SETTINGS_MAIN; (void)wifi_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
    ui_nav_request(ctx, 0);
    return APP_REDRAW_NONE;
}
static void settings_exit(app_ctx_t *ctx) {
    s_batt_calibration_confirm = false;
    s_screen_offset_pressed = -1;
    s_screen_offset_retries = 0;
    ui_lock_style_close();

    // 释放预览内存；阅读器按当前路径读取TF成品，缓存失效时才重新解码。
    // Release preview RAM; the reader loads prepared SD art and decodes only stale/missing caches.
    ui_reader_background_release();
    s_reading_bg_confirm=s_reading_bg_ready=s_reading_bg_pending=false;s_reading_bg_notice[0]=0;
    s_font_sync_confirm=false;s_font_sync_path[0]=0;
    pin_setup_close();
    (void)ctx; ui_keyboard_end(); settings_bluetooth_close();
}
static void on_before_lock(app_ctx_t *ctx) {
    s_batt_calibration_confirm = false;
    s_screen_offset_pressed = -1;
    s_screen_offset_retries = 0;
    if(s_page==SETTINGS_LOCK_STYLE){ui_lock_style_close();s_page=SETTINGS_MAIN;}
    s_font_sync_confirm=false;s_font_sync_path[0]=0;
    // 锁屏唤醒后 tick 不会再补解码，预览页会停在白框：锁屏前退回主列表并释放缓存。
    // After waking from lock no tick re-runs the decode, so the preview page would sit on a
    // white frame: drop back to the main list and release the cache before locking.
    if (s_page == SETTINGS_READING_BG || s_page == SETTINGS_READING_BG_PREVIEW) {
        s_page = SETTINGS_MAIN;
        s_reading_bg_confirm=s_reading_bg_ready=s_reading_bg_pending=false;s_reading_bg_notice[0]=0;
        ui_reader_background_release();
    }
    if (s_page == SETTINGS_PIN_ENTRY) { pin_setup_close(); s_page = SETTINGS_LOCK_PIN; }
    (void)ctx; settings_bluetooth_close();
    (void)ble_pt_stop(2000);
}
static bool no_menu_handle(app_ctx_t *ctx) { (void)ctx; return false; }
static void settings_media_lost(app_ctx_t *ctx) {
    ui_lock_style_media_lost();
    // 拔卡后背景图片和列表都失效；退回主列表，避免停留在一个空预览。
    // Losing the card invalidates the image and the list; fall back to the main list instead
    // of staying on an empty preview.
    if (s_page == SETTINGS_READING_BG || s_page == SETTINGS_READING_BG_PREVIEW) {
        s_page = SETTINGS_MAIN;
        s_reading_bg_confirm=s_reading_bg_ready=s_reading_bg_pending=false;s_reading_bg_notice[0]=0;
        ui_reader_background_release();
    }
    (void)ctx; s_avatar_ready=s_avatar_pending=false; s_avatar_path[0]=0;
}
static void settings_media_ready(app_ctx_t *ctx) { (void)ctx; ui_lock_style_media_ready(); settings_prepare_profile(); }
static bool main_page_visible(app_ctx_t *ctx) { (void)ctx; return s_page == SETTINGS_MAIN; }
static bool settings_present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (s_page == SETTINGS_SCREEN_OFFSET && redraw == APP_REDRAW_PAGE) {
        s_screen_offset_retries = SCREEN_OFFSET_RETRIES;
        screen_offset_present(ctx);
        return true;
    }
    if (s_page == SETTINGS_PIN_ENTRY && (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL)) {
        display_main_transition_cancel(); render(ctx, ctx->fb);
        guard_draw_result(ctx->hl, display_refresh_area(ctx->hl, DISPLAY_REFRESH_DETAIL, ui_pinpad_full(), DISPLAY_REFRESH_DEFAULT));
        return true;
    }
    if (redraw == APP_REDRAW_AREA && s_page == SETTINGS_TEXT_EDIT) {
        if (s_input_settle) {
            guard_draw_result(ctx->hl, display_refresh_area(ctx->hl, DISPLAY_REFRESH_SETTLE, s_input_area, DISPLAY_REFRESH_DEFAULT));
            s_input_settle = false;
            return true;
        }
        guard_draw_result(ctx->hl, display_refresh_area(ctx->hl, s_input_layout ? DISPLAY_REFRESH_DETAIL : DISPLAY_REFRESH_FEEDBACK, s_input_area, DISPLAY_REFRESH_DEFAULT));
        return true;
    }
    if (redraw == APP_REDRAW_AREA && s_page == SETTINGS_MEMORY && s_memory_present_pending) {
        s_memory_present_pending = false;
        memory_render_content(ctx->fb);
        guard_draw_result(ctx->hl, display_refresh_area(ctx->hl, DISPLAY_REFRESH_DETAIL,
            display_rect_to_epd(memory_area_hint(ctx)), DISPLAY_REFRESH_DEFAULT));
        return true;
    }
    if (redraw == APP_REDRAW_AREA && s_scroll_present_pending &&
        (s_page == SETTINGS_MAIN ||
         s_page == SETTINGS_SHELF_STYLE || s_page == SETTINGS_SYSTEM_FONT ||
         s_page == SETTINGS_AVATAR ||
         s_page == SETTINGS_READING_BG)) {
        s_scroll_present_pending = false;
        render(ctx, ctx->fb);
        int top = s_page == SETTINGS_MAIN ? 160 : s_page == SETTINGS_SHELF_STYLE ? 242 : 190;
        EpdRect area = {0, top, UI_LOCK_WIDTH, UI_NAV_REFRESH_END - top};
        // 只驱动变化像素，滚动次数不触发黑白清屏；题头与底栏保持稳定。
        // 脏区向外扩 1 行/1 字节，让差分跳过的白底邻像素重新走 GL16 白推，擦掉旧分隔线残影。
        // Drive changed pixels only; scroll counts never trigger a wipe, and header/nav stay stable.
        // Dilate the dirty masks by one row/byte so skipped white neighbours run the GL16
        // white tick again and old divider residue is erased.
        guard_draw_result(ctx->hl, display_refresh_area(ctx->hl, DISPLAY_REFRESH_MAIN_SCROLL, area, DISPLAY_REFRESH_DEFAULT));
        return true;
    }
    return false;
}

const app_desc_t app_device_settings = {
    .title = "設定 Settings", .detail = "顯示、連線與裝置", .enter_full = false,
    .owns_keys = true, .menu_handle_enabled = no_menu_handle,
    .main_page_visible = main_page_visible,
    .on_enter = on_enter, .on_exit = settings_exit, .on_before_lock = on_before_lock, .render = render, .on_tick = on_tick, .on_gesture = on_gesture, .on_key = on_key,
    .gesture_contact_allowed = settings_gesture_contact_allowed,
    .present = settings_present,
    .on_media_lost=settings_media_lost, .on_media_ready=settings_media_ready,
};
