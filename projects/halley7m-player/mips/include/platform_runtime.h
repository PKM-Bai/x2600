#ifndef PLAYER_PLATFORM_RUNTIME_H
#define PLAYER_PLATFORM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

/* Called once by main, before creating tasks. No U-Boot services remain live. */
void platform_init(void);
uint64_t platform_time_us(void);
size_t platform_free_heap(void);
size_t platform_isr_stack_unused(void);
/* Nonblocking UART0 input: -1 means no byte available. */
int platform_uart_getchar(void);
void platform_panic(const char *message) __attribute__((noreturn));
void platform_assert_failed(const char *file, unsigned line) __attribute__((noreturn));
unsigned platform_irq_depth(void);

#endif
