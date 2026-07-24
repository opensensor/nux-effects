#include <stddef.h>

void *memset(void *destination, int value, size_t length)
{
    unsigned char *output = (unsigned char *)destination;
    const unsigned char byte = (unsigned char)value;

    for (size_t index = 0U; index < length; ++index) {
        output[index] = byte;
    }
    return destination;
}
