#ifndef AI_ENTROPY_TEST_HAL_H
#define AI_ENTROPY_TEST_HAL_H
#include <stdint.h>
typedef struct { uint32_t CLKGR0; } ai_entropy_test_cpm_t;
extern ai_entropy_test_cpm_t ai_entropy_test_cpm;
#define CPM_Instance (&ai_entropy_test_cpm)
unsigned long _local_irq_save(void);
void local_irq_restore(unsigned long flags);
#endif
