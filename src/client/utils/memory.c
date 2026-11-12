#include "std_include.h"
#include "utils/memory.h"

int safe_str(const void *p, char *dst, size_t n)
{
    __try {
        strncpy(dst, (const char *)p, n - 1);
        dst[n - 1] = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int safe_ptr(const void *p, void **out)
{
    __try {
        *out = *(void *const *)p;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
