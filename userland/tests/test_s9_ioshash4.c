/* Strict C11 / C++17 P0 four-lane IOSurface checksum regression.
 * Check the optimized and scalar implementations agree bit-for-bit across
 * all tail lengths, unaligned memory, patterned and random surface bytes.
 */
#include "../src/s9_ioshash4.h"
#include "../src/metalvk/s9_bootarg_flag.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint64_t rng=UINT64_C(0x731c825d2ee1b9a7);
static uint64_t next_random(void) {
    rng^=rng<<7;rng^=rng>>9;rng^=rng<<8;
    return rng;
}
static void compare(const uint8_t *mem,size_t n) {
    const uint64_t a=s9_ioshash4_fast(mem,n);
    const uint64_t b=s9_ioshash4_reference(mem,n);
    if(a!=b) {fprintf(stderr,"P0 MISMATCH size=%zu fast=%llx scalar=%llx\n",
        n,(unsigned long long)a,(unsigned long long)b);abort();}
}
int main(void) {
    assert(s9_has_boot_flag("p0hash=1 p0verify=1 p1_mps_diag=1 p3power=1 p3rail=0","p0hash=1"));
    assert(s9_has_boot_flag("p0hash=1 p0verify=1 p1_mps_diag=1 p3power=1 p3rail=0","p3power=1"));
    assert(!s9_has_boot_flag("notp0hash=1 p0hash=10","p0hash=1"));
    compare(NULL,0);
    uint8_t *raw=(uint8_t *)malloc(12u*1024u*1024u+64u);
    assert(raw);
    uint8_t *buf=raw+3; /* unaligned source, including trailing bytes */
    for(size_t i=0;i<12u*1024u*1024u+32u;++i)buf[i]=(uint8_t)next_random();
    for(size_t n=0;n<=4097;++n)compare(buf,n);
    for(unsigned off=0;off<16;++off) {
        for(size_t n=0;n<512;++n)compare(buf+off,n);
    }
    const size_t cases[]={3,7,8,9,15,16,17,31,32,33,64,65,512,1024,4096,16384,
                           65536,524288,1024*1024,1920*1080*4,11*1024*1024};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        const size_t n=cases[i];
        const uint64_t baseline=s9_ioshash4_fast(buf,n);
        compare(buf,n);
        const size_t positions[]={0,n/2,n-1};
        for(size_t j=0;j<3;++j) {
            const size_t at=positions[j];
            buf[at]^=0x35;
            compare(buf,n);
            assert(s9_ioshash4_fast(buf,n)!=baseline); /* mutations including tail bytes */
            buf[at]^=0x35;
            assert(s9_ioshash4_fast(buf,n)==baseline);
        }
    }
    const size_t big=11u*1024u*1024u;
    volatile uint64_t sink=0;
    clock_t begin=clock();
    for(unsigned j=0;j<5;++j)sink^=s9_ioshash4_fast(buf,big);
    clock_t fast=clock()-begin;
    begin=clock();
    for(unsigned j=0;j<5;++j)sink^=s9_ioshash4_reference(buf,big);
    clock_t reference=clock()-begin;
    printf("P0 full-byte 4-lane checksum PASS, 11MiB x5 CPU ticks fast=%ld scalar=%ld (hardware-dependent), sink=%llu\n",
           (long)fast,(long)reference,(unsigned long long)sink);
    free(raw);
    return 0;
}
