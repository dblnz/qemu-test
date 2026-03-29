; Minimal x86 boot sector that periodically writes a message to COM1 (serial port).
; Assemble with: nasm -f bin -o guest.bin boot.asm

[bits 16]
[org 0x7c00]

start:
    ; Initialize COM1 (0x3f8)
    mov dx, 0x3f9           ; Interrupt Enable Register
    xor al, al
    out dx, al              ; Disable all interrupts

    mov dx, 0x3fb           ; Line Control Register
    mov al, 0x80
    out dx, al              ; Enable DLAB (set baud rate divisor)

    mov dx, 0x3f8           ; Divisor low byte
    mov al, 0x01            ; 115200 baud
    out dx, al

    mov dx, 0x3f9           ; Divisor high byte
    xor al, al
    out dx, al

    mov dx, 0x3fb           ; Line Control Register
    mov al, 0x03            ; 8 bits, no parity, one stop bit (8N1)
    out dx, al

    mov dx, 0x3fa           ; FIFO Control Register
    mov al, 0xc7            ; Enable FIFO, clear, 14-byte threshold
    out dx, al

    mov dx, 0x3fc           ; Modem Control Register
    mov al, 0x03            ; RTS/DSR set
    out dx, al

    ; Install a minimal IRQ0 (timer) handler
    cli
    xor ax, ax
    mov es, ax
    mov word [es:0x20], timer_isr  ; int 0x08 vector (IRQ0)
    mov word [es:0x22], cs

    ; Unmask IRQ0 in the PIC
    in al, 0x21
    and al, 0xFE
    out 0x21, al
    sti

.main_loop:
    mov si, message
.print_loop:
    lodsb
    test al, al
    jz .delay
    mov bl, al

.wait_tx:
    mov dx, 0x3fd
    in al, dx
    test al, 0x20
    jz .wait_tx

    mov al, bl
    mov dx, 0x3f8
    out dx, al
    jmp .print_loop

.delay:
    ; Sleep ~1s using hlt. PIT fires at ~18.2 Hz, so 18 ticks ≈ 1s.
    mov cx, 18
.sleep_loop:
    hlt
    dec cx
    jnz .sleep_loop
    jmp .main_loop

timer_isr:
    push ax
    mov al, 0x20
    out 0x20, al
    pop ax
    iret

message: db "HELLO FROM GUEST", 13, 10, 0

; Pad to 510 bytes and add boot signature
times 510 - ($ - $$) db 0
dw 0xaa55
