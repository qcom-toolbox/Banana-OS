#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Apply the Banana-only OSMesa winsys to the pinned Mesa source, idempotently."""
from pathlib import Path
import re, shutil, sys
root = Path(sys.argv[1]); here = Path(__file__).resolve().parent
def write(path, text):
 if path.read_text() != text: path.write_text(text)
p = root/'src/gallium/targets/osmesa/meson.build'; s=p.read_text()
if "'banana_virgl.c'" not in s:
 s=s.replace("'osmesa_target.c',", "'osmesa_target.c', 'banana_virgl.c',")
if "'banana_crocus.c'" not in s:
 s=s.replace("'banana_virgl.c',", "'banana_virgl.c', 'banana_crocus.c', 'banana_drm.c',")
 s=s.replace('driver_virgl, dep_ws2_32,', 'driver_virgl, driver_crocus, dep_ws2_32,')
 s=s.replace('inc_gallium_drivers,', 'inc_gallium_drivers, inc_virtio,')
 s=s.replace('dep_ws2_32, dep_thread,', 'driver_virgl, dep_ws2_32, dep_thread,')
if 'libgalliumvl_stub' not in s: s=s.replace('libmesa, libgallium,', 'libmesa, libgallium, libgalliumvl_stub,')
if 'inc_virtio' not in s: s=s.replace('inc_gallium_drivers,', 'inc_gallium_drivers, inc_virtio,')
# (crocus linked in whatever order the lines above found the source in: on a
#  fresh one driver_virgl only comes from the last of them, and the driver -
#  GALLIUM_CROCUS - was left out of OSMesa, its screen a stub)
if 'driver_crocus' not in s: s=s.replace('driver_virgl, dep_ws2_32,', 'driver_virgl, driver_crocus, dep_ws2_32,')
# iris (gen 8+) beside it
if "'banana_iris.c'" not in s: s=s.replace("'banana_crocus.c',", "'banana_crocus.c', 'banana_iris.c',")
# A fresh tree may have driver_virgl between crocus and dep_ws2_32.
# Match the dependency itself, not an assumed ordering of adjacent drivers.
if 'driver_iris' not in s: s=s.replace('dep_ws2_32,', 'driver_iris, dep_ws2_32,')
# r600 (AMD Radeon HD 2000 - 6000) beside them, with libdrm_radeon's surface code
if "'banana_r600.c'" not in s: s=s.replace("'banana_crocus.c',", "'banana_crocus.c', 'banana_r600.c', 'banana_radeon_surface.c',")
if 'driver_r600' not in s: s=s.replace('dep_ws2_32,', 'driver_r600, dep_ws2_32,')
write(p, s)
dst=p.parent/'banana_virgl.c'; source=(here/'banana/virgl_winsys.c').read_text()
if not dst.exists() or dst.read_text() != source: dst.write_text(source)
for name, src in (('banana_crocus.c', 'crocus_screen.c'), ('banana_iris.c', 'iris_screen.c'), ('banana_drm.c', 'drm_shim.c'),
                  ('banana_r600.c', 'r600_screen.c'), ('banana_radeon_surface.c', 'radeon_surface.c')):
 dst=p.parent/name; source=(here/'banana'/src).read_text()
 if not dst.exists() or dst.read_text() != source: dst.write_text(source)
p=root/'src/gallium/targets/osmesa/osmesa_target.c';s=p.read_text()
if 'banana_virgl_create_screen' not in s:
 s=s.replace('struct pipe_screen *\nosmesa_create_screen(void);', 'struct pipe_screen *banana_virgl_create_screen(void);\n\nstruct pipe_screen *\nosmesa_create_screen(void);')
 s=s.replace('   /* We use a null software winsys', '''   const char *driver = getenv("GALLIUM_DRIVER");
   if (!driver || !strcmp(driver, "virgl")) {
      screen = banana_virgl_create_screen();
      if (screen) return debug_screen_wrap(screen);
      if (driver) return NULL;
   }

   /* We use a null software winsys''')
