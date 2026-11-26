/* P12751: boot-immutable PanVK selection and independent, bounded paired
 * ICD stage evidence. Included after P1274 helpers. Never writes policy. */
#ifndef HAKIOS_P12751_H
#define HAKIOS_P12751_H
#define P12751_STAGES 512u
#define P12751_BOOT_STAGE_LIMIT 320u
static int p12751_next=-1,p12751_ack=-1;
static unsigned p12751_stage_n,p12751_stage_done,p12751_stage_drops,p12751_stage_select;
struct p12751_stage_row {
    uint64_t at,end,task,tid,cpu,user,sys,minor,major,images0,images1;
    uint32_t pid,stage,ok,usage;
};
static struct p12751_stage_row p12751_stage_rows[P12751_STAGES];
static LCK_GRP_DECLARE(p12751_group,"p12751-stage");
static LCK_SPIN_DECLARE(p12751_lock,&p12751_group);
static bool p12751_enabled(void){
    int v=0;return p1274_boot_enabled() && p1275_boot_enabled() &&
        PE_parse_boot_argn("p12751",&v,sizeof(v)) && v==1;
}
static int p12751_latch_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=__atomic_load_n(&p12751_latched,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    uint64_t tid=0,task=0;uint32_t pid=0;hakios_p1273_identity(&tid,&task,&pid);
    if(!p12751_enabled() || pid!=1 || v<0 || v>7 ||
       __atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)!=0 || mach_absolute_time()>=p1274_ticks_ms(225000))return EPERM;
    int expected=-1;
    return __atomic_compare_exchange_n(&p12751_latched,&expected,v,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)?0:EBUSY;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12751_boot_latch,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12751_latch_sysctl,"I","P12751 PID1 once: bit0 opt bit1 early bit2 kept data");
static int p12751_revision=1;
SYSCTL_INT(_kern_hakios,OID_AUTO,p12751_revision,CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,&p12751_revision,0,"P12751 host revision");
static int p12751_next_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=__atomic_load_n(&p12751_next,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    int boot=__atomic_load_n(&p12751_latched,__ATOMIC_ACQUIRE);
    if(!p12751_enabled() || boot<4 || v<0 || v>3 ||
       __atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)!=3 || p125_thermal_reason())return EPERM;
    /* Next boot request only; PID1 latch is immutable. */
    __atomic_store_n(&p12751_ack,-1,__ATOMIC_RELEASE);
    __atomic_store_n(&p12751_next,v,__ATOMIC_RELEASE);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12751_next,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12751_next_sysctl,"I","P12751 request next boot persisted by cputop; no live switch");
static int p12751_ack_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=__atomic_load_n(&p12751_ack,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(!p12751_enabled() || v<0 || v>3 || v!=__atomic_load_n(&p12751_next,__ATOMIC_ACQUIRE))return EPERM;
    __atomic_store_n(&p12751_ack,v,__ATOMIC_RELEASE);return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12751_ack,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12751_ack_sysctl,"I","P12751 acknowledged persistence, next boot only");
static bool p12751_stage_start_ok(uint64_t now){
    unsigned state=__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE);
    return (state==0 && now<p1274_ticks_ms(225000)) || (state==2 && p1275_mode_now(now));
}
static bool p12751_stage_end_ok(uint64_t now){
    unsigned state=__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE);
    if(state==0)return now<p1274_ticks_ms(235000);
    if(state==1 || state==2)return now<p1274_ticks_ms(730000);
    return state==3 && now>=p1274_end && now-p1274_end<=p1274_ticks_ms(10000);
}
static bool p12751_stage_sealed(uint64_t now){
    return __atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)==3 &&
       (p12751_stage_n==p12751_stage_done || !p12751_stage_end_ok(now));
}
/* op,stage,start,thread_delta_ns,process user/sys us,minor/major faults,
 * before/after loaded image counts, success, usage valid. Kernel stamps end
 * and identity. No arbitrary strings or pointers, no new stage after freeze. */
