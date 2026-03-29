; Minimal x86 boot sector that writes a message to COM1 (serial port) and halts.
; Assemble with: nasm -f bin -o guest.bin boot.asm

[bits 16]
[org 0x7c00]

start:
    cli

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

    ; Send message character by character
    mov si, message
.print_loop:
    lodsb                   ; Load byte from [si] into al, increment si
    test al, al
    jz .done
    mov bl, al              ; Save character in bl

.wait_tx:
    mov dx, 0x3fd           ; Line Status Register
    in al, dx
    test al, 0x20           ; Transmit buffer empty?
    jz .wait_tx

    mov al, bl              ; Restore character
    mov dx, 0x3f8           ; Data register
    out dx, al
    jmp .print_loop

.done:
    hlt
    jmp .done

message: db "HELLO FROM GUEST", 13, 10, 0

; Pad to 510 bytes and add boot signature
times 510 - ($ - $$) db 0
dw 0xaa55
