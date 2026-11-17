/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PCI_H
#define PCI_H

#include "types.h"

/* PCI configuration space access (mechanism #1, ports 0xCF8/0xCFC) and
 * a simple bus scan. */

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  irq_line;
} pci_dev_t;

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v);

/* Calls cb for every present function; stops early if cb returns non-zero.
 * Returns what the last cb call returned (0 if the scan completed). */
int pci_scan(int (*cb)(const pci_dev_t* d, void* ctx), void* ctx);
/* the functions the first scan found (every scan after it visits only
 * those): how many, -1 before it, -2 if too many to keep; the scans so far */
int      pci_function_count(void);
uint32_t pci_scan_count(void);
/* selftest: a full scan now finds exactly those - how many, or -1 (msg) */
int      pci_check_list(char* msg, int cap);

/* First device matching vendor:device, returns 1 if found. */
int pci_find(uint16_t vendor, uint16_t device, pci_dev_t* out);

/* Base address register `bar` (0-5): memory BARs return the address (one
 * above 4 GiB is mapped first: kernel/paging.h; 0 if out of reach),
 * I/O BARs return the port base; *is_io tells which. */
uintptr_t pci_bar(const pci_dev_t* d, int bar, int* is_io);

/* the size of memory BAR `bar` (bytes, 0 if none / not a memory BAR): the
 * standard probe - all ones written, read back, the BAR restored - with the
 * function's memory decoding off for those few cycles (nothing reaches it) */
uint64_t pci_bar_size(const pci_dev_t* d, int bar);

/* Enables I/O + memory decoding and bus mastering (needed for DMA). The
 * driver taking the device is remembered by its source file ("net/e1000.c"
 * -> "e1000"): Device Manager shows it. */
void pci_enable_from(const pci_dev_t* d, const char* file);
#define pci_enable(d) pci_enable_from((d), __FILE__)
/* the driver that enabled bus:dev.fn ("e1000"), or NULL */
const char* pci_driver_of(uint8_t bus, uint8_t dev, uint8_t fn);
/* a driver that drives a device without pci_enable() says so */
void pci_claim(const pci_dev_t* d, const char* driver);

#endif
