#if defined(_WIN32) && !defined(__CYGWIN__)

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>

#ifndef HAVE_POSIX_MEMALIGN
int posix_memalign(void** memptr, size_t alignment, size_t size) {
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  void* ptr = _aligned_malloc(size, alignment);
  if (!ptr) {
    return ENOMEM;
  }
  *memptr = ptr;
  return 0;
}
#endif

#endif
