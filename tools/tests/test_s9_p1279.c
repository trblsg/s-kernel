#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../userland/src/s9_g2_bind.h"
#include "../../userland/src/s9_p1279_gl.h"
#include "../../userland/src/s9_ioshash4.h"
int main(void){
    struct s9_g2_state g={0};
    const unsigned t=0xde1;
    s9_g2_record(&g,0,t,100,t);g.verified[0]=1;
    s9_g2_forward(&g,1,1,1,1);s9_g2_record(&g,1,t,200,t);
    assert(s9_g2_redundant(&g,0,t,100,t)&&g.verified[0]);
    assert(s9_g2_redundant(&g,1,t,200,t)&&!g.verified[1]);
    s9_g2_forward(&g,1,1,2,2);s9_g2_record(&g,2,t,300,t);
    assert(g.valid[0]&&g.valid[1]&&g.valid[2]&&!g.invalidations);
    s9_g2_forward(&g,1,1,1,2);assert(!g.valid[0]&&!g.valid[1]&&!g.valid[2]);
    s9_g2_record(&g,0,t,100,t);s9_g2_forward(&g,1,0,0,0);assert(!g.valid[0]&&g.raw_binds==1);
    s9_g2_record(&g,0,t,100,t);s9_g2_forward(&g,0,1,0,0);assert(!g.valid[0]);
    struct s9_p1279_gl p={0};s9_p1279_begin(&p,100);
    s9_p1279_hash(&p,4096,120,1);s9_p1279_frame(&p,500,400,10,0,1,200,300);
    s9_p1279_hash(&p,8192,240,0);s9_p1279_frame(&p,1000,500,20,1,1,600,500);
    assert(p.frames==2&&p.zero_frames==1&&p.zero_hash_bytes==4096&&p.zero_hash_us==120&&p.hash_bytes==12288);
    assert(p.cpu_n==1&&p.cpu==200&&p.paired_wall==300&&p.max_wall==500&&p.attach==30);
    s9_p1279_share(&p,P9_FORMAT,1000,20);s9_p1279_backend(&p,0x10502,1000);
    assert(p.share_n[P9_FORMAT]==1&&p.share_bytes[P9_FORMAT]==1000&&p.backend_n[8]==1);
    s9_p1279_backend(&p,0x10501u|((5u|(12u<<3))<<17),2000);
    assert(p.import_stage_n[5]==1&&p.backend_last_error==0x10501u&&p.backend_n[8]==2&&p.backend_bytes[8]==3000);
    assert(!s9_p1279_report_due(&p,5000099)&&s9_p1279_report_due(&p,5000100));
    s9_p1279_reset(&p,5000100);assert(!p.frames&&!p.hash_bytes&&p.first==5000100);
    size_t n=11*1024*1024+7;unsigned char *b=malloc(n);assert(b);
    for(size_t i=0;i<n;i++)b[i]=(unsigned char)(i*17u+i/31u);
    assert(s9_ioshash4_fast(b,n)==s9_ioshash4_reference(b,n));uint64_t old=s9_ioshash4_fast(b,n);
    b[n-1]^=1;assert(s9_ioshash4_fast(b,n)!=old&&s9_ioshash4_fast(b,n)==s9_ioshash4_reference(b,n));free(b);
    puts("Rev9 per-unit G2, hash4 large-range/tail and recorder accounting: PASS");return 0;
}
