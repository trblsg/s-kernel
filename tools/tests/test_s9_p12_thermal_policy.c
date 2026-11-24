/* P12 hardware-independent compile/run of exact thermal eligibility helper.
 * cc -std=c11 -Wall -Wextra -Werror -O2 tools/tests/test_s9_p12_thermal_policy.c -o test-p12 && ./test-p12
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "../../drivers/power/exynos/hakios.p12_thermal.h"

static bool valid(uint32_t mode,uint32_t policy,uint32_t frames,
    uint32_t fingers,bool touch,bool late,int display,bool fresh,int32_t temp)
{
    return s9_p12_cpu_eligible(mode,policy,frames,fingers,touch,late,
        display,fresh,temp);
}
int main(void)
{
    /* Never active by default or while a finger is involved. */
    assert(!valid(0,2,100,0,false,false,1,true,72000));
    assert(!valid(1,1,100,0,false,false,1,true,72000));
    assert(!valid(1,3,100,0,false,false,1,true,72000));
    assert(!valid(1,4,100,0,false,false,1,true,72000));
    assert(!valid(1,2,29,0,false,false,1,true,72000));
    assert(!valid(1,2,100,1,false,false,1,true,72000));
    assert(!valid(1,2,100,0,true,false,1,true,72000));
    /* Any frame miss forces the stock rate target immediately. */
    assert(!valid(1,2,100,0,false,true,1,true,72000));
    assert(!valid(1,2,100,0,false,false,0,true,72000));
    assert(!valid(1,2,100,0,false,false,-1,true,72000));
    assert(!valid(1,2,100,0,false,false,1,false,72000));
    assert(!valid(1,2,100,0,false,false,1,true,67999));
    assert(!valid(1,2,100,0,false,false,1,true,83000));
    assert(valid(1,2,30,0,false,false,1,true,68000));
    assert(valid(1,2,60,0,false,false,1,true,72000));
    /* Exact stock target preserved if ineligible; reduced work demand
       never raises frequency or changes any frame deadline. */
    for(uint32_t load=0;load<=100;load++){
        assert(s9_p12_cpu_rated_load(load,false)==load);
        assert(s9_p12_cpu_rated_load(load,true)<=load);
    }
    assert(s9_p12_cpu_rated_load(70,true)==60);
    assert(s9_p12_cpu_rated_load(100,true)==85);
    puts("P12 CPU thermal stable-frame fail-open policy PASS");
    return 0;
}