write(p, s)
# the GPU first (an Intel one, once gpu3d on), then VirGL, then the processor
if 'banana_crocus_create_screen' not in s:
 s=s.replace('struct pipe_screen *banana_virgl_create_screen(void);', 'struct pipe_screen *banana_virgl_create_screen(void);\nstruct pipe_screen *banana_crocus_create_screen(int asked);')
 s=s.replace('''   const char *driver = getenv("GALLIUM_DRIVER");
   if (!driver || !strcmp(driver, "virgl")) {''', '''   const char *driver = getenv("GALLIUM_DRIVER");
   if (!driver || !strcmp(driver, "crocus")) {
      screen = banana_crocus_create_screen(driver != NULL);
      if (screen) return debug_screen_wrap(screen);
      if (driver) return NULL;
   }
   if (!driver || !strcmp(driver, "virgl")) {''')
write(root/'src/gallium/targets/osmesa/osmesa_target.c', s)
# iris first (it turns down the GPUs crocus takes, and crocus those iris takes)
if 'banana_iris_create_screen' not in s:
 s=s.replace('struct pipe_screen *banana_crocus_create_screen(int asked);', 'struct pipe_screen *banana_crocus_create_screen(int asked);\nstruct pipe_screen *banana_iris_create_screen(int asked);')
 s=s.replace("""   if (!driver || !strcmp(driver, "crocus")) {""", """   if (!driver || !strcmp(driver, "iris")) {
      screen = banana_iris_create_screen(driver != NULL);
      if (screen) return debug_screen_wrap(screen);
      if (driver) return NULL;
   }
   if (!driver || !strcmp(driver, "crocus")) {""")
write(root/'src/gallium/targets/osmesa/osmesa_target.c', s)
# r600 first: it asks the kernel's interface its name, taking only a Radeon
if 'banana_r600_create_screen' not in s:
 s=s.replace('struct pipe_screen *banana_iris_create_screen(int asked);', 'struct pipe_screen *banana_iris_create_screen(int asked);\nstruct pipe_screen *banana_r600_create_screen(int asked);')
 s=s.replace("""   if (!driver || !strcmp(driver, "iris")) {""", """   if (!driver || !strcmp(driver, "r600")) {
      screen = banana_r600_create_screen(driver != NULL);
      if (screen) return debug_screen_wrap(screen);
      if (driver) return NULL;
   }
   if (!driver || !strcmp(driver, "iris")) {""")
write(root/'src/gallium/targets/osmesa/osmesa_target.c', s)

# Mesa's radeon winsys on Banana OS's radeon interface: GEM_MMAP gives the
# buffer's address in the app itself (no mmap of the device; the kernel keeps
# the mapping until the buffer is closed), and there is no DMA engine ring
p=root/'src/gallium/winsys/radeon/drm/radeon_drm_bo.c';s=p.read_text()
if 'Banana: GEM_MMAP maps' not in s:
 s=s.replace("""   ptr = os_mmap(0, args.size, PROT_READ|PROT_WRITE, MAP_SHARED,
                 bo->rws->fd, args.addr_ptr);
   if (ptr == MAP_FAILED) {""", """#ifdef __BANANA_OS__ /* Banana: GEM_MMAP maps the buffer itself */
   ptr = args.addr_ptr ? (void *)(uintptr_t)args.addr_ptr : MAP_FAILED;
#else
   ptr = os_mmap(0, args.size, PROT_READ|PROT_WRITE, MAP_SHARED,
                 bo->rws->fd, args.addr_ptr);
#endif
   if (ptr == MAP_FAILED) {
#ifdef __BANANA_OS__
      mtx_unlock(&bo->u.real.map_mutex);
      fprintf(stderr, "radeon: gem_mmap gave no address\\n");
      return NULL;
#endif""")
 s=s.replace("""   if (bo->u.real.ptr)
      os_munmap(bo->u.real.ptr, bo->base.size);
""", """#ifndef __BANANA_OS__ /* (Banana: GEM_CLOSE unmaps it) */
   if (bo->u.real.ptr)
      os_munmap(bo->u.real.ptr, bo->base.size);
#endif
""")
 s=s.replace("""   os_munmap(bo->u.real.ptr, bo->base.size);
   bo->u.real.ptr = NULL;
""", """#ifndef __BANANA_OS__ /* (Banana: kept, GEM_MMAP gives it again) */
   os_munmap(bo->u.real.ptr, bo->base.size);
#endif
   bo->u.real.ptr = NULL;
""")
 assert s.count('__BANANA_OS__') >= 4, 'radeon_drm_bo.c: not patched'
