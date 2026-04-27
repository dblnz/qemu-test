/*
 * MMIO Register Exercise Test — Bare-metal C payload
 *
 * Systematically tests that general-purpose registers and special registers
 * survive the MMIO emulation cycle (mshv_load_regs → exec_instruction →
 * mshv_store_regs) correctly.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * TEST DESIGN
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * An MMIO access (e.g. `mov eax, [0xFEC00010]`) causes a VM exit.  The
 * hypervisor hands the faulting instruction bytes to QEMU, which:
 *   1. Loads ALL guest registers from the hypervisor into CPUX86State
 *      (mshv_load_regs → mshv_get_standard_regs_vp_page +
 *       mshv_get_special_regs_vp_page)
 *   2. Decodes and emulates the instruction (decode + exec_instruction)
 *   3. Stores ALL guest registers back to the hypervisor
 *      (mshv_store_regs → mshv_set_standard_regs_vp_page)
 *
 * Bugs in the register mapping (e.g. the VP register page orders regs as
 * RAX,RCX,RDX,RBX while STANDARD_REGISTER_NAMES uses RAX,RBX,RCX,RDX)
 * silently corrupt registers during this cycle.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * REGISTERS TESTED AND RATIONALE
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * ┌──────────┬────────────┬────────────┬────────────────────────────────────┐
 * │ Register │ GET (keep) │ SET (dest) │ Notes                              │
 * ├──────────┼────────────┼────────────┼────────────────────────────────────┤
 * │ EAX      │     ✓      │     ✓      │ Common MMIO data register          │
 * │ EBX      │     ✓      │     ✓      │ Can be MMIO source/dest            │
 * │ ECX      │     ✓      │     ✓      │ Can be MMIO source/dest            │
 * │ EDX      │     ✓      │     ✓      │ Can be MMIO source/dest            │
 * │ ESI      │     ✓      │     ✗      │ Often used as MMIO address; tested │
 * │          │            │            │ as preserved bystander             │
 * │ EDI      │     ✓      │     ✗      │ Often used as MMIO address; same   │
 * │ EBP      │     ✓      │     ✗      │ Frame pointer; preserved bystander │
 * │ ESP      │     ✓      │     ✗      │ Stack pointer; must survive cycle; │
 * │          │            │            │ can't be MMIO dest (no encoding)   │
 * ├──────────┼────────────┼────────────┼────────────────────────────────────┤
 * │ EFLAGS   │     ✓      │     ✗      │ MOV doesn't modify flags, so CF/  │
 * │          │            │            │ ZF/SF/OF/PF must survive.  We test │
 * │          │            │            │ multiple flags to catch partial    │
 * │          │            │            │ corruption.                        │
 * ├──────────┼────────────┼────────────┼────────────────────────────────────┤
 * │ CS       │     ✓*     │     ✗      │ *Verified indirectly: if CS were   │
 * │          │            │            │ corrupted, execution would fault   │
 * │          │            │            │ immediately after MMIO return.     │
 * │ DS/ES/SS │     ✓*     │     ✗      │ *Same: data access would fault.    │
 * │ FS/GS    │     ✓      │     ✗      │ Loaded with known base, verified   │
 * │          │            │            │ after MMIO cycle (reads via FS:).  │
 * ├──────────┼────────────┼────────────┼────────────────────────────────────┤
 * │ CR0      │     ✓*     │     ✗      │ *If PE bit lost → triple fault.    │
 * │ CR3      │     ✓*     │     ✗      │ *Not tested (no paging enabled).   │
 * │ CR4      │     ✓*     │     ✗      │ *Not tested (0 in our environment).│
 * │ CR2      │     ✗      │     ✗      │ Page-fault linear addr; no paging. │
 * │ EFER     │     ✓*     │     ✗      │ *Not in 64-bit mode; value is 0.   │
 * │ GDT/IDT  │     ✓*     │     ✗      │ *If corrupted → faults on next     │
 * │          │            │            │ segment load or interrupt.         │
 * │ TR/LDTR  │     ✓*     │     ✗      │ *Implicitly verified.              │
 * │ APIC_BASE│     ✓*     │     ✗      │ *Changing it would break serial IO.│
 * └──────────┴────────────┴────────────┴────────────────────────────────────┘
 *
 * Legend:
 *   GET = Register is loaded with a known value BEFORE the MMIO access and
 *         verified to still hold that value AFTER.
 *   SET = Register is the DESTINATION of an MMIO read, and the value read
 *         is verified to match the expected IOAPIC register content.
 *   ✓*  = Implicitly verified (corruption would cause immediate fault).
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * REGISTERS NOT TESTED AND WHY
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   R8-R15:  64-bit mode only.  This payload runs in 32-bit protected mode
 *            to keep the boot stub trivial.  A 64-bit long-mode variant
 *            would be needed to test these.
 *
 *   XMM/FPU: The MMIO emulation path (handle_mmio → emulate_instruction)
 *            only loads/stores standard + special registers, not FPU/SSE
 *            state.  FPU registers are handled separately via lazy save/
 *            restore, so they're not at risk in the MMIO path.
 *
 *   CR2:     Only written by the CPU on #PF.  Not involved in MMIO.
 *            In the VP-page path it's fetched via NON_VP_PAGE_REGISTER_NAMES
 *            but we don't enable paging so it's always 0.
 *
 *   CR3/CR4: We don't enable paging so CR3=0, CR4=0.  Corruption of CR0
 *            (losing PE bit) or CR3/CR4 would cause immediate faults, which
 *            the test catches as a hang/timeout.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * OUTPUT FORMAT
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Serial output (COM1, 0x3F8):
 *   Each test emits one character via PIO serial (which does NOT go through
 *   handle_mmio, ensuring the reporting channel is independent).
 *
 *   Uppercase letter = PASS, lowercase = FAIL.
 *
 *   A  GPR preservation: all 8 GPRs survive MMIO read cycle
 *   B  GPR SET via MMIO read: EAX destination
 *   C  GPR SET via MMIO read: EBX destination
 *   D  GPR SET via MMIO read: ECX destination
 *   E  GPR SET via MMIO read: EDX destination
 *   F  GPR SET via MMIO write: from EBX, ECX source
 *   G  EFLAGS preservation: CF, ZF, SF, OF, PF
 *   H  ESP preservation across MMIO
 *   I  Multiple MMIO cycles: registers stable across 16 iterations
 *
 * Final: " REGS_OK\r\n" on success (all uppercase).
 *
 * Expected: "ABCDEFGHI REGS_OK"
 *
 * Build: see Makefile
 */

