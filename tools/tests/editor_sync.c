/* SPDX-License-Identifier: BSD-2-Clause */
#include "../../sdk/examples/shelltest/interactive.c"
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"editor sync failed at %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void child_session(int mode) {
 struct termios t;
 if(mode!=1){usleep(200000);tcgetattr(0,&t);cfmakeraw(&t);tcsetattr(0,TCSANOW,&t);}
 if(mode==2){
  for(int i=0;i<100;i++){if(write_all(1,"REDRAW",6))_exit(2);usleep(10000);}
  _exit(0);
 }
 if(write_all(1,"READY",5))_exit(2);
 if(mode==0){usleep(50000);if(write_all(1,"FINISHED",8))_exit(2);}
 if(mode==1){sleep(2);_exit(0);}
 if(mode==3){
  char output[3000];memset(output,'A',sizeof(output));
  if(write_all(1,output,sizeof(output))||write_all(1,"\033[?25hTAIL",10))_exit(2);
  for(;;)pause();
 }
 char c=0;ssize_t n=read(0,&c,1);_exit(n==1&&c=='K'?0:1);
}
static void begin(int mode){
 g_raw_head=g_raw_count=g_typed_head=g_typed_count=0;
 struct winsize w={30,100,0,0};
 g_pid=forkpty(&g_m,NULL,NULL,&w);REQUIRE(g_pid>=0);
 if(!g_pid){if(write_all(1,"HEADING",7))_exit(2);child_session(mode);}
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
 begin(3);REQUIRE(wait_for("TAIL",1500));REQUIRE(editor_ready(1000));
 type("\r");
 FILE* report=tmpfile();REQUIRE(report);pty_diagnostic(report);REQUIRE(!fflush(report));
 REQUIRE(!fseek(report,0,SEEK_SET));char diagnostic[8192];
 size_t n=fread(diagnostic,1,sizeof(diagnostic)-1,report);diagnostic[n]=0;REQUIRE(!fclose(report));
 REQUIRE(strstr(diagnostic,"queued input=1"));
 REQUIRE(strstr(diagnostic,"sent (escaped): \\x0d"));
 REQUIRE(strstr(diagnostic,"\\x1b[?25hTAIL"));
 /* Diagnostics must not drain the pending key. */
 char* path=ptsname(g_m);REQUIRE(path);int slave=open(path,O_RDONLY|O_NONBLOCK|O_NOCTTY);
 REQUIRE(slave>=0);char key;REQUIRE(read(slave,&key,1)==1&&key=='\r');close(slave);
 stop_child();
 /* A partial CSI from an earlier session must not eat the next heading. */
 g_esc=2;g_osc=1;
 char* av[]={"true",NULL};REQUIRE(start("/bin/true",av)==0);
 REQUIRE(!g_esc&&!g_osc);REQUIRE(finish()==0);
 REQUIRE(g_fail==0);puts("Editor synchronization: readiness, bounded failure and non-draining PTY diagnostics passed");
 return 0;
}
