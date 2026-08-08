# Banana OS 0.3 Makefile
# Requires: nasm, gcc-multilib, ld, grub-pc-bin, grub-common, xorriso, mtools

CC      = gcc
CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-pic \
          -nostdlib -nostdinc \
          -Wall -Wextra -O2 \
          -mstackrealign -mno-sse -mno-sse2 -mno-mmx -mno-3dnow \
          -I kernel -I shell
LDFLAGS = -m elf_i386 -nostdlib -T boot/linker.ld
AS      = nasm
ASFLAGS = -f elf32

OBJS = boot/boot.o \
       kernel/terminal.o \
       kernel/keyboard.o \
       kernel/timer.o \
       kernel/rtc.o \
       kernel/gui.o \
       kernel/wallpaper_data.o \
       kernel/fb.o \
       kernel/font8x8.o \
       kernel/gfx.o \
       kernel/daemon.o \
       kernel/sysinfo.o \
       kernel/task.o \
       kernel/task_switch.o \
       kernel/idt.o \
       kernel/isr.o \
       kernel/fs.o \
       kernel/usb.o \
       kernel/kernel.o \
       shell/editor.o \
       shell/shell.o

.PHONY: all clean run

all: Banana_OS.iso

boot/boot.o:       boot/boot.asm;              $(AS) $(ASFLAGS) $< -o $@
kernel/terminal.o: kernel/terminal.c;          $(CC) $(CFLAGS) -c $< -o $@
kernel/keyboard.o: kernel/keyboard.c;          $(CC) $(CFLAGS) -c $< -o $@
kernel/timer.o:    kernel/timer.c;             $(CC) $(CFLAGS) -c $< -o $@
kernel/rtc.o:      kernel/rtc.c;               $(CC) $(CFLAGS) -c $< -o $@
kernel/gui.o:      kernel/gui.c;               $(CC) $(CFLAGS) -c $< -o $@
kernel/wallpaper_data.o: kernel/wallpaper_data.c; $(CC) $(CFLAGS) -c $< -o $@
kernel/fb.o:       kernel/fb.c;                $(CC) $(CFLAGS) -c $< -o $@
kernel/font8x8.o:  kernel/font8x8.c;           $(CC) $(CFLAGS) -c $< -o $@
kernel/gfx.o:      kernel/gfx.c;               $(CC) $(CFLAGS) -c $< -o $@
kernel/daemon.o:   kernel/daemon.c;            $(CC) $(CFLAGS) -c $< -o $@
kernel/sysinfo.o:  kernel/sysinfo.c;           $(CC) $(CFLAGS) -c $< -o $@
kernel/task.o:     kernel/task.c;              $(CC) $(CFLAGS) -c $< -o $@
kernel/task_switch.o: kernel/task_switch.asm;  $(AS) $(ASFLAGS) $< -o $@
kernel/idt.o:      kernel/idt.c;                $(CC) $(CFLAGS) -c $< -o $@
kernel/isr.o:      kernel/isr.asm;              $(AS) $(ASFLAGS) $< -o $@
kernel/fs.o:       kernel/fs.c;                $(CC) $(CFLAGS) -c $< -o $@
kernel/usb.o:      kernel/usb.c;               $(CC) $(CFLAGS) -c $< -o $@
kernel/kernel.o:   kernel/kernel.c;            $(CC) $(CFLAGS) -c $< -o $@
shell/editor.o:    shell/editor.c;             $(CC) $(CFLAGS) -c $< -o $@
shell/shell.o:     shell/shell.c;              $(CC) $(CFLAGS) -c $< -o $@

kernel.bin: $(OBJS)
	ld $(LDFLAGS) -o $@ $^

Banana_OS.iso: kernel.bin
	cp kernel.bin iso/boot/kernel.bin
	grub-mkrescue -o Banana_OS.iso iso

run: Banana_OS.iso
	qemu-system-i386 -cdrom Banana_OS.iso

clean:
	rm -f $(OBJS) kernel.bin iso/boot/kernel.bin Banana_OS.iso
