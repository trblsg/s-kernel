/* S9 G3D Power Lab portable state-policy regression: no hardware involved.
 * Build as strict C11 and C++17 and run under UBSan.
 */
#include "../../drivers/gpu/mali/hakios.mali_lab_policy.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
    /* Default must never power gate. */
    assert(s9_g3d_lab_eligible(0,0,1,0,950,0,UINT64_MAX,UINT64_MAX,10000)
           == S9_G3D_LAB_DISABLED);
    /* A screen-on 10fps cadence (job/frame every 100ms) never gates. */
    for(unsigned mode=1;mode<=3;++mode)
        for(uint64_t t=0;t<1000;t+=7)
            assert(s9_g3d_lab_eligible(mode,mode>=2,1,0,950,0,
                                      t%100,t%100,mode==3?1500:10000)
                   == S9_G3D_LAB_JOB_RECENT);
    assert(s9_g3d_lab_eligible(2,1,1,0,950,0,12000,12000,10000)
           == S9_G3D_LAB_ALLOW);
    assert(s9_g3d_lab_eligible(3,1,1,0,950,0,2000,2000,1500)
           == S9_G3D_LAB_ALLOW);
    assert(s9_g3d_lab_eligible(2,1,1,0,950,1,12000,12000,10000)
           == S9_G3D_LAB_QUEUED);
    assert(s9_g3d_lab_eligible(2,1,0,0,950,0,12000,12000,10000)
           == S9_G3D_LAB_NOT_READY);
    assert(s9_g3d_lab_eligible(2,1,1,7,950,0,12000,12000,10000)
           == S9_G3D_LAB_FAULT);
    assert(s9_g3d_lab_eligible(2,1,1,0,925,0,12000,12000,10000)
           == S9_G3D_LAB_VOLTAGE);
    assert(s9_g3d_lab_eligible(2,1,1,0,950,0,500,12000,10000)
           == S9_G3D_LAB_JOB_RECENT);
    assert(s9_g3d_lab_eligible(2,1,1,0,950,0,12000,500,10000)
           == S9_G3D_LAB_FRAME_RECENT);
    /* Post-lock recheck must reject a queue that appeared during locking. */
    assert(s9_g3d_lab_eligible(2,1,1,0,950,0,20000,20000,10000)
           == S9_G3D_LAB_ALLOW);
    assert(s9_g3d_lab_eligible(2,1,1,0,950,1,20000,20000,10000)
           != S9_G3D_LAB_ALLOW);
    /* Only conservative voltage targets are accepted; no 600mV APM dive. */
    for(unsigned mv=0;mv<=1200;++mv){
        int desired=mv==900||mv==925||mv==950;
        assert(s9_g3d_lab_mv_allowed(mv)==desired);
    }
    assert(s9_g3d_lab_vsel(900)==96);
    assert(s9_g3d_lab_vsel(925)==100);
    assert(s9_g3d_lab_vsel(950)==104);
    assert(s9_g3d_lab_mv_from_vsel(96)==900);
    assert(s9_g3d_lab_mv_from_vsel(100)==925);
    assert(s9_g3d_lab_mv_from_vsel(104)==950);
    puts("S9 G3D power eligibility, post-lock race prevention and voltage bounds: PASS");
    return 0;
}
