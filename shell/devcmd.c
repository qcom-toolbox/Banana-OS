/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "devcmd.h"
#include "../kernel/devices.h"
#include "../kernel/pci.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/kheap.h"
#include "../kernel/gui.h"
#include "../kernel/gpu.h"
#include "../kernel/gpu_i915.h"
#include "../usb/usbcore.h"

/*
 * devmgr                 every device, by type (! = no driver)
 * devmgr -c              ... by connection (each under its bridge, controller or hub)
 * devmgr show N|TEXT     one device's properties (its number in the list, or part of its name)
 * devmgr problems        the devices without a driver
 * devmgr rescan          look for USB devices plugged in or out, then list
 * devmgr gui             the Device Manager window (devmgmt.msc too)
 * lspci [-v] [-n]        the PCI functions, as Linux's lspci
 */

static void say(const char* s) { terminal_writeln(s); }

static device_t* scan(int* n) {
    device_t* d = (device_t*)kmalloc(sizeof(device_t) * DEV_MAX);
    if (!d) { say("devmgr: out of memory"); *n = 0; return NULL; }
    *n = devices_scan(d, DEV_MAX);
    return d;
}

static void line_of(const device_t* d, int idx, int indent, char* out, int cap) {
    char pad[24];
    if (indent > (int)sizeof(pad) - 1) indent = (int)sizeof(pad) - 1;
    for (int i = 0; i < indent; i++) pad[i] = ' ';
    pad[indent] = 0;
    ksnprintf(out, (size_t)cap, "%s%c %3d  %-44s %s", pad, d->status == DS_NODRIVER ? '!' : ' ', idx,
              d->name, d->driver[0] ? d->driver : d->status == DS_BUILTIN ? "(none needed)" : "(no driver)");
}

/* q in s, any case */
static int has_text(const char* s, const char* q) {
    int n = (int)strlen(q);
    for (; *s; s++) if (!strncasecmp(s, q, (size_t)n)) return 1;
    return 0;
}

static void by_type(device_t* d, int n, int problems_only) {
    char b[160];
    for (int c = 0; c < DC_COUNT; c++) {
        int any = 0;
        for (int i = 0; i < n; i++) {
            if (d[i].cat != c || (problems_only && d[i].status != DS_NODRIVER)) continue;
            if (!any) { terminal_write_color(device_category_name(c), VGA_COLOR_YELLOW, VGA_COLOR_BLACK); terminal_writeln(""); any = 1; }
            line_of(&d[i], i, 2, b, sizeof(b));
            if (d[i].status == DS_NODRIVER) { terminal_write_color(b, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK); terminal_writeln(""); }
            else say(b);
        }
    }
    if (problems_only) {
        int k = 0;
        for (int i = 0; i < n; i++) if (d[i].status == DS_NODRIVER) k++;
        if (!k) say("Every device has a driver (or needs none).");
    }
}

static void conn_tree(device_t* d, int n, int parent, int depth) {
    char b[160];
    for (int i = 0; i < n; i++) {
        if (d[i].parent != parent || d[i].cat == DC_COMPUTER) continue;
        line_of(&d[i], i, 2 + depth * 2, b, sizeof(b));
        say(b);
        if (depth < 8) conn_tree(d, n, i, depth + 1);
    }
}

