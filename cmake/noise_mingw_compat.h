#pragma once

#if defined(_WIN32) && !defined(__CYGWIN__)

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int posix_memalign(void** memptr, size_t alignment, size_t size);

#ifdef __cplusplus
}
#endif

#endif
