#include <time.h>
#include "uv.h"
#include "platform-uv.h"


uint64_t pomelo_platform_uv_hrtime(pomelo_platform_uv_t * platform) {
    (void) (platform);
    return uv_hrtime();
}


uint64_t pomelo_platform_uv_now(pomelo_platform_uv_t * platform) {
    (void) platform;
    uv_timespec64_t timespec;

    if (uv_clock_gettime(UV_CLOCK_REALTIME, &timespec) < 0) {
        return 0;
    }

    return timespec.tv_sec * 1000ULL + timespec.tv_nsec / 1000000ULL;
}
