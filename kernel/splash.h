#ifndef SPLASH_H
#define SPLASH_H

/*
 * The boot screen: a banana with three dots taking turns under it, from
 * the moment the kernel can draw until the system is ready (the login
 * screen, the desktop or the shell prompt). The console's text is kept
 * meanwhile and shows up when the splash ends.
 */

void splash_start(void);     /* kernel_main, once the timer runs */
void splash_end(void);       /* the screen goes back to the console */
int  splash_active(void);
void splash_screen_changed(void);   /* a new resolution: drawn again at it */
void splash_tick(void);      /* the timer interrupt: the dots */

#endif
