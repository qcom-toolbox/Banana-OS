/* Builds the vendored stb_image decoder (third_party/stb/stb_image.h,
 * v2.30, public domain / MIT - see THIRD-PARTY-NOTICES) for the kernel:
 * PNG, JPEG (baseline + progressive), BMP and GIF from memory buffers,
 * with every host dependency (stdio, SIMD, floating point HDR paths,
 * thread-locals) compiled out and allocation routed to the kernel heap.
 * The minimal libc headers it still includes live in ./libc. */

void* kmalloc(unsigned int size);
void* krealloc(void* ptr, unsigned int size);
void  kfree(void* ptr);

#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(sz)        kmalloc((unsigned int)(sz))
#define STBI_REALLOC(p, newsz) krealloc((p), (unsigned int)(newsz))
#define STBI_FREE(p)           kfree(p)
/* refuse absurd dimensions before trying to allocate for them */
#define STBI_MAX_DIMENSIONS    8192

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
