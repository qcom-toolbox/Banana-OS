#ifndef MONCMDS_H
#define MONCMDS_H

/* Watching the machine (shell/moncmds.c): `free` (RAM), `df` (disk space)
 * and `htop` (the colourful `top`). Returns 1 if `line` was one of them
 * (and ran it). */
int moncmd_dispatch(const char* line);

#endif
