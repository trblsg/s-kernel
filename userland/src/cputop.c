/*
 * Which processes take the processors: every few seconds, the processor time each process was given since the
 * last look (user and system together), the most first, as one line for the run's log:
 *
 *   cputop: 25 s: 412% in all: backboardd[13] 96% SpringBoard[41] 88% WeatherWidget[131] 40% ...
 *
 * (100% is one core's whole time.) tools/frames.py says when the cores were all busy ("cores busy") while the
 * window server's frames came late; this says with whom it shared them. A process born since the last look counts
 * with all the time it has had.
 *
 *   usage: cputop [SECONDS [LOOKS]]     every SECONDS (default 5), LOOKS times (default: until the run ends)
 */
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "p1274_user.h"

/* libproc's, which the iPhoneOS SDK does not declare (XNU's bsd/sys/proc_info.h, libsyscall/wrappers/libproc) */
#define PROC_ALL_PIDS           1
#define PROC_PIDTASKINFO        4
struct proc_taskinfo {
	uint64_t pti_virtual_size;
	uint64_t pti_resident_size;
	uint64_t pti_total_user;        /* in mach_absolute_time's units */
	uint64_t pti_total_system;
	uint64_t pti_threads_user;
	uint64_t pti_threads_system;
	int32_t pti_policy;
	int32_t pti_faults;
	int32_t pti_pageins;
	int32_t pti_cow_faults;
	int32_t pti_messages_sent;
	int32_t pti_messages_received;
	int32_t pti_syscalls_mach;
	int32_t pti_syscalls_unix;
	int32_t pti_csw;
	int32_t pti_threadnum;
	int32_t pti_numrunning;
	int32_t pti_priority;
};
int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);

#define MAX_PROCS       2048
#define SHOWN           10

struct sample {
	int pid;
	uint64_t time;                  /* user + system, mach_absolute_time's units */
    uint64_t user,sys,resident,virt;
    int32_t faults,pageins,csw,threads,running,cow,sent,received;
};

static struct sample last[MAX_PROCS], now[MAX_PROCS];
static int last_count;
/* Poll-observed lifetime, not a launchd/DAS decision or exact spawn hook.
 * Names are mapped to stable scalar categories, never sent as free text. */
static unsigned p1274_process_code(const char *name)
{
    const char *names[]={"PosterBoard","CollectionsPoster","EmojiPosterExtension","WeatherPoster",
        "PridePosterExtension","GradientPosterExtension","PhotosPosterProvider","UnityPosterExtension",
        "ExtragalacticPoster","healthappd","DayStreamProces","HistoricalAnaly","dasd","runningboardd",
        "backboardd","SpringBoard","launchd","healthd","filecoordinationd","chronod","wifid","locationd"};
    if(!strcmp(name,"mediaserverd"))return 24u;
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)
        if(!strcmp(name,names[i]) || ((i==10 || i==11 || i==16) && !strncmp(name,names[i],strlen(names[i]))))return i+1;
    return strstr(name,"Poster")?23u:0u;
}

struct share {
	int pid;
	uint64_t time;
};

static int
by_time(const void *a, const void *b)
{
	const struct share *x = a, *y = b;

	return x->time < y->time ? 1 : x->time > y->time ? -1 : 0;
}

static uint64_t
time_before(int pid, int *found,const struct sample **prior)
{
	for (int i = 0; i < last_count; i++) {
		if (last[i].pid == pid) {
			*found = 1;*prior=&last[i];
			return last[i].time;
		}
	}
	*found = 0;*prior=NULL;
	return 0;
}

/* Explicit USB request, persisted on the already running 5s sampler.
 * No boot mode is changed live. Atomic rename in a trusted root-owned dir. */
