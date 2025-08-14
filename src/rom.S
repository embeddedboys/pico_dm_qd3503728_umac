.section .rodata,"a"
.balign 16
.global __rom_begin
__rom_begin:
.incbin "./rom.bin"
__rom_end:
.global __rom_end
.balign 16