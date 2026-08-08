#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

void keyboard_init(void);
char keyboard_getchar(void);
char keyboard_try_getchar(void); /* non-blocking: returns 0 if no key */
void keyboard_readline(char* buf, int maxlen);
const char* keyboard_layout_name(void);
const char* keyboard_layouts_help(void);
int keyboard_set_layout(const char* name);

/* One-shot: 1 exactly once per Ctrl+Alt+Delete press, then clears. */
int keyboard_ctrl_alt_del_pending(void);

#endif