static int p12751_stage_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    uint64_t v[12],now=mach_absolute_time(),tid=0,task=0;uint32_t pid=0;
    if(!p12751_enabled() || !req->newptr || req->newlen!=sizeof(v) ||
       __atomic_load_n(&p12751_latched,__ATOMIC_ACQUIRE)<0)return EPERM;
    int e=SYSCTL_IN(req,v,sizeof(v));if(e)return e;
    if(v[0]>1 || v[1]<7 || v[1]>22 || v[1]==20 || v[1]==21 || !v[2] || v[2]>now ||
       v[10]>1 || v[11]>1 || v[8]>UINT32_MAX || v[9]>UINT32_MAX)return EINVAL;
    hakios_p1273_identity(&tid,&task,&pid);if(!pid || !task || !tid)return EPERM;
    lck_spin_lock(&p12751_lock);now=mach_absolute_time();
    if(v[0]==0){
        if(!p12751_stage_start_ok(now) || now-v[2]>p1274_ticks_ms(1000)){e=EPERM;goto out;}
        unsigned limit=__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)==0?P12751_BOOT_STAGE_LIMIT:P12751_STAGES;
        unsigned same=0;
        for(unsigned i=0;i<p12751_stage_n;i++){
            const struct p12751_stage_row *r=&p12751_stage_rows[i];
            if(r->task==task){same++;if(r->tid==tid && r->stage==v[1] && r->at==v[2]){e=EINVAL;goto out;}}
        }
        if(p12751_stage_n>=limit || same>=64){p12751_stage_drops++;e=ENOSPC;goto out;}
        struct p12751_stage_row *r=&p12751_stage_rows[p12751_stage_n++];
        r->at=v[2];r->task=task;r->tid=tid;r->pid=pid;r->stage=(uint32_t)v[1];
        r->cpu=UINT64_MAX;r->images0=v[8];r->ok=2;
    }else{
        if(!p12751_stage_end_ok(now) || p12751_stage_sealed(now)){e=EPERM;goto out;}
        bool found=false;
        for(unsigned i=p12751_stage_n;i>0;i--){
            struct p12751_stage_row *r=&p12751_stage_rows[i-1];
            if(r->at==v[2] && r->tid==tid && r->task==task && r->stage==v[1] && !r->end){
                r->end=now;r->cpu=v[3];r->user=v[4];r->sys=v[5];r->minor=v[6];r->major=v[7];
                r->images1=v[9];r->ok=(uint32_t)v[10];r->usage=(uint32_t)v[11];
                p12751_stage_done++;found=true;break;
            }
        }
        if(!found){e=ENOENT;goto out;}
    }
    e=0;
out:lck_spin_unlock(&p12751_lock);return e;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12751_stage_event,CTLTYPE_OPAQUE|CTLFLAG_WR|CTLFLAG_LOCKED|CTLFLAG_ANYBODY|CTLFLAG_KERN,NULL,0,p12751_stage_sysctl,"S,p12751stage","P12751 bounded paired advisory ICD stage records");
static int p12751_stage_info_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    char buf[128];lck_spin_lock(&p12751_lock);
    unsigned sealed=p12751_stage_sealed(mach_absolute_time())?1:0,n=p12751_stage_n,done=p12751_stage_done,drops=p12751_stage_drops;
    lck_spin_unlock(&p12751_lock);
    snprintf(buf,sizeof(buf),"version=1 sealed=%u count=%u done=%u drops=%u",sealed,n,done,drops);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12751_stage_info,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12751_stage_info_sysctl,"A","P12751 paired stage coverage");
static int p12751_stage_select_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=(int)__atomic_load_n(&p12751_stage_select,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);if(e||!req->newptr)return e;
    if(v<0 || v>=(int)P12751_STAGES)return EINVAL;
    __atomic_store_n(&p12751_stage_select,(unsigned)v,__ATOMIC_RELEASE);return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12751_stage_select,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12751_stage_select_sysctl,"I","P12751 stage cursor");
static int p12751_stage_page_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    unsigned s=__atomic_load_n(&p12751_stage_select,__ATOMIC_ACQUIRE);
    struct p12751_stage_row r;char buf[448];
    lck_spin_lock(&p12751_lock);
    if(!p12751_stage_sealed(mach_absolute_time()) || s>=p12751_stage_n){lck_spin_unlock(&p12751_lock);return EBUSY;}
    r=p12751_stage_rows[s];lck_spin_unlock(&p12751_lock);
    snprintf(buf,sizeof(buf),"version=1 select=%u at=%llu end=%llu pid=%u task=%llu tid=%llu stage=%u cpu=%llu user=%llu sys=%llu minor=%llu major=%llu images0=%llu images1=%llu ok=%u usage=%u",s,
      (unsigned long long)r.at,(unsigned long long)r.end,r.pid,(unsigned long long)r.task,(unsigned long long)r.tid,r.stage,
      (unsigned long long)r.cpu,(unsigned long long)r.user,(unsigned long long)r.sys,(unsigned long long)r.minor,(unsigned long long)r.major,
      (unsigned long long)r.images0,(unsigned long long)r.images1,r.ok,r.usage);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12751_stage_page,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12751_stage_page_sysctl,"A","P12751 immutable paired stage row");
static int p12751_stage_focus_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    char buf[256];size_t used=0;unsigned count=0;
    lck_spin_lock(&p12751_lock);
    if(!p12751_stage_sealed(mach_absolute_time())){lck_spin_unlock(&p12751_lock);return EBUSY;}
    used=(size_t)snprintf(buf,sizeof(buf),"version=1");
    for(unsigned i=p12751_stage_n;i>0 && count<24;i--){
        unsigned stage=p12751_stage_rows[i-1].stage;
        if(stage!=7 && stage!=11 && stage!=8)continue;
        int n=snprintf(buf+used,sizeof(buf)-used," s=%u",i-1);
        if(n<0 || (size_t)n>=sizeof(buf)-used)break;
        used+=(size_t)n;count++;
    }
    lck_spin_unlock(&p12751_lock);return SYSCTL_OUT(req,buf,used+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12751_stage_focus,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12751_stage_focus_sysctl,"A","P12751 newest key stages");
#endif
