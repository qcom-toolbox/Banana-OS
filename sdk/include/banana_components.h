/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BANANA_COMPONENTS_H
#define BANANA_COMPONENTS_H
/* Internal packages backing integrated OS features, not application entries.
 * Shared by kernel launchers/settings and the user-space App Store. */
static inline int banana_system_component(const char* name) {
    static const char* const names[] = { "terminal", "bash", "zsh", "webworker", "libjsworker", "webmedia", "webgl" };
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char* a = name;
        const char* b = names[i];
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}
#endif
