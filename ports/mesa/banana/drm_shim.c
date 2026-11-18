/* SPDX-License-Identifier: MIT
 * Banana OS: the part of libdrm (xf86drm.h) Mesa's GPU drivers call, on the
 * system's i915 interface (Intel: crocus, iris) or its radeon one (r600) -
 * /dev/dri/card0 opened through the SDK, its ioctl() going to the kernel
 * (kernel/gpu_gem.c, kernel/radeon_drm.c). There is one GPU; an Intel one
 * is on the PCI bus at 00:02.0, its IDs from I915_GETPARAM. Sharing buffers
 * between processes (PRIME, flink) is not there.
 */
#if defined(__BANANA_OS__) && (defined(GALLIUM_CROCUS) || defined(GALLIUM_R600))
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include "drm-uapi/i915_drm.h"

int drmIoctl(int fd, unsigned long request, void* arg) {
    int r;
    do r = ioctl(fd, request, arg);
    while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* the driver's own requests (radeon: DRM_RADEON_INFO, _GEM_CREATE, _CS...) */
int drmCommandNone(int fd, unsigned long index) {
    return drmIoctl(fd, DRM_IO(DRM_COMMAND_BASE + index), NULL) ? -errno : 0;
}
int drmCommandRead(int fd, unsigned long index, void* data, unsigned long size) {
    return drmIoctl(fd, _IOC(_IOC_READ, DRM_IOCTL_BASE, DRM_COMMAND_BASE + index, (unsigned)size), data) ? -errno : 0;
}
int drmCommandWrite(int fd, unsigned long index, void* data, unsigned long size) {
    return drmIoctl(fd, _IOC(_IOC_WRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + index, (unsigned)size), data) ? -errno : 0;
}
int drmCommandWriteRead(int fd, unsigned long index, void* data, unsigned long size) {
    return drmIoctl(fd, _IOC(_IOC_READ | _IOC_WRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + index, (unsigned)size), data) ? -errno : 0;
}

static char* copy_n(const char* s, int n) {
    char* d = malloc((size_t)n + 1);
    if (!d) return NULL;
    memcpy(d, s, (size_t)n);
    d[n] = 0;
    return d;
}

drmVersionPtr drmGetVersion(int fd) {
    struct drm_version v;
    memset(&v, 0, sizeof(v));
    if (drmIoctl(fd, DRM_IOCTL_VERSION, &v)) return NULL;
    char name[64] = {0}, date[64] = {0}, desc[128] = {0};
    v.name = name; v.name_len = sizeof(name) - 1;
    v.date = date; v.date_len = sizeof(date) - 1;
    v.desc = desc; v.desc_len = sizeof(desc) - 1;
    if (drmIoctl(fd, DRM_IOCTL_VERSION, &v)) return NULL;
    drmVersionPtr r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->version_major = v.version_major;
    r->version_minor = v.version_minor;
    r->version_patchlevel = v.version_patchlevel;
    r->name_len = (int)strlen(name); r->name = copy_n(name, r->name_len);
    r->date_len = (int)strlen(date); r->date = copy_n(date, r->date_len);
    r->desc_len = (int)strlen(desc); r->desc = copy_n(desc, r->desc_len);
    return r;
}

void drmFreeVersion(drmVersionPtr v) {
    if (!v) return;
    free(v->name); free(v->date); free(v->desc);
    free(v);
}

static int param(int fd, int which) {
    int value = 0;
    struct drm_i915_getparam gp = { .param = which, .value = &value };
    return drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) ? -1 : value;
}

/* one device: the GPU at 00:02.0, its nodes /dev/dri/card0 and renderD128 */
static drmDevicePtr make_device(int fd) {
    int id = param(fd, I915_PARAM_CHIPSET_ID);
    if (id < 0) return NULL;
    int rev = param(fd, I915_PARAM_REVISION);
    struct whole {
        drmDevice dev;
        char* nodes[DRM_NODE_MAX];
        char primary[32], render[32];
        drmPciBusInfo bus;
        drmPciDeviceInfo info;
    }* w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    strcpy(w->primary, "/dev/dri/card0");
    strcpy(w->render, "/dev/dri/renderD128");
    w->nodes[DRM_NODE_PRIMARY] = w->primary;
    w->nodes[DRM_NODE_RENDER] = w->render;
    w->dev.nodes = w->nodes;
    w->dev.available_nodes = (1 << DRM_NODE_PRIMARY) | (1 << DRM_NODE_RENDER);
    w->dev.bustype = DRM_BUS_PCI;
    w->bus.domain = 0; w->bus.bus = 0; w->bus.dev = 2; w->bus.func = 0;
    w->info.vendor_id = 0x8086;
    w->info.device_id = (uint16_t)id;
    w->info.subvendor_id = 0x8086;
    w->info.subdevice_id = 0;
    w->info.revision_id = (uint8_t)(rev < 0 ? 0 : rev);
    w->dev.businfo.pci = &w->bus;
    w->dev.deviceinfo.pci = &w->info;
    return &w->dev;
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr* device) {
    (void)flags;
    *device = make_device(fd);
    return *device ? 0 : -ENODEV;
}
int drmGetDevice(int fd, drmDevicePtr* device) { return drmGetDevice2(fd, 0, device); }

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max) {
    (void)flags;
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    drmDevicePtr d = make_device(fd);
    close(fd);
    if (!d) return 0;
    if (devices && max > 0) devices[0] = d;
    else drmFreeDevice(&d);
    return 1;
}
int drmGetDevices(drmDevicePtr devices[], int max) { return drmGetDevices2(0, devices, max); }

