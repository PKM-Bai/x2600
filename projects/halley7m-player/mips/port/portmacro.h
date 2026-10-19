#ifndef PLAYER_PORTMACRO_H
#define PLAYER_PORTMACRO_H

#include <stdint.h>
#include <irqflags.h>

typedef uint32_t StackType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t TickType_t;

#define portCHAR char
#define portFLOAT float
#define portDOUBLE double
#define portLONG long
#define portSHORT short
#define portSTACK_TYPE uint32_t
#define portBASE_TYPE long
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portSTACK_GROWTH (-1)
#define portTICK_PERIOD_MS ((TickType_t)1000 / configTICK_RATE_HZ)
#define portTICK_TYPE_IS_ATOMIC 1
#define portBYTE_ALIGNMENT 8
#define portNOP() __asm__ volatile("nop")
#define portMEMORY_BARRIER() __asm__ volatile("" ::: "memory")
#define portTASK_FUNCTION_PROTO(fn, arg) void fn(void *arg)
#define portTASK_FUNCTION(fn, arg) void fn(void *arg)
#define portCRITICAL_NESTING_IN_TCB 0

void vPortYield(void);
void os_enter_critical(void);
void os_exit_critical(void);
int vPortIsInsideInterrupt(void);

#define portYIELD() vPortYield()
#define portENTER_CRITICAL() os_enter_critical()
#define portEXIT_CRITICAL() os_exit_critical()
#define portDISABLE_INTERRUPTS() local_irq_disable()
#define portENABLE_INTERRUPTS() local_irq_enable()
#define portSET_INTERRUPT_MASK_FROM_ISR() _local_irq_save()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(flags) local_irq_restore(flags)
#define portEND_SWITCHING_ISR(required) do { if (required) vPortYield(); } while (0)
#define portYIELD_FROM_ISR(required) portEND_SWITCHING_ISR(required)
#define xPortIsInsideInterrupt() vPortIsInsideInterrupt()

static inline uint32_t port_clz(uint32_t value)
{
    uint32_t result;
    __asm__("clz %0,%1" : "=r"(result) : "r"(value));
    return result;
}
#if configMAX_PRIORITIES > 32
#error MIPS priority bitmap supports at most 32 priorities
#endif
#define portRECORD_READY_PRIORITY(p, bitmap) ((bitmap) |= (1UL << (p)))
#define portRESET_READY_PRIORITY(p, bitmap) ((bitmap) &= ~(1UL << (p)))
#define portGET_HIGHEST_PRIORITY(p, bitmap) ((p) = 31UL - port_clz(bitmap))

#endif
