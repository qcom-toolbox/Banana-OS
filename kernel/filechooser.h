#ifndef FILECHOOSER_H
#define FILECHOOSER_H

#include "types.h"

/*
 * "Choose a file": a panel drawn inside another window (Settings uses it to
 * pick a wallpaper picture and a font to install). Places on the left
 * (Home, Pictures, Downloads, USB sticks, the whole disk), the folder's
 * folders and the files the filter accepts; a double click (or Open)
 * chooses, Esc or Cancel closes it.
 */

typedef int  (*fc_filter_t)(const char* name);
typedef void (*fc_done_t)(const char* path);

void     fc_open(const char* title, const char* start_dir, fc_filter_t filter, fc_done_t done);
int      fc_active(void);
void     fc_close(void);
void     fc_draw(int x, int y, int w, int h);
int      fc_click(int mx, int my);          /* (inside the last drawn panel) */
void     fc_wheel(int dz);
void     fc_key(char c);
int      fc_nav(int code);                  /* kbnav.h keys: arrows, Enter, Esc, Tab (places) */
uint32_t fc_generation(void);               /* changes when it must be drawn again */

#endif