void drmFreeDevice(drmDevicePtr* device) {
    if (device && *device) { free(*device); *device = NULL; }   /* (one block: make_device) */
}
void drmFreeDevices(drmDevicePtr devices[], int count) {
    for (int i = 0; devices && i < count; i++) drmFreeDevice(&devices[i]);
}
int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b) {
    return a && b && a->bustype == b->bustype && a->deviceinfo.pci->device_id == b->deviceinfo.pci->device_id;
}
int drmGetDeviceFromDevId(dev_t id, uint32_t flags, drmDevicePtr* device) { (void)id; (void)flags; (void)device; return -ENODEV; }

char* drmGetDeviceNameFromFd2(int fd) { (void)fd; return strdup("/dev/dri/renderD128"); }
char* drmGetDeviceNameFromFd(int fd) { (void)fd; return strdup("/dev/dri/card0"); }
char* drmGetPrimaryDeviceNameFromFd(int fd) { (void)fd; return strdup("/dev/dri/card0"); }
char* drmGetRenderDeviceNameFromFd(int fd) { (void)fd; return strdup("/dev/dri/renderD128"); }
int drmGetNodeTypeFromFd(int fd) { (void)fd; return DRM_NODE_RENDER; }

int drmGetCap(int fd, uint64_t capability, uint64_t* value) {
    struct drm_get_cap c = { .capability = capability };
    int r = drmIoctl(fd, DRM_IOCTL_GET_CAP, &c);
    if (!r) *value = c.value;
    return r ? -errno : 0;
}
int drmSetClientCap(int fd, uint64_t capability, uint64_t value) {
    struct drm_set_client_cap c = { .capability = capability, .value = value };
    return drmIoctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &c) ? -errno : 0;
}

/* (one process: nothing to share buffers or fences with) */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int* prime_fd) {
    (void)fd; (void)handle; (void)flags; *prime_fd = -1; errno = EOPNOTSUPP; return -EOPNOTSUPP;
}
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t* handle) {
    (void)fd; (void)prime_fd; *handle = 0; errno = EOPNOTSUPP; return -EOPNOTSUPP;
}
int drmGetMagic(int fd, drm_magic_t* magic) { (void)fd; *magic = 0; errno = EOPNOTSUPP; return -EOPNOTSUPP; }
int drmAuthMagic(int fd, drm_magic_t magic) { (void)fd; (void)magic; return 0; }

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t* handle) {
    struct drm_syncobj_create c = { .flags = flags };
    int r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &c);
    if (!r) *handle = c.handle;
    return r ? -errno : 0;
}
int drmSyncobjDestroy(int fd, uint32_t handle) {
    struct drm_syncobj_destroy d = { .handle = handle };
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &d) ? -errno : 0;
}
int drmSyncobjWait(int fd, uint32_t* handles, unsigned n, int64_t timeout_nsec, unsigned flags, uint32_t* first) {
    struct drm_syncobj_wait w = { .handles = (uint64_t)(uintptr_t)handles, .timeout_nsec = timeout_nsec,
                                  .count_handles = n, .flags = flags };
    int r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w);
    if (!r && first) *first = w.first_signaled;
    return r ? -errno : 0;
}
int drmSyncobjHandleToFD(int fd, uint32_t handle, int* obj_fd) { (void)fd; (void)handle; *obj_fd = -1; return -EOPNOTSUPP; }
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t* handle) { (void)fd; (void)obj_fd; *handle = 0; return -EOPNOTSUPP; }
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int* sync_file_fd) { (void)fd; (void)handle; *sync_file_fd = -1; return -EOPNOTSUPP; }
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd) { (void)fd; (void)handle; (void)sync_file_fd; return -EOPNOTSUPP; }

#else
typedef int banana_drm_shim_unused;
#endif