static void p12751_persist_next(void){
    if(p12751_boot_mode()<4 || geteuid()!=0)return;
    int next=-1,ack=-1;size_t n=sizeof(next);
    if(sysctlbyname("kern.hakios.p12751_next",&next,&n,NULL,0)!=0 || n!=sizeof(next) || next<0 || next>3)return;
    n=sizeof(ack);if(sysctlbyname("kern.hakios.p12751_ack",&ack,&n,NULL,0)!=0 || n!=sizeof(ack) || ack==next)return;
    const char *dir="/private/var/db";
    struct stat st;
    if(lstat(dir,&st)!=0 || !S_ISDIR(st.st_mode) || st.st_uid!=0 || (st.st_mode&0022))return;
    char path[]="/private/var/db/HakiosMetal.nextboot.XXXXXX";
    int fd=mkstemp(path);if(fd<0)return;
    char v[2]={(char)('0'+next),'\n'};
    bool ok=fchmod(fd,0600)==0 && write(fd,v,2)==2 && fsync(fd)==0;
    if(close(fd)!=0)ok=false;
    if(ok)ok=rename(path,"/private/var/db/HakiosMetal.nextboot")==0;
    if(!ok){unlink(path);return;}
    // Flush the containing directory if supported. File content was fsynced.
    int d=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(d>=0){(void)fsync(d);close(d);}
    if(sysctlbyname("kern.hakios.p12751_ack",NULL,NULL,&next,sizeof(next))==0)
        printf("P12751 nextboot persisted=%d (current mode unchanged)\n",next);
}
int
main(int argc, char **argv)
{
	unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 5, looks = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
	static int pids[MAX_PROCS];
	static struct share shares[MAX_PROCS];
	mach_timebase_info_data_t timebase;
	uint64_t start = mach_absolute_time(), before = start;

	if (seconds == 0) {
		seconds = 5;
	}
	mach_timebase_info(&timebase);
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (unsigned look = 0; looks == 0 || look <= looks; look++) {
		p12751_persist_next();
		uint64_t sample_begin=mach_absolute_time();
		int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)), count = 0, shown = 0;
		uint64_t at = mach_absolute_time(), all = 0;

		for (int i = 0; i < bytes / (int)sizeof(int) && count < MAX_PROCS; i++) {
			struct proc_taskinfo info;

			if (pids[i] <= 0 || proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &info, sizeof(info)) != sizeof(info)) {
				continue;
			}
			now[count].pid = pids[i];
			now[count].time = info.pti_total_user + info.pti_total_system;
            now[count].user=info.pti_total_user;now[count].sys=info.pti_total_system;
            now[count].faults=info.pti_faults;now[count].pageins=info.pti_pageins;now[count].csw=info.pti_csw;
            now[count].threads=info.pti_threadnum;now[count].running=info.pti_numrunning;
            now[count].resident=info.pti_resident_size;now[count].virt=info.pti_virtual_size;
            now[count].cow=info.pti_cow_faults;now[count].sent=info.pti_messages_sent;now[count].received=info.pti_messages_received;
			count++;
		}
		if (look > 0) {
			int n = 0;
			const bool p1274_observe=p1274_user_observing();
            const bool p12751_observe=p12751_boot_mode()>=0;
            unsigned detailed=0;

			for (int i = 0; i < count; i++) {
				int found;
				const struct sample *prior=NULL;
				uint64_t then = time_before(now[i].pid, &found,&prior);
				if((p1274_observe || p12751_observe) && (!found || now[i].time>then)){
					char name[64]={0};
					if(proc_name(now[i].pid,name,sizeof(name))>0){
						unsigned code=p1274_process_code(name);
						if(p12751_observe && detailed<12 && (code<=9 || code==13 || code==14 || code==23 || code==24) && code &&
                            now[i].time>then && now[i].time-then>(at-before)/20){
                            bool valid=prior && now[i].user>=prior->user && now[i].sys>=prior->sys &&
                              prior->faults>=0 && prior->pageins>=0 && prior->csw>=0 &&
                              now[i].faults>=prior->faults && now[i].pageins>=prior->pageins && now[i].csw>=prior->csw;
                            printf("P12751 cpu at=%llu pid=%d code=%u user=%llu sys=%llu faults=%d pageins=%d csw=%d threads=%d running=%d valid=%u\n",
                              (unsigned long long)at,now[i].pid,code,
                              (unsigned long long)(valid?now[i].user-prior->user:0),
                              (unsigned long long)(valid?now[i].sys-prior->sys:0),
                              valid?now[i].faults-prior->faults:0,valid?now[i].pageins-prior->pageins:0,valid?now[i].csw-prior->csw:0,
                              now[i].threads,now[i].running,valid?1:0);
                            printf("P12752 mem at=%llu pid=%d code=%u rss=%llu vsize=%llu cow_total=%d sent_total=%d recv_total=%d\n",
                                (unsigned long long)at,now[i].pid,code,(unsigned long long)now[i].resident,
                                (unsigned long long)now[i].virt,now[i].cow,now[i].sent,now[i].received);
                            detailed++;
                        }
                        if(p1274_observe && code && (!found || now[i].time>then))
							(void)p1274_emit(4,code,(uint64_t)now[i].pid,
							    now[i].time>=then?now[i].time-then:0,now[i].time,
							    found?0:1,at-before,at);
					}
				}

				if (now[i].time > then) {
					shares[n].pid = now[i].pid;
					shares[n].time = now[i].time - then;
					all += shares[n].time;
					n++;
				}
			}
			qsort(shares, (size_t)n, sizeof(shares[0]), by_time);
			/* the shares as percent of one core over the time between the looks (both in mach_absolute_time's units) */
			double span = (double)(at - before);
			printf("cputop: %llu s: %.0f%% in all:", (unsigned long long)((at - start) * timebase.numer / timebase.denom / 1000000000ull),
			    span > 0 ? 100.0 * (double)all / span : 0);
			for (int i = 0; i < n && shown < SHOWN; i++) {
				char name[64] = "?";
				double percent = span > 0 ? 100.0 * (double)shares[i].time / span : 0;

				if (percent < 1) {
					break;
				}
				proc_name(shares[i].pid, name, sizeof(name));
				printf(" %s[%d] %.0f%%", name, shares[i].pid, percent);
				shown++;
			}
			printf("\n");
			printf("cputop-meta: ticks=%llu sample_ticks=%llu processes=%d truncated=%u\n",
			    (unsigned long long)at,(unsigned long long)(mach_absolute_time()-sample_begin),count,bytes>=(int)sizeof(pids)?1u:0u);
		}
		memcpy(last, now, sizeof(now[0]) * (size_t)count);
		last_count = count;
		before = at;
		if (looks != 0 && look == looks) {
			break;
		}
		sleep(seconds);
	}
	return 0;
}
