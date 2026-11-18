/* SPDX-License-Identifier: MIT
 * Banana OS: what libdrm's radeon_surface.c takes from libdrm_macros.h */
#ifndef LIBDRM_LIBDRM_H
#define LIBDRM_LIBDRM_H
#define drm_private __attribute__((visibility("hidden")))
#define drm_public
#endif
