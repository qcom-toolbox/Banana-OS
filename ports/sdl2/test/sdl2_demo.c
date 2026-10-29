/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    int selftest = argc > 1 && !strcmp(argv[1], "--selftest");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS)) {
        printf("SDL init: %s\n", SDL_GetError()); return 1;
    }
    int cycles = selftest ? 100 : 1;
    for (int i = 0; i < cycles; i++) {
        SDL_Window *win = SDL_CreateWindow("SDL2 subset: Escape to close",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320, 200,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!win) { printf("SDL window: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
        SDL_Surface *surface = SDL_GetWindowSurface(win);
        SDL_Rect rect = {40, 40, 100, 60};
        if (SDL_FillRect(surface, NULL, 0x182030) ||
            SDL_FillRect(surface, &rect, 0x50c080) || SDL_UpdateWindowSurface(win)) {
            printf("SDL draw: %s\n", SDL_GetError()); SDL_Quit(); return 1;
        }
        if (selftest) SDL_Delay(1);
        else {
            int running = 1;
            while (running) {
                SDL_Event event;
                if (!SDL_WaitEventTimeout(&event, 50)) continue;
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_KEYDOWN && event.key.keysym.sym == 27)) running = 0;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    surface = SDL_GetWindowSurface(win);
                    SDL_FillRect(surface, NULL, 0x182030);
                    SDL_FillRect(surface, &rect, 0x50c080);
                    SDL_UpdateWindowSurface(win);
                }
                if (event.type == SDL_MOUSEBUTTONDOWN)
                    printf("mouse: button=%u x=%d y=%d\n", event.button.button, event.button.x, event.button.y);
                if (event.type == SDL_KEYDOWN) printf("key: %d\n", event.key.keysym.sym);
            }
        }
        SDL_DestroyWindow(win);
    }
    SDL_Quit();
    puts("sdl2-compat: PASS");
    return 0;
}
