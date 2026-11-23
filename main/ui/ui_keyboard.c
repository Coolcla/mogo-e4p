/* SPDX-License-Identifier: Apache-2.0
 * 中文：注音、QWERTY、數字及符號共用互動；鍵盤常亮，不做閃爍刷新。
 * English: Shared Zhuyin, QWERTY, numbers and symbols; steady keys without timed screen flashing.
 * 用户修订：普通输入只画变化区域，布局切换才重画键盘，不累计整页清屏次数。
 * User revision: paint changed input regions; repaint keys only on layout switches, without counting whole-page cleanup.
 * 使用注音組字與離線詞語候選；英文字母只由 QWERTY 直接輸入。
 * Compose Zhuyin with offline word candidates; QWERTY enters English letters directly.
 * 用户修订：删除键长按连续删除；抬手、移开、多点或输入会话结束立即取消，仅局部快刷。
 * 用户修订：按键按下加深、下沉两像素，松开复原，只刷该键；同音字可继续翻页。
 * User revision: darken/inset a pressed key, restore on release with local updates, and page through all homophones.
 * User revision: held backspace repeats; release, movement, multitouch or session exit cancels immediately, with local fast updates only.
 * 用户修订：键盘与候选使用白底黑字；候选变化三次、停手400ms后仅整理词栏，不打断连打、不全屏黑闪。
 * User revision: white keys/candidates with black text; settle the word strip after three changes and a 400ms pause, without interrupting typing or flashing the whole screen.
 */
#include "ui_keyboard.h"
#include "ui_ime.h"
#include <ui_kit.h>
#include "ttf_font.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static ui_text_edit_t *s_edit;
static ui_ime_result_t *s_candidates;
static char s_raw[UI_IME_RAW_MAX + 1], s_preferred[9], s_notice[80];
static bool s_ascii, s_chinese, s_nine, s_upper;
static unsigned s_panel;
static size_t s_page, s_window;
static bool s_pressed, s_probe, s_keys_only;
static int s_probe_x, s_probe_y;
static EpdRect s_pressed_area, s_feedback_area, s_key_clip;
static unsigned s_chars_since_settle;
enum { DIRTY_FIELD = 1, DIRTY_WORDS = 2, DIRTY_SPACE = 8, DIRTY_LAYOUT = 16, DIRTY_FEEDBACK = 32, DIRTY_WORDS_SETTLE = 64 };
static unsigned s_dirty, s_candidates_revision;
enum { WORDS_SETTLE_CHANGES = 3, WORDS_SETTLE_IDLE_MS = 400, CANDIDATE_PX = 32 };
static unsigned s_word_updates;
static int64_t s_words_idle_since = -1;
static bool s_delete_hold;
static EpdRect s_delete_area;
static int s_delete_top;
static int64_t s_delete_next_ms, s_delete_last_ms;
static uint32_t stamp(const char *text) {
    uint32_t value = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) value = (value ^ *p) * 16777619u;
    return value;
}
static const char *const symbols[] = {"，","。","？","！","：","；","“","”","（","）","、","…","—","·","-","_","/","@","#","&","+","=","%","\\","*","$","<",">","[","]"};
static const char *const ascii_symbols[] = {"!","@","#","$","%","^","&","*","(",")","-","_","+","=","/","\\",":",";","\"","'",".",",","?","<",">","[","]","{","}","|","`","~"};
static const char *const zhuyin_keys[4][11] = {
    {"ㄅ","ㄉ","ˇ","ˋ","ㄓ","ˊ","˙","ㄚ","ㄞ","ㄢ","ㄦ"},
    {"ㄆ","ㄊ","ㄍ","ㄐ","ㄔ","ㄗ","ㄧ","ㄛ","ㄟ","ㄣ"},
    {"ㄇ","ㄋ","ㄎ","ㄑ","ㄕ","ㄘ","ㄨ","ㄜ","ㄠ","ㄤ"},
    {"ㄈ","ㄌ","ㄏ","ㄒ","ㄖ","ㄙ","ㄩ","ㄝ","ㄡ","ㄥ"},
};
static const unsigned zhuyin_key_count[4] = {11,10,10,10};
typedef enum { ZHU_INITIAL, ZHU_MEDIAL, ZHU_FINAL, ZHU_TONE } zhu_kind_t;
typedef struct { const char *symbol, *roman; zhu_kind_t kind; } zhu_unit_t;
static const zhu_unit_t zhuyin_map[] = {
    {"ㄅ","b",ZHU_INITIAL},{"ㄆ","p",ZHU_INITIAL},{"ㄇ","m",ZHU_INITIAL},{"ㄈ","f",ZHU_INITIAL},
    {"ㄉ","d",ZHU_INITIAL},{"ㄊ","t",ZHU_INITIAL},{"ㄋ","n",ZHU_INITIAL},{"ㄌ","l",ZHU_INITIAL},
    {"ㄍ","g",ZHU_INITIAL},{"ㄎ","k",ZHU_INITIAL},{"ㄏ","h",ZHU_INITIAL},{"ㄐ","j",ZHU_INITIAL},
    {"ㄑ","q",ZHU_INITIAL},{"ㄒ","x",ZHU_INITIAL},{"ㄓ","zh",ZHU_INITIAL},{"ㄔ","ch",ZHU_INITIAL},
    {"ㄕ","sh",ZHU_INITIAL},{"ㄖ","r",ZHU_INITIAL},{"ㄗ","z",ZHU_INITIAL},{"ㄘ","c",ZHU_INITIAL},
    {"ㄙ","s",ZHU_INITIAL},{"ㄧ","i",ZHU_MEDIAL},{"ㄨ","u",ZHU_MEDIAL},{"ㄩ","v",ZHU_MEDIAL},
    {"ㄚ","a",ZHU_FINAL},{"ㄛ","o",ZHU_FINAL},{"ㄜ","e",ZHU_FINAL},{"ㄝ","e",ZHU_FINAL},
    {"ㄞ","ai",ZHU_FINAL},{"ㄟ","ei",ZHU_FINAL},{"ㄠ","ao",ZHU_FINAL},{"ㄡ","ou",ZHU_FINAL},
    {"ㄢ","an",ZHU_FINAL},{"ㄣ","en",ZHU_FINAL},{"ㄤ","ang",ZHU_FINAL},{"ㄥ","eng",ZHU_FINAL},
    {"ㄦ","er",ZHU_FINAL},{"˙","",ZHU_TONE},{"ˊ","",ZHU_TONE},{"ˇ","",ZHU_TONE},
    {"ˋ","",ZHU_TONE},{"ˉ","",ZHU_TONE},
};
static EpdRect rect(int x, int y, int w, int h, int top) { return (EpdRect){x, y + top, w, h}; }
static EpdRect number_key_rect(unsigned col, int top) { return rect(24 + (int)col * 64, 164, 56, 44, top); }
static EpdRect bottom_backspace_rect(int top) { return rect(456, 430, 72, 72, top); }
static EpdRect bottom_space_rect(int top) { return rect(188, 430, 156, 72, top); }
static EpdRect input_method_rect(int top) { return rect(360, 430, 88, 72, top); }
static int letter_row_y(unsigned row) { return 220 + (int)row * 64; }

