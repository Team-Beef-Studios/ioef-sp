/*
===========================================================================
android_crash.c -- native crash handler for the standalone Android/Quest build.

Catches fatal signals (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE) and writes a
backtrace to BOTH logcat (tag "EFXR-crash") and a persistent file on
/sdcard/EFXR/efxr_crash.log.  The file matters because the headset is often
used UNPLUGGED: you can reproduce a crash with no PC attached, then pull the
log later.

Each frame is logged as  "<so-path> + 0x<offset>"; feed that offset to
  aarch64-linux-android-addr2line -e <unstripped lib>.so 0x<offset>
(the unstripped libs live in android/obj/local/arm64-v8a/) to get file:line.
We append, so repeated crashes accumulate, and we chain to the previously
installed (debuggerd) handler so the system tombstone is still produced.
===========================================================================
*/

#include <signal.h>
#include <unwind.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <android/log.h>

#define EFXR_CRASH_LOG   "/sdcard/EFXR/efxr_crash.log"
#define EFXR_ALTSTACK    (64 * 1024)
#define EFXR_MAX_FRAMES  64

static struct sigaction s_old[NSIG];

/* ---- stack unwinding via libgcc/libunwind _Unwind_Backtrace ---- */
typedef struct { uintptr_t *pcs; int count; int max; } efxr_bt_t;

static _Unwind_Reason_Code efxr_unwindCb(struct _Unwind_Context *ctx, void *arg)
{
	efxr_bt_t *s = (efxr_bt_t *)arg;
	uintptr_t pc = _Unwind_GetIP(ctx);
	if (pc && s->count < s->max) {
		s->pcs[s->count++] = pc;
	}
	return _URC_NO_REASON;
}

static void efxr_emit(int fd, const char *s)
{
	if (fd >= 0) { ssize_t r = write(fd, s, strlen(s)); (void)r; }
	__android_log_write(ANDROID_LOG_FATAL, "EFXR-crash", s);
}

static void efxr_crashHandler(int sig, siginfo_t *info, void *uc)
{
	char line[640];
	uintptr_t pcs[EFXR_MAX_FRAMES];
	efxr_bt_t st = { pcs, 0, EFXR_MAX_FRAMES };
	int fd, i;
	time_t t = time(NULL);

	fd = open(EFXR_CRASH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);

	snprintf(line, sizeof line,
		"\n=== EFXR native crash @ %ld: signal %d (%s), fault addr %p ===\n",
		(long)t, sig, strsignal(sig), info ? info->si_addr : (void *)0);
	efxr_emit(fd, line);

	_Unwind_Backtrace(efxr_unwindCb, &st);

	for (i = 0; i < st.count; i++) {
		Dl_info dl;
		const char *fname = "?";
		uintptr_t base = 0;
		const char *sym = NULL;
		uintptr_t soff = 0, off;

		if (dladdr((void *)pcs[i], &dl)) {
			if (dl.dli_fname) fname = dl.dli_fname;
			base = (uintptr_t)dl.dli_fbase;
			sym  = dl.dli_sname;
			if (sym && dl.dli_saddr) soff = pcs[i] - (uintptr_t)dl.dli_saddr;
		}
		off = pcs[i] - base;   /* offset within the .so -> addr2line input */

		if (sym)
			snprintf(line, sizeof line, "#%02d  %s + 0x%lx  (%s+0x%lx)\n",
			         i, fname, (unsigned long)off, sym, (unsigned long)soff);
		else
			snprintf(line, sizeof line, "#%02d  %s + 0x%lx\n",
			         i, fname, (unsigned long)off);
		efxr_emit(fd, line);
	}

	efxr_emit(fd, "=== end backtrace ===\n");
	if (fd >= 0) close(fd);

	/* Chain to the previous handler (debuggerd) so the system tombstone is
	   still written, then fall back to the default disposition. */
	if (sig > 0 && sig < NSIG) {
		if ((s_old[sig].sa_flags & SA_SIGINFO) && s_old[sig].sa_sigaction) {
			s_old[sig].sa_sigaction(sig, info, uc);
			return;
		}
		if (s_old[sig].sa_handler && s_old[sig].sa_handler != SIG_DFL &&
		    s_old[sig].sa_handler != SIG_IGN) {
			s_old[sig].sa_handler(sig);
			return;
		}
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

void EFXR_InstallCrashHandler(void)
{
	static char altstk[EFXR_ALTSTACK];
	stack_t ss;
	struct sigaction sa;
	int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
	unsigned i;

	/* Alternate signal stack so we can still unwind on a stack overflow. */
	memset(&ss, 0, sizeof ss);
	ss.ss_sp = altstk;
	ss.ss_size = sizeof altstk;
	ss.ss_flags = 0;
	sigaltstack(&ss, NULL);

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = efxr_crashHandler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
		sigaction(sigs[i], &sa, &s_old[sigs[i]]);
	}

	__android_log_write(ANDROID_LOG_INFO, "EFXR-crash",
		"crash handler installed (backtrace -> " EFXR_CRASH_LOG ")");
}
