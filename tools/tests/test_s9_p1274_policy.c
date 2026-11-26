#include <assert.h>
#include <stdio.h>
#include "../../drivers/power/exynos/hakios.p1274_policy.h"
int main(void){
    assert(s9_p1274_cap(P1274_BASE)==0);
    assert(s9_p1274_cap(P1274_CAP1469)==1469000);
    assert(s9_p1274_cap(P1274_CAP1261)==1261000);
    assert(s9_p1274_cap(P1274_CAP1066)==1066000);
    assert(s9_p1274_cores(P1274_REC2)==2);
    assert(s9_p1274_cores(P1274_REC1)==1);
    assert(s9_p1274_cores(P1274_REC0)==0);
    for(unsigned p=0;p<=9;p++){
        assert(s9_p1274_fast_cap(p,-273000,true,1261000)==1261000);
        assert(s9_p1274_fast_cap(p,76000,false,0)==1066000);
        assert(s9_p1274_fast_cap(p,64000,false,1066000)==1066000);
        for(int t=60000;t<=90000;t+=1000){
            uint32_t old=1261000,cap=s9_p1274_fast_cap(p,t,true,old);
            assert(cap && cap<=old);
        }
    }
    /* All profiles and the post-lease p==0 guard share proactive safety. */
    assert(s9_p1274_fast_cap(P1274_BASE,73000,true,0)==1261000);
    assert(s9_p1274_fast_cap(P1274_LOG2,73000,true,0)==1261000);
    assert(s9_p1274_fast_cap(0,73000,true,0)==1261000);
    assert(s9_p1274_fast_cap(0,65000,true,0)==1469000);
    assert(s9_p1274_fast_cap(P1274_FAST,69000,true,0)==1261000);
    assert(s9_p1274_fast_cap(P1274_FAST,70000,false,0)==1261000);
    assert(s9_p1274_fast_cap(P1274_FAST,73000,false,0)==1066000);
    assert(s9_p12741_little_cap(64000,64000,false,0)==0);
    assert(s9_p12741_little_cap(67000,68000,false,0)==1248000);
    assert(s9_p12741_little_cap(70000,69000,false,0)==1053000);
    assert(s9_p12741_little_cap(64000,76000,false,0)==1053000);
    assert(s9_p12741_little_cap(-273000,64000,false,0)==1053000);
    assert(s9_p12741_little_cap(64000,64000,false,1053000)==1053000);
    assert(s9_p12741_severity(4,67000,60)==1);
    assert(s9_p12741_severity(1,67000,199)==1);
    assert(s9_p12741_severity(1,67000,200)==2);
    assert(s9_p12741_severity(2,70000,50)==2);
    assert(s9_p12741_severity(8,64000,50)==3);
    assert(s9_p12741_severity(0,64000,0)==0);
    assert(s9_p1274_trigger(false,true,80000,true)==0);
    assert(s9_p1274_trigger(true,false,69000,false)==0);
    assert(s9_p1274_trigger(true,true,70000,true)==7);
    puts("P1274 exact portable policy PASS");return 0;
}
