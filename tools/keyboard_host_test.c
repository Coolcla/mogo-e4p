/* SPDX-License-Identifier: Apache-2.0
 * Verify the shipped Zhuyin/QWERTY layouts, Taiwan Traditional candidates,
 * always-present number row, and every backspace location.
 */
#include "../main/ui/ui_keyboard.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int html_test_fail_after = -1;
static void draw_rect(EpdRect r) {
    assert(r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0 &&
           r.x + r.width <= 684 && r.y + r.height <= 1102);
}
void ui_fill_round_rect(uint8_t *fb,EpdRect r,int radius,uint8_t gray){(void)fb;(void)radius;(void)gray;draw_rect(r);}
void ui_draw_round_rect(uint8_t *fb,EpdRect r,int radius,uint8_t gray){(void)fb;(void)radius;(void)gray;draw_rect(r);}
void epd_draw_line(int a,int b,int c,int d,uint8_t gray,uint8_t *fb){(void)gray;(void)fb;assert(a>=0&&c>=0&&a<684&&c<684&&b>=0&&d>=0&&b<1102&&d<1102);}
void ui_text_fixed_vc(uint8_t *fb,int x,int y,int px,const char *text,enum EpdFontFlags align,bool inverse){(void)fb;(void)align;(void)inverse;assert(x>=0&&x<=684&&y>=0&&y<1102&&px>0&&text);}
int ui_text_fixed_width_px(int px,const char *text){int n=0;for(const unsigned char *p=(const unsigned char *)text;*p;++p)if((*p&192)!=128)n+=*p>=128?px:px/2;return n;}
void ui_text_input_composition(uint8_t *fb,const char *text,EpdRect r,int px){(void)fb;(void)text;(void)px;draw_rect(r);}
bool ui_rect_hit(EpdRect r,uint16_t x,uint16_t y){return x>=r.x&&y>=r.y&&x<r.x+r.width&&y<r.y+r.height;}
void epd_fill_rect(EpdRect r,uint8_t gray,uint8_t *fb){(void)gray;(void)fb;draw_rect(r);}
void ui_clear_rect_fast(uint8_t *fb,EpdRect r){(void)fb;draw_rect(r);}
EpdRect ui_rect_union(EpdRect a,EpdRect b){int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;int r=a.x+a.width>b.x+b.width?a.x+a.width:b.x+b.width,d=a.y+a.height>b.y+b.height?a.y+a.height:b.y+b.height;return(EpdRect){x,y,r-x,d-y};}
void ui_text_input_draw(uint8_t *fb,const ui_text_edit_t *edit,EpdRect r,int px,bool masked,const char *suffix){(void)fb;(void)edit;(void)px;(void)masked;(void)suffix;draw_rect(r);}

static char text[96];
static ui_text_edit_t edit;
static void begin(bool ascii) {
    text[0] = 0;
    ui_text_edit_init(&edit, text, sizeof(text));
    ui_keyboard_begin(&edit, ascii);
    ui_keyboard_draw(NULL, 560);
}
static void key(int x, int y) {
    assert(ui_keyboard_tap(x, y, 560, 0) == UI_KEYBOARD_CHANGED);
}
static void zhuyin(unsigned row, unsigned col) {
    EpdRect r = zhuyin_key_rect(row, col, 560);
    key(r.x + r.width / 2, r.y + r.height / 2);
}
static size_t find_candidate(const char *value) {
    for (size_t i = 0; i < s_candidates->count; ++i)
        if (!strcmp(s_candidates->items[i].text, value)) return i;
    assert(!"missing candidate");
    return 0;
}

int main(void) {
    begin(false);
    assert(s_chinese && s_nine);

    /* Numeric row is reachable in Chinese mode; shared backspace deletes it. */
    key(26, 562);
    assert(!strcmp(text, "1"));
    key(492, 992);
    assert(!text[0]);

    /* iOS-style Zhuyin keys compose ni-hao and return Traditional candidates. */
    zhuyin(2, 1); /* ㄋ */
    zhuyin(1, 6); /* ㄧ */
    zhuyin(3, 2); /* ㄏ */
    zhuyin(2, 8); /* ㄠ */
    assert(!strcmp(s_raw, "ㄋㄧㄏㄠ"));
    assert(find_candidate("你好") < s_candidates->count);
    key(492, 992);
    assert(strcmp(s_raw, "ㄋㄧㄏㄠ"));
    zhuyin(2, 8);
    assert(commit(find_candidate("你好")));
    assert(!strcmp(text, "你好") && !s_raw[0]);

    ui_ime_result_t *phrase = calloc(1, sizeof(*phrase));
    assert(phrase);
    assert(ui_ime_candidates("woxiangdushu", false, "", "", phrase));
    bool traditional = false;
    for (size_t i = 0; i < phrase->count; ++i)
        if (!strcmp(phrase->items[i].text, "我想讀書")) traditional = true;
    assert(traditional);
    free(phrase);

    /* 中/En switches input method; both bottom and QWERTY-row delete work. */
    begin(false);
    key(412, 992);
    assert(!s_chinese && !s_nine);
    key(30, 746);
    assert(!strcmp(text, "q"));
    key(492, 992);
    assert(!text[0]);
    key(30, 746);
    key(620, 900);
    assert(!text[0]);

    /* Number/symbol page still has both backspaces and the top number row. */
    key(60, 992);
    assert(s_panel == 1);
    key(90, 562);
    assert(!strcmp(text, "2"));
    key(492, 992);
    assert(!text[0]);
    key(30, 898);
    assert(text[0]);
    key(620, 900);
    assert(!text[0]);

    /* Returning to 中 restores Zhuyin and its shared delete button. */
    key(412, 992);
    assert(s_chinese && s_nine && !s_panel);
    zhuyin(2, 1);
    assert(s_raw[0]);
    key(492, 992);
    assert(!s_raw[0]);

    /* Password/ASCII input cannot switch to Zhuyin but retains its backspace. */
    begin(true);
    key(412, 992);
    assert(!s_chinese && !s_nine);
    key(30, 746);
    key(492, 992);
    assert(!text[0]);
    puts("keyboard/IME: PASS (iOS Zhuyin layout, Traditional candidates, number row, 中/En and backspace in all pages)");
    ui_keyboard_end();
    return 0;
}
