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
void klog(const char* f,...){(void)f;}
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
 /* a source repeating the same place (a parked pen, a keyboard's spare pointer
  * interface) must not pull the pointer back from a relative mouse's motion */
 integrate=0;
 int rel=mouse_source();CHECK(rel>0&&rel!=src);
 mouse_inject_src(rel,40,-25,0);m=mouse_read();CHECK(!m.abs&&m.dx==40&&m.dy==-25);
 mouse_inject_abs_src(src,50000,20000,0);m=mouse_read();CHECK(!m.abs);
 mouse_inject_src(rel,-5,5,0);mouse_inject_abs_src(src,50000,20000,0);m=mouse_read();CHECK(!m.abs&&m.dx==-5&&m.dy==5);
 mouse_inject_abs_src(src,51000,20000,0);m=mouse_read();CHECK(m.abs&&m.ax==51000);  /* (it moved: it wins) */
 mouse_inject_abs_src(src,51000,20000,1);m=mouse_read();CHECK(m.btn_left);          /* (its buttons still count) */
 mouse_inject_abs_src(src,51000,20000,0);m=mouse_read();CHECK(!m.btn_left);
 mouse_source_free(rel);
 integrate=1;
 mouse_source_free(src);
 integrate=1;mouse_pkt_size=4;
 mouse_on_aux_byte(9);mouse_on_aux_byte(0);mouse_on_aux_byte(0);mouse_on_aux_byte(1);
 m=mouse_read();CHECK(m.btn_left&&m.dz==1&&m.abs);
 packet(0,0,0);m=mouse_read();CHECK(!m.btn_left);
 /* The firmware's USB legacy emulation answered at 0x60 / 0x64 at boot (a
  * controller "present"); the USB handoff ended it and the ports float at
  * 0xFF. Those bytes must not become motion: before, each was "left 1,
  * down 1, every button held" and dragged the pointer to the bottom-left. */
 packet(0,0,0);m=mouse_read();
 integrate=0;g_ps2_present=1;mouse_enabled=1;mouse_pkt_size=3;mouse_pkt_i=0;
 test_port64=0xFF;test_port60=0xFF;
 int moved=0,held=0;
 for(int i=0;i<50;i++){m=mouse_read();moved|=m.dx||m.dy;held|=m.btn_left||m.btn_right||m.btn_middle;
  int dx=0,dy=0;moved|=mouse_irq_motion(&dx,&dy)&&(dx||dy);}
 CHECK(!moved&&!held);
 CHECK(g_ps2_present==0&&!mouse_enabled&&!ps2_controller_present());
 CHECK(ps2_status()==0);
 /* a USB mouse still moves the pointer afterwards */
 int usb=mouse_source();mouse_inject_src(usb,12,-4,0);m=mouse_read();CHECK(m.dx==12&&m.dy==-4&&!m.btn_left);
 mouse_source_free(usb);
 test_port64=0;test_port60=0;
 printf("Mouse input: %u checks passed\n",checks);return 0;
}