static const zhu_unit_t *zhuyin_unit(const char *text, size_t *bytes) {
    for (size_t i = 0; i < sizeof(zhuyin_map) / sizeof(zhuyin_map[0]); ++i) {
        size_t n = strlen(zhuyin_map[i].symbol);
        if (!strncmp(text, zhuyin_map[i].symbol, n)) { *bytes = n; return &zhuyin_map[i]; }
    }
    return NULL;
}

static bool compose_zhuyin_syllable(const char *initial, const char *medial, const char *final,
                                     char *out, size_t capacity) {
    char body[16] = "";
    const bool jqx = initial[0] && (initial[0]=='j'||initial[0]=='q'||initial[0]=='x');
    if (medial[0] && final[0]) {
        const char *combined = NULL;
        if (!strcmp(medial,"i")) {
            if (!strcmp(final,"e")) combined="ie";
            else if (!strcmp(final,"ao")) combined="iao";
            else if (!strcmp(final,"ou")) combined="iu";
            else if (!strcmp(final,"an")) combined="ian";
            else if (!strcmp(final,"en")) combined="in";
            else if (!strcmp(final,"ang")) combined="iang";
            else if (!strcmp(final,"eng")) combined="ing";
            else if (!strcmp(final,"a")) combined="ia";
        } else if (!strcmp(medial,"u")) {
            if (!strcmp(final,"eng")) combined="ong";
            else if (!strcmp(final,"ei")) combined="ui";
            else if (!strcmp(final,"en")) combined="un";
            else if (!strcmp(final,"a")) combined="ua";
            else if (!strcmp(final,"o")) combined="uo";
            else if (!strcmp(final,"ai")) combined="uai";
            else if (!strcmp(final,"an")) combined="uan";
            else if (!strcmp(final,"ang")) combined="uang";
        } else if (!strcmp(medial,"v")) {
            if (!strcmp(final,"eng")) combined="iong";
            else if (!strcmp(final,"e")) combined="ue";
            else if (!strcmp(final,"an")) combined=jqx?"uan":"van";
            else if (!strcmp(final,"en")) combined=jqx?"un":"vn";
        }
        if (combined) snprintf(body,sizeof(body),"%s",combined);
        else snprintf(body,sizeof(body),"%s%s",medial,final);
        if (!initial[0]) {
            const char *standalone=NULL;
            if(!strcmp(medial,"i")) {
                if(!strcmp(final,"a"))standalone="ya";else if(!strcmp(final,"e"))standalone="ye";
                else if(!strcmp(final,"ao"))standalone="yao";else if(!strcmp(final,"ou"))standalone="you";
                else if(!strcmp(final,"an"))standalone="yan";else if(!strcmp(final,"en"))standalone="yin";
                else if(!strcmp(final,"ang"))standalone="yang";else if(!strcmp(final,"eng"))standalone="ying";
            } else if(!strcmp(medial,"u")) {
                if(!strcmp(final,"a"))standalone="wa";else if(!strcmp(final,"o"))standalone="wo";
                else if(!strcmp(final,"ai"))standalone="wai";else if(!strcmp(final,"ei"))standalone="wei";
                else if(!strcmp(final,"an"))standalone="wan";else if(!strcmp(final,"en"))standalone="wen";
                else if(!strcmp(final,"ang"))standalone="wang";else if(!strcmp(final,"eng"))standalone="weng";
            } else if(!strcmp(medial,"v")) {
                if(!strcmp(final,"e"))standalone="yue";else if(!strcmp(final,"an"))standalone="yuan";
                else if(!strcmp(final,"en"))standalone="yun";else if(!strcmp(final,"eng"))standalone="yong";
            }
            if (standalone) {
                if (snprintf(out,capacity,"%s",standalone) >= (int)capacity) return false;
                return out[0] != 0;
            }
        }
    } else if (medial[0]) {
        if (!initial[0]) snprintf(body,sizeof(body),"%s",!strcmp(medial,"i")?"yi":!strcmp(medial,"u")?"wu":"yu");
        else if (!strcmp(medial,"v") && (initial[0]=='j'||initial[0]=='q'||initial[0]=='x')) snprintf(body,sizeof(body),"u");
        else snprintf(body,sizeof(body),"%s",medial);
    } else snprintf(body,sizeof(body),"%s",final);
    if (snprintf(out,capacity,"%s%s",initial,body) >= (int)capacity) return false;
    return out[0] != 0;
}

