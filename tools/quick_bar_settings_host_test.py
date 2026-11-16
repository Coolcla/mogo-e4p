# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：执行真实快捷栏管理手势，覆盖容量、排列与保存失败。/ English: Exercise real quick-bar management gestures, capacity, ordering and failed saves."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/apps/app_device_settings.c").read_text()
start = source.index("static app_redraw_t quick_settings_gesture(")
at = source.index("{", start) + 1
depth = 1
while depth:
    depth += (source[at] == "{") - (source[at] == "}")
    at += 1
gesture = source[start:at]
constants = re.search(r"enum \{ QUICK_SETTINGS_TOP.*?\};", source).group(0)
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app/app_quick_items.h"
typedef struct {int tab;} app_ctx_t;
typedef struct {int type, x0, y0;} ui_gesture_event_t;
typedef struct {int x,y,width,height;} EpdRect;
typedef enum {APP_REDRAW_NONE,APP_REDRAW_PAGE} app_redraw_t;
enum {UI_GESTURE_TAP,UI_GESTURE_SWIPE_U,SETTINGS_MAIN,SETTINGS_QUICK_BAR};
static int s_page=SETTINGS_QUICK_BAR;
static char s_quick_notice[96];
static app_quick_layout_t saved=APP_QUICK_LAYOUT_DEFAULT;
static bool save_fails;
static unsigned saves;
static const app_quick_layout_t *app_settings_quick_layout(void){return &saved;}
static bool app_settings_set_quick_layout(const app_quick_layout_t *layout){
    assert(app_quick_layout_valid(layout));++saves;
    if(save_fails)return false;
    saved=*layout;return true;
}
static int ui_nav_hit(int x,int y){(void)x;return y>=1100?2:-1;}
static void ui_nav_request(app_ctx_t *ctx,int tab){ctx->tab=tab;}
static void settings_prepare_profile(void){}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
'''
code += constants + "\n" + gesture + r'''
static app_ctx_t ctx;
static void tap(int x,int y){
    ui_gesture_event_t ev={UI_GESTURE_TAP,x,y};
    (void)quick_settings_gesture(&ctx,&ev);
    assert(app_quick_layout_valid(&saved));
}
static void row(unsigned index,int x){tap(x,QUICK_SETTINGS_TOP+index*QUICK_SETTINGS_ROW_H+38);}
int main(void){
    app_quick_layout_t defaults=saved;
    row(8,200);assert(saves==0&&saved.count==8&&strstr(s_quick_notice,"最多"));
    row(0,200);assert(saved.count==7&&saved.order[7]==APP_QUICK_WIFI);
    row(8,200);assert(saved.count==8&&saved.order[7]==APP_QUICK_CELLULAR);
    for(unsigned i=7;i>0;--i)row(i,516);
    assert(saved.order[0]==APP_QUICK_CELLULAR);
    row(0,596);assert(saved.order[1]==APP_QUICK_CELLULAR);
    unsigned before=saves;row(0,516);row(7,596);row(8,516);assert(saves==before);
    app_quick_layout_t previous=saved;
    save_fails=true;row(0,200);
    assert(!memcmp(&previous,&saved,sizeof(saved))&&strstr(s_quick_notice,"保存失败"));
    save_fails=false;
    while(saved.count)row(0,200);
    assert(saved.count==0);row(8,200);assert(saved.count==1);
    tap(300,1038);assert(!memcmp(&saved,&defaults,sizeof(saved)));
    tap(300,1120);assert(ctx.tab==2);
    tap(60,100);assert(s_page==SETTINGS_MAIN);
    puts("PASS: manager enforces eight slots, selects every item, reorders across rows, rejects edge moves, retains failed saves and restores defaults");
}
'''
with tempfile.TemporaryDirectory(prefix="quick-settings-") as directory:
    folder = Path(directory)
    c_file = folder / "test.c"
    c_file.write_text(code)
    binary = folder / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-Imain", str(c_file), "-o", str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], cwd=root, check=True)
