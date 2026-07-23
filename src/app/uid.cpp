#include "app/uid.h"

#include <ctype.h>

void uid_normalize(const char* in, char* out, size_t cap) {
    size_t j = 0;
    for (; *in && j + 1 < cap; in++) {
        char c = *in;
        if (c == ':' || c == '-' || c == ' ') continue;
        out[j++] = (char)toupper((unsigned char)c);
    }
    if (cap) out[j] = '\0';
}