static bool zhuyin_to_pinyin(const char *raw, char *out, size_t capacity,
                             size_t raw_ends[UI_IME_RAW_MAX + 1],
                             size_t roman_ends[UI_IME_RAW_MAX + 1], size_t *syllable_count) {
    char initial[4]="", medial[4]="", final[5]="";
    size_t source=0, used=0, count=0;
    out[0]=0;
    while (raw[source]) {
        size_t bytes=0; const zhu_unit_t *unit=zhuyin_unit(raw+source,&bytes);
        if (!unit) return false;
        source += bytes;
        if (unit->kind==ZHU_TONE) {
            if (initial[0]||medial[0]||final[0]) {
                char syllable[16];
                if (!compose_zhuyin_syllable(initial,medial,final,syllable,sizeof(syllable))) return false;
                size_t n=strlen(syllable); if(used+n>=capacity)return false;
                memcpy(out+used,syllable,n); used+=n; out[used]=0; raw_ends[count]=source; roman_ends[count++]=used;
                initial[0]=medial[0]=final[0]=0;
            }
            continue;
        }
        if (unit->kind==ZHU_INITIAL && (initial[0]||medial[0]||final[0])) {
            char syllable[16];
            if (!compose_zhuyin_syllable(initial,medial,final,syllable,sizeof(syllable))) return false;
            size_t n=strlen(syllable); if(used+n>=capacity)return false;
            memcpy(out+used,syllable,n); used+=n; out[used]=0; raw_ends[count]=source-bytes; roman_ends[count++]=used;
            initial[0]=medial[0]=final[0]=0;
        } else if (unit->kind==ZHU_MEDIAL && (medial[0]||final[0])) {
            char syllable[16];
            if (!compose_zhuyin_syllable(initial,medial,final,syllable,sizeof(syllable))) return false;
            size_t n=strlen(syllable); if(used+n>=capacity)return false;
            memcpy(out+used,syllable,n); used+=n; out[used]=0; raw_ends[count]=source-bytes; roman_ends[count++]=used;
            initial[0]=medial[0]=final[0]=0;
        } else if (unit->kind==ZHU_FINAL && final[0]) {
            char syllable[16];
            if (!compose_zhuyin_syllable(initial,medial,final,syllable,sizeof(syllable))) return false;
            size_t n=strlen(syllable); if(used+n>=capacity)return false;
            memcpy(out+used,syllable,n); used+=n; out[used]=0; raw_ends[count]=source-bytes; roman_ends[count++]=used;
            initial[0]=medial[0]=final[0]=0;
        }
        char *target=unit->kind==ZHU_INITIAL?initial:unit->kind==ZHU_MEDIAL?medial:final;
        snprintf(target,unit->kind==ZHU_FINAL?sizeof(final):sizeof(initial),"%s",unit->roman);
    }
    if (initial[0]||medial[0]||final[0]) {
        char syllable[16];
        if (!compose_zhuyin_syllable(initial,medial,final,syllable,sizeof(syllable))) return false;
        size_t n=strlen(syllable); if(used+n>=capacity)return false;
        memcpy(out+used,syllable,n); used+=n; out[used]=0; raw_ends[count]=source; roman_ends[count++]=used;
    } else if (count && source>raw_ends[count-1]) raw_ends[count-1]=source;
    if (syllable_count) *syllable_count=count;
    return true;
}

