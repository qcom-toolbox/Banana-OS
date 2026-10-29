/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Experimental source-level SDL2 subset. Not the SDL distribution or its ABI. */
#ifndef BANANA_SDL2_COMPAT_H
#define BANANA_SDL2_COMPAT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int32_t Sint32;
typedef Sint32 SDL_Keycode;
typedef struct SDL_Window SDL_Window;
typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct {
    Uint32 format;
    Uint8 BitsPerPixel, BytesPerPixel;
    Uint32 Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormat;
typedef struct {
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h, pitch;
    void *pixels;
    SDL_Rect clip_rect;
} SDL_Surface;
typedef struct { Sint32 scancode; SDL_Keycode sym; Uint16 mod; Uint32 unused; } SDL_Keysym;
typedef struct { Uint32 type, timestamp; } SDL_CommonEvent;
typedef struct { Uint32 type, timestamp, windowID; Uint8 event, padding1, padding2, padding3; Sint32 data1, data2; } SDL_WindowEvent;
typedef struct { Uint32 type, timestamp, windowID; Uint8 state, repeat, padding2, padding3; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct { Uint32 type, timestamp, windowID, which, state; Sint32 x, y, xrel, yrel; } SDL_MouseMotionEvent;
typedef struct { Uint32 type, timestamp, windowID, which; Uint8 button, state, clicks, padding1; Sint32 x, y; } SDL_MouseButtonEvent;
typedef struct { Uint32 type, timestamp, windowID, which; Sint32 x, y; Uint32 direction; } SDL_MouseWheelEvent;
typedef union {
    Uint32 type;
    SDL_CommonEvent common;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    Uint8 padding[56];
} SDL_Event;
#define SDL_INIT_TIMER 0x00000001u
#define SDL_INIT_AUDIO 0x00000010u /* recognized but unsupported */
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_EVENTS 0x00004000u
#define SDL_WINDOWPOS_UNDEFINED 0x1fff0000
#define SDL_WINDOWPOS_CENTERED 0x2fff0000
#define SDL_WINDOW_OPENGL 0x00000002u /* recognized but unsupported */
#define SDL_WINDOW_SHOWN 0x00000004u
#define SDL_WINDOW_RESIZABLE 0x00000020u
#define SDL_QUIT 0x100u
#define SDL_WINDOWEVENT 0x200u
#define SDL_KEYDOWN 0x300u
#define SDL_MOUSEMOTION 0x400u
#define SDL_MOUSEBUTTONDOWN 0x401u
#define SDL_MOUSEBUTTONUP 0x402u
#define SDL_MOUSEWHEEL 0x403u
#define SDL_WINDOWEVENT_RESIZED 5
#define SDL_WINDOWEVENT_FOCUS_GAINED 12
#define SDL_WINDOWEVENT_FOCUS_LOST 13
#define SDL_PRESSED 1
#define SDL_RELEASED 0
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_PIXELFORMAT_RGB888 0x16161804u
int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
Uint32 SDL_WasInit(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
int SDL_SetError(const char *fmt, ...);
void SDL_ClearError(void);
Uint32 SDL_GetTicks(void);
Uint64 SDL_GetTicks64(void);
void SDL_Delay(Uint32 ms);
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
                                  Uint32 r, Uint32 g, Uint32 b, Uint32 a);
void SDL_FreeSurface(SDL_Surface *surface);
int SDL_FillRect(SDL_Surface *surface, const SDL_Rect *rect, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *rect, SDL_Surface *dst, SDL_Rect *target);
SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
Uint32 SDL_GetWindowID(SDL_Window *window);
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
void SDL_SetWindowTitle(SDL_Window *window, const char *title);
SDL_Surface *SDL_GetWindowSurface(SDL_Window *window);
int SDL_UpdateWindowSurface(SDL_Window *window);
int SDL_PollEvent(SDL_Event *event);
int SDL_WaitEventTimeout(SDL_Event *event, int timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
