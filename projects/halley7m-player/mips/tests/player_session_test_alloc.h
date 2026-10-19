#ifndef PLAYER_SESSION_TEST_ALLOC_H
#define PLAYER_SESSION_TEST_ALLOC_H
#include <stddef.h>
void *player_test_memalign(size_t alignment, size_t bytes);
void player_test_free(void *pointer);
#endif
