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
    assert(s9_p124_floor_request(0)==0);
    assert(s9_p124_floor_request(1)==949000);
    assert(s9_p124_floor_request(2)==832000);
    assert(s9_p124_floor_request(3)==0 && s9_p124_floor_request(4)==0);
    assert(s9_p124_temp_ok(true,2000,65000,64000,65000));
    assert(!s9_p124_temp_ok(true,2001,65000,64000,65000));
    assert(!s9_p124_temp_ok(true,1,66000,64000,65000));
    assert(!s9_p124_temp_ok(false,1,72000,70700,69000));
    assert(!s9_p124_temp_ok(false,1,76000,64000,65000));
    assert(!s9_p124_temp_ok(false,1,65000,64000,72000));
    assert(!s9_p124_temp_ok(false,1,-273000,64000,65000));
    assert(s9_p124_boost_request(3,1794000,1,1,0)==1469000);
    assert(s9_p124_boost_request(3,1248000,0,0,1)==1248000);
    assert(s9_p124_boost_request(3,1469000,1,0,1)==1469000);
    assert(s9_p124_boost_request(4,1794000,1,1,0)==1261000);
    assert(s9_p124_boost_request(4,1469000,1,0,1)==1066000);
    assert(s9_p124_boost_request(4,1248000,0,0,1)==1053000);
    for(unsigned m=0;m<7;m++)
      for(unsigned f=0;f<2000000;f+=1000)
        for(unsigned big=0;big<2;big++)
          for(unsigned burst=0;burst<2;burst++)
            for(unsigned moving=0;moving<2;moving++)
              assert(s9_p124_boost_request(m,f,big,burst,moving)<=f);
    /* Warm quiet-floor admission accepts the real refused 67/65.6/65
     * baseline; interaction retains extra headroom. */
    assert(s9_p1241_temp_ok(1,true,100,67000,65600,65000));
    assert(s9_p1241_temp_ok(2,true,100,68000,67000,67000));
    assert(!s9_p1241_temp_ok(1,true,100,68001,67000,67000));
    assert(!s9_p1241_temp_ok(1,true,100,68000,67001,67000));
    assert(!s9_p1241_temp_ok(1,true,2001,67000,65600,65000));
    assert(s9_p1241_temp_ok(3,true,100,67000,65000,66000));
    assert(!s9_p1241_temp_ok(3,true,100,67000,65600,65000));
    assert(!s9_p1241_temp_ok(1,false,100,76000,66000,65000));
    assert(!s9_p1241_temp_ok(1,false,100,67000,70000,65000));
    assert(!s9_p1241_temp_ok(3,false,100,67000,66000,72000));
    assert(s9_p1241_rise_ok(68999,66999,67999,66000,65000,65000));
    assert(!s9_p1241_rise_ok(69000,66000,65000,66000,65000,65000));
    assert(!s9_p1241_rise_ok(67000,67000,65000,66000,65000,65000));
    assert(!s9_p1241_rise_ok(67000,66000,68000,66000,65000,65000));
    puts("P12/P12.4.1 CPU thermal lower-only fail-open policy PASS");
    return 0;
}
