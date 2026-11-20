/* SPDX-License-Identifier: MIT
 * Portable strict C11/C++17 regression for S9 one-build diagnostic bundle.
 * No SDK, GL context, syscalls or GPU needed.
 */
#include "../src/s9_g2_bind.h"
#include "../src/s9_gl_diag.h"
#include "../src/metalvk/s9_bootarg_flag.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const char *args="p1_userdict=1 p11_trace=1 p12_diag=1 "
                     "g2_bind=0 g21_diag=1 glpath=1 glhash=1 "
                     "p_layout_probe=1 mali_gov_frame=455";
    assert(s9_has_boot_flag(args,"p12_diag=1"));
    assert(s9_has_boot_flag(args,"g21_diag=1"));
    assert(s9_has_boot_flag(args,"glpath=1"));
    assert(s9_has_boot_flag(args,"glhash=1"));
    assert(s9_has_boot_flag(args,"p_layout_probe=1"));
    assert(!s9_has_boot_flag(args,"g2_bind=1"));
    assert(!s9_has_boot_flag("no_glhash=1 glhash=10","glhash=1"));
    struct s9_g2_state g={0};
    const unsigned tex=0x0de1;
    assert(!s9_g2_redundant(&g,0,tex,10,tex));
    g.attempts=10;
    g.client_same=3;g.client_different=7;
    g.cache_invalid=2;g.cache_same=5;g.cache_different=3;
    g.forwarded_binds=8;g.raw_binds=2;g.raw_deletes=1;
    g.other_target_or_unit=4;
    g.verify_serial=127;
    s9_g2_record(&g,0,tex,10,tex);
    assert(s9_g2_redundant(&g,0,tex,10,tex));
    s9_g2_reset_counters(&g);
    assert(g.attempts==0 && g.client_same==0 && g.client_different==0);
    assert(g.cache_same==0 && g.cache_invalid==0 && g.cache_different==0);
    assert(g.raw_binds==0 && g.forwarded_binds==0 && g.raw_deletes==0);
    assert(g.other_target_or_unit==0);
    assert(g.verify_serial==127); /* across 60-frame buckets */
    assert(s9_g2_redundant(&g,0,tex,10,tex)); /* state persists */
    struct s9_gl_diag d;
    memset(&d,0,sizeof(d));
    for(unsigned i=0;i<100;++i) {
        const int sample=s9_gl_diag_sample16(&d.bind_serial);
        assert(sample==(i%16==15));
    }
    d.bind_native_us=30;d.bind_lookup_us=20;d.bind_check_us=10;
    d.bind_upload_us=40;d.hash_samples=3;d.hash_us=200;
    d.hash_bytes=8192;d.hash_same=2;d.hash_changed=1;
    d.draw_native_calls=5;d.draw_native_us=30;d.draw_other_us=50;
    d.flush_raw_us=444;d.hash_serial=17;d.draw_serial=19;
    s9_gl_diag_reset_window(&d);
    assert(d.bind_serial==100 && d.hash_serial==17 && d.draw_serial==19);
    assert(d.bind_native_us==0 && d.hash_us==0 && d.hash_bytes==0);
    assert(d.hash_samples==0 && d.draw_native_calls==0 && d.flush_raw_us==0);
    for(unsigned i=100;i<112;++i) {
        const int sample=s9_gl_diag_sample16(&d.bind_serial);
        assert(sample==(i%16==15));
    }
    puts("S9 unified P1.2/G2.1/GLpath/GLhash diagnostic counters: PASS");
    return 0;
}
