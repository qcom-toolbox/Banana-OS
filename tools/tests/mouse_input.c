/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char* u32_to_str(uint32_t,char*,int);
#include "input-under-test.c"
static unsigned checks;
static int integrate=1;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
void usb_poll(void){}
void i2chid_poll(void){}
void vmtools_poll_pointer(void){if(integrate)mouse_set_abs(32000,16000);}
void tp_tick(touchpad_t* t){(void)t;}
void tp_frame(touchpad_t* t,int fingers,int x,int y,int buttons,int flags){(void)t;(void)fingers;(void)x;(void)y;(void)buttons;(void)flags;}
static void packet(int buttons,int dx,int dy){
 uint8_t first=8|(buttons&7)|(dx<0?16:0)|(dy<0?32:0);
 mouse_on_aux_byte(first);mouse_on_aux_byte((uint8_t)dx);mouse_on_aux_byte((uint8_t)dy);
 if(mouse_pkt_size==4)mouse_on_aux_byte(0);
}
int main(void){
 mouse_enabled=1;mouse_pkt_size=3;
 for(int i=0;i<120;i++){
  packet(1,0,0);mouse_state_t m=(i&1)?mouse_read_raw():mouse_read();
  CHECK(m.abs&&m.ax==32000&&m.ay==16000);CHECK(m.btn_left&&!m.btn_right);
  packet(0,0,0);m=mouse_read();CHECK(!m.btn_left&&m.abs);
  CHECK(mouse_ring_len==0);
 }
 /* Absolute integration must not hide queued fast press/release pairs. */
 packet(1,0,0);packet(0,0,0);
 mouse_state_t burst=mouse_read();CHECK(burst.abs&&burst.btn_left);
 burst=mouse_read();CHECK(burst.abs&&!burst.btn_left&&mouse_ring_len==0);
 /* Host integration can stop and restart when the VM loses/regains focus. */
 integrate=0;packet(1,0,0);burst=mouse_read();CHECK(burst.btn_left);
 integrate=1;packet(0,0,0);burst=mouse_read();CHECK(burst.abs&&!burst.btn_left);
 integrate=0;packet(1,7,-3);mouse_state_t m=mouse_read();
 CHECK(m.btn_left&&m.dx==7&&m.dy==-3&&!m.abs);
 packet(0,0,0);m=mouse_read();CHECK(!m.btn_left);
 int src=mouse_source();CHECK(src>0);
 mouse_inject_abs_src(src,50000,20000,1);m=mouse_read();CHECK(m.abs&&m.ax==50000&&m.btn_left);
 mouse_inject_abs_src(src,50000,20000,0);m=mouse_read();CHECK(m.abs&&!m.btn_left);
 mouse_source_free(src);
 integrate=1;mouse_pkt_size=4;
 mouse_on_aux_byte(9);mouse_on_aux_byte(0);mouse_on_aux_byte(0);mouse_on_aux_byte(1);
 m=mouse_read();CHECK(m.btn_left&&m.dz==1&&m.abs);
 packet(0,0,0);m=mouse_read();CHECK(!m.btn_left);
 printf("Mouse input: %u checks passed\n",checks);return 0;
}
