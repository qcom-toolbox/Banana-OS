; kernel/appbin.asm - the apps that come with Banana OS (Media Player,
; Music, Amethyst, Photos, Banana Code, App Store): their packages for this kernel's CPU, zlib-compressed, built by
; the Makefile (apps/*/). kernel/builtin_apps.c installs them at boot.

global app_mediaplayer, app_mediaplayer_end, app_music, app_music_end, app_amethyst, app_amethyst_end, app_photos, app_photos_end, app_code, app_code_end, app_store, app_store_end

section .rodata
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
%endif
