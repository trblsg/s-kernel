/* SPDX-License-Identifier: MIT-0 */
/*
 * What the bring-up aids and the personas change in kern_exec.c, kept here: boot-arg systrace (the stage of an exec that refused
 * the image), applog= and applog_name= (a program's standard output on the console), the sysctl kern.exec_inject (libraries
 * dyld inserts into named programs), xpcproxy_idversion=, and the credential a posix_spawn asks of a persona there is none of.
 * kern_exec.c calls the functions below (sys/hakios.exec.h).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/namei.h>
#include <sys/imgact.h>
#include <sys/user.h>
#include <kern/startup.h>
#include <sys/hakios.exec.h>

/* Bring-up aid: with boot-arg systrace set, say which stage of an exec refused the image. */
TUNABLE(uint32_t, hakios_exec_trace, "systrace", 0);

void
hakios_exec_trace_report(struct image_params *imgp, const char *stage, int error)
{
	printf("exectrace: %s: %s failed: %d\n", imgp->ip_ndp->ni_cnd.cn_pnbuf, stage, error);
}

void
hakios_exec_trace_signature(proc_t p, struct image_params *imgp, int error, int cs_error)
{
	printf("exectrace: %s[%d]: process_signature failed: %d (csflags 0x%x, mac %d, cs_error %d)\n",
	    p->p_comm, proc_getpid(p), error, imgp->ip_csflags, imgp->ip_mac_return, cs_error);
}

/*
 * Bring-up aid, boot-arg applog=1: what applications and other programs started without a
 * terminal say is worth reading here. Their standard output and error, which launchd opens on
 * /dev/null, go to the console instead (that shows NSLog and the "Terminating app due to
 * uncaught exception" kind of message). With applog=2 every program also gets
 * OS_ACTIVITY_DT_MODE in its environment, which makes os_log write to standard error as well:
 * everything any program logs, and a much slower system. applog_name=NAME[,NAME...] with
 * applog=1 does that for the programs whose file names begin with one of the NAMEs only,
 * which is how to read what one application extension has to say (launchd starts those for
 * whoever asks, so there is no job description of ours to put the variable in).
 */
static TUNABLE(uint32_t, exec_applog, "applog", 0);
static TUNABLE_STR(exec_applog_name, 128, "applog_name", "");

static bool
exec_applog_oslog(struct image_params *imgp)
{
	const char *name, *p = exec_applog_name;

	if (exec_applog >= 2) {
		return true;
	}
	if (exec_applog == 0 || *p == 0 || imgp->ip_ndp == NULL) {
		return false;
	}
	name = imgp->ip_ndp->ni_cnd.cn_nameptr;
	while (*p != 0) {
		const char *word = p;

		while (*p != 0 && *p != ',') {
			p++;
		}
		if (p != word && (size_t)(p - word) <= imgp->ip_ndp->ni_cnd.cn_namelen && strncmp(word, name, (size_t)(p - word)) == 0) {
			return true;
		}
		if (*p == ',') {
			p++;
		}
	}
	return false;
}

const char *
hakios_exec_applog_path(int fd, const char *path)
{
	if (exec_applog && (fd == 1 || fd == 2) && strcmp(path, "/dev/null") == 0) {
		return "/dev/console";
	}
	return path;
}

/*
 * Bring-up aid: the sysctl kern.exec_inject holds "LIBRARY NAME NAME ..."; every program
 * whose file name is one of the NAMEs is started with DYLD_INSERT_LIBRARIES=LIBRARY in its
 * environment, so that dyld loads our library into it. Several such groups may follow each
 * other, separated by ";". A program named in more than one group gets every one of those
 * libraries, joined with ":" in the one DYLD_INSERT_LIBRARIES, so SpringBoard can carry both
 * our window-server library and the tweak loader. launchd refuses to pass DYLD_ variables
 * from a job description, which is why the kernel adds this one. Set once by our init (from
 * etc/inject.conf) before it hands over to launchd.
 */
static char exec_inject_conf[512];
SYSCTL_STRING(_kern, OID_AUTO, exec_inject, CTLFLAG_RW | CTLFLAG_LOCKED, exec_inject_conf, sizeof(exec_inject_conf),
    "LIBRARY NAME... [; LIBRARY NAME...]: programs started with the library inserted");

static bool
exec_inject_env(struct image_params *imgp, char *env, size_t size)
{
	const char *name, *p = exec_inject_conf;
	size_t namelen, used;
	bool any = false;

	if (*p == 0 || imgp->ip_ndp == NULL) {
		return false;
	}
	name = imgp->ip_ndp->ni_cnd.cn_nameptr;
	namelen = imgp->ip_ndp->ni_cnd.cn_namelen;
	used = strlcpy(env, "DYLD_INSERT_LIBRARIES=", size);
	while (*p != 0) {
		const char *library;
		size_t liblen;
		bool matched = false;

		while (*p == ' ' || *p == ';') {
			p++;
		}
		library = p;
		while (*p != 0 && *p != ' ' && *p != ';') {
			p++;
		}
		liblen = (size_t)(p - library);
		while (*p == ' ') {
			const char *word = ++p;

			while (*p != 0 && *p != ' ' && *p != ';') {
				p++;
			}
			if (!matched && (size_t)(p - word) == namelen && namelen != 0 && liblen != 0 &&
			    strncmp(word, name, namelen) == 0) {
				matched = true;         /* this group names this program; its library is added once */
			}
		}
		if (matched && liblen != 0 && used + 1 + liblen < size) {
			if (any) {
				env[used++] = ':';      /* dyld takes several libraries separated by ":" */
			}
			memcpy(env + used, library, liblen);
			used += liblen;
			env[used] = 0;
			any = true;
		}
	}
	return any;
}

