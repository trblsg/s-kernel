#ifndef P127542_AUDIO_H
#define P127542_AUDIO_H
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/sysctl.h>

/* Reuse the existing cputop sample cadence; no additional idle timer.
 * Fixed argv only. Never expose arbitrary execution through the console.
 * Each child is bounded; cancellation/temperature/display revoke the request. */
extern char **environ;
static pid_t audio_child;
static int audio_generation,audio_stage,audio_seen;
static uint64_t audio_started;
static int audio_read(const char *name){
    int v=0;size_t n=sizeof(v);
    return sysctlbyname(name,&v,&n,NULL,0)==0 && n==sizeof(v)?v:0;
}
static void audio_report(int result){
    if(sysctlbyname("kern.hakios.p127542_audio_result",NULL,0,&result,sizeof(result))!=0)
        printf("p127542-audio: result refused generation=%d\n",audio_generation);
    else printf("p127542-audio: result=%d\n",result);
    fflush(stdout);
    unlink("/tmp/p127542-record.wav");
    audio_generation=audio_stage=0;
}
static int audio_spawn(int stage){
    char *play[]={"/redwood/bin/sound_test","headless","route","level","0.25","tone","2",NULL};
    char *rec[]={"/redwood/bin/record_test","3","--gate",NULL};
    char *replay[]={"/redwood/bin/sound_test","replay_gate",NULL};
    char **argv=stage==1?play:stage==2?rec:replay;
    int err=posix_spawn(&audio_child,argv[0],NULL,NULL,argv,environ);
    if(err){audio_child=0;return err;}
    audio_started=mach_absolute_time();
    printf("p127542-audio: generation=%d stage=%d\n",audio_generation,stage);
    fflush(stdout);return 0;
}
static void audio_tick(void){
    int request=audio_read("kern.hakios.p127542_audio");
    if(audio_child){
        int status=0;
        pid_t p=waitpid(audio_child,&status,WNOHANG);
        if(p<0 && errno==EINTR)return;
        mach_timebase_info_data_t tb;mach_timebase_info(&tb);
        uint64_t elapsed=mach_absolute_time()-audio_started;
        bool expired=tb.numer && elapsed>12000000000ull*tb.denom/tb.numer;
        if(p==0 && (request!=audio_generation || expired)){
            kill(audio_child,SIGKILL);return; /* reap on the next active tick */
        }
        if(p==0)return;
        audio_child=0;
        if(p<0 || expired || !WIFEXITED(status) || WEXITSTATUS(status) || request!=audio_generation){
            audio_report(-audio_generation);return;
        }
        if(++audio_stage>3){audio_report(audio_generation);return;}
        if(audio_spawn(audio_stage))audio_report(-audio_generation);
        return;
    }
    if(request>0 && request!=audio_seen){
        audio_seen=request;
        if(sysctlbyname("kern.hakios.p127542_audio_claim",NULL,0,&request,sizeof(request))!=0)return;
        audio_generation=request;audio_stage=1;
        unlink("/tmp/p127542-record.wav");
        if(audio_spawn(audio_stage))audio_report(-audio_generation);
    }
}

#endif
