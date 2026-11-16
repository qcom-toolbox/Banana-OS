/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WEB_WEBGL_OPS_H
#define WEB_WEBGL_OPS_H
/* The WebGL command stream: what the page's worker (web/webgl.c) records and
 * the GPU worker (apps/webgl/gl.c) plays back with OpenGL ES 2.0. Each
 * command is two 32-bit words, op and n, then n words of arguments
 * (integers, floats as their bits, strings and data as a byte count then
 * the bytes, padded to 4). Objects are numbered by the page: the GPU
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
    GLC_COUNT
};
enum { GLK_BUFFER = 0, GLK_TEXTURE, GLK_FRAMEBUFFER, GLK_RENDERBUFFER, GLK_PROGRAM, GLK_SHADER, GLK_KINDS };
#define GLC_MAX_IDS 65536    /* objects a context may number */
#endif
