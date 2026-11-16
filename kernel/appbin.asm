; SPDX-License-Identifier: GPL-3.0-or-later
; kernel/appbin.asm - the apps that come with Banana OS (Media Player,
; Music, Amethyst, Photos, Banana Code, App Store; Terminal and the ports - GNU bash, coreutils... - on x86_64): their packages for this kernel's CPU,
; zlib-compressed, built by the Makefile (apps/*/, ports/*/). kernel/builtin_apps.c installs them at boot.

global builtin_meta, builtin_meta_end
global app_terminal, app_terminal_end
global app_mediaplayer, app_mediaplayer_end, app_music, app_music_end, app_amethyst, app_amethyst_end, app_photos, app_photos_end, app_code, app_code_end, app_store, app_store_end

section .payload progbits alloc noexec nowrite align=16
align 16
%ifidn __OUTPUT_FORMAT__, elf64
app_mediaplayer: incbin "apps/mediaplayer/mediaplayer-x86_64.bpk.z"
app_mediaplayer_end:
align 16
app_music: incbin "apps/music/music-x86_64.bpk.z"
app_music_end:
align 16
app_amethyst: incbin "apps/amethyst/amethyst-x86_64.bpk.z"
app_amethyst_end:
align 16
app_photos: incbin "apps/photos/photos-x86_64.bpk.z"
app_photos_end:
align 16
app_code: incbin "apps/code/code-x86_64.bpk.z"
app_code_end:
align 16
app_store: incbin "apps/store/store-x86_64.bpk.z"
app_store_end:
align 16
%include "build/ports64.inc"               ; (the ports: the Makefile's PORTS64)
align 16
global app_libjsworker, app_libjsworker_end
app_libjsworker: incbin "build/libjsworker-x86_64.bpk.z"
app_libjsworker_end:
align 16
global app_webworker, app_webworker_end
app_webworker: incbin "apps/webworker/webworker-x86_64.bpk.z"
app_webworker_end:
align 16
global app_webmedia, app_webmedia_end
app_webmedia: incbin "apps/webmedia/webmedia-x86_64.bpk.z"
app_webmedia_end:
align 16
global app_webgl, app_webgl_end
app_webgl: incbin "apps/webgl/webgl-x86_64.bpk.z"
app_webgl_end:
align 16
app_terminal: incbin "apps/terminal/terminal-x86_64.bpk.z"
app_terminal_end:
section .rodata                 ; (not .payload: that goes to the heap once released)
align 16
builtin_meta: incbin "build/builtin-meta-x86_64.bin"     ; (tools/mkbuiltinmeta.py)
builtin_meta_end:
section .payload
%else
app_mediaplayer: incbin "apps/mediaplayer/mediaplayer-i686.bpk.z"
app_mediaplayer_end:
align 16
app_music: incbin "apps/music/music-i686.bpk.z"
app_music_end:
align 16
app_amethyst: incbin "apps/amethyst/amethyst-i686.bpk.z"
app_amethyst_end:
align 16
app_photos: incbin "apps/photos/photos-i686.bpk.z"
app_photos_end:
align 16
app_code: incbin "apps/code/code-i686.bpk.z"
app_code_end:
align 16
app_store: incbin "apps/store/store-i686.bpk.z"
app_store_end:
global app_webworker, app_webworker_end
app_webworker:
app_webworker_end:
app_terminal:
app_terminal_end:
section .rodata                 ; (not .payload: that goes to the heap once released)
align 16
builtin_meta: incbin "build/builtin-meta-i686.bin"       ; (tools/mkbuiltinmeta.py)
builtin_meta_end:
section .payload
%endif
