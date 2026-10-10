#ifndef BUILTIN_APPS_H
#define BUILTIN_APPS_H

/* The apps that come with Banana OS (Media Player, Music, Amethyst Music: apps/), built
 * into the kernel. At boot each is installed into /apps - or upgraded when
 * this kernel has a newer version; one that was removed stays removed
 * until a new version comes. */
void builtin_apps_install(void);

#endif
