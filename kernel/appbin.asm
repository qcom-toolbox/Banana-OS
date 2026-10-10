; kernel/appbin.asm - the apps that come with Banana OS (Media Player,
; Music): their packages for this kernel's CPU, zlib-compressed, built by
; the Makefile (apps/*/). kernel/builtin_apps.c installs them at boot.

global app_mediaplayer, app_mediaplayer_end, app_music, app_music_end

section .rodata
align 16
%ifidn __OUTPUT_FORMAT__, elf64
app_mediaplayer: incbin "apps/mediaplayer/mediaplayer-x86_64.bpk.z"
app_mediaplayer_end:
align 16
app_music: incbin "apps/music/music-x86_64.bpk.z"
app_music_end:
%else
app_mediaplayer: incbin "apps/mediaplayer/mediaplayer-i686.bpk.z"
app_mediaplayer_end:
align 16
app_music: incbin "apps/music/music-i686.bpk.z"
app_music_end:
%endif
