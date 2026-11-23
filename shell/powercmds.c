/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "powercmds.h"
#include "laptop.h"
#include "terminal.h"
#include "kstring.h"
#include "timer.h"
static void line(const char* fmt,...) __attribute__((format(printf, 1, 2)));
static void line(const char* fmt,...) {
    char b[256];__builtin_va_list ap;__builtin_va_start(ap,fmt);
    kvsnprintf(b,sizeof(b),fmt,ap);__builtin_va_end(ap);terminal_write(b);
}
void cmd_laptop(const char* args) {
    (void)args;laptop_info_t p;laptop_get_info(&p);
    line("ACPI AML: %s (%s), EC: %s\n",p.runtime?"ready":"unavailable",p.status,p.ec?"present":"none");
    line("AC adapter: %s, lid: %s\n",!p.ac_present?"not reported":p.ac_online?"online":"offline",p.lid<0?"not reported":p.lid?"open":"closed");
    if(!p.battery_count) terminal_writeln("Battery: no ACPI battery reported");
    for(int i=0;i<p.battery_count;i++) {
        laptop_battery_t* b=&p.batteries[i];
        if(!b->present) { line("BAT%d: absent\n",i);continue; }
        line("BAT%d: %s %s\n",i,b->model,b->chemistry);
        if(!b->valid) { terminal_writeln("  telemetry unavailable");continue; }
        line("  %d%%, %s%s, %u mV\n",b->percent,b->state&2?"charging":b->state&1?"discharging":"idle",b->state&4?" (critical)":"",b->millivolts);
        line("  remaining/full/design: %u/%u/%u %s, rate %u %s\n",
             b->remaining,b->full,b->design,b->unit?"mAh":"mWh",b->rate,b->unit?"mA":"mW");
        if(b->minutes>=0) line("  estimated remaining: %d minutes\n",b->minutes);
    }
    if(p.brightness>=0) line("Panel backlight: %d%%%s\n",p.brightness,p.brightness_pending?" (change queued)":"");
    line("AML allocations: %u KiB; sample age: %u ms\n",p.aml_bytes/1024,(uint32_t)(timer_ms()-p.updated_ms));
}
void cmd_usbc(const char* args) {
    while(*args==' ') args++;
    if(!strncmp(args,"role ",5)) {
        args+=5;while(*args==' ') args++;
        char n[8];unsigned len=0;
        while(*args && *args!=' ' && len<sizeof(n)-1) n[len++]=*args++;
        n[len]=0;while(*args==' ') args++;
        unsigned port;
        if(!k_parse_u32(n,&port) || (strcmp(args,"source") && strcmp(args,"sink"))) {
            terminal_writeln("Usage: usbc role PORT source|sink");return;
        }
        if(laptop_usbc_role(port,!strcmp(args,"source"))<0) terminal_writeln("usbc: no ready port or a request is already pending");
        else terminal_writeln("usbc: power-role request queued; firmware may reject it. Run usbc to check.");
        return;
    }
    if(*args) { terminal_writeln("Usage: usbc [role PORT source|sink]");return; }
    laptop_info_t p;laptop_get_info(&p);ucsi_t* u=&p.usbc;
    if(!u->ready) {
        if(u->version) line("USB-C: UCSI version %x is unsupported or failed to initialize\n",u->version);
        else terminal_writeln("USB-C: no ready ACPI UCSI interface");
        return;
    }
    line("UCSI %x, USB PD %x, Type-C %x, %u connector(s)%s\n",u->version,u->pd_version,u->typec_version,u->num_ports,u->failed?" (transport failed)":"");
    if(u->role_result) line("Last role request: port %u %s, %s\n",u->role_port,
        u->role_source?"source":"sink",u->role_result>0?"command accepted (check actual role below)":"failed");
    if(u->last_error) {
        line("Last command failure: 0x%x, %s\n",u->error_command,ucsi_error_text(u->last_error));
        if(u->last_error==UCSI_ERROR_FIRMWARE) line("  PPM 0x%x: %s\n",u->ppm_error,ucsi_ppm_error_text(u->ppm_error));
    }
    for(unsigned i=0;i<u->num_ports;i++) {
        ucsi_port_t* c=&u->ports[i];
        if(!c->valid) { line("USB-C %u: status unavailable\n",i+1);continue; }
        if(!c->connected) { line("USB-C %u: disconnected\n",i+1);continue; }
        const char* mode=c->mode==3?"USB Power Delivery":c->mode==4?"Type-C 1.5 A":c->mode==5?"Type-C 3 A":c->mode==2?"Battery Charging":"USB default";
        line("USB-C %u: connected, %s, %s\n",i+1,c->source?"source (supplying power)":"sink (receiving power)",mode);
        if(c->mode==3) {
            if(c->contract_valid) {
                if(c->millivolts) line("  contract: %u mV, %u mA, %u mW\n",c->millivolts,c->milliamps,c->milliwatts);
                else line("  contract: %u mW (voltage not reported)\n",c->milliwatts);
            } else line("  contract details unavailable; RDO 0x%x\n",c->rdo);
        }
        line("  battery charging: %s\n",c->charging==1?"nominal":c->charging==2?"slow":c->charging==3?"trickle":"not charging");
    }
    line("Last snapshot: %u ms ago; command errors: %u\n",(uint32_t)(timer_ms()-u->last_ms),u->errors);
    terminal_writeln("PD policy is managed by firmware. USB4/Thunderbolt and display Alt Modes require separate drivers.");
}