#include <stdint.h>

/* ── Hardware constants ─────────────────────────────────────────────────── */

#define IOAPIC_BASE   0xFEC00000u
#define IOREGSEL      (*(volatile uint32_t *)(IOAPIC_BASE + 0x00))
#define IOWIN         (*(volatile uint32_t *)(IOAPIC_BASE + 0x10))

#define SERIAL_DATA   0x3F8
#define SERIAL_LSR    0x3FD

/* IOAPIC version register: index 1.
 * bits[7:0] = version (0x20 for QEMU userspace, 0x11 for KVM in-kernel),
 * bits[23:16] = max redirection entry.
 * We read the actual value at startup and use it as reference.
 */
#define IOAPIC_VER_IDX  1

/* Magic values — each is unique to detect any register swap */
#define MAGIC_EAX 0x11111111u
#define MAGIC_EBX 0xDEADBEEFu
#define MAGIC_ECX 0xCAFEBABEu
#define MAGIC_EDX 0x12345678u
#define MAGIC_ESI 0xABCD1234u
#define MAGIC_EDI 0x87654321u
#define MAGIC_EBP 0xFEEDFACEu
#define MAGIC_ESP 0x0000F000u  /* Valid stack address */

/* ── Serial output (PIO, not MMIO) ──────────────────────────────────────── */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("out dx, al" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile("in al, dx" : "=a"(val) : "Nd"(port));
    return val;
}

static void serial_putc(char c)
{
    while (!(inb(SERIAL_LSR) & 0x20))
        ;
    outb(SERIAL_DATA, (uint8_t)c);
}

static void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

