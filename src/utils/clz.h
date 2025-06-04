#ifndef POMELO_UTILS_CLZ_H
#define POMELO_UTILS_CLZ_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif


/**
 * Counts the number of leading zero bits in a 64-bit value.
 * @param value The 64-bit value to count leading zeros in
 * @return Number of leading zeros (0-64), returns 64 if value is 0
 */
size_t pomelo_clz_64(uint64_t value);


/// @brief Calculate the leading zero mask
uint64_t pomelo_clz_64_mask(uint64_t value);


#ifdef __cplusplus
}
#endif
#endif // POMELO_UTILS_CLZ_H
