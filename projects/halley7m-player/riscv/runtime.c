#include "player_amp.h"
#include "FreeRTOS.h"
#include "task.h"
#include <x2600.h>
#include <stdarg.h>

static volatile struct player_amp_shared *const shared =
    (volatile struct player_amp_shared *)(uintptr_t)PLAYER_SHARED_PHYS;
extern int main(void);

/* This official toolchain compiles RV32IMC but ships an RV32E-only libgcc.
 * FreeRTOS's priority bitmap needs clz; supply it in our ILP32 image instead
 * of linking an E-ABI helper with incompatible registers/stack alignment. */
int __clzsi2(uint32_t value)
{
    int count = 0;
    if (!value) return 32;
    if (value < 0x00010000u) { count += 16; value <<= 16; }
    if (value < 0x01000000u) { count += 8; value <<= 8; }
    if (value < 0x10000000u) { count += 4; value <<= 4; }
    if (value < 0x40000000u) { count += 2; value <<= 2; }
    if (value < 0x80000000u) ++count;
    return count;
}

/* RV32IMC/ILP32 unsigned 64-bit quotient. The official libgcc's RV32E
 * _udivdi3.o cannot be linked into this image, including unoptimized Debug.
 * Use only 32-bit limbs so this implementation needs no division, remainder,
 * variable 64-bit shift or recursive compiler runtime helper.
 * Division by zero returns UINT64_MAX, matching RISC-V unsigned DIV behavior.
 */
uint64_t __udivdi3(uint64_t numerator, uint64_t denominator)
{
    uint32_t numerator_low = (uint32_t)numerator;
    uint32_t numerator_high = (uint32_t)(numerator >> 32);
    uint32_t denominator_low = (uint32_t)denominator;
    uint32_t denominator_high = (uint32_t)(denominator >> 32);
    uint32_t remainder_low = 0, remainder_high = 0;
    uint32_t quotient_low = 0, quotient_high = 0;
    if (!(denominator_low | denominator_high)) return UINT64_MAX;
    for (int bit = 63; bit >= 0; --bit) {
        uint32_t incoming = bit >= 32 ?
            (numerator_high >> (bit - 32)) & 1u : (numerator_low >> bit) & 1u;
        /* Preserve the 65th bit before the remainder is doubled. */
        uint32_t overflow = remainder_high >> 31;
        remainder_high = (remainder_high << 1) | (remainder_low >> 31);
        remainder_low = (remainder_low << 1) | incoming;
        if (overflow || remainder_high > denominator_high ||
            (remainder_high == denominator_high && remainder_low >= denominator_low)) {
            uint32_t before = remainder_low;
            remainder_low -= denominator_low;
            remainder_high = remainder_high - denominator_high - (before < denominator_low);
            if (bit >= 32) quotient_high |= UINT32_C(1) << (bit - 32);
            else quotient_low |= UINT32_C(1) << bit;
        }
    }
    return ((uint64_t)quotient_high << 32) | quotient_low;
}

static void remote_fault(uint32_t error) __attribute__((noreturn));
static void remote_fault(uint32_t error)
{
    local_irq_disable();
    if (player_amp_header_valid(shared)) {
        shared->remote_error = error;
        shared->remote_state = PLAYER_AMP_FAULT;
        player_amp_barrier();
    }
    for (;;) __asm__ volatile("wfi");
}

/* The main core owns UART0. Keep both cores from interleaving serial output. */
void prom_putchar(const char c) { (void)c; }
void prom_putstr(const char *s) { (void)s; }
void prom_printk(const char *format, ...) { (void)format; }
void prom_panic(const char *format, ...) { (void)format; remote_fault(1); }
void assert_failed(uint8_t *file, uint32_t line) { (void)file; remote_fault(0x10000000u | line); }
void player_riscv_assert(const char *file, unsigned line) { (void)file; remote_fault(0x20000000u | line); }
void vApplicationMallocFailedHook(void) { remote_fault(2); }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; (void)name; remote_fault(3); }
void do_exception(void) { uint32_t cause; __asm__ volatile("csrr %0,mcause" : "=r"(cause)); remote_fault(0x30000000u | cause); }
void do_reserved(void) { remote_fault(4); }

void riscv_c_entry(void)
{
    if (!player_amp_header_valid(shared)) remote_fault(5);
    /* Verify before irq_init modifies loadable interrupt-handler data. */
    uint32_t crc = player_amp_crc32((const void *)(uintptr_t)PLAYER_RISCV_PHYS,
                                    shared->riscv_image_bytes);
    shared->remote_build_crc32 = crc;
    if (crc != shared->riscv_image_crc32) remote_fault(6);
    trap_init();
    irq_init();
    local_irq_disable();
    (void)main();
    remote_fault(7);
}