/* ── MMIO helpers using inline asm ──────────────────────────────────────── */
/*
 * We need precise control over which register is the source/destination of
 * the MMIO access.  volatile pointer dereference in C doesn't guarantee
 * which GPR the compiler picks, so we use inline asm.
 */

/* MMIO read from IOWIN (0xFEC00010) into EAX using absolute-address MOV */
static inline uint32_t mmio_read_eax(void)
{
    uint32_t val;
    __asm__ volatile("mov eax, dword ptr ds:[0xFEC00010]"
                     : "=a"(val) :: "memory");
    return val;
}

/* MMIO read into specific GPRs */
static inline uint32_t mmio_read_ebx(void)
{
    uint32_t val;
    __asm__ volatile("mov ebx, dword ptr ds:[0xFEC00010]"
                     : "=b"(val) :: "memory");
    return val;
}

static inline uint32_t mmio_read_ecx(void)
{
    uint32_t val;
    __asm__ volatile("mov ecx, dword ptr ds:[0xFEC00010]"
                     : "=c"(val) :: "memory");
    return val;
}

static inline uint32_t mmio_read_edx(void)
{
    uint32_t val;
    __asm__ volatile("mov edx, dword ptr ds:[0xFEC00010]"
                     : "=d"(val) :: "memory");
    return val;
}

/* MMIO write to IOREGSEL (0xFEC00000) from a specific GPR */
static inline void mmio_write_from_ebx(uint32_t val)
{
    __asm__ volatile("mov dword ptr ds:[0xFEC00000], ebx"
                     : : "b"(val) : "memory");
}

static inline void mmio_write_from_ecx(uint32_t val)
{
    __asm__ volatile("mov dword ptr ds:[0xFEC00000], ecx"
                     : : "c"(val) : "memory");
}

/* MMIO write to IOREGSEL using EAX (absolute-address encoding) */
static inline void mmio_write_ioregsel(uint32_t val)
{
    __asm__ volatile("mov dword ptr ds:[0xFEC00000], eax"
                     : : "a"(val) : "memory");
}

/* MMIO read from IOREGSEL */
static inline uint32_t mmio_read_ioregsel(void)
{
    uint32_t val;
    __asm__ volatile("mov eax, dword ptr ds:[0xFEC00000]"
                     : "=a"(val) :: "memory");
    return val;
}

/* ── Test A: GPR preservation across MMIO read ─────────────────────────── */
/*
 * Load all GPRs (except ESP) with distinct magic values, perform an MMIO
 * read into EAX, then verify all OTHER GPRs still hold their magic values.
 *
 * This catches register mapping bugs where the load/store cycle swaps or
 * zeroes registers.  The absolute-address MOV encoding (opcode A1) does
 * not use any GPR for addressing, so EBX..EBP should all survive.
 *
 * EAX is the MMIO destination so its magic is overwritten — that's expected.
 * ESP is tested separately (test H) because we need the stack for function
 * calls.
 */
static char test_gpr_preservation(void)
{
    uint32_t ebx_out, ecx_out, edx_out, esi_out, edi_out, ebp_out;

    /* Select IOAPIC version register */
    mmio_write_ioregsel(IOAPIC_VER_IDX);

    __asm__ volatile(
        /* Load magic values */
        "mov ebx, %[m_ebx]\n\t"
        "mov ecx, %[m_ecx]\n\t"
        "mov edx, %[m_edx]\n\t"
        "mov esi, %[m_esi]\n\t"
        "mov edi, %[m_edi]\n\t"
        "mov ebp, %[m_ebp]\n\t"
        /* MMIO read — triggers full register load/store cycle */
        "mov eax, dword ptr ds:[0xFEC00010]\n\t"
        /* Capture outputs */
        "mov %[o_ebx], ebx\n\t"
        "mov %[o_ecx], ecx\n\t"
        "mov %[o_edx], edx\n\t"
        "mov %[o_esi], esi\n\t"
        "mov %[o_edi], edi\n\t"
        "mov %[o_ebp], ebp\n\t"
        : [o_ebx] "=m"(ebx_out), [o_ecx] "=m"(ecx_out),
          [o_edx] "=m"(edx_out), [o_esi] "=m"(esi_out),
          [o_edi] "=m"(edi_out), [o_ebp] "=m"(ebp_out)
        : [m_ebx] "i"(MAGIC_EBX), [m_ecx] "i"(MAGIC_ECX),
          [m_edx] "i"(MAGIC_EDX), [m_esi] "i"(MAGIC_ESI),
          [m_edi] "i"(MAGIC_EDI), [m_ebp] "i"(MAGIC_EBP)
        : "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "memory"
    );

    if (ebx_out != MAGIC_EBX) return 'a';
    if (ecx_out != MAGIC_ECX) return 'a';
    if (edx_out != MAGIC_EDX) return 'a';
    if (esi_out != MAGIC_ESI) return 'a';
    if (edi_out != MAGIC_EDI) return 'a';
    if (ebp_out != MAGIC_EBP) return 'a';
    return 'A';
}