static void show(const device_t* d, int idx) {
    char b[160];
    ksnprintf(b, sizeof(b), "%d: %s", idx, d->name);
    terminal_write_color(b, VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    terminal_writeln("");
    ksnprintf(b, sizeof(b), "  Device type:   %s", device_category_name(d->cat)); say(b);
    if (d->maker[0]) { ksnprintf(b, sizeof(b), "  Manufacturer:  %s", d->maker); say(b); }
    if (d->loc[0]) { ksnprintf(b, sizeof(b), "  Location:      %s", d->loc); say(b); }
    ksnprintf(b, sizeof(b), "  Status:        %s", device_status_text(d)); say(b);
    ksnprintf(b, sizeof(b), "  Driver:        %s", d->driver[0] ? d->driver : d->status == DS_BUILTIN ? "(none needed)" : "(none)"); say(b);
    if (d->hwid[0]) { ksnprintf(b, sizeof(b), "  Hardware ID:   %s", d->hwid); say(b); }
    if (d->key[0]) { ksnprintf(b, sizeof(b), "  Drive:         %s (lsblk, mount, eject)", d->key); say(b); }
    for (int r = 0; r < d->nres; r++) { ksnprintf(b, sizeof(b), "  %s", d->res[r]); say(b); }
}

static int lspci_cb(const pci_dev_t* p, void* ctx) {
    int flags = *(int*)ctx;
    char name[64], b[200];
    pci_device_name(p, name, sizeof(name));
    const char* v = pci_vendor_name(p->vendor);
    const char* drv = pci_driver_of(p->bus, p->dev, p->fn);
    if (flags & 2)
        ksnprintf(b, sizeof(b), "%02x:%02x.%x %02x%02x: %04x:%04x", p->bus, p->dev, p->fn, p->class_code, p->subclass, p->vendor, p->device);
    else
        ksnprintf(b, sizeof(b), "%02x:%02x.%x %s: %s %s [%04x:%04x]", p->bus, p->dev, p->fn,
                  pci_class_name(p->class_code, p->subclass, p->prog_if), v ? v : "", name, p->vendor, p->device);
    say(b);
    if (flags & 1) {
        if (p->irq_line && p->irq_line != 0xFF) { ksnprintf(b, sizeof(b), "\tInterrupt: IRQ %d", p->irq_line); say(b); }
        for (int i = 0; i < 6; i++) {
            uint32_t raw = pci_read32(p->bus, p->dev, p->fn, (uint8_t)(0x10 + i * 4));
            if (!raw) continue;
            if (raw & 1) ksnprintf(b, sizeof(b), "\tRegion %d: I/O ports at %04x", i, raw & ~3u);
            else ksnprintf(b, sizeof(b), "\tRegion %d: Memory at %08x", i, raw & ~15u);
            say(b);
            if (!(raw & 1) && ((raw >> 1) & 3) == 2) i++;
        }
        ksnprintf(b, sizeof(b), "\tKernel driver in use: %s", drv ? drv : "(none)");
        say(b);
    }
    return 0;
}

static int g_tests_ok, g_tests_n;
static void report_test(const char* name, int ok, const char* detail) {
    char line[320];
    ksnprintf(line, sizeof(line), "  %s  %s%s%s", ok ? "ok  " : "FAIL", name, detail && detail[0] ? ": " : "", detail ? detail : "");
    say(line);
    g_tests_n++;
    g_tests_ok += ok != 0;
}

int devcmd_dispatch(int argc, char** argv) {
    if (argc < 1) return 0;
    if (!strcmp(argv[0], "lspci")) {
        int flags = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "-vv")) flags |= 1;
            else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "-nn")) flags |= 2;
        }
        pci_scan(lspci_cb, &flags);
        return 1;
    }
    if (!strcmp(argv[0], "devmgmt.msc") || !strcmp(argv[0], "devmgmt")) {
        if (!gui_open_devmgr()) say("devmgr: the Device Manager window is part of the desktop (startx); here: devmgr");
        return 1;
    }
    if (!strcmp(argv[0], "gpu3d")) {
        const char* sub = argc > 1 ? argv[1] : "";
        char buf[400];
        if (!strcmp(sub, "test")) {
            say("gpu3d: testing the render engine (if the screen freezes, the GPU hung: note what was printed last)");
            g_tests_ok = g_tests_n = 0;
            i915_tests(report_test);
            ksnprintf(buf, sizeof(buf), "gpu3d: %d of %d passed", g_tests_ok, g_tests_n);
            say(buf);
            return 1;
        }
        if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
            gem_set_enabled(sub[1] == 'n');
            say(sub[1] == 'n' ? "gpu3d: on - new OpenGL apps may use Intel acceleration; glinfo reports the actual renderer"
                              : "gpu3d: off - OpenGL apps started from now on draw on the processor");
            if (sub[1] == 'n' && !i915_present()) { terminal_write("gpu3d: but "); say(i915_why()); }
            return 1;
        }
        if (sub[0] && strcmp(sub, "status")) { say("usage: gpu3d [status | on | off | test]   (Intel graphics, 1st - 10th generation Core)"); return 1; }
        say(gem_enabled() ? "gpu3d: on (Intel acceleration allowed; glinfo reports the actual renderer)" : "gpu3d: off (OpenGL apps draw on the processor; gpu3d on to use the GPU again)");
        if (!i915_present()) { terminal_write("gpu3d: "); say(i915_why()); return 1; }
        i915_info(buf, sizeof(buf));
        say(buf);
        gem_stats(buf, sizeof(buf));
        say(buf);
        return 1;
    }
    if (!strcmp(argv[0], "gpu")) {
        const char* w = argc > 2 ? argv[2] : "";
        if (argc < 2 || strcmp(argv[1], "experimental")) { say("usage: gpu experimental [on | off]   (lsgpu: the graphics card)"); return 1; }
        if (!strcmp(w, "on")) {
            gpu_set_experimental(1);
            say("gpu: experimental drivers on (AMD Radeon HD 5000-7000, RX 400/500: hardware cursor and vsync)");
            if (gpu_radeon_try() == 0) say("gpu: the Radeon driver runs now");
            else { terminal_write("gpu: not used here: "); say(gpu_radeon_status()); }
        } else if (!strcmp(w, "off")) {
            gpu_set_experimental(0);
            say("gpu: experimental drivers off from the next start");
        } else {
            terminal_write(gpu_experimental() ? "gpu: experimental drivers on - " : "gpu: experimental drivers off - ");
            say(gpu_radeon_status());
        }
        return 1;
    }
    if (strcmp(argv[0], "devmgr") && strcmp(argv[0], "lsdev")) return 0;
    const char* sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "gui")) {
        if (!gui_open_devmgr()) say("devmgr: the Device Manager window is part of the desktop (startx)");
        return 1;
    }
    if (!strcmp(sub, "rescan")) { usb_rescan(); sub = ""; }
    int n;
    device_t* d = scan(&n);
    if (!d) return 1;
    if (!sub[0] || !strcmp(sub, "-t")) by_type(d, n, 0);
    else if (!strcmp(sub, "-c")) {
        say(d[0].name);
        conn_tree(d, n, -1, 0);
    } else if (!strcmp(sub, "problems")) by_type(d, n, 1);
    else if (!strcmp(sub, "show") && argc > 2) {
        const char* q = argv[2];
        int idx = -1;
        uint32_t v;
        int k = k_parse_u32(q, &v);
        if (k > 0 && !q[k]) idx = (int)v;
        else
            for (int i = 0; i < n && idx < 0; i++) if (has_text(d[i].name, q) || !strcasecmp(d[i].driver, q)) idx = i;
        if (idx < 0 || idx >= n) say("devmgr: no such device (devmgr lists them with their numbers)");
        else show(&d[idx], idx);
    } else say("usage: devmgr [-c | show N|NAME | problems | rescan | gui]   lspci [-v] [-n]");
    kfree(d);
    return 1;
}