static bool load_candidates(size_t start) {
    if (!s_candidates || !s_edit || !s_chinese) return false;
    char before[129]; size_t n = s_edit->cursor;
    if (n >= sizeof(before)) n = sizeof(before) - 1;
    memcpy(before, s_edit->text, n); before[n] = 0;
    char roman[UI_IME_RAW_MAX + 1]; size_t ends[UI_IME_RAW_MAX + 1], roman_ends[UI_IME_RAW_MAX + 1], count=0;
    if (!zhuyin_to_pinyin(s_raw,roman,sizeof(roman),ends,roman_ends,&count)) return false;
    (void)count;
    return ui_ime_candidates_page(roman, false, s_preferred, before, start, s_candidates);
}
static void refresh(void) {
    s_page = s_window = 0;
    if (!s_candidates || !s_edit || !s_chinese) return;
    ++s_candidates_revision;
    if (!load_candidates(0)) snprintf(s_notice, sizeof(s_notice), "注音無法組字，請刪除後重試");
}
void ui_keyboard_end(void) {
    ui_keyboard_hold_cancel();
    s_pressed = s_probe = s_keys_only = false; s_feedback_area = (EpdRect){0};
    free(s_candidates); s_candidates = NULL; s_edit = NULL; ui_ime_release(); s_dirty = 0;
    s_word_updates = 0; s_words_idle_since = -1;
    memset(s_raw, 0, sizeof(s_raw)); memset(s_preferred, 0, sizeof(s_preferred));
    memset(s_notice, 0, sizeof(s_notice)); s_page = 0;
    s_chars_since_settle = 0;
}
void ui_keyboard_begin(ui_text_edit_t *edit, bool ascii_only) {
    ui_keyboard_end(); s_edit = edit; s_ascii = ascii_only;
    s_chinese = !ascii_only; s_nine = !ascii_only; s_upper = false; s_panel = 0;
    if (!ascii_only) s_candidates = heap_caps_malloc(sizeof(*s_candidates), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ascii_only && !s_candidates) { s_chinese = s_nine = false; snprintf(s_notice, sizeof(s_notice), "記憶體不足，暫用英文鍵盤"); }
    refresh();
}
bool ui_keyboard_pending(void) { return s_raw[0] != 0; }
static bool insert(const char *text) {
    if (s_edit && ui_text_edit_insert(s_edit, text)) {
        for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
            if ((*p & 0xc0) != 0x80) ++s_chars_since_settle;
        s_notice[0] = 0; return true;
    }
    snprintf(s_notice, sizeof(s_notice), "文字已達到長度上限"); return false;
}
static bool commit(size_t index) {
    if (!s_candidates || index >= s_candidates->count) return false;
    const ui_ime_candidate_t *item = &s_candidates->items[index];
    if (!insert(item->text)) return false;
    size_t consume = item->consume, length = strlen(s_raw), raw_consume=consume;
    if (s_nine && consume) {
        char roman[UI_IME_RAW_MAX + 1]; size_t raw_ends[UI_IME_RAW_MAX + 1];
        size_t roman_ends[UI_IME_RAW_MAX + 1], count=0;
        if (!zhuyin_to_pinyin(s_raw,roman,sizeof(roman),raw_ends,roman_ends,&count)) return false;
        raw_consume=0;
        for(size_t i=0;i<count;++i) if(roman_ends[i]==consume) { raw_consume=raw_ends[i]; break; }
        if (!raw_consume) return false;
    }
    if (raw_consume > length) return false;
    memmove(s_raw, s_raw + raw_consume, length - raw_consume + 1);
    s_preferred[0] = 0; refresh(); return true;
}
static bool same_rect(EpdRect a, EpdRect b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height; }
static bool intersects(EpdRect a, EpdRect b) { return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height; }
static void key_paint(uint8_t *fb, EpdRect r, const char *text, int px, bool dark, bool utility) {
    if (s_probe) { if (ui_rect_hit(r, s_probe_x, s_probe_y)) s_key_clip = r; return; }
    if (s_keys_only && !intersects(r, s_key_clip)) return;
    bool pressed = s_pressed && same_rect(r, s_pressed_area);
    bool inverse = dark != pressed;
    ui_fill_round_rect(fb, r, 12, inverse ? UI_GRAY_BLACK : UI_GRAY_WHITE);
    ui_draw_round_rect(fb, r, 12, UI_GRAY_BLACK);
    if (utility) ui_draw_round_rect(fb, (EpdRect){r.x+1,r.y+1,r.width-2,r.height-2}, 11, UI_GRAY_BLACK);
    if (!strcmp(text, "⌫")) {
        int cx = r.x + r.width / 2, cy = r.y + r.height / 2 + (pressed ? 2 : 0);
        for (int d = 0; d < 2; ++d) {
            uint8_t ink = inverse ? UI_GRAY_WHITE : UI_GRAY_BLACK;
            epd_draw_line(cx - 17, cy, cx - 7, cy - 11 + d, ink, fb);
            epd_draw_line(cx - 17, cy, cx - 7, cy + 11 - d, ink, fb);
            epd_draw_line(cx - 7, cy - 11 + d, cx + 17, cy - 11 + d, ink, fb);
            epd_draw_line(cx - 7, cy + 11 - d, cx + 17, cy + 11 - d, ink, fb);
            epd_draw_line(cx + 17 - d, cy - 11, cx + 17 - d, cy + 11, ink, fb);
            epd_draw_line(cx - 2, cy - 5 + d, cx + 8, cy + 5 + d, ink, fb);
            epd_draw_line(cx - 2, cy + 5 + d, cx + 8, cy - 5 + d, ink, fb);
        }
    } else {
        const int cx = r.x + r.width / 2;
        const int cy = r.y + r.height / 2 + (pressed ? 2 : 0);
        const int old_weight = ttf_get_weight();
        const int old_stroke = ttf_get_stroke_adjust_fine();
        ttf_set_weight(500);
        int bold_stroke = old_stroke + 16;
        if (bold_stroke > 32) bold_stroke = 32;
        ttf_set_stroke_adjust_fine_scoped(bold_stroke);
        if (inverse) ui_text_fixed_vc(fb, cx, cy, px, text, EPD_DRAW_ALIGN_CENTER, true);
        else ui_text_title_bold_vc(fb, cx, cy, px, text, text, EPD_DRAW_ALIGN_CENTER);
        ttf_set_stroke_adjust_fine_scoped(old_stroke);
        ttf_set_weight(old_weight);
    }
}
static size_t visible(size_t start, EpdRect boxes[5], int top) {
    if (!s_candidates || !s_chinese) return 0;
    int left = 78, limit = 606; size_t count = 0;
    while (start + count < s_candidates->count && count < 5) {
        const char *text = s_candidates->items[start + count].text;
        int width = ui_text_fixed_width_px(CANDIDATE_PX, text) + 24;
        if (width < 78) width = 78;
        if (width > limit - 78) width = limit - 78;
        if (left + width > limit) break;
        boxes[count] = rect(left, 104, width - 6, 56, top);
        left += width; ++count;
    }
    return count;
}
bool ui_keyboard_page(int direction) {
    if (!s_candidates) return false;
    EpdRect boxes[5]; size_t count = visible(s_page, boxes, 0);
    if (direction > 0 && count) {
        if (s_page + count < s_candidates->count) s_page += count;
        else if (s_candidates->has_more) {
            size_t next = s_window + s_candidates->count;
            if (!load_candidates(next)) return false;
            s_window = next; s_page = 0;
        } else return false;
        s_dirty |= DIRTY_WORDS; return true;
    }
    if (direction < 0 && (s_page || s_window)) {
        if (!s_page) {
            size_t previous = s_window >= UI_IME_CANDIDATES ? s_window - UI_IME_CANDIDATES : 0;
            if (!load_candidates(previous)) return false;
            s_window = previous; s_page = s_candidates->count;
        }
        size_t begin = 0, previous = 0;
        while (begin < s_page) { previous = begin; size_t n = visible(begin, boxes, 0); if (!n) break; begin += n; }
        s_page = previous; s_dirty |= DIRTY_WORDS; return true;
    }
    return false;
}
static void composition_text(char out[UI_IME_RAW_MAX * 7 + 1]) {
    snprintf(out, UI_IME_RAW_MAX * 7 + 1, "%s", s_raw);
}
static void paint_words(uint8_t *fb, int top) {
    if (!s_probe && !s_keys_only && s_notice[0]) ui_text_fixed_vc(fb, 28, top + 75, 22, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    else if (!s_probe && !s_keys_only && s_raw[0]) {
        char text[UI_IME_RAW_MAX * 7 + 1]; composition_text(text);
        ui_text_input_composition(fb, text, rect(28, 57, 552, 35, top), 25);
    }
    else if (!s_probe && !s_keys_only && !s_ascii) ui_text_fixed_vc(fb, 28, top + 75, 22, s_chinese ? "注音組字，選擇詞語" : "英文鍵盤", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect boxes[5]; size_t count = visible(s_page, boxes, top);
    for (size_t i = 0; i < count; ++i) {
        char text[UI_IME_TEXT_MAX]; snprintf(text, sizeof(text), "%s", s_candidates->items[s_page + i].text);
        while (strlen(text) && ui_text_fixed_width_px(CANDIDATE_PX, text) > boxes[i].width - 12) {
            size_t n = strlen(text) - 1; while (n && ((unsigned char)text[n] & 192) == 128) --n; text[n] = 0;
        }
        key_paint(fb, boxes[i], text, CANDIDATE_PX, false, i == 0);
    }
    key_paint(fb, rect(24, 104, 44, 56, top), "‹", 30, false, true);
    key_paint(fb, rect(616, 104, 44, 56, top), "›", 30, false, true);
}
static EpdRect zhuyin_key_rect(unsigned row, unsigned col, int top) {
    const int width = row == 0 ? 54 : 56, gap = row == 0 ? 4 : 8;
    const int total = (int)zhuyin_key_count[row] * width + ((int)zhuyin_key_count[row] - 1) * gap;
    const int left = (684 - total) / 2;
    return rect(left + (int)col * (width + gap), 220 + (int)row * 48, width, 44, top);
}
static void paint_zhuyin(uint8_t *fb, int top) {
    for (unsigned row=0;row<4;++row) for (unsigned col=0;col<zhuyin_key_count[row];++col)
        key_paint(fb,zhuyin_key_rect(row,col,top),zhuyin_keys[row][col],25,false,false);
}
void ui_keyboard_draw(uint8_t *fb, int top) {
    if (!s_probe && !s_keys_only) {
    epd_fill_rect(rect(16, -12, 652, 534, top), UI_GRAY_WHITE, fb);
    }
    if (s_panel != 1) {
        static const char digits[] = "1234567890";
        for (unsigned col = 0; col < 10; ++col) {
            char label[2] = {digits[col], 0};
            key_paint(fb, number_key_rect(col, top), label, 26, false, false);
        }
    }
    paint_words(fb, top);
    if (s_panel || !s_nine) {
        static const char *const rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        for (int row = 0; row < 3; ++row) {
            int len = s_panel ? 10 : (int)strlen(rows[row]);
            int left = s_panel || !row ? 24 : row == 1 ? 55 : 117;
            for (int col = 0; col < len; ++col) {
                char letter[2] = {0}; const char *label;
                if (s_panel == 1) { letter[0] = row == 0 ? "1234567890"[col] : ascii_symbols[(row - 1) * 10 + col][0]; label = letter; }
                else if (s_panel >= 2) label = (s_chinese && !s_ascii && s_panel != 3 ? symbols : ascii_symbols)[s_panel == 3 && row == 0 && col < 6 ? (const int[]){30,31,28,29,26,27}[col] : row * 10 + col];
                else { letter[0] = s_upper ? rows[row][col] - 32 : rows[row][col]; label = letter; }
                key_paint(fb, rect(left + col * 64, letter_row_y((unsigned)row), 56, 56, top), label, 28, false, false);
            }
        }
        if (s_panel) {
            key_paint(fb, rect(600, letter_row_y(2), 56, 56, top), "⌫", 28, false, true);
        } else {
            key_paint(fb, rect(24, letter_row_y(2), 80, 56, top), s_upper ? "大寫" : "小寫", 26, s_upper, true);
            key_paint(fb, rect(578, letter_row_y(2), 82, 56, top), "⌫", 31, false, true);
        }
    } else paint_zhuyin(fb, top);
    key_paint(fb, rect(24, 430, 72, 72, top), s_panel == 1 ? "ABC" : "123", 24, false, true);
    key_paint(fb, rect(104, 430, 76, 72, top), s_panel == 2 ? "更多" : "符號", 23, false, true);
    key_paint(fb, bottom_space_rect(top), s_raw[0] ? "選擇" : "空格", 25, false, false);
    key_paint(fb, input_method_rect(top), "中 / En", 18, false, true);
    key_paint(fb, bottom_backspace_rect(top), "⌫", 29, false, true);
    key_paint(fb, rect(536, 430, 124, 72, top), "確定", 27, true, false);
}
static void backspace(void) {
    size_t n = strlen(s_raw);
    if (n) {
        do { --n; } while (n && ((unsigned char)s_raw[n] & 0xc0) == 0x80);
        s_raw[n] = 0; s_preferred[0] = 0; refresh();
    }
    else if (s_edit) { ui_text_edit_backspace(s_edit); refresh(); }
    s_notice[0] = 0;
}
static void letter_key(char letter) {
    if (s_chinese) return;
    char text[2] = {s_upper ? letter - 32 : letter, 0}; insert(text);
}
static ui_keyboard_result_t tap(int x, int y, int top) {
    if (!s_edit || y < top || y >= top + 510 || x < 24 || x >= 660) return UI_KEYBOARD_NONE;
    if (s_panel != 1 && y >= top + 164 && y < top + 208) {
        for (unsigned col = 0; col < 10; ++col) if (ui_rect_hit(number_key_rect(col, top), x, y)) {
            if (s_raw[0]) snprintf(s_notice, sizeof(s_notice), "請先選擇詞語，再輸入數字");
            else { char value[2] = {"1234567890"[col], 0}; insert(value); }
            return UI_KEYBOARD_CHANGED;
        }
    }
    if (y >= top + 104 && y < top + 160) {
        if (x < 68) return ui_keyboard_page(-1) ? UI_KEYBOARD_CHANGED : UI_KEYBOARD_NONE;
        if (x >= 616) return ui_keyboard_page(1) ? UI_KEYBOARD_CHANGED : UI_KEYBOARD_NONE;
        EpdRect boxes[5]; size_t count = visible(s_page, boxes, top);
        for (size_t i = 0; i < count; ++i) if (ui_rect_hit(boxes[i], x, y)) { commit(s_page + i); return UI_KEYBOARD_CHANGED; }
        return UI_KEYBOARD_NONE;
    }
    if (y >= top + 430 && y < top + 502) {
        if (ui_rect_hit(rect(536, 430, 124, 72, top), x, y)) {
            if (!s_raw[0]) return UI_KEYBOARD_DONE;
            snprintf(s_notice, sizeof(s_notice), "請先選擇詞語，再儲存");
        } else if (ui_rect_hit(bottom_backspace_rect(top), x, y)) {
            backspace();
        } else if (ui_rect_hit(input_method_rect(top), x, y)) {
            if (s_ascii || !s_candidates) snprintf(s_notice, sizeof(s_notice), "密碼使用英文、數字與符號");
            else if (s_raw[0]) snprintf(s_notice, sizeof(s_notice), "請先選擇詞語，再切換語言");
            else { s_nine = !s_nine; s_chinese = s_nine; s_upper = false; s_panel = 0; refresh(); }
        } else if (ui_rect_hit(bottom_space_rect(top), x, y)) {
            if (s_raw[0]) { if (!commit(s_page)) snprintf(s_notice, sizeof(s_notice), "沒有相符詞語，請繼續輸入或刪除"); }
            else insert(" ");
        } else if (ui_rect_hit(rect(104, 430, 76, 72, top), x, y)) {
            if (s_raw[0]) snprintf(s_notice, sizeof(s_notice), "請先選詞，再輸入數字符號");
            else s_panel = s_panel == 2 ? 3 : 2;
        } else if (ui_rect_hit(rect(24, 430, 72, 72, top), x, y)) {
            if (s_raw[0]) snprintf(s_notice, sizeof(s_notice), "請先選詞，再輸入數字符號");
            else s_panel = s_panel == 1 ? 0 : 1;
        } else return UI_KEYBOARD_NONE;
        return UI_KEYBOARD_CHANGED;
    }
    if (s_panel || !s_nine) {
        static const char *const rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        for (int row = 0; row < 3; ++row) {
            if (s_panel && row == 2 && ui_rect_hit(rect(600, letter_row_y(2), 56, 56, top), x, y)) { backspace(); return UI_KEYBOARD_CHANGED; }
            if (!s_panel && row == 2 && ui_rect_hit(rect(24, letter_row_y(2), 80, 56, top), x, y)) { s_upper = !s_upper; return UI_KEYBOARD_CHANGED; }
            if (!s_panel && row == 2 && ui_rect_hit(rect(578, letter_row_y(2), 82, 56, top), x, y)) { backspace(); return UI_KEYBOARD_CHANGED; }
            int left = s_panel || !row ? 24 : row == 1 ? 55 : 117;
            int len = s_panel ? 10 : (int)strlen(rows[row]);
            for (int col = 0; col < len; ++col) if (ui_rect_hit(rect(left + col * 64, letter_row_y((unsigned)row), 56, 56, top), x, y)) {
                if (!s_panel) letter_key(rows[row][col]);
                else if (s_panel == 1) { char value[2] = {row == 0 ? "1234567890"[col] : ascii_symbols[(row - 1) * 10 + col][0], 0}; insert(value); }
                else insert((s_chinese && !s_ascii && s_panel != 3 ? symbols : ascii_symbols)[s_panel == 3 && row == 0 && col < 6 ? (const int[]){30,31,28,29,26,27}[col] : row * 10 + col]);
                return UI_KEYBOARD_CHANGED;
            }
        }
    } else {
        for(unsigned row=0;row<4;++row)for(unsigned col=0;col<zhuyin_key_count[row];++col)
            if(ui_rect_hit(zhuyin_key_rect(row,col,top),x,y)){
                size_t used=strlen(s_raw),bytes=strlen(zhuyin_keys[row][col]);
                if(used+bytes>UI_IME_RAW_MAX){snprintf(s_notice,sizeof(s_notice),"注音過長，請先選擇詞語");return UI_KEYBOARD_CHANGED;}
                memcpy(s_raw+used,zhuyin_keys[row][col],bytes+1);s_notice[0]=0;s_preferred[0]=0;refresh();
                return UI_KEYBOARD_CHANGED;
            }
    }
    return UI_KEYBOARD_NONE;
}

static void feedback_dirty(EpdRect area) {
    s_feedback_area = s_feedback_area.width ? ui_rect_union(s_feedback_area, area) : area;
    s_dirty |= DIRTY_FEEDBACK;
}
bool ui_keyboard_release(void) {
    ui_keyboard_hold_cancel();
    if (!s_pressed) return false;
    feedback_dirty(s_pressed_area); s_pressed = false; return true;
}
bool ui_keyboard_press(int x, int y, int top, int64_t now_ms) {
    s_words_idle_since = -1;
    ui_keyboard_release();
    ui_keyboard_hold_start(x, y, top, now_ms);
    if (!s_edit || x < 0 || y < 0) return false;
    s_probe = true; s_probe_x = x; s_probe_y = y; s_key_clip = (EpdRect){0};
    ui_keyboard_draw(NULL, top); s_probe = false;
    if (!s_key_clip.width) return false;
    s_pressed_area = s_key_clip; s_pressed = true;
    feedback_dirty(s_pressed_area); return true;
}
void ui_keyboard_hold_cancel(void) { s_delete_hold = false; }
void ui_keyboard_hold_start(int x, int y, int top, int64_t now_ms) {
    ui_keyboard_hold_cancel();
    if (!s_edit || now_ms < 0) return;
    EpdRect panel_area = rect(600, letter_row_y(2), 56, 56, top);
    EpdRect qwerty_area = rect(578, letter_row_y(2), 82, 56, top);
    EpdRect area = s_panel && ui_rect_hit(panel_area, x, y) ? panel_area
        : !s_panel && !s_nine && ui_rect_hit(qwerty_area, x, y) ? qwerty_area
        : bottom_backspace_rect(top);
    if (!ui_rect_hit(area, x, y)) return;
    s_delete_area = area; s_delete_top = top;
    s_delete_hold = true; s_delete_last_ms = now_ms; s_delete_next_ms = now_ms + UI_LONG_PRESS_MS;
}
ui_keyboard_result_t ui_keyboard_hold_tick(bool held, int x, int y, int64_t now_ms) {
    if (!s_delete_hold) return UI_KEYBOARD_NONE;
    if (!s_edit || !held || now_ms < s_delete_last_ms || !ui_rect_hit(s_delete_area, x, y)) {
        ui_keyboard_hold_cancel(); return UI_KEYBOARD_NONE;
    }
    s_delete_last_ms = now_ms;
    if (now_ms < s_delete_next_ms) return UI_KEYBOARD_NONE;
    if (!s_raw[0] && !s_edit->cursor) { ui_keyboard_hold_cancel(); return UI_KEYBOARD_NONE; }
    // 推屏耗时不补发连删，保持可控；每次复用普通删除的脏区标记。
    // Never catch up after display delays; reuse ordinary backspace dirty-region tracking.
    s_delete_next_ms = now_ms + 120;
    ui_keyboard_result_t result = ui_keyboard_tap(s_delete_area.x + s_delete_area.width / 2,
        s_delete_area.y + s_delete_area.height / 2, s_delete_top, now_ms);
    if (result == UI_KEYBOARD_NONE) ui_keyboard_hold_cancel();
    return result;
}

ui_keyboard_result_t ui_keyboard_idle_tick(bool touching, int64_t now_ms) {
    // 连打、按住或多指时不整理；一次停顿只推一次，不追补累计的候选变化。
    // Skip typing, held keys and multitouch; settle once per pause without replaying accumulated changes.
    if (!s_edit || s_dirty || touching || s_pressed || s_delete_hold ||
        now_ms < 0 || s_word_updates < WORDS_SETTLE_CHANGES) {
        s_words_idle_since = -1; return UI_KEYBOARD_NONE;
    }
    if (s_words_idle_since < 0 || now_ms < s_words_idle_since) {
        s_words_idle_since = now_ms; return UI_KEYBOARD_NONE;
    }
    if (now_ms - s_words_idle_since < WORDS_SETTLE_IDLE_MS) return UI_KEYBOARD_NONE;
    s_dirty |= DIRTY_WORDS_SETTLE;
    return UI_KEYBOARD_CHANGED;
}

ui_keyboard_result_t ui_keyboard_tap(int x, int y, int top, int64_t now_ms) {
    (void)now_ms;
    if (!s_edit) return UI_KEYBOARD_NONE;
    uint32_t text = stamp(s_edit->text), raw = stamp(s_raw), notice = stamp(s_notice);
    size_t cursor = s_edit->cursor; unsigned revision = s_candidates_revision;
    bool nine = s_nine, chinese = s_chinese, upper = s_upper, pending = s_raw[0] != 0;
    unsigned panel = s_panel;
    ui_keyboard_result_t result = tap(x, y, top);
    if (result != UI_KEYBOARD_CHANGED) return result == UI_KEYBOARD_NONE && s_dirty ? UI_KEYBOARD_CHANGED : result;
    if (text != stamp(s_edit->text) || cursor != s_edit->cursor) s_dirty |= DIRTY_FIELD;
    if (nine != s_nine || chinese != s_chinese || upper != s_upper || panel != s_panel) s_dirty |= DIRTY_LAYOUT;
    else {
        if (revision != s_candidates_revision || raw != stamp(s_raw) || notice != stamp(s_notice)) s_dirty |= DIRTY_WORDS;
        if (pending != (s_raw[0] != 0)) s_dirty |= DIRTY_SPACE;
    }
    return s_dirty ? result : UI_KEYBOARD_NONE;
}
static void include(ui_keyboard_update_t *update, EpdRect area) {
    update->area = update->area.width ? ui_rect_union(update->area, area) : area;
}
ui_keyboard_update_t ui_keyboard_update(uint8_t *fb, int top, EpdRect field, int px,
                                        bool masked, const char *suffix, bool force_field) {
    ui_keyboard_update_t update = {.layout = (s_dirty & DIRTY_LAYOUT) != 0,
        .settle = s_dirty == DIRTY_WORDS_SETTLE && !force_field};
    if (!s_edit) return update;
    if (force_field || (s_dirty & DIRTY_FIELD)) {
        ui_clear_rect_fast(fb, field);
        ui_text_input_draw(fb, s_edit, field, px, masked, suffix);
        include(&update, field); update.field = true;
    }
    if (update.layout) {
        ui_keyboard_draw(fb, top);
        include(&update, rect(16, -12, 652, 534, top));
        s_word_updates = 0; s_words_idle_since = -1;
    } else {
        if (s_dirty & (DIRTY_WORDS | DIRTY_WORDS_SETTLE)) {
            EpdRect area = rect(24, 52, 636, 114, top);
            epd_fill_rect(area, UI_GRAY_WHITE, fb); paint_words(fb, top); include(&update, area);
            if (update.settle) s_word_updates = 0;
            else if ((s_dirty & DIRTY_WORDS) && s_word_updates < WORDS_SETTLE_CHANGES) ++s_word_updates;
            s_words_idle_since = -1;
        }
        if (s_dirty & DIRTY_SPACE) {
            EpdRect area = bottom_space_rect(top);
            key_paint(fb, area, s_raw[0] ? "選擇" : "空格", 26, false, false); include(&update, area);
        }
    }
    if (!update.layout && (s_dirty & DIRTY_FEEDBACK)) {
        s_keys_only = true; s_key_clip = s_feedback_area;
        ui_keyboard_draw(fb, top); s_keys_only = false;
        include(&update, s_feedback_area);
    }
    if (!update.settle && update.field && s_chars_since_settle >= 5) {
        update.settle = true;
        s_chars_since_settle %= 5;
    }
    s_feedback_area = (EpdRect){0}; s_dirty = 0;
    return update;
}
