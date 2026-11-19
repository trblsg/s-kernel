/* Pure C11/C++17 G2 native-bind fast-path regression.
 * Deliberately tests state invalidation that protects internal raw GLES
 * operations and texture-name deletion/reuse. No Apple SDK dependency.
 */
#include "../src/s9_g2_bind.h"
#include "../src/metalvk/s9_bootarg_flag.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const unsigned tex2d=0x0de1;
    struct s9_g2_state s;
    memset(&s,0,sizeof(s));
    assert(!s9_g2_redundant(&s,0,tex2d,4,tex2d));
    s9_g2_record(&s,0,tex2d,4,tex2d);
    assert(s9_g2_redundant(&s,0,tex2d,4,tex2d));
    assert(!s9_g2_redundant(&s,0,tex2d,5,tex2d));
    assert(!s9_g2_redundant(&s,1,tex2d,4,tex2d));
    assert(!s9_g2_redundant(&s,8,tex2d,4,tex2d));
    assert(!s9_g2_redundant(&s,0,0x8513,4,tex2d)); /* different target */
    s9_g2_record(&s,1,tex2d,6,tex2d);
    assert(s9_g2_redundant(&s,0,tex2d,4,tex2d));
    assert(s9_g2_redundant(&s,1,tex2d,6,tex2d));
    s9_g2_invalidate(&s); /* internal raw glBindTexture, glDeleteTextures */
    assert(!s9_g2_redundant(&s,0,tex2d,4,tex2d));
    assert(!s9_g2_redundant(&s,1,tex2d,6,tex2d));
    s9_g2_record(&s,1,tex2d,7,tex2d);
    assert(s9_g2_redundant(&s,1,tex2d,7,tex2d));
    s9_g2_record(&s,8,tex2d,7,tex2d); /* out-of-range => fail closed */
    assert(!s9_g2_redundant(&s,1,tex2d,7,tex2d));
    s9_g2_record(&s,0,tex2d,0,tex2d); /* unbind texture zero is state */
    assert(s9_g2_redundant(&s,0,tex2d,0,tex2d));
    s.attempts=17;s.skipped=5;s.executed=12;s.verify_serial=127;
    s9_g2_reset_counters(&s);
    assert(s.attempts==0 && s.skipped==0 && s.executed==0);
    assert(s.verify_serial==127); /* verification cadence never restarts per frame bucket */
    assert(s9_g2_redundant(&s,0,tex2d,0,tex2d)); /* reset keeps state */
    assert(s9_has_boot_flag("p1_userdict=1 p11_trace=1 g2_bind=1 g2_verify=1", "g2_bind=1"));
    assert(!s9_has_boot_flag("notg2_bind=1 g2_bind=10","g2_bind=1"));
    assert(!s9_has_boot_flag("p11_trace=10", "p11_trace=1"));
    puts("P1.1/G2 safe per-unit bind cache, invalidation, flags: PASS");
    return 0;
}
