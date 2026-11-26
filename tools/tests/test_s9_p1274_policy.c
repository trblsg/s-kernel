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
    assert(s9_p1274_fast_cap(P1274_BASE,73000,true,0)==0);
    assert(s9_p1274_fast_cap(P1274_FAST,69000,true,0)==1261000);
    assert(s9_p1274_fast_cap(P1274_FAST,70000,false,0)==1469000);
    assert(s9_p1274_fast_cap(P1274_FAST,73000,false,0)==1261000);
    assert(s9_p1274_trigger(false,true,80000,true)==0);
    assert(s9_p1274_trigger(true,false,69000,false)==0);
    assert(s9_p1274_trigger(true,true,70000,true)==7);
    puts("P1274 exact portable policy PASS");return 0;
}
