#!/usr/bin/env python3
"""中文：执行真实校准设置页的绘制与确认交互。
English: Execute real calibration-page painting and confirmation interactions.
"""
from pathlib import Path
import subprocess
import tempfile

from home_cover_cache_host_test import function
from run_reader_baseline import toolchain

ROOT = Path(__file__).resolve().parents[1]
unit = r'''
#include "device_power.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define EPD_DRAW_ALIGN_LEFT 0
#define APP_REDRAW_NONE 0
#define APP_REDRAW_PAGE 1
typedef int app_redraw_t;
typedef struct {int x,y,width,height;} EpdRect;
static bool s_batt_calibration_confirm;
static device_power_batt_calibration_status_t s_batt_calibration_status;
static device_power_batt_calibration_status_t status={.total_bins=21};
static char s_notice[96], painted[4096];
static unsigned queries, toggles, resets;
static bool fail;
static void append(const char *s){assert(strlen(painted)+strlen(s)+2<sizeof(painted));strcat(painted,s);strcat(painted,"\n");}
void device_power_get_batt_calibration(device_power_batt_calibration_status_t *out){++queries;*out=status;}
esp_err_t device_power_set_batt_calibration(bool enabled){++toggles;if(fail)return -1;status.enabled=enabled;return 0;}
esp_err_t device_power_reset_batt_calibration(void){++resets;if(fail)return -1;status.applied=false;status.sampled_bins=status.trusted_bins=0;return 0;}
static void back_header(uint8_t *fb,const char *s){(void)fb;append(s);}
static void section(uint8_t *fb,int y,const char *s){(void)fb;assert(y==438);append(s);}
static void setting_toggle(uint8_t *fb,int y,const char *s,const char *detail,bool enabled){(void)fb;assert(y==272&&enabled==status.enabled);append(s);append(detail);}
static void ui_text_box(uint8_t *fb,EpdRect r,int px,const char *s,int align,bool bold){(void)fb;(void)bold;assert(r.x==48&&r.width==588&&r.y>=482&&r.y+r.height<=888&&r.height>=40&&px>=22&&px<=28&&align==0);append(s);}
static void ui_draw_button(uint8_t *fb,EpdRect r,const char *s,bool enabled){(void)fb;(void)enabled;assert(r.x==36&&r.y==706&&r.width==612&&r.height==78);append(s);}
static void ui_nav_draw(uint8_t *fb,int page){(void)fb;assert(page==3);}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
'''
source = ROOT / "main/apps/app_device_settings.c"
for name in ("batt_calibration_render", "batt_calibration_touch"):
    unit += function(name, source) + "\n"
unit += r'''
int main(void){
    uint8_t fb=0;
    batt_calibration_render(&fb);assert(queries==1&&!toggles&&!resets);
    assert(strstr(painted,"等待有效样本")&&strstr(painted,"0 / 21")&&strstr(painted,"停止学习"));
    assert(batt_calibration_touch(48,300)==APP_REDRAW_PAGE&&status.enabled&&toggles==1);
    assert(strstr(s_notice,"已开启"));
    status.sampled_bins=4;status.trusted_bins=3;painted[0]=0;
    batt_calibration_render(&fb);assert(strstr(painted,"正在学习")&&strstr(painted,"4 / 21"));
    status.applied=true;
    assert(batt_calibration_touch(48,300)==APP_REDRAW_PAGE&&!status.enabled&&status.applied);
    assert(strstr(s_notice,"已有校准继续生效"));painted[0]=0;
    batt_calibration_render(&fb);assert(strstr(painted,"已应用部分校准"));
    assert(batt_calibration_touch(48,720)==APP_REDRAW_PAGE&&s_batt_calibration_confirm&&!resets);
    painted[0]=0;batt_calibration_render(&fb);assert(strstr(painted,"确认重置校准"));
    assert(batt_calibration_touch(20,720)==APP_REDRAW_PAGE&&!s_batt_calibration_confirm&&!resets);
    assert(batt_calibration_touch(48,720)==APP_REDRAW_PAGE);
    assert(batt_calibration_touch(48,720)==APP_REDRAW_PAGE&&resets==1&&!status.applied&&!status.enabled);
    assert(strstr(s_notice,"学习开关保持不变"));
    fail=true;
    assert(batt_calibration_touch(48,300)==APP_REDRAW_PAGE&&!status.enabled&&strstr(s_notice,"保存失败"));
    assert(batt_calibration_touch(48,720)==APP_REDRAW_PAGE);
    assert(batt_calibration_touch(48,720)==APP_REDRAW_PAGE&&!s_batt_calibration_confirm&&strstr(s_notice,"未完整保存"));
    status.save_error=true;strcpy(s_notice,"旧的成功提示");painted[0]=0;
    batt_calibration_render(&fb);assert(strstr(painted,"校准保存失败")&&!strstr(painted,"旧的成功提示"));
    assert(batt_calibration_touch(48,800)==APP_REDRAW_NONE);
    puts("PASS: pure calibration painting, coverage/states, retained model on disable, reset confirmation/cancel and persistence error feedback");
}
'''
with tempfile.TemporaryDirectory(prefix="battery-calibration-ui-") as directory:
    folder = Path(directory)
    env, _ = toolchain(folder)
    (folder / "driver").mkdir()
    (folder / "driver/i2c_master.h").write_text("#pragma once\ntypedef void *i2c_master_bus_handle_t;\n")
    (folder / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
    c, binary = folder / "test.c", folder / "test"
    c.write_text(unit)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I" + str(folder), "-Icomponents/device_power/include",
                    str(c), "-o", str(binary)], cwd=ROOT, env=env, check=True)
    subprocess.run([str(binary)], cwd=ROOT, env=env, check=True)