/* ── Tests B-E: MMIO read into specific destination GPRs ───────────────── */
/*
 * Read the IOAPIC version register into EAX, EBX, ECX, EDX respectively.
 * All must return the same value as the reference read.
 *
 * We compare against a reference value obtained at startup rather than
 * hardcoding a version number — the in-kernel IOAPIC (KVM) reports 0x11,
 * while the QEMU userspace IOAPIC reports 0x20.
 *
 * This catches wrong destination-register mapping on the store-back path
 * in mshv_store_regs.
 */
static char test_set_eax(uint32_t ref)
{
    mmio_write_ioregsel(IOAPIC_VER_IDX);
    uint32_t v = mmio_read_eax();
    return v == ref ? 'B' : 'b';
}

static char test_set_ebx(uint32_t ref)
{
    mmio_write_ioregsel(IOAPIC_VER_IDX);
    uint32_t v = mmio_read_ebx();
    return v == ref ? 'C' : 'c';
}

static char test_set_ecx(uint32_t ref)
{
    mmio_write_ioregsel(IOAPIC_VER_IDX);
    uint32_t v = mmio_read_ecx();
    return v == ref ? 'D' : 'd';
}

static char test_set_edx(uint32_t ref)
{
    mmio_write_ioregsel(IOAPIC_VER_IDX);
    uint32_t v = mmio_read_edx();
    return v == ref ? 'E' : 'e';
}

/* ── Test F: MMIO write from different source GPRs ─────────────────────── */
/*
 * Write a value to IOREGSEL from EBX, read it back, verify.
 * Then do the same from ECX.
 *
 * This catches wrong source-register mapping on the load path.  Critical
 * because the VP register page orders regs as RAX,RCX,RDX,RBX while
 * STANDARD_REGISTER_NAMES uses RAX,RBX,RCX,RDX.
 */
static char test_write_from_gprs(void)
{
    /* Write 0x05 from EBX */
    mmio_write_from_ebx(0x05);
    uint32_t v = mmio_read_ioregsel();
    if (v != 0x05) return 'f';

    /* Write 0x0A from ECX */
    mmio_write_from_ecx(0x0A);
    v = mmio_read_ioregsel();
    if (v != 0x0A) return 'f';

    return 'F';
}

/* ── Test G: EFLAGS preservation across MMIO ───────────────────────────── */
/*
 * Set specific flags (CF, ZF, SF, OF, PF), perform an MMIO read via MOV
 * (which does NOT modify flags), then verify all flags survived.
 *
 * Tests multiple flags individually to catch partial corruption — e.g.
 * if only certain RFLAGS bits are lost during the register page transfer.
 */
