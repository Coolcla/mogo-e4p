/* SPDX-License-Identifier: Apache-2.0
 * 中文：共用手机式墨水屏键盘，一次只有当前页面使用，退出清除输入状态。
 * English: Shared smartphone-style e-paper keyboard; only the active page owns it, with state cleared on exit.
 */
#pragma once
#include "ui_text_input.h"
typedef enum { UI_KEYBOARD_NONE, UI_KEYBOARD_CHANGED, UI_KEYBOARD_DONE } ui_keyboard_result_t;
/// 綁定編輯緩衝區；一般輸入使用注音與 QWERTY，密碼僅使用英文、數字及符號。
/// Bind the editor; normal input uses Zhuyin and QWERTY, passwords use English, digits and symbols.
void ui_keyboard_begin(ui_text_edit_t *edit, bool ascii_only);
/// 清除注音組字、候選與編輯指標。/ Clear Zhuyin composition, candidates and the editor pointer.
void ui_keyboard_end(void);
/// 固定 636×510 iOS 式鍵盤：左右候選箭頭下方是數字列，再往下為注音／QWERTY 鍵。
/// Paint a fixed 636x510 keyboard with numbers below candidate arrows and above Zhuyin/QWERTY keys.
void ui_keyboard_draw(uint8_t *fb, int top);
/// 松手触发按键；时间参数保留兼容，不再使用英文九宫格连续点按。
/// Activate on release; keep the time argument for compatibility without English T9 multi-tap.
ui_keyboard_result_t ui_keyboard_tap(int x, int y, int top, int64_t now_ms);
/// 按下即绘制轻量反馈，不提交字符；松开或取消恢复。/ Lightweight press feedback without committing a character.
bool ui_keyboard_press(int x, int y, int top, int64_t now_ms);
/// 清除按下反馈并取消连删；返回是否需要局部重绘。/ Clear feedback/repeat; true requests local repaint.
bool ui_keyboard_release(void);
/// 按下删除键时计时；不立即删除，短按仍在松手时处理。/ Arm deletion on press; short taps still delete on release.
void ui_keyboard_hold_start(int x, int y, int top, int64_t now_ms);
/// 松手、滑动、切页或中断取消连续删除。/ Cancel repeat on release, movement, page changes or interruptions.
void ui_keyboard_hold_cancel(void);
/// 單指按住 500ms 後每 120ms 刪除一位，先刪注音組字，再刪文字；不補發積壓。
/// After a 500ms hold, delete one Zhuyin symbol or text character every 120ms; no catch-up bursts.
ui_keyboard_result_t ui_keyboard_hold_tick(bool held, int x, int y, int64_t now_ms);
/// 词栏累计三次变化且无触摸停顿 400ms 后，安排一次局部灰阶整理；离页自动取消。
/// After three word-strip changes and a 400ms touch-free pause, request one local gray settle; exit cancels it.
ui_keyboard_result_t ui_keyboard_idle_tick(bool touching, int64_t now_ms);
/// 尚未確認的注音組字不可保存。/ Unconfirmed Zhuyin composition prevents saving.
bool ui_keyboard_pending(void);
/// 左右滑动候选页，边界不循环。/ Swipe candidate pages without wrapping at the edges.
bool ui_keyboard_page(int direction);
typedef struct {
    EpdRect area;       ///< 实际重绘区域，宽为零表示无变化 / Actual painted bounds; zero width means unchanged
    bool layout;        ///< 布局切换用局部灰阶，其余为快速输入反馈 / Local grayscale for layout switches, fast input otherwise
    bool field;         ///< 已重绘输入值或光标 / Input value or caret was repainted
    bool settle;        ///< 仅词栏的全像素 GL16，不使用差分或整屏刷新 / Full-pixel GL16 for the word strip only, never a differential or whole-screen refresh
} ui_keyboard_update_t;
/// 只重繪變化的輸入框、注音組字、候選或鍵盤；僅從事件回呼呼叫。
/// Paint changed field, Zhuyin composition, candidates or keyboard only; call from event handlers only.
ui_keyboard_update_t ui_keyboard_update(uint8_t *fb, int top, EpdRect field, int px,
                                        bool masked, const char *suffix, bool force_field);
