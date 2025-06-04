#if defined(_WIN64) || defined(_WIN32)
#include <intrin.h>
#endif
#include "clz.h"


size_t pomelo_clz_64(uint64_t value) {
    if (value == 0) return 64;

#if defined(_WIN64)
    unsigned long index = 0;
    _BitScanReverse64(&index, (unsigned __int64) (value));
    return 63 - index;

#elif defined(_WIN32)
    unsigned long index = 0;
    size_t result;
    if (_BitScanReverse(&index, (unsigned long) (value >> 32))) {
        result = 31 - index;
    } else {
        _BitScanReverse(&index, (unsigned long) value);
        result = 63 - index;
    }
    return result;  

#elif defined(__GNUC__) || defined(__clang__)
    return (size_t) __builtin_clzll(value);

#else // Portable implementation

    size_t count = 0;
    uint64_t mask = 1ULL << 63;
    while ((value & mask) == 0) {
        count++;
        mask >>= 1;
    }

    return count;
#endif
}


uint64_t pomelo_clz_64_mask(uint64_t value) {
    if (value == 0) return 0;

#if defined(_WIN64)
    unsigned long index = 0;
    _BitScanReverse64(&index, (unsigned __int64)value);
    return 1ULL << index;

#elif defined(_WIN32)
    unsigned long index = 0;
    if (_BitScanReverse(&index, (unsigned long)(value >> 32))) {
        return 1ULL << (index + 32);
    }
    _BitScanReverse(&index, (unsigned long)value);
    return 1ULL << index;

#elif defined(__GNUC__) || defined(__clang__)
    return 1ULL << (63 - __builtin_clzll(value));

#else
    // Optimized portable version
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    value |= value >> 32;
    return (value + 1) >> 1;
#endif
}
