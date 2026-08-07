#ifndef IDT_H
#define IDT_H

/* Installs handlers for the 32 CPU exception vectors so a fault produces
 * a readable panic screen instead of an unhandled triple-fault reset. */
void idt_init(void);

#endif
