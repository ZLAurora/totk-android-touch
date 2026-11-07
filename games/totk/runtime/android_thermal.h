#pragma once
#include <stdint.h>
#include <stdatomic.h>
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
void runtime_thermal_bind(_Atomic uint32_t *floor);
void runtime_thermal_frame(unsigned guest_interval);
#else
static inline void runtime_thermal_bind(_Atomic uint32_t *floor) { (void)floor; }
static inline void runtime_thermal_frame(unsigned interval) { (void)interval; }
#endif
