/* SPDX-License-Identifier: Apache-2.0
 * Check pixel-equivalent partial refreshes for the current Zhuyin keyboard.
 */
#include "../main/ui/ui_keyboard.c"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 684
#define HEIGHT 1102
#define BYTES (WIDTH * HEIGHT)
int html_test_fail_after = -1;
static unsigned width_calls, text_calls;
static void pixel(uint8_t *fb,int x,int y,uint8_t gray) {
    assert(x>=0&&x<WIDTH&&y>=0&&y<HEIGHT);
    if (fb) fb[y*WIDTH+x]=gray;
}
static bool inside(EpdRect r,int radius,int x,int y) {
    if(x<r.x||x>=r.x+r.width||y<r.y||y>=r.y+r.height)return false;
    int cx=x<r.x+radius?r.x+radius-1:x>=r.x+r.width-radius?r.x+r.width-radius:x;
    int cy=y<r.y+radius?r.y+radius-1:y>=r.y+r.height-radius?r.y+r.height-radius:y;
    return (x-cx)*(x-cx)+(y-cy)*(y-cy)<=radius*radius;
}
void epd_fill_rect(EpdRect r,uint8_t gray,uint8_t *fb){
    for(int y=r.y;y<r.y+r.height;y++)for(int x=r.x;x<r.x+r.width;x++)pixel(fb,x,y,gray);
}
void ui_fill_round_rect(uint8_t *fb,EpdRect r,int radius,uint8_t gray){
    gray=ui_control_gray(gray);
    for(int y=r.y;y<r.y+r.height;y++)for(int x=r.x;x<r.x+r.width;x++)
        if(inside(r,radius,x,y))pixel(fb,x,y,gray);
}
void ui_draw_round_rect(uint8_t *fb,EpdRect r,int radius,uint8_t gray){
    gray=ui_control_gray(gray);EpdRect inner={r.x+1,r.y+1,r.width-2,r.height-2};
    for(int y=r.y;y<r.y+r.height;y++)for(int x=r.x;x<r.x+r.width;x++)
        if(inside(r,radius,x,y)&&!inside(inner,radius>0?radius-1:0,x,y))pixel(fb,x,y,gray);
}
void epd_draw_line(int x,int y,int xx,int yy,uint8_t gray,uint8_t *fb){
    int dx=abs(xx-x),sx=x<xx?1:-1,dy=-abs(yy-y),sy=y<yy?1:-1,e=dx+dy;
    for(;;){pixel(fb,x,y,gray);if(x==xx&&y==yy)break;int ee=2*e;
        if(ee>=dy){e+=dy;x+=sx;}if(ee<=dx){e+=dx;y+=sy;}}
}
bool ui_rect_hit(EpdRect r,uint16_t x,uint16_t y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
EpdRect ui_rect_union(EpdRect a,EpdRect b){
    int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
    int r=a.x+a.width>b.x+b.width?a.x+a.width:b.x+b.width;
    int d=a.y+a.height>b.y+b.height?a.y+a.height:b.y+b.height;
    return(EpdRect){x,y,r-x,d-y};
}
void ui_clear_rect_fast(uint8_t *fb,EpdRect r){epd_fill_rect(r,UI_GRAY_WHITE,fb);}
int ui_text_effective_px(int px){return px;}
int ui_text_fixed_width_px(int px,const char *text){
    ++width_calls;int width=0;
    for(const unsigned char *p=(const unsigned char*)text;*p;p++)
        if((*p&192)!=128)width+=*p>=128?px:px/2;
    return width;
}
int ui_text_fixed_context_width_px(int px,const char *text,const char *sample){
    (void)sample;return ui_text_fixed_width_px(px,text);
}
void ui_text_fixed_vc(uint8_t *fb,int x,int y,int px,const char *text,
                      enum EpdFontFlags align,bool inverse){
    ++text_calls;int width=ui_text_fixed_width_px(px,text);
    if(align==EPD_DRAW_ALIGN_CENTER)x-=width/2;
    else if(align==EPD_DRAW_ALIGN_RIGHT)x-=width;
    for(const unsigned char *p=(const unsigned char*)text;*p;p++)if((*p&192)!=128){
        int advance=*p>=128?px:px/2;
        for(int j=0;j<px/2;j++)for(int i=1;i<advance-1;i++)
            if((i+j+*p)%4)pixel(fb,x+i,y-px/4+j,inverse?255:0);
        x+=advance;
    }
}
void ui_text_fixed_context_vc(uint8_t *fb,int x,int y,int px,const char *text,
                              const char *sample,enum EpdFontFlags align,bool inverse){
    (void)sample;ui_text_fixed_vc(fb,x,y,px,text,align,inverse);
}

static uint8_t *fb,*reference,*previous;
static char value[121];
static ui_text_edit_t edit;
static const EpdRect field={36,243,612,82};
static unsigned updates,layouts,settles;
static int64_t now_ms=100;
static ui_keyboard_update_t last_update;
static void fresh(uint8_t *pixels){
    memset(pixels,255,BYTES);
    ui_text_input_draw(pixels,&edit,field,28,false,NULL);
    ui_keyboard_draw(pixels,560);
}
static void repaint(bool force){
    memcpy(previous,fb,BYTES);
    ui_keyboard_update_t update=ui_keyboard_update(fb,560,field,28,false,NULL,force);
    fresh(reference);
    for(int y=0;y<HEIGHT;y++)for(int x=0;x<WIDTH;x++){
        if(fb[y*WIDTH+x]!=reference[y*WIDTH+x]){
            fprintf(stderr,"stale input pixel at %d,%d: %u vs %u\\n",x,y,fb[y*WIDTH+x],reference[y*WIDTH+x]);abort();
        }
        if(fb[y*WIDTH+x]!=previous[y*WIDTH+x])assert(ui_rect_hit(update.area,x,y));
    }
    if(update.area.width)updates++;
    if(update.layout)layouts++;
    last_update=update;
    if(update.settle){
        assert(!update.field&&!update.layout&&update.area.x==24&&update.area.y==612&&
               update.area.width==636&&update.area.height==114);
        ++settles;
    }
}
static void key(int x,int y){
    ui_keyboard_result_t result=ui_keyboard_tap(x,y,560,now_ms+=100);
    if(result==UI_KEYBOARD_CHANGED)repaint(false);
}
static void begin(bool ascii){
    value[0]=0;ui_text_edit_init(&edit,value,sizeof(value));
    ui_keyboard_begin(&edit,ascii);fresh(fb);
}
static void zhuyin(unsigned row,unsigned col){
    EpdRect r=zhuyin_key_rect(row,col,560);
    key(r.x+r.width/2,r.y+r.height/2);
}

int main(void){
    fb=malloc(BYTES);reference=malloc(BYTES);previous=malloc(BYTES);assert(fb&&reference&&previous);
    begin(true);
    for(size_t i=0;i<BYTES;i++)assert(fb[i]==UI_GRAY_BLACK||fb[i]==UI_GRAY_WHITE);

    /* Top number row and both English backspace positions repaint cleanly. */
    key(26,562);assert(!strcmp(value,"1"));
    key(492,1000);assert(!value[0]);
    key(30,742);assert(!strcmp(value,"q"));
    key(492,1000);assert(!value[0]);
    key(30,742);key(620,900);assert(!value[0]);

    /* Zhuyin composition, shared delete, and 中/En transition. */
    begin(false);
    zhuyin(2,1);zhuyin(1,6);zhuyin(3,2);zhuyin(2,8);
    assert(!strcmp(s_raw,"ㄋㄧㄏㄠ"));
    key(492,1000);assert(strcmp(s_raw,"ㄋㄧㄏㄠ"));
    key(412,1000);assert(!s_chinese&&!s_nine);
    key(30,742);key(492,1000);assert(!value[0]);
    key(412,1000);assert(s_chinese&&s_nine);

    /* Number/symbol page preserves the common and page-specific backspace. */
    begin(true);key(60,1000);assert(s_panel==1);
    key(90,562);assert(!strcmp(value,"2"));
    key(492,1000);assert(!value[0]);
    key(30,898);assert(value[0]);key(620,900);assert(!value[0]);

    /* Three Zhuyin edits settle only the word strip after the 400 ms pause. */
    begin(false);
    assert(WORDS_SETTLE_CHANGES==3&&WORDS_SETTLE_IDLE_MS==400);
    zhuyin(2,1);zhuyin(1,6);zhuyin(3,2);
    int64_t last=now_ms;
    assert(ui_keyboard_idle_tick(false,last+399)==UI_KEYBOARD_NONE);
    assert(ui_keyboard_idle_tick(false,last+400)==UI_KEYBOARD_CHANGED);
    repaint(false);assert(last_update.settle&&settles==1);
    for(int i=0;i<20;i++)assert(ui_keyboard_idle_tick(false,last+1000+i*1000)==UI_KEYBOARD_NONE);

    /* Paging through a candidate list also remains pixel-equivalent. */
    begin(false);s_nine=false;strcpy(s_raw,"shi");refresh();fresh(fb);
    if(s_candidates->count>5){
        assert(ui_keyboard_page(1));repaint(false);
        assert(ui_keyboard_page(-1));repaint(false);
    }
    assert(ui_text_input_tap(&edit,field,28,false,NULL,550,285));
    ui_keyboard_update_t update=ui_keyboard_update(fb,560,field,28,false,NULL,true);
    assert(update.field&&!update.layout&&update.area.y==field.y&&update.area.height==field.height);
    ui_keyboard_end();ui_ime_release();free(fb);free(reference);free(previous);
    printf("keyboard incremental paint: PASS (%u bounded updates, %u layout changes, %u candidate settle)\\n",updates,layouts,settles);
}
