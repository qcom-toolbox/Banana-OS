#ifndef STARTMENU_H
#define STARTMENU_H

#include "types.h"
#include "fb.h"

/*
 * The Start menu, laid out like Windows 7's: on the left (white) the
 * programs with big icons and their names beside them, "All Programs",
 * and a search box at the bottom; on the right (blue) the user's picture,
 * the places (the home folder, Documents, Pictures, Music, Downloads,
 * Computer), Settings and the like, and the Shut down button with its
 * arrow (Restart, Lock, Exit to the shell). kernel/gui.c shows it and
 * hands it the pointer and the keys while it is open.
 */

/* what the menu asks the desktop to do */
enum { SM_TERMINAL = 1, SM_FILES, SM_BROWSER, SM_NOTEPAD, SM_APPS, SM_TASKMGR, SM_SETTINGS, SM_INSTALL,
       SM_SHUTDOWN, SM_RESTART, SM_LOCK, SM_QUIT };

typedef struct {
    void (*action)(int sm);                     /* SM_*; the desktop closes the menu */
    void (*run_app)(const char* name);          /* an installed app */
    void (*open_path)(const char* path);        /* a folder (Files) or a file (opened as Files would) */
    void (*close)(void);                        /* the menu closes (Esc, a click on nothing) */
    /* a program's icon, `big`: 28 x 28, else 14 x 14 (kernel/gui.c's drawings) */
    void (*program_icon)(int sm, const char* app, int x, int y, int big, uint32_t bg);
    int  (*installer)(void);                    /* "Install Banana OS" is offered (live CD) */
} startmenu_host_t;

void startmenu_init(const startmenu_host_t* host);
void startmenu_reset(void);                     /* opening: the programs, no search */
void startmenu_draw(const fb_info_t* fi);
int  startmenu_contains(const fb_info_t* fi, int mx, int my);
void startmenu_click(const fb_info_t* fi, int mx, int my);
void startmenu_hover(const fb_info_t* fi, int mx, int my);
void startmenu_key(char c);                     /* typing searches, Enter opens */
void startmenu_escape(void);                    /* Esc: clears the search, else closes */
void startmenu_arrow(char code);                /* ESC [ A/B/C/D: up, down, right, left */
void startmenu_wheel(int dz);
uint32_t startmenu_signature(void);
/* the Start button (a round orb) at x, y in a bar of height h */
void startmenu_draw_orb(int x, int y, int h, int pressed, int hover);
#define STARTMENU_ORB_W 44

#endif
