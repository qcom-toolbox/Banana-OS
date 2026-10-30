/* SPDX-License-Identifier: BSD-2-Clause */
#include "../../sdk/examples/shelltest/interactive.c"
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"editor sync failed at %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void child_session(int mode) {
 struct termios t;
 if(mode!=1){usleep(200000);tcgetattr(0,&t);cfmakeraw(&t);tcsetattr(0,TCSANOW,&t);}
 if(mode==2){
  for(int i=0;i<100;i++){write(1,"REDRAW",6);usleep(10000);}
  _exit(0);
 }
 write(1,"READY",5);
 if(mode==0){usleep(50000);write(1,"FINISHED",8);}
 if(mode==1){sleep(2);_exit(0);}
 char c=0;ssize_t n=read(0,&c,1);_exit(n==1&&c=='K'?0:1);
}
static void begin(int mode){
 struct winsize w={30,100,0,0};
 g_pid=forkpty(&g_m,NULL,NULL,&w);REQUIRE(g_pid>=0);
 if(!g_pid){write(1,"HEADING",7);child_session(mode);}
 mark();
}
static void stop_child(void){kill(g_pid,SIGKILL);waitpid(g_pid,NULL,0);close(g_m);g_m=-1;}
int main(void){
 begin(0);
 REQUIRE(wait_for("HEADING",1000));
 struct termios t;REQUIRE(tcgetattr(g_m,&t)==0&&(t.c_lflag&ICANON));
 REQUIRE(editor_ready(1500));REQUIRE(strstr(g_out,"FINISHED"));
 REQUIRE(tcgetattr(g_m,&t)==0&&!(t.c_lflag&(ICANON|ECHO)));
 type("K");REQUIRE(finish()==0);
 begin(1);REQUIRE(wait_for("HEADING",1000));
 long started=now_ms();REQUIRE(!editor_ready(150));REQUIRE(now_ms()-started<600);stop_child();
 begin(2);REQUIRE(wait_for("REDRAW",1000));
 started=now_ms();REQUIRE(!editor_ready(150));REQUIRE(now_ms()-started<600);stop_child();
 REQUIRE(g_fail==0);puts("Editor synchronization: delayed raw mode, redraw completion and bounded failure passed");
 return 0;
}