write(p, s)
p=root/'src/gallium/winsys/radeon/drm/radeon_drm_winsys.c';s=p.read_text()
if 'Banana: no DMA ring' not in s:
 s=s.replace("""   if (ws->info.gfx_level >= EVERGREEN) {
      ws->info.ip[AMD_IP_SDMA].num_queues = 1;
   }""", """   if (ws->info.gfx_level >= EVERGREEN) {
      ws->info.ip[AMD_IP_SDMA].num_queues = 1;
   }
#if defined(__BANANA_OS__) || defined(BANANA_MESA_OSMESA) /* Banana: no DMA ring (kernel/radeon_drm.c: the 3D one; the reference build too, for the host tests) */
   ws->info.ip[AMD_IP_SDMA].num_queues = 0;
#endif""")
 assert 'Banana: no DMA ring' in s, 'radeon_drm_winsys.c: not patched'
# (no video decoder ring either: the kernel says so, and that is no error to print)
if 'Banana: no UVD' not in s:
 s=s.replace("""   if (radeon_get_drm_value(ws->fd, RADEON_INFO_RING_WORKING,
                            "UVD Ring working", &value)) {""", """#if defined(__BANANA_OS__) || defined(BANANA_MESA_OSMESA) /* Banana: no UVD */
   if (radeon_get_drm_value(ws->fd, RADEON_INFO_RING_WORKING, NULL, &value)) {
#else
   if (radeon_get_drm_value(ws->fd, RADEON_INFO_RING_WORKING,
                            "UVD Ring working", &value)) {
#endif""")
 assert 'Banana: no UVD' in s, 'radeon_drm_winsys.c: UVD not patched'
write(p, s)

p=root/'src/gallium/drivers/crocus/crocus_batch.c';s=p.read_text()
# a source patched by an older version of this script (contexts on every GPU)
s=s.replace('''#ifdef __BANANA_OS__
   /* Banana OS keeps no hardware context between batches (no MI_SET_CONTEXT):
    * every batch sets the whole GPU state again */
   if (ret != -EIO)
      crocus_lost_context_state(batch);
#endif
''', '')
if 'Banana OS: no hardware contexts' not in s:
 s=s.replace('''   /* Start a new batch buffer. */
   crocus_batch_reset(batch);
''', '''   /* Start a new batch buffer. */
   crocus_batch_reset(batch);

   /* Banana OS: no hardware contexts on Ironlake (MI_SET_CONTEXT is gen 6+,
    * kernel/gpu_i915.c): the GPU's state does not outlive a batch there, so
    * every batch sets all of it again (the kernel says which GPUs keep it) */
   if (ret != -EIO && !batch->screen->devinfo.has_context_isolation)
      crocus_lost_context_state(batch);
''')
write(p, s)
p=root/'src/gallium/drivers/virgl/meson.build';s=p.read_text().replace('if not with_platform_windows\n', "if not with_platform_windows and not get_option('osmesa')\n");write(p, s)
p=root/'src/gallium/auxiliary/target-helpers/inline_sw_helper.h';s=p.read_text()
s=re.sub(r'(?m)^#if defined\(GALLIUM_VIRGL\)(?: && !defined\(BANANA_MESA_OSMESA\))*$',
         '#if defined(GALLIUM_VIRGL) && !defined(BANANA_MESA_OSMESA)', s)
write(p, s)

