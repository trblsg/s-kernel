/* Compact read-only loader -> kernel ADT property ABI. No write authority. */
#ifndef HAKIOS_P545_ECT_ABI_H
#define HAKIOS_P545_ECT_ABI_H
#include <stdint.h>
#define P545_ECT_MAX_ROWS 32u
struct p545_ect_pair { uint32_t master, slave; };
struct p545_ect_snapshot {
    uint32_t abi, size, version, fingerprint, mif_n, int_n;
    struct p545_ect_pair mif[P545_ECT_MAX_ROWS], intr[P545_ECT_MAX_ROWS];
};
#endif
