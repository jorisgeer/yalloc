/* dbg.h - debug provisions

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
  */

#ifdef Backtrace // glibc-linux or macos 10.5+, see build.sh

 #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
 #endif

  #include <execinfo.h>

 static void showtrace(void)
 {
   static void *buf[256];

   int len = backtrace(buf,100);
   backtrace_symbols_fd(buf,len,2);
 }
#elif Yal_signal
 static void showtrace(void) {}
#endif

static void callstack(heapdesc *hd)
{
#if Yal_enable_stack
  ub4 pos,cur;
  ub4 fln,*stack;

  if (hd == nil) { minidiag(0,Lnone,Info,0,"no callstack"); return; }

  cur = hd->flnpos;
  stack = hd->flnstack;

  for (pos = 0; pos < 16; pos++) {
    fln = stack[pos];
    if (fln) minidiag(fln,Lnone,Info,hd->id,"%u%s",pos,pos == cur ? " <--" : "");
  }
#else
  minidiag(0,Lnone,Debug,hd ? hd->id : 0,"no callstack");
#endif
}

#if Yal_signal && ! defined __FreeBSD__

static struct sigaction g_orgsa;

static void *region_near(size_t ip,char *buf,ub4 len);

static Noret void mysigact(int sig,siginfo_t *si,void *pp)
{
  void *adr = nil;
  size_t ip;
  cchar *name;
  heapdesc *hd = thread_heap;
  ub4 fln = Fdbg << 16;
  ub4 id = hd ? hd->id : 0;
  unsigned long pid = Atomget(global_pid,Monone);
  char buf[256];

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
  minidiag(fln|__LINE__,Lsig,Fatal,id,"yalloc[%lu]: sig%s at adr %zx",pid,name,ip);
  if (adr) {
    memset(buf,0,256);
    region_near(ip,buf,255);
    minidiag(fln|__LINE__,Lsig,Fatal,id,"%.250s\n",buf);
  }

  if (global_stats_opt) yal_mstats(nil,global_stats_opt | Yal_stats_print,fln|__LINE__,"signal");

  if (hd) callstack(hd);

  showtrace();

  osread(0,buf,64); // pauze, so interceptible by debugger

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

#ifdef Hasasm

static unsigned long asm_get_caller(void)
{
  unsigned long x;

#ifdef __aarch64__
	__asm__("mov %0, x30" : "=r"(x) );
#endif
	return x;
}
#define Caller() asm_get_caller()

#else
 #define Caller()
#endif
