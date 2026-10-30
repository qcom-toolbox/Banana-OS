/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual entry/backend/SDK function bodies, with deterministic storage shims. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "fs.h"
#include "kfile.h"
#include "fsdisk.h"
#include "../../sdk/include/banana_api.h"
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
static kfile_t files[4], *fds[256];
static fs_file_t file;
static fs_dir_t dir;
static int installed = 1, backend_rc, calls, close_during_sync;
static kfile_t* hold(int fd) {
    kfile_t* f = fd >= 0 && fd < 256 ? fds[fd] : NULL;
    if (f) f->refs++;
    return f;
}
static void unhold(kfile_t** f) { if (*f) { CHECK((*f)->refs > 0); (*f)->refs--; } }
#define HOLD(var, fd) __attribute__((cleanup(unhold))) kfile_t* var = hold(fd)
fs_file_t* fs_file_info(int idx) { return idx == 7 ? &file : NULL; }
int fs_find_dir(const char* path) { return !strcmp(path,"/home") ? 8 : -1; }
const fs_dir_t* fs_get_dir(int idx) { return idx == 8 ? &dir : NULL; }
int fsdisk_is_installed(void) { return installed; }
int fsdisk_sync_durable(void) {
    calls++;
    if (close_during_sync) { CHECK(files[0].refs == 2); fds[3] = NULL; files[0].refs--; }
    return backend_rc;
}
#include "sync_entry.inc"

/* Exercise backup freeze both before and during contention on the actual backend. */
static int g_have_target, g_frozen, g_busy, g_e4, g_v5, g_target, g_last_err;
static int freeze_in_wait, flushes, result;
void task_sleep_ms(uint32_t ms) { CHECK(ms == 10); g_busy = 0; if (freeze_in_wait) g_frozen = 1; }
int ext4_flush(void) { flushes++; return result; }
static int v5_sync(const int* p) { (void)p; flushes++; return result; }
static int write_snapshot_to(const int* p) { (void)p; flushes++; return result; }
#include "sync_backend.inc"

static banana_api_t api;
static const banana_api_t* __banana = &api;
#define fsync sdk_fsync
#define fdatasync sdk_fdatasync
#include "sync_sdk.inc"
#undef fsync
#undef fdatasync
_Static_assert(__builtin_offsetof(banana_api_t,fd_sync) == __builtin_offsetof(banana_api_t,poll_mask)+sizeof(void*),"append ABI 31");
int main(void) {
    CHECK(BANANA_API_VERSION >= 31);
    CHECK(a_fd_sync(-1) == -9 && a_fd_sync(256) == -9 && a_fd_sync(3) == -9);
    files[0] = (kfile_t){.type=KF_FILE,.fidx=7,.pos=42,.refs=1}; fds[3] = &files[0];
    CHECK(a_fd_sync(3) == -9); file.used=1;
    CHECK(a_fd_sync(3) == 0 && calls == 1 && files[0].pos == 42 && files[0].refs == 1);
    backend_rc=FSDISK_ERR_IO; CHECK(a_fd_sync(3) == -5);
    backend_rc=FSDISK_ERR_FROZEN; CHECK(a_fd_sync(3) == -16);
    backend_rc=0; installed=0; int live_calls=calls;
    CHECK(a_fd_sync(3) == 0 && calls == live_calls && files[0].pos == 42);
    file.mnt=2; CHECK(a_fd_sync(3) == -38 && calls == live_calls); file.mnt=0;
    installed=1;
    file.mnt=2; CHECK(a_fd_sync(3) == -38); file.mnt=0;
    int before=calls;
    for (int kind=KF_SOCK; kind<=KF_DEV; kind++) {
        files[1]=(kfile_t){.type=kind,.refs=1}; fds[4]=&files[1];
        CHECK(a_fd_sync(4) == -22 && files[1].refs == 1);
    }
    CHECK(calls==before);
    files[2]=(kfile_t){.type=KF_DIR,.path="/home",.refs=1}; fds[5]=&files[2];
    CHECK(a_fd_sync(5) == -9); dir.used=1; CHECK(a_fd_sync(5) == 0);
    installed=0; live_calls=calls;
    CHECK(a_fd_sync(5) == 0 && calls == live_calls);
    dir.mnt=1; CHECK(a_fd_sync(5) == -38 && calls == live_calls); dir.mnt=0; installed=1;
    files[2].path="/deleted"; CHECK(a_fd_sync(5) == -9);
    api.size=__builtin_offsetof(banana_api_t,fd_sync);
    CHECK(sdk_fsync(3)==-1 && errno==ENOSYS);
    api.size=sizeof(api); CHECK(sdk_fsync(3)==-1 && errno==ENOSYS);
    api.fd_sync=a_fd_sync; CHECK(sdk_fdatasync(3)==0);
    backend_rc=FSDISK_ERR_IO; CHECK(sdk_fsync(3)==-1 && errno==EIO);
    backend_rc=FSDISK_ERR_FROZEN; CHECK(sdk_fdatasync(3)==-1 && errno==EBUSY);
    CHECK(sdk_fsync(255)==-1 && errno==EBADF);
    backend_rc=0; close_during_sync=1; CHECK(a_fd_sync(3)==0 && files[0].refs==0 && !fds[3]);
    CHECK(a_fd_sync(3)==-9);
    CHECK(sync_disk(1)==FSDISK_ERR_NO_TARGET);
    g_have_target=1;g_frozen=1; CHECK(sync_disk(0)==0 && flushes==0);
    CHECK(sync_disk(1)==FSDISK_ERR_FROZEN && flushes==0);
    g_frozen=0;g_busy=1;freeze_in_wait=1;
    CHECK(sync_disk(1)==FSDISK_ERR_FROZEN && flushes==0);
    g_frozen=0;freeze_in_wait=0;g_e4=1; CHECK(sync_disk(1)==0 && flushes==1 && !g_busy);
    result=-1; CHECK(sync_disk(1)==FSDISK_ERR_IO && !g_busy && g_last_err==FSDISK_ERR_IO);
    printf("fd_sync: %d checks passed\n",checks);
    return 0;
}
