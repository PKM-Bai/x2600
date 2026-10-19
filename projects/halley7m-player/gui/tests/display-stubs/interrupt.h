#ifndef DISPLAY_HOST_INTERRUPT_H
#define DISPLAY_HOST_INTERRUPT_H
void ll_request_irq(int irq, void (*handler)(int, void *), void *argument);
void ll_release_irq(int irq);
#endif
