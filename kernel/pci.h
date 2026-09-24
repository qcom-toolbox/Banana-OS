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

/* First device matching vendor:device, returns 1 if found. */
int pci_find(uint16_t vendor, uint16_t device, pci_dev_t* out);

/* Base address register `bar` (0-5): memory BARs return the address,
 * I/O BARs return the port base; *is_io tells which. */
uint32_t pci_bar(const pci_dev_t* d, int bar, int* is_io);

/* Enables I/O + memory decoding and bus mastering (needed for DMA). */
void pci_enable(const pci_dev_t* d);

#endif