p=root/'src/gallium/drivers/virgl/virgl_context.c';s=p.read_text().replace('#ifndef _WIN32\n#include <libsync.h>', '#if !defined(_WIN32) && !defined(__BANANA_OS__)\n#include <libsync.h>')
# Mesa's own copy: the sync-file dependency that gave <libsync.h> its include
# path is left out for OSMesa (meson.build above), so the Linux reference build
# could not find it
s=s.replace('#include <libsync.h>', '#include "util/libsync.h"');write(p, s)
p=root/'src/gallium/frontends/osmesa/osmesa.c';s=p.read_text()
if 'Banana: validate the selected screen' not in s:
 s=s.replace('   if (sharelist) {\n      st_shared = sharelist->st;', '''   /* Banana: validate the selected screen before using its callbacks. */
   struct pipe_frontend_screen *manager = get_st_manager();
   if (!manager || !manager->screen) return NULL;

   if (sharelist) {
      st_shared = sharelist->st;''')
 s=s.replace('      osmesa->depth_stencil_format = PIPE_FORMAT_Z24_UNORM_S8_UINT;', '''      const enum pipe_format choices[] = {PIPE_FORMAT_Z24_UNORM_S8_UINT,
         PIPE_FORMAT_S8_UINT_Z24_UNORM, PIPE_FORMAT_Z32_FLOAT_S8X24_UINT};
      for (unsigned candidate = 0; candidate < ARRAY_SIZE(choices); candidate++) {
         if (manager->screen->is_format_supported(manager->screen, choices[candidate],
               PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_DEPTH_STENCIL)) {
            osmesa->depth_stencil_format = choices[candidate]; break;
         }
      }
      if (osmesa->depth_stencil_format == PIPE_FORMAT_NONE) { FREE(osmesa); return NULL; }''')
write(p, s)
# Banana's OSMesa has no Linux DRM modifier/udmabuf interface.
p=root/'src/gallium/drivers/llvmpipe/lp_texture.c';s=p.read_text().replace('#ifndef _WIN32\n', '#if !defined(_WIN32) && !defined(__BANANA_OS__)\n');write(p,s)
p=root/'src/gallium/drivers/llvmpipe/lp_scene.c';s=p.read_text()
if '#undef HAVE_REALLOCARRAY' not in s:
 s=s.replace('#include "util/reallocarray.h"', '#ifdef __BANANA_OS__\n#undef HAVE_REALLOCARRAY\n#endif\n#include "util/reallocarray.h"')
write(p,s)

# crocus's Intel code: what the SDK's C library has not (INTEL_MEASURE's
# control fifo: a debugging aid; memmem in the shader validator)
p=root/'src/intel/common/intel_measure.c';s=p.read_text()
if 'Banana: no fifos' not in s:
 s=s.replace('#include "intel_measure.h"\n', '#include "intel_measure.h"\n#ifdef __BANANA_OS__ /* Banana: no fifos */\n#define mkfifoat(dir, path, mode) (errno = ENOSYS, -1)\n#endif\n', 1)
# (openat is the SDK's since API version 28: a source patched before keeps it no more)
s=s.replace('#define openat(dir, path, flags) open((path), (flags))\n', '')
write(p, s)
# iris's shader validator too; Mesa's sync_file.h wants <linux/ioctl.h> (ports/mesa/include)
memmem_c = """#ifdef __BANANA_OS__ /* Banana: memmem */
static void *banana_memmem(const void *h, size_t hl, const void *n, size_t nl)
{
   if (!nl) return (void *)h;
   for (size_t i = 0; i + nl <= hl; i++)
      if (!memcmp((const char *)h + i, n, nl)) return (char *)h + i;
   return NULL;
}
#define memmem banana_memmem
#endif
"""
p=root/'src/intel/compiler/brw_eu_validate.c';s=p.read_text()
if 'Banana: memmem' not in s:
 s=s.replace('#include "brw_eu.h"\n', '#include "brw_eu.h"\n' + memmem_c, 1)
write(p, s)
p=root/'src/intel/compiler/elk/elk_eu_validate.c';s=p.read_text()
if 'Banana: memmem' not in s:
 s=s.replace('#include "elk_eu.h"\n', """#include "elk_eu.h"
#ifdef __BANANA_OS__ /* Banana: memmem */
static void *banana_memmem(const void *h, size_t hl, const void *n, size_t nl)
{
   if (!nl) return (void *)h;
   for (size_t i = 0; i + nl <= hl; i++)
      if (!memcmp((const char *)h + i, n, nl)) return (char *)h + i;
   return NULL;
}
#define memmem banana_memmem
#endif
""", 1)
write(p, s)
