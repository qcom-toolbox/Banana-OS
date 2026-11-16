/*
 * Copyright 2008 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Author: Stanislaw Skowronek
 */

#ifndef ATOM_BITS_H
#define ATOM_BITS_H

/* (Banana OS: every read inside the image - bios_size bytes; 0 past it) */
static inline uint8_t get_u8(const struct atom_context *c, int ptr)
{
    return ptr >= 0 && (uint32_t)ptr < c->bios_size ? ((unsigned char *)c->bios)[ptr] : 0;
}
#define U8(ptr) get_u8(ctx->ctx, (ptr))
#define CU8(ptr) get_u8(ctx, (ptr))
static inline uint16_t get_u16(const struct atom_context *c, int ptr)
{
    return get_u8(c, ptr)|(((uint16_t)get_u8(c, ptr+1))<<8);
}
#define U16(ptr) get_u16(ctx->ctx, (ptr))
#define CU16(ptr) get_u16(ctx, (ptr))
static inline uint32_t get_u32(const struct atom_context *c, int ptr)
{
    return get_u16(c, ptr)|(((uint32_t)get_u16(c, ptr+2))<<16);
}
#define U32(ptr) get_u32(ctx->ctx, (ptr))
#define CU32(ptr) get_u32(ctx, (ptr))
#define CSTR(ptr) (((char *)(ctx->bios))+(ptr))

#endif
