/* tests/reclaim_state_cache_test.cpp: the allocator Lua is compiled with there (given to every file of the test
** with -include, for third_party/lua/lmem.c's l_realloc / l_free). */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *test_realloc(void *block, size_t oldSize, size_t size);
void test_free(void *block, size_t oldSize);
#ifdef __cplusplus
}
#endif
