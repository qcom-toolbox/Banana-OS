#ifndef IDT_H
#define IDT_H

/* Installs handlers for the 32 CPU exception vectors so a fault produces
 * a readable panic screen instead of an unhandled triple-fault reset,
 * and remaps the 8259 PICs to vectors 32-47 with every IRQ masked. */
void idt_init(void);

/* Registers `handler` for hardware IRQ line `irq` (0-15) and unmasks it.
 * Handlers run with interrupts off and must be short: the EOI is sent by
 * the common stub after they return. Interrupts stay globally disabled
 * until the kernel executes `sti` (kernel_main, after timer_init). */
void irq_install(int irq, void (*handler)(void));

#endif
