; Only protected mode example program i wrote that seems to start on emulator

[BITS 16]       
[ORG 0x7C00]    

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    lgdt [gdt_descriptor]

    mov ax, 1
    lmsw ax

    jmp CODE_SEG:protected_mode_start

protected_mode_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x9000

    mov ax, DATA_SEG_VID
    mov es, ax

    mov byte [es:0], 'A'
    mov byte [es:1], 0x04
    mov byte [es:2], 'B'
    mov byte [es:3], 0x02
    mov byte [es:4], 'C'
    mov byte [es:5], 0x01

    cli
    hlt
hang:
    jmp hang

; --- GDT ---
gdt_start:
    dw 0x0000, 0x0000, 0x0000, 0x0000
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    dw 0x0000
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    dw 0x0000
    dw 0xFFFF
    dw 0x8000
    db 0x0B
    db 0x92
    dw 0x0000
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG    equ 0x08
DATA_SEG    equ 0x10
DATA_SEG_VID equ 0x18

times 510 - ($ - $$) db 0
dw 0xAA55
