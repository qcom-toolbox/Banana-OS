; SPDX-License-Identifier: GPL-3.0-or-later
; kernel/fontzbin.asm - the system fonts (kernel/fontbin.asm's), zlib-compressed
; (build/fonts/*.ttf.z, the Makefile): kernel/font.c inflates them at boot.
; Half the size in each kernel - the ISO must stay within the 128 MiB boot
; area of disks installed by builds before the larger one (kernel/fsdisk.c:
; BOOT_LEGACY_LBA), or `update` refuses them. (The web worker app links
; fontbin.asm: its package is compressed whole.)

global fontz_sans, fontz_sans_end, fontz_sans_bold, fontz_sans_bold_end
global fontz_mono, fontz_mono_end, fontz_mono_bold, fontz_mono_bold_end
global fontz_emoji, fontz_emoji_end, fontz_cjk, fontz_cjk_end, fontz_hangul, fontz_hangul_end

section .rodata
fontz_sans: incbin "build/fonts/DejaVuSans.ttf.z"
fontz_sans_end:
fontz_sans_bold: incbin "build/fonts/DejaVuSans-Bold.ttf.z"
fontz_sans_bold_end:
fontz_mono: incbin "build/fonts/DejaVuSansMono.ttf.z"
fontz_mono_end:
fontz_mono_bold: incbin "build/fonts/DejaVuSansMono-Bold.ttf.z"
fontz_mono_bold_end:
fontz_emoji: incbin "build/fonts/NotoEmoji-Regular.ttf.z"
fontz_emoji_end:
fontz_cjk: incbin "build/fonts/DroidSansFallbackFull.ttf.z"
fontz_cjk_end:
fontz_hangul: incbin "build/fonts/NanumGothic-Regular.ttf.z"
fontz_hangul_end:
