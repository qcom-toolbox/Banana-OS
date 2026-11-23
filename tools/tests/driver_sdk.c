/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual SDK adapters + USB enumeration/rebinding, simulated host controller.
 * No RF electronics or physical ACPI/PD validation is implied. */
#include "../../kernel/types.h"
#include "../../kernel/kstring.h"
extern int printf(const char*,...);
extern int fflush(void*);
extern void abort(void);
extern void* calloc(size_t,size_t);
extern void free(void*);
static unsigned checks;
#define CHECK(c) do { checks++;if(!(c)){printf("FAIL %d: %s\n",__LINE__,#c);fflush(0);abort();} }while(0)
#define IO_H
static uint32_t io_flags=0x200,config=0xabcd0007,config_address;
static unsigned config_width;
static uint32_t irq_save(void) { uint32_t f=io_flags;io_flags=0;return f; }
static void irq_restore(uint32_t f) { io_flags=f; }
static void outl(uint16_t p,uint32_t v) {
    if(p==0xcf8) config_address=v;
    else { CHECK(p==0xcfc);config=v;config_width=4; }
}
static void outw(uint16_t p,uint16_t v) {
    CHECK(p==0xcfc || p==0xcfe);unsigned shift=(p-0xcfc)*8;
    config=(config&~(65535u<<shift))|((uint32_t)v<<shift);config_width=2;
}
static void outb(uint16_t p,uint8_t v) {
    CHECK(p>=0xcfc && p<=0xcff);unsigned shift=(p-0xcfc)*8;
    config=(config&~(255u<<shift))|((uint32_t)v<<shift);config_width=1;
}
static uint8_t inb(uint16_t p) { (void)p;return 0; }
static uint16_t inw(uint16_t p) { (void)p;return 0; }
static uint32_t inl(uint16_t p) { (void)p;return 0; }
#include "../../kernel/driver.c"
#include "../../usb/usbcore.c"
#include "../../kernel/driver_ext.c"
static uint32_t clock_ms;
static unsigned allocs,native_rx,wifi_rx_count,submits,binds,detaches,task_calls,firmware_pins;
static int fail_task,power_fail,light,usb_finish,live_nics;
static netdev_t* last_network;
static bdrv_handle_t eth_handle,wifi_handle;
static usb_xfer_t* pending;
static fs_file_t firmware;
static laptop_info_t native_power;
static uint16_t vendor=0x1234,product=0xabcd;
static int builtin_probe(usb_device_t* d) { return d->desc.idVendor==0xbeef?0:-1; }
static int builtin_attach(usb_device_t* d) { (void)d;return 0; }
#define BUILTIN(n) const usb_driver_t n={#n,builtin_probe,builtin_attach,NULL,NULL}
BUILTIN(hub_driver);BUILTIN(mt7601u_driver);BUILTIN(rtl8188eu_driver);BUILTIN(rtl8811cu_driver);BUILTIN(r8152_driver);BUILTIN(r8153_driver);
BUILTIN(cdc_ncm_driver);BUILTIN(ax88179_driver);BUILTIN(ax88179a_driver);BUILTIN(cdc_ecm_driver);BUILTIN(xpad_driver);
BUILTIN(uvc_driver);BUILTIN(uaudio_driver);BUILTIN(adb_driver);BUILTIN(hid_driver);BUILTIN(usb_storage_driver);
void usbhid_describe(usb_device_t* d,char* out,int cap) { (void)d;if(cap>0)out[0]=0; }
void* kmalloc(size_t n) { allocs++;return calloc(1,n); }
void kfree(void* p) { if(p) { allocs--;free(p); } }
void klog(const char* fmt,...) { (void)fmt; }
int mmio_map(uint64_t address,uint64_t size) { return address && size; }
uint32_t pci_read32(uint8_t b,uint8_t d,uint8_t f,uint8_t offset) {
    (void)b;(void)d;(void)f;(void)offset;return config;
}
int task_create(const char* name,void (*entry)(void)) { CHECK(name && entry);return fail_task?-1:8; }
int task_current_pid(void) { return 7; }
void task_sleep_ms(uint32_t n) { clock_ms+=n; }
void task_set_background(void) {}
int task_create_stack(const char* name,void (*fn)(void),uint32_t bytes) {
    CHECK(name && fn && bytes>=16384);task_calls++;return fail_task?-1:10;
}
uint32_t timer_ms(void) { return clock_ms; }
void timer_sleep_ms(uint32_t n) { clock_ms+=n; }
void timer_idle(void) {}
void net_register_device(netdev_t* n) { n->present=1;last_network=n;live_nics++; }
void net_unregister_device(netdev_t* n) { n->present=0;live_nics--; }
void net_rx(netdev_t* n,const uint8_t* f,uint32_t len) { CHECK(n->present && f && len==64);native_rx++; }
int wifi_register(wifi_radio_t* r) { CHECK(r->ops);r->state=WIFI_DOWN;r->nd.present=1;return 0; }
void wifi_unregister(wifi_radio_t* r) { r->ops->station(r,NULL,0);r->nd.present=0; }
void wifi_rx(wifi_radio_t* r,const uint8_t* f,uint32_t n,int rssi) { CHECK(r->nd.present && f && n==32 && rssi==-60);wifi_rx_count++; }
void laptop_get_info(laptop_info_t* out) { *out=native_power;driver_power_merge(out); }
int laptop_usbc_role(unsigned port,int source) { return port==1 && source==0?0:-1; }
int gpu_backlight(int percent) { return driver_power_backlight(percent); }
int fs_find_file(const char* path) { return !strcmp(path,"/apps/test/fw.bin")?0:-1; }
fs_file_t* fs_get_file(int i) { CHECK(i==0);return &firmware; }
void fs_pin(int i) { CHECK(i==0);firmware_pins++; }
void fs_unpin(int i) { CHECK(i==0 && firmware_pins);firmware_pins--; }
static int receive_eth(void* ctx) {
    CHECK(ctx==(void*)1);uint8_t f[64]={0};
    CHECK(!net_receive(eth_handle,f,sizeof(f)));return 1;
}
static int send_eth(void* ctx,const void* f,uint32_t n) { CHECK(ctx==(void*)1 && f && n==64);return 0; }
static int link_eth(void* ctx) { CHECK(ctx==(void*)1);return 1; }
static int receive_wifi(void* ctx) {
    CHECK(ctx==(void*)2);uint8_t f[32]={0};
    CHECK(!wifi_receive(wifi_handle,f,32,-60));return 1;
}
static int send_wifi(void* ctx,const uint8_t* f,uint32_t n) { CHECK(ctx==(void*)2 && f && n==32);return 0; }
static int channel_wifi(void* ctx,unsigned ch) { CHECK(ctx==(void*)2 && ch==6);return 0; }
static int station_wifi(void* ctx,const uint8_t* bssid,unsigned aid) { CHECK(ctx==(void*)2);(void)bssid;(void)aid;return 0; }
static const uint8_t descriptors[]={9,2,25,0,1,1,0,0x80,50,9,4,0,0,1,0xff,0,0,0,7,5,0x81,2,64,0,0};
static int hc_control(usb_device_t* d,const usb_setup_t* s,void* out,uint32_t timeout) {
    (void)d;CHECK(timeout==2000);
    if(s->bRequest==USB_REQ_GET_DESCRIPTOR) {
        if((s->wValue>>8)==USB_DT_DEVICE) {
            usb_device_desc_t dev={18,1,0x200,0xff,0,0,64,vendor,product,0x100,0,0,0,1};
            unsigned n=s->wLength<sizeof(dev)?s->wLength:sizeof(dev);memcpy(out,&dev,n);return (int)n;
        }
        if((s->wValue>>8)==USB_DT_CONFIG) {
            unsigned n=s->wLength<sizeof(descriptors)?s->wLength:sizeof(descriptors);memcpy(out,descriptors,n);return (int)n;
        }return -1;
    }
    if(s->bRequest==USB_REQ_SET_CONFIGURATION) return 0;
    CHECK(s->bRequest==0x55 && s->wLength==4);memset(out,0xaa,4);return 4;
}
static int hc_mps(usb_device_t* d,uint16_t mps) { (void)d;CHECK(mps==64);return 0; }
static int hc_open(usb_device_t* d,const usb_ep_desc_t* e) { CHECK(d->present && e->bEndpointAddress==0x81);return 0; }
static int hc_submit(usb_xfer_t* t) { CHECK(t->status==USB_XFER_PENDING);submits++;pending=t;return 0; }
static void hc_poll(usb_hc_t* hc) {
    (void)hc;if(pending && usb_finish) { pending->status=USB_XFER_OK;pending->actual=pending->len;pending=NULL; }
}
static const usb_hc_ops_t hc_ops={hc_control,hc_mps,hc_open,hc_submit,hc_poll,NULL,NULL,NULL,NULL};
static usb_hc_t hc={"model",&hc_ops,NULL,1,"USB SDK model",0};
static bdrv_handle_t last_usb;
static int module_probe(void* ctx,bdrv_handle_t h) {
    CHECK(ctx==(void*)3);bdrv_usb_info_t info={.size=sizeof(info)};
    CHECK(!usb_information(h,&info) && info.vendor==vendor && info.product==product);
    CHECK(usb_configuration(h,0,NULL,0)==25);last_usb=h;return 0;
}
static int module_attach(void* ctx,bdrv_handle_t h,void** devctx) {
    CHECK(ctx==(void*)3 && h==last_usb);binds++;*devctx=(void*)4;return 0;
}
static void module_detach(void* ctx) { CHECK(ctx==(void*)4);detaches++; }
static int power_sample(void* ctx,bdrv_power_sample_t* out) {
    CHECK(ctx==(void*)5 && out->size==sizeof(*out));
    out->flags=15;out->ac_online=1;out->lid_open=1;out->brightness=light;out->battery_count=1;
    out->batteries[0].present=out->batteries[0].valid=1;out->batteries[0].percent=72;
    out->batteries[0].minutes=120;out->batteries[0].remaining=36000;out->batteries[0].full=50000;
    kstrlcpy(out->batteries[0].model,"SDK battery",48);
    return power_fail?-1:0;
}
static int backlight(void* ctx,int value) { CHECK(ctx==(void*)5);light=value;return value; }
typedef struct { uint32_t magic,version,size;const char* arch;const char* os;void (*slots[20])(void); } frozen_v1;
_Static_assert(sizeof(frozen_v1)==BDRV_API_V1_SIZE,"original version-1 table size changed");
_Static_assert(__builtin_offsetof(frozen_v1,slots)==__builtin_offsetof(banana_driver_api_t,log),"v1 log offset");
int main(void) {
    bdrv_pci_t pci={.bus=0,.dev=1,.fn=0};
    d_pci_write(&pci,4,2,5);CHECK(config==0xabcd0005 && config_width==2 && io_flags==0x200 && config_address==0x80000804);
    d_pci_write(&pci,5,1,0x12);CHECK(config==0xabcd1205 && config_width==1);
    d_pci_write(&pci,5,2,0);CHECK(config==0xabcd1205);
    d_pci_write(&pci,255,4,0);CHECK(config==0xabcd1205);
    d_pci_write(NULL,4,2,0);CHECK(config==0xabcd1205);
    CHECK(d_pci_read(&pci,4,2)==0x1205 && d_pci_read(&pci,5,2)==0xffffffffu);
    CHECK(!d_alloc_dma(~0ul,4096,NULL) && !d_alloc_dma(32,33,NULL));
    CHECK(!d_mmio_map(~(uint64_t)0-3,8) && !d_mmio_map(0,4096));
    CHECK(d_task_create("invalid",NULL,NULL)<0);
    frozen_v1* old=calloc(1,sizeof(*old));old->magic=BANANA_DRV_MAGIC;old->version=1;old->size=sizeof(*old);
    CHECK(!bdrv_extension((const banana_driver_api_t*)old,BDRV_EXT_WIFI,1,sizeof(bdrv_wifi_api_t)));free(old);
    banana_driver_api_t api={.magic=BANANA_DRV_MAGIC,.version=1,.size=sizeof(api),.extension=driver_extension};
    CHECK(bdrv_extension(&api,BDRV_EXT_WIFI,1,sizeof(bdrv_wifi_api_t))==&wifi_api);
    CHECK(!driver_extension(BDRV_EXT_WIFI,2,0) && !driver_extension(BDRV_EXT_WIFI,1,sizeof(wifi_api)+1));
    CHECK(!driver_extension(0,1,0));
    bdrv_net_device_t eth={.size=sizeof(eth),.name="sdk-eth",.model="SDK Ethernet",.ctx=(void*)1,
        .mac={2,1,2,3,4,5},.send=send_eth,.poll=receive_eth,.link_up=link_eth};
    eth_handle=net_add(&eth);CHECK(eth_handle && live_nics==1);
    uint8_t frame[64]={0};CHECK(net_receive(eth_handle,frame,64)<0);CHECK(last_network->poll(last_network)==1 && native_rx==1);
    CHECK(!last_network->send(last_network,frame,64));CHECK(last_network->send(last_network,frame,1515)<0);
    eth.mac[0]=1;CHECK(!net_add(&eth));
    net_remove(eth_handle);CHECK(!live_nics && net_receive(eth_handle,frame,64)<0);
    bdrv_wifi_radio_t wifi={.size=sizeof(wifi),.name="sdk-wifi",.model="SDK radio",.ctx=(void*)2,
        .mac={2,4,3,2,1,0},.poll=receive_wifi,.send=send_wifi,.set_channel=channel_wifi,.set_station=station_wifi};
    wifi_handle=wifi_add(&wifi);CHECK(wifi_handle);
    module_wifi_t* w=radio(wifi_handle);CHECK(w && w->native.state==WIFI_DOWN);
    CHECK(wifi_receive(wifi_handle,frame,32,-60)<0);
    CHECK(w->native.ops->poll(&w->native)==1 && wifi_rx_count==1);
    CHECK(!w->native.ops->channel(&w->native,6));
    CHECK(!w->native.ops->send(&w->native,frame,32));
    wifi_remove(wifi_handle);CHECK(!radio(wifi_handle) && wifi_receive(wifi_handle,frame,32,-60)<0);
    /* Register after enumeration: native usbd rebinds the unclaimed device. */
    g_usbd_pid=7;CHECK(!usb_register_hc(&hc));
    usb_device_t* d=usb_new_device(&hc,1,USB_SPEED_HIGH,1,64,NULL);
    CHECK(d && !d->driver);
    const bdrv_usb_id_t ids[]={{0x1234,0xabcd}};
    bdrv_usb_driver_t usb={sizeof(usb),"sdk-usb",ids,1,(void*)3,module_probe,module_attach,NULL,module_detach};
    CHECK(!usb_add(&usb));usb_poll();CHECK(d->driver==&usb_module_driver && binds==1);
    CHECK(usb_add(&usb)<0); /* overlapping IDs do not steal an existing binding */
    bdrv_handle_t first=last_usb;bdrv_usb_info_t info={.size=sizeof(info)};
    CHECK(!usb_information(first,&info));CHECK(usb_configuration(first,0,frame,24)<0);
    CHECK(usb_configuration(first,0,frame,sizeof(frame))==25);
    CHECK(usb_configuration(first,4,frame,sizeof(frame))<0);
    CHECK(usb_request(first,0xc0,0x55,0,0,frame,4)==4 && frame[0]==0xaa);
    bdrv_usb_endpoint_t ep={7,5,0x81,2,64,0};CHECK(!usb_endpoint(first,&ep));
    ep.address=0;CHECK(usb_endpoint(first,&ep)<0);ep.address=0x81;
    bdrv_handle_t tx=transfer_create(first,0x81,frame,64);CHECK(tx);
    CHECK(!transfer_submit(tx) && transfer_status(tx,NULL)==BDRV_USB_PENDING);
    CHECK(transfer_submit(tx)<0 && transfer_release(tx)<0);
    usb_finish=1;usb_poll();uint32_t actual=0;
    CHECK(transfer_status(tx,&actual)==0 && actual==64 && !transfer_release(tx));
    CHECK(transfer_submit(tx)<0);
    tx=transfer_create(first,0x81,frame,64);usb_finish=0;CHECK(!transfer_submit(tx));
    usb_device_gone(d);CHECK(detaches==1 && usb_information(first,&info)<0);
    CHECK(transfer_status(tx,NULL)==BDRV_USB_PENDING && transfer_release(tx)<0);
    usb_finish=1;usb_poll();CHECK(!transfer_release(tx));
    d=usb_new_device(&hc,1,USB_SPEED_HIGH,1,64,NULL);
    CHECK(d && binds==2 && last_usb!=first && usb_information(first,&info)<0);
    /* A second registered driver must request another late bind of the same shim. */
    vendor=0x4321;product=0xdcba;usb_device_t* other=usb_new_device(&hc,2,USB_SPEED_HIGH,2,64,NULL);
    CHECK(other && !other->driver);
    const bdrv_usb_id_t ids2[]={{0x4321,0xdcba}};usb.ids=ids2;
    CHECK(!usb_add(&usb));usb_poll();CHECK(other->driver==&usb_module_driver && binds==3);
    vendor=0xbeef;product=1;usb_device_t* built=usb_new_device(&hc,3,USB_SPEED_HIGH,3,64,NULL);
    CHECK(built && built->driver==&hub_driver);
    usb_poll();CHECK(built->driver==&hub_driver);
    /* Reused unbound native slot cannot revive an old SDK device handle. */
    bdrv_handle_t second=last_usb;other->generation++;
    CHECK(usb_information(second,&info)<0);
    usb_device_gone(d);usb_device_gone(other);usb_device_gone(built);
    bdrv_power_provider_t p={sizeof(p),"vendor EC",(void*)5,power_sample,backlight};
    fail_task=1;CHECK(!power_add(&p));fail_task=0;light=60;
    bdrv_handle_t ph=power_add(&p);CHECK(ph && task_calls==2);
    native_power.brightness=-1;native_power.lid=-1;
    power_poll_once();laptop_info_t result;laptop_get_info(&result);
    CHECK(result.battery_count==1 && result.batteries[0].percent==72 && result.ac_online && result.lid==1 && result.brightness==60);
    CHECK(driver_power_backlight(85)==85 && light==60);power_poll_once();CHECK(light==85);
    bdrv_power_sample_t snap={.size=sizeof(snap)};CHECK(!power_snapshot(&snap) && snap.brightness==85);
    native_power.battery_count=1;native_power.batteries[0].percent=40;native_power.brightness=20;
    native_power.ac_present=1;native_power.ac_online=0;native_power.lid=0;
    laptop_get_info(&result);CHECK(result.batteries[0].percent==40 && result.brightness==20 && !result.ac_online && !result.lid);
    clock_ms+=10001;CHECK(driver_power_backlight(-1)<0);
    power_poll_once();power_fail=1;power_poll_once();CHECK(driver_power_backlight(-1)<0);
    power_fail=0;power_poll_once();power_remove(ph);CHECK(driver_power_backlight(-1)<0);
    native_power.usbc.ready=1;native_power.usbc.num_ports=1;native_power.usbc.version=0x120;
    native_power.usbc.ports[0].valid=native_power.usbc.ports[0].connected=1;
    bdrv_usbc_status_t us={.size=sizeof(us)};CHECK(!usbc_snapshot(1,&us) && us.valid && us.connected && us.ucsi_version==0x120);
    CHECK(usbc_snapshot(0,&us)<0);
    firmware.content="FIRMWARE";firmware.size=8;
    CHECK(firmware_read("/apps/test/fw.bin",0,NULL,0)==8);
    CHECK(firmware_read("/apps/test/fw.bin",2,frame,64)==6 && !memcmp(frame,"RMWARE",6));
    CHECK(firmware_read("/apps/test/fw.bin",9,frame,1)<0);
    CHECK(firmware_read("relative",0,frame,1)<0 && !firmware_pins);
    CHECK(firmware_read("/apps/test/fw.bin",8,frame,1)==0);
    for(unsigned i=0;i<nnet;i++) free(networks[i]);
    for(unsigned i=0;i<nradio;i++) free(radios[i]);
    printf("Driver SDK bridges: %u checks passed\n",checks);return 0;
}
