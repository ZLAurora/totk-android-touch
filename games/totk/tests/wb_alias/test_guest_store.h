/* Native guest-style store, from the repository's map-limit test helper. */
#pragma once
#include <stdint.h>
/* A libc/TSan memset interceptor describes its write BEFORE the first machine store faults. Make the ownership
 * transition with a real guest-style native store first; the following fully instrumented memset then observes the
 * actual post-fault ordering. This is not a sanitizer suppression: an unprotected probe still performs the bad write. */
static void lag_guest_store(void *dst, uint8_t byte) {
#if defined(__x86_64__)
    __asm__ volatile("movb %b1, (%0)" : : "r"(dst), "q"(byte) : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("strb %w1, [%0]" : : "r"(dst), "r"((unsigned)byte) : "memory");
#else
    *(volatile uint8_t *)dst = byte;
#endif
}
