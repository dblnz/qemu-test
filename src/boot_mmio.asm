; Bare-metal MMIO emulation test boot sector.
;
; Exercises the handle_mmio code path in MSHV by performing MMIO accesses to
; the IOAPIC (always present at 0xFEC00000 on both PC/Q35).  Results are
; reported via serial PIO, which does NOT go through handle_mmio.
;
; Designed to catch register-handling bugs that can be introduced when
; optimizing register transfer (e.g. using hv_vp_register_page):
;
;   T1 – Basic MMIO write + read
;        Write IOREGSEL to select version register, read IOWIN.
;        Verifies MMIO read/write fundamentally work.  Checks that the
;        returned value is plausible (non-zero version, non-zero max
;        redir) rather than hardcoding a specific version, since KVM's
;        in-kernel IOAPIC reports 0x11 while QEMU userspace reports 0x20.
;
;   T2 – GPR preservation across MMIO
;        Load EBX..EBP with distinct magic values, perform MMIO read
;        into EAX (absolute-address encoding, no GPR used for address).
;        Verify all other GPRs survive the load → emulate → store cycle.
;        Catches symmetric index-mapping bugs (page layout != QEMU layout).
;
;   T3 – MMIO read into different destination GPRs (EAX, EBX, ECX, EDX)
;        All reads of the same register must return the same value.
;        Catches wrong destination-register mapping on store-back.
;
;   T4 – MMIO write from different source GPRs (EBX, ECX)
;        Write a value from EBX to IOREGSEL, read back.  Then from ECX.
;        Catches wrong source-register mapping on load.
;        Critical because the register page orders regs as
;        rax,rcx,rdx,rbx while STANDARD_REGISTER_NAMES uses rax,rbx,rcx,rdx.
;
;   T5 – EFLAGS preservation
;        Set CF via STC, perform MMIO read (MOV does not touch flags),
;        verify CF is still set.
;
; Expected serial output on success: "ABCDE MMIO_OK\r\n"
; A lowercase letter indicates which test failed first.
;
; Assemble: nasm -f bin -o guest_mmio.bin boot_mmio.asm

IOAPIC_BASE equ 0xFEC00000
IOREGSEL    equ IOAPIC_BASE + 0x00      ; register selector (R/W)
IOWIN       equ IOAPIC_BASE + 0x10      ; data window

SERIAL_DATA equ 0x3F8                   ; COM1 THR
SERIAL_LSR  equ 0x3FD                   ; COM1 Line Status Register

; Magic GPR values for T2 – deliberately asymmetric so every swap is visible
MAGIC_EBX   equ 0xDEADBEEF
MAGIC_ECX   equ 0xCAFEBABE
MAGIC_EDX   equ 0x12345678
MAGIC_ESI   equ 0xABCD1234
MAGIC_EDI   equ 0x87654321
MAGIC_EBP   equ 0xFEEDFACE

; ──────────────────────────────────────────────
; 16-bit real-mode entry
; ──────────────────────────────────────────────
[bits 16]
[org 0x7c00]

start:
    cli
    lgdt [gdtdesc]
    mov eax, 1
    mov cr0, eax                    ; protected mode
    jmp 0x08:start32                ; far jump to 32-bit CS

; ──────────────────────────────────────────────
; 32-bit protected-mode code
; ──────────────────────────────────────────────
[bits 32]
start32:
    mov ax, 0x10                    ; flat data segment
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7c00

    ; ==================================================================
    ; T1: Basic MMIO write + read
    ; ==================================================================
    ; Select IOAPIC version register (index 1)
    mov eax, 0x01
    mov edi, IOREGSEL
    mov [edi], eax                  ; MMIO WRITE → IOREGSEL

    ; Read IOAPIC version via data window
    mov esi, IOWIN
    mov eax, [esi]                  ; MMIO READ  ← IOWIN

    ; IOAPIC version register: bits [7:0] = version (0x20 for QEMU
    ; userspace IOAPIC, 0x11 for KVM in-kernel), bits [23:16] = max
    ; redirection entry.  Check that the read returned something
    ; plausible rather than hardcoding a specific version.
    test al, al                     ; version byte must be non-zero
    jz .t1_fail
    ; Also verify upper half is plausible (max redir > 0)
    test eax, 0x00FF0000
    jz .t1_fail
    mov al, 'A'
    jmp .t1_out
.t1_fail:
    mov al, 'a'
.t1_out:
    call serial_out

    ; ==================================================================
    ; T2: GPR preservation across MMIO read
    ; ==================================================================
    ; IOREGSEL is still 1 (version register) from T1.
    mov ebx, MAGIC_EBX
    mov ecx, MAGIC_ECX
    mov edx, MAGIC_EDX
    mov esi, MAGIC_ESI
    mov edi, MAGIC_EDI
    mov ebp, MAGIC_EBP

    ; MMIO read using absolute-address encoding (A1 opcode) — no GPR
    ; is consumed as an address operand, so all six above must survive.
    mov eax, [IOWIN]                ; MMIO READ  ← 0xFEC00010

    cmp ebx, MAGIC_EBX
    jne .t2_fail
    cmp ecx, MAGIC_ECX
    jne .t2_fail
    cmp edx, MAGIC_EDX
    jne .t2_fail
    cmp esi, MAGIC_ESI
    jne .t2_fail
    cmp edi, MAGIC_EDI
    jne .t2_fail
    cmp ebp, MAGIC_EBP
    jne .t2_fail
    mov al, 'B'
    jmp .t2_out
