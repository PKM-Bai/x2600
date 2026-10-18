#ifndef REGBA_RUNTIME_SELFTEST_H
#define REGBA_RUNTIME_SELFTEST_H

/* Asynchronous, nondestructive RTOS/FPU/allocator check. Returns 0 or -errno.
 * PASS is emitted only after actual task preemption and post-OOM scheduling.
 * Probe error codes: 1=GPR, 2=HI, 3=LO, 4=FPR, 5=FCSR. */
int runtime_selftest_start(void);

/* Synchronous DDR check of one exclusively allocated application-heap block.
 * Leaves about 2 MiB free; does not cover reserved/live memory or all 64 MiB.
 * The caller must be a task with the scheduler running. Returns 0 or -errno. */
int runtime_memory_test(void);

#endif