/*
 * The variables added to the environment of the program being started, one at a time: *cursor says which are done. The text is in
 * the caller's buffer or a constant; NULL at the end.
 */
const char *
hakios_exec_extra_env(struct image_params *imgp, unsigned *cursor, char *buf, size_t size)
{
	if (*cursor == 0) {
		*cursor = 1;
		if (exec_inject_env(imgp, buf, size)) {
			return buf;
		}
	}
	if (*cursor == 1) {
		*cursor = 2;
		if (exec_applog_oslog(imgp)) {
			return "OS_ACTIVITY_DT_MODE=YES";
		}
	}
	return NULL;
}

/* Whether the program just exec'd gets a new identity version: not xpcproxy, unless xpcproxy_idversion=1. */
bool
hakios_exec_bumps_pidversion(proc_t p)
{
	static TUNABLE(uint32_t, xpcproxy_idversion, "xpcproxy_idversion", 0);

	/*
	 * (redwood) Except when xpcproxy becomes the service it was spawned for: then the process keeps
	 * xpcproxy's identity version, as if launchd had spawned the service itself, which is what launchd
	 * means by eliding xpcproxy ("service will attempt to elide xpcproxy"; here it spawns xpcproxy
	 * all the same). RunningBoard keeps the audit token of such a process from before the exec and
	 * never takes the newer one: every entitlement check it made for it failed (SecTaskLoadEntitlements
	 * error 3, a thousand times a minute with Safari open), and Safari's web processes got no
	 * assertions ("target is not running or doesn't have entitlement
	 * com.apple.runningboard.assertions.webkit"). Boot-arg xpcproxy_idversion=1: a new version, as XNU.
	 */
	return xpcproxy_idversion || strncmp(p->p_comm, "xpcproxy", sizeof(p->p_comm)) != 0;
}

/*
 * A posix_spawn that asks for a persona that does not exist: the credential it asks for, without the persona. cred is the
 * reference spawn_persona_adopt() holds, with the uid, gid and groups already set; it is consumed here.
 *
 * The checks of spawn_validate_persona() that are gone: two of the three checks below cannot be passed in this port, and both stand in
 * the way of something ordinary. The entitlement is one nobody carries here (nothing is
 * signed, AMFI is a stub), and the persona is one nobody makes -- yet a package manager
 * becomes root by asking for exactly this: Sileo spawns apt and dpkg into persona 99 with
 * the uid it wants (its APTWrapper). Refused here, the spawn failed before the program was
 * ever started, and the application waited for an answer that could not come. The check
 * that still means something -- the size of the group list -- stays; see
 * spawn_persona_adopt() below for what happens then.
 */
int
hakios_spawn_adopt_credential(proc_t p, kauth_cred_t cred)
{
	/*
	 * hakios: there are no personas in this port -- nothing makes them, and
	 * kpersona_find_by_type() answers "no such process" to everyone who asks. Darwin's own
	 * programs get along without them, but a package manager becomes root by way of one:
	 * Sileo asks posix_spawn for persona 99 with OVERRIDE and uid 0 (its APTWrapper), and
	 * that is how it runs apt and dpkg. With the persona missing the lookup failed, the
	 * whole spawn failed with ESRCH, and the application sat for ever waiting for an
	 * answer from a program that was never started. What the caller is after is the
	 * credential, not the persona, so the credential is what it gets: the uid, gid and
	 * groups it asked for, set on the process being spawned.
	 *
	 * The thread's own credential is not touched here: the thread running this is the
	 * parent's, and the new process gets its credential when its image is activated.
	 *
	 * And the count of processes per real uid follows, as persona_proc_adopt() keeps it:
	 * fork1() counted the new process under the parent's uid (mobile's), its exit takes
	 * it off under the one it has then (root's). Without that, every program Sileo ran as
	 * root took one away from root's count that had never been given -- and once Sileo's
	 * own mv, cp and rm ran at all, a few dozen of them brought root's count to nothing,
	 * and the next process of root's to exit panicked the kernel ("chgproccnt: lost user").
	 */
	__block kauth_cred_t new_cred = cred;
	__block uid_t old_ruid = KAUTH_UID_NONE;
	uid_t new_ruid = kauth_cred_getruid(cred);

	(void)proc_update_label(p, true, ^kauth_cred_t (kauth_cred_t cur_cred) {
		old_ruid = kauth_cred_getruid(cur_cred);
		if (new_cred != cur_cred) {
		        kauth_cred_ref(new_cred);
		        kauth_cred_unref(&cur_cred);
		}
		return new_cred;
	});
	kauth_cred_unref(&cred);
	if (old_ruid != KAUTH_UID_NONE && old_ruid != new_ruid) {
		(void)chgproccnt(new_ruid, 1);
		(void)chgproccnt(old_ruid, -1);
	}
	return 0;
}
