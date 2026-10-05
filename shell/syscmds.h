#ifndef SYSCMDS_H
#define SYSCMDS_H

/* Storage, apps and sound (shell/syscmds.c): `mount`, `umount`, `pkg`,
 * installed apps by name, `play`, `beep`, `volume`, `lsaudio`.
 * Returns 1 if `line` was one of them (and ran it). */
int syscmd_dispatch(const char* line);

#endif
