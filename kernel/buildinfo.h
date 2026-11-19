/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BUILDINFO_H
#define BUILDINFO_H
const char* banana_build(void);              /* "357" (the release build-357), or "dev" */
const char* banana_commit(void);             /* the git commit, 7 characters */
#endif
