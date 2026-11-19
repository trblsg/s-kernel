/* SPDX-License-Identifier: MIT
 * Exact boot-argument matcher, pure C for native cross-platform unit tests.
 * Prevents p1_userdict=10 or prefixed tokens from enabling an SPI accidentally.
 */
#ifndef S9_BOOTARG_FLAG_H
#define S9_BOOTARG_FLAG_H
#include <string.h>
static inline int s9_has_boot_flag(const char *args, const char *token) {
    if (!args || !token || !*token) return 0;
    const size_t need = strlen(token);
    const char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') ++p;
        if ((size_t)(p - start) == need && memcmp(start, token, need) == 0)
            return 1;
    }
    return 0;
}
#endif
