/* dbg.h - debug provisions

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
  */

#ifdef Backtrace // glibc-linux or macos 10.5+, see build.sh. FreeBSD calls malloc()

 #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
 #endif

  #include <execinfo.h>

 static void showtrace(void)
 {
   void *buf[64];
   int fd = Yal_err_fd;

   if (fd < 0) fd = Yal_Err_fd;

   ub4 len = (ub4)backtrace(buf,64);
   if (len < 64) backtrace_symbols_fd(buf,(int)len,fd);
 }
#elif Yal_signal
 static void showtrace(void) {}
#endif

static void callstack(heapdesc *hd)
{
#if Yal_enable_stack
  ub4 pos,cur;
  ub4 fln,*stack;
  enum Loc loc;

  if (hd == nil) { minidiag(0,Lnone,Info,0,"no callstack"); return; }

  cur = hd->flnpos;
  stack = hd->flnstack;

  for (pos = 0; pos < Yal_stack_len; pos++) {
    fln = stack[pos];
    loc = hd->locstack[pos];
    if (fln) minidiag(fln,loc,Info,hd->id,"%s%u%s",pos == 0 ? "\n" : "",pos,pos == cur ? " <--" : "");
  }
#else
  minidiag(0,Lnone,Debug,hd ? hd->id : 0,"no callstack");
#endif
}

#ifdef Hasasm

static unsigned long asm_get_caller(void)
{
  unsigned long x = 0;

#ifdef __aarch64__
	__asm__("mov %0, x30" : "=r"(x) );
#endif
	return x;
}
#define Caller() asm_get_caller()

#else
 #define Caller() 0
#endif

#if Yal_signal

static struct sigaction g_orgsa;

static void *region_near(size_t ip,char *buf,ub4 len);

static Noret void mysigact(int sig,siginfo_t *si,void *pp)
{
  void *adr = nil;
  size_t ip;
  cchar *name;
  heapdesc *hd;
  ub4 fln = Fdbg << 16;
  ub4 id = 0;
  unsigned long pid = Atomget(global_pid,Monone);
  ub4 statopt = global_stats_opt;
  static char buf[256];

  hd = tid_gethd();
  if (hd) id = hd->id;

  switch(sig) {

  case SIGSEGV:
    name = "segv";
    adr = si->si_addr;
    break;

  case SIGBUS:
    name = "bus";
    adr = si->si_addr;
    break;

  default:
    name = "def";
    minidiag(fln|__LINE__,Lsig,Fatal,id,"yalloc: signal %d\n",sig);
  }
  ip = (size_t)adr;
  minidiag(fln|__LINE__,Lsig,Fatal,id,"yalloc[%lu]: sig%s at adr %zx caller %lx\n%s",pid,name,ip,Caller(),global_cmdline);
  if (adr) {
    region_near(ip,buf,255);
    minidiag(fln|__LINE__,Lsig,Fatal,id,"%.250s\n",buf);
  }

  if (global_check & 8) statopt |= Yal_stats_totals | Yal_stats_sum;
  if (statopt) yal_mstats(nil,statopt | Yal_stats_print,fln|__LINE__,"signal");

  callstack(hd);

  showtrace();

#ifdef Yal_dev
  osread(0,buf,64); // pauze, so interceptible by debugger
#endif

  if (g_orgsa.sa_sigaction) (*g_orgsa.sa_sigaction)(sig,si,pp);
  else if (g_orgsa.sa_handler) (*g_orgsa.sa_handler)(sig);
  _Exit(1);
}

static void setsigs(void)
{
  struct sigaction sa;

#ifdef Backtrace // glibc-linux or macos
  void *btbuf[4];

  backtrace(btbuf,4);  // make sure libgcc is loaded at handler time

#endif

  memset(&sa,0,sizeof(sa));

  sa.sa_sigaction = mysigact;

  sa.sa_flags = SA_SIGINFO;

  sigaction(SIGSEGV, &sa,&g_orgsa);
  sigaction(SIGBUS, &sa,&g_orgsa);
}
#else
 static void setsigs(void) {}
#endif // Yal_signal
