#ifndef SHELL_H
#define SHELL_H

void shell_run(void);

/* Runs a full shell instance (its own prompt, history, commands - "help",
 * "top", "edit", etc. all work exactly as in the main console) bound to a
 * single GUI terminal window's virtual terminal `vt`. Never returns
 * (loops for the lifetime of the OS, like shell_run()); meant to be run
 * as its own cooperative task (see kernel/task.h) so each open terminal
 * window makes independent progress instead of sharing one shell's
 * output with whichever window last had focus. */
void shell_run_window(int vt);

#endif