.t2_fail:
    mov al, 'b'
.t2_out:
    call serial_out

    ; ==================================================================
    ; T3: MMIO read into different destination GPRs
    ; ==================================================================
    ; Re-select version register
    mov edi, IOREGSEL
    mov eax, 0x01
    mov [edi], eax                  ; MMIO WRITE → IOREGSEL

    ; Reference read into EAX
    mov eax, [IOWIN]                ; MMIO READ  ← IOWIN
    push eax                        ; save reference on stack

    ; Read into EBX — must match reference
    mov ebx, [IOWIN]
    cmp ebx, [esp]
    jne .t3_fail

    ; Read into ECX
    mov ecx, [IOWIN]
    cmp ecx, [esp]
    jne .t3_fail

    ; Read into EDX
    mov edx, [IOWIN]
    cmp edx, [esp]
    jne .t3_fail

    add esp, 4                      ; clean stack
    mov al, 'C'
    jmp .t3_out
.t3_fail:
    add esp, 4
    mov al, 'c'
.t3_out:
    call serial_out

    ; ==================================================================
    ; T4: MMIO write from different source GPRs
    ; ==================================================================
    ; Write 0x05 from EBX to IOREGSEL, read back, verify.
    mov ebx, 0x05
    mov [IOREGSEL], ebx             ; MMIO WRITE from EBX
    mov eax, [IOREGSEL]             ; MMIO READ  back
    cmp eax, 0x05
    jne .t4_fail

    ; Write 0x0A from ECX to IOREGSEL, read back, verify.
    mov ecx, 0x0A
    mov [IOREGSEL], ecx             ; MMIO WRITE from ECX
    mov eax, [IOREGSEL]             ; MMIO READ  back
    cmp eax, 0x0A
    jne .t4_fail

    mov al, 'D'
    jmp .t4_out
.t4_fail:
    mov al, 'd'
.t4_out:
    call serial_out

    ; ==================================================================
    ; T5: EFLAGS preservation across MMIO
    ; ==================================================================
    ; Select version register again
    mov edi, IOREGSEL
    mov eax, 0x01
    mov [edi], eax

    ; Set CF, clear ZF
    stc                             ; CF = 1
    mov eax, 1                      ; ZF = 0 (MOV doesn't touch flags,
                                    ; but this ensures EAX is non-zero
                                    ; for a later test if needed)

    ; MMIO read — MOV does not modify EFLAGS
    mov eax, [IOWIN]                ; MMIO READ

    ; Verify CF is still 1
    jnc .t5_fail

    mov al, 'E'
    jmp .t5_out
.t5_fail:
    mov al, 'e'
.t5_out:
    call serial_out

    ; ==================================================================
    ; All tests done — emit result marker
    ; ==================================================================
    mov al, ' '
    call serial_out

    mov esi, ok_msg
    call serial_out_string

    hlt
    jmp $ - 1                       ; safety: loop on wake

; ──────────────────────────────────────────────
; serial_out — send byte in AL to COM1
;   Waits for TX-ready, preserves all regs except EFLAGS
; ──────────────────────────────────────────────
serial_out:
    push eax
    push edx
.wait:
    mov dx, SERIAL_LSR
    in al, dx
    test al, 0x20                   ; TX holding register empty?
    jz .wait
    pop edx
    pop eax                         ; restore original AL
    push edx
    mov dx, SERIAL_DATA
    out dx, al
    pop edx
    ret

; ──────────────────────────────────────────────
; serial_out_string — send NUL-terminated string at ESI
;   Preserves ESI
; ──────────────────────────────────────────────
serial_out_string:
    push esi
.next:
    lodsb
    test al, al
    jz .done
    call serial_out
    jmp .next
.done:
    pop esi
    ret

; ──────────────────────────────────────────────
; Data
; ──────────────────────────────────────────────
ok_msg: db "MMIO_OK", 13, 10, 0

; ──────────────────────────────────────────────
; GDT — flat 32-bit code + data segments
; ──────────────────────────────────────────────
align 4
gdt:
    dq 0                            ; null descriptor
    ; code: base=0 limit=4G 32-bit execute/read DPL=0
    dw 0xFFFF, 0
    db 0, 0x9A, 0xCF, 0
    ; data: base=0 limit=4G 32-bit read/write DPL=0
    dw 0xFFFF, 0
    db 0, 0x92, 0xCF, 0
gdtdesc:
    dw gdtdesc - gdt - 1            ; limit
    dd gdt                          ; base

; ──────────────────────────────────────────────
; Boot signature + padding
; ──────────────────────────────────────────────
times 510 - ($ - $$) db 0
dw 0xAA55
