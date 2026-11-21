/* P6.3 shadow classifier portable C11/C++17 validation.
 * No thermal clock/PMU writes occur in this header. */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include "../../drivers/power/exynos/hakios.p6_policy.h"
int main(void)
{
    struct s9_p6_shadow s={0,0,0,0};
    /* Cold static compositor ~10fps: candidate only, no actuation. */
    s=s9_p6_shadow_eval(s,72000,65000,10,12,0,50,100);
    assert(s.park_candidate==1 && s.big_hot==0 && s.gpu_hot==0);
    /* Interactive 60Hz output must never qualify for M3 parking. */
    s=s9_p6_shadow_eval(s,72000,65000,10,12,0,300,16);
    assert(s.park_candidate==0);
    s=s9_p6_shadow_eval(s,72000,65000,12,70,1,0,100);
    assert(s.park_candidate==0);
    /* BIG Exynos9810 control and hotplug reference thresholds, shadow only. */
    s=s9_p6_shadow_eval(s,83000,88000,30,20,0,0,100);
    assert(s.big_hot==1 && s.gpu_hot==1);
    s=s9_p6_shadow_eval(s,96000,95000,30,20,0,0,100);
    assert(s.big_hot==2 && s.gpu_hot==2);
    s=s9_p6_shadow_eval(s,-273000,-273000,30,20,0,0,100);
    assert(s.big_hot==2 && s.gpu_hot==2);
    s=s9_p6_shadow_eval(s,92000,91000,30,20,0,0,100);
    assert(s.big_hot==2 && s.gpu_hot==2);
    s=s9_p6_shadow_eval(s,90000,89000,30,20,0,0,100);
    assert(s.big_hot==1 && s.gpu_hot==1);
    s=s9_p6_shadow_eval(s,77000,82000,30,20,0,0,100);
    assert(s.big_hot==0 && s.gpu_hot==0);
    /* Unknown/invalid load and hot sensors cannot be positive evidence. */
    s=s9_p6_shadow_eval(s,130000,-1,UINT32_MAX,UINT32_MAX,0,0,100);
    assert(s.park_candidate==0 && s.big_hot==0 && s.gpu_hot==0);
    return 0;
}
