#define _BSD_SOURCE
#include <stdlib.h>

void *memalign(size_t, size_t);

void *valloc(size_t size) {
    return memalign(65536, size);
}