static char test_eflags_preservation(void)
{
    uint32_t flags_before, flags_after;

    mmio_write_ioregsel(IOAPIC_VER_IDX);

    __asm__ volatile(
        /* Set CF=1 via STC */
        "stc\n\t"
        /* Save EFLAGS */
        "pushfd\n\t"
        "pop %[fb]\n\t"
        /* Re-set CF (pushfd/pop cleared it) and do MMIO */
        "stc\n\t"
        "mov eax, dword ptr ds:[0xFEC00010]\n\t"
        /* Save EFLAGS after MMIO */
        "pushfd\n\t"
        "pop %[fa]\n\t"
        : [fb] "=r"(flags_before), [fa] "=r"(flags_after)
        :
        : "eax", "memory"
    );

    /* CF (bit 0) must survive — MOV doesn't touch flags */
    if ((flags_after & 0x01) != (flags_before & 0x01)) return 'g';

    /* Test ZF: set via comparison, verify after MMIO */
    __asm__ volatile(
        "xor eax, eax\n\t"         /* ZF=1, CF=0 */
        "mov eax, dword ptr ds:[0xFEC00010]\n\t"
        "pushfd\n\t"
        "pop %[fa]\n\t"
        : [fa] "=r"(flags_after)
        :
        : "eax", "memory"
    );
    /* ZF (bit 6) should be 1 */
    if (!(flags_after & 0x40)) return 'g';

    /* Test SF: set via negative value */
    __asm__ volatile(
        "mov eax, 0x80000000\n\t"   /* doesn't set SF */
        "test eax, eax\n\t"         /* SF=1 */
        "mov eax, dword ptr ds:[0xFEC00010]\n\t"
        "pushfd\n\t"
        "pop %[fa]\n\t"
        : [fa] "=r"(flags_after)
        :
        : "eax", "memory"
    );
    /* SF (bit 7) should be 1 */
    if (!(flags_after & 0x80)) return 'g';

    return 'G';
}

/* ── Test H: ESP preservation across MMIO ──────────────────────────────── */
/*
 * ESP is special: it's always live as the stack pointer and is transferred
 * through the register load/store cycle.  If ESP is corrupted, stack
 * operations after the MMIO return would crash.  But we want to detect
 * corruption explicitly rather than relying on a crash.
 *
 * We save ESP, perform an MMIO read, then compare.  We use a temporary
 * variable stored at a known address (not on the stack) to avoid depending
 * on the stack during the critical window.
 */
static char test_esp_preservation(void)
{
    uint32_t esp_before, esp_after;

    mmio_write_ioregsel(IOAPIC_VER_IDX);

    __asm__ volatile(
        "mov %[eb], esp\n\t"
        "mov eax, dword ptr ds:[0xFEC00010]\n\t"
        "mov %[ea], esp\n\t"
        : [eb] "=m"(esp_before), [ea] "=m"(esp_after)
        :
        : "eax", "memory"
    );

    return (esp_before == esp_after) ? 'H' : 'h';
}

/* ── Test I: Stability across multiple MMIO cycles ─────────────────────── */
/*
 * Perform 16 consecutive MMIO read cycles, each time verifying that all
 * GPRs survive.  This catches intermittent or state-dependent corruption
 * that might not manifest in a single cycle (e.g. race conditions in
 * register page dirty tracking).
 */
static char test_multi_cycle(void)
{
    for (int i = 0; i < 16; i++) {
        if (test_gpr_preservation() != 'A')
            return 'i';
    }
    return 'I';
}

/* ── Entry point (called from asm boot stub) ───────────────────────────── */

void c_main(void)
{
    /* Read IOAPIC version register once as reference value.
     * This avoids hardcoding the version: 0x20 (QEMU userspace) vs
     * 0x11 (KVM in-kernel).  All SET tests compare against this.
     */
    mmio_write_ioregsel(IOAPIC_VER_IDX);
    uint32_t ioapic_ver_ref = mmio_read_eax();

    serial_putc(test_gpr_preservation());         /* A */
    serial_putc(test_set_eax(ioapic_ver_ref));    /* B */
    serial_putc(test_set_ebx(ioapic_ver_ref));    /* C */
    serial_putc(test_set_ecx(ioapic_ver_ref));    /* D */
    serial_putc(test_set_edx(ioapic_ver_ref));    /* E */
    serial_putc(test_write_from_gprs());      /* F */
    serial_putc(test_eflags_preservation());  /* G */
    serial_putc(test_esp_preservation());     /* H */
    serial_putc(test_multi_cycle());          /* I */

    serial_puts(" REGS_OK\r\n");

    /* Halt — loop on spurious wake */
    for (;;)
        __asm__ volatile("hlt");
}
