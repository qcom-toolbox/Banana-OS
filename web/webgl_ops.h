/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WEB_WEBGL_OPS_H
#define WEB_WEBGL_OPS_H
/* The WebGL command stream: what the page's worker (web/webgl.c) records and
 * the GPU worker (apps/webgl/gl.c) plays back with OpenGL ES 2.0. Each
 * command is two 32-bit words, op and n, then n words of arguments
 * (integers, floats as their bits, strings and data as a byte count then
 * the bytes, padded to 4). The commands after GLC_ATTRIB_DIVISOR are
 * WebGL 2's (OpenGL ES 3.0). Objects are numbered by the page: the GPU
 * worker maps each number to its GL name. A command that answers is the
 * last of a flush: the reply holds its result only. */
enum {
    GLC_RESIZE = 1,          /* w, h: the drawing buffer */
    GLC_CREATE,              /* kind (GLK_*), id[, shader type] */
    GLC_DELETE,              /* kind, id */
    GLC_SHADER_SOURCE,       /* id, string */
    GLC_COMPILE,             /* id */
    GLC_ATTACH, GLC_DETACH,  /* program, shader */
    GLC_LINK, GLC_USE, GLC_VALIDATE,   /* program */
    GLC_SHADER_PARAM,        /* id, pname -> int */
    GLC_PROGRAM_PARAM,       /* id, pname -> int */
    GLC_SHADER_LOG, GLC_PROGRAM_LOG,   /* id -> string */
    GLC_ATTRIB_LOC,          /* program, string -> int */
    GLC_BIND_ATTRIB_LOC,     /* program, index, string */
    GLC_UNIFORM_LOC,         /* program, string -> int */
    GLC_ACTIVE,              /* program, uniform?, index -> size, type, string (size 0: none) */
    GLC_UNIFORM_F,           /* location, n (1-4), n floats */
    GLC_UNIFORM_I,           /* location, n, n ints */
    GLC_UNIFORM_FV,          /* location, components, count, floats */
    GLC_UNIFORM_IV,          /* location, components, count, ints */
    GLC_UNIFORM_MAT,         /* location, dimension (2-4), count, floats */
    GLC_ATTRIB_PTR,          /* index, size, type, normalized, stride, offset */
    GLC_ENABLE_ATTRIB, GLC_DISABLE_ATTRIB,   /* index */
    GLC_ATTRIB_F,            /* index, n, n floats */
    GLC_BIND_BUFFER,         /* target, id */
    GLC_BUFFER_DATA,         /* target, usage, size, data bytes (count 0: size zeroes) */
    GLC_BUFFER_SUB,          /* target, offset, data bytes */
    GLC_DRAW_ARRAYS,         /* mode, first, count */
    GLC_DRAW_ELEMENTS,       /* mode, count, type, offset */
    GLC_VIEWPORT, GLC_SCISSOR,          /* x, y, w, h */
    GLC_CLEAR_COLOR,         /* 4 floats */
    GLC_CLEAR_DEPTH,         /* float */
    GLC_CLEAR_STENCIL,       /* int */
    GLC_CLEAR,               /* mask */
    GLC_ENABLE, GLC_DISABLE, /* capability */
    GLC_BLEND_FUNC,          /* src rgb, dst rgb, src alpha, dst alpha */
    GLC_BLEND_EQ,            /* rgb, alpha */
    GLC_BLEND_COLOR,         /* 4 floats */
    GLC_DEPTH_FUNC, GLC_DEPTH_MASK,     /* int */
    GLC_DEPTH_RANGE,         /* 2 floats */
    GLC_COLOR_MASK,          /* 4 ints */
    GLC_CULL_FACE, GLC_FRONT_FACE,      /* int */
    GLC_LINE_WIDTH,          /* float */
    GLC_POLYGON_OFFSET,      /* 2 floats */
    GLC_STENCIL_FUNC,        /* face, func, ref, mask */
    GLC_STENCIL_OP,          /* face, fail, zfail, zpass */
    GLC_STENCIL_MASK,        /* face, mask */
    GLC_HINT,                /* target, mode */
    GLC_PIXEL_STORE,         /* pname, value */
    GLC_ACTIVE_TEXTURE,      /* unit */
    GLC_BIND_TEXTURE,        /* target, id */
    GLC_TEX_PARAM_I,         /* target, pname, int */
    GLC_TEX_PARAM_F,         /* target, pname, float */
    GLC_TEX_IMAGE,           /* target, level, internal format, w, h, format, type, data bytes (0: none) */
    GLC_TEX_SUB_IMAGE,       /* target, level, x, y, w, h, format, type, data bytes */
    GLC_GEN_MIPMAP,          /* target */
    GLC_BIND_FRAMEBUFFER,    /* target, id */
    GLC_FB_TEXTURE,          /* target, attachment, texture target, id, level */
    GLC_FB_RENDERBUFFER,     /* target, attachment, rb target, id */
    GLC_CHECK_FB,            /* target -> int */
    GLC_BIND_RENDERBUFFER,   /* target, id */
    GLC_RB_STORAGE,          /* target, format, w, h */
    GLC_READ_PIXELS,         /* x, y, w, h, format, type -> bytes */
    GLC_GET_ERROR,           /* -> int */
    GLC_GET_PARAM,           /* pname -> count, floats */
    GLC_IS_ENABLED,          /* cap -> int */
    GLC_GET_STRING,          /* name -> string */
    GLC_FINISH,
    GLC_COPY_TEX_IMAGE,      /* target, level, internal format, x, y, w, h */
    GLC_COPY_TEX_SUB_IMAGE,  /* target, level, xoff, yoff, x, y, w, h */
    GLC_SAMPLE_COVERAGE,     /* float, invert */
    GLC_STENCIL_MASK_SEP_DUMMY,
    GLC_DRAW_ARRAYS_INST,    /* mode, first, count, instances */
    GLC_DRAW_ELEMENTS_INST,  /* mode, count, type, offset, instances */
    GLC_ATTRIB_DIVISOR,      /* index, divisor */
    GLC_BIND_VAO,            /* id */
    GLC_TEX_IMAGE3D,         /* target, level, internal format, w, h, depth, format, type, data bytes */
    GLC_TEX_SUB_IMAGE3D,     /* target, level, x, y, z, w, h, depth, format, type, data bytes */
    GLC_TEX_STORAGE2D,       /* target, levels, internal format, w, h */
    GLC_TEX_STORAGE3D,       /* target, levels, internal format, w, h, depth */
    GLC_DRAW_BUFFERS,        /* buffers... */
    GLC_READ_BUFFER,         /* source */
    GLC_BLIT_FB,             /* src x0 y0 x1 y1, dst x0 y0 x1 y1, mask, filter */
    GLC_RB_STORAGE_MS,       /* target, samples, format, w, h */
    GLC_FB_TEX_LAYER,        /* target, attachment, texture id, level, layer */
    GLC_BIND_BUFFER_BASE,    /* target, index, id */
    GLC_BIND_BUFFER_RANGE,   /* target, index, id, offset, size */
    GLC_UNIFORM_BLOCK_INDEX, /* program, string -> int */
    GLC_UNIFORM_BLOCK_BINDING,   /* program, block index, binding */
    GLC_UNIFORM_BLOCK_PARAM, /* program, block index, pname -> int */
    GLC_UNIFORM_UI,          /* location, n, n uints */
    GLC_UNIFORM_UIV,         /* location, components, count, uints */
    GLC_UNIFORM_MAT_NS,      /* location, columns, rows, count, floats */
    GLC_ATTRIB_IPTR,         /* index, size, type, stride, offset */
    GLC_ATTRIB_I,            /* index, unsigned, 4 ints */
    GLC_CLEAR_BUFFER_FV,     /* buffer, draw buffer, floats */
    GLC_CLEAR_BUFFER_IV,     /* unsigned, buffer, draw buffer, ints */
    GLC_CLEAR_BUFFER_FI,     /* buffer, draw buffer, depth, stencil */
    GLC_DRAW_RANGE_ELEMENTS, /* mode, start, end, count, type, offset */
    GLC_BIND_SAMPLER,        /* unit, id */
    GLC_SAMPLER_PARAM_I,     /* id, pname, int */
    GLC_SAMPLER_PARAM_F,     /* id, pname, float */
    GLC_FRAG_DATA_LOC,       /* program, string -> int */
    GLC_COPY_BUFFER_SUB,     /* read target, write target, read offset, write offset, size */
    GLC_GET_BUFFER_SUB,      /* target, offset, length -> bytes */
    GLC_BEGIN_QUERY,         /* target, id */
    GLC_END_QUERY,           /* target */
    GLC_QUERY_RESULT,        /* id, pname -> int */
    GLC_BIND_TF,             /* target, id */
    GLC_BEGIN_TF,            /* primitive mode */
    GLC_END_TF,
    GLC_TF_VARYINGS,         /* program, buffer mode, strings... */
    GLC_PAUSE_TF, GLC_RESUME_TF,
    GLC_COUNT
};
enum { GLK_BUFFER = 0, GLK_TEXTURE, GLK_FRAMEBUFFER, GLK_RENDERBUFFER, GLK_PROGRAM, GLK_SHADER,
       GLK_VAO, GLK_QUERY, GLK_SAMPLER, GLK_TF, GLK_KINDS };
#define GLC_MAX_IDS 65536    /* objects a context may number */
#endif
