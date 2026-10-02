#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "types.h"

/* The system-wide clipboard: one text, shared by terminals, the editor
 * and the web browser. */

#define CLIPBOARD_MAX (64u * 1024u)

void        clipboard_set(const char* text, uint32_t len);
const char* clipboard_get(uint32_t* len);     /* "" when empty, never NULL */
uint32_t    clipboard_generation(void);       /* bumped on every change */

#endif
