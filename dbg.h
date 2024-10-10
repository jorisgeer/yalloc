/* dbg.h - debug provisions

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
  */

#ifdef Backtrace // glibc-linux or macos
 #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #include <execinfo.h>
  #undef _POSIX_C_SOURCE
 #endif

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
  ub4 i;
  ub4 pos = hd->flnpos;
  ub4 fln,*stack = hd->flnstack;

  for (i = 0; i <= pos; i++) {
    fln = stack[i];
    if (fln || i == 0) minidiag(fln,Lnone,Info,hd ? hd->id : 0,"pos %u/%u",i,pos);
  }
#endif
}

#if Yal_signal

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
    minidiag(fln|__LINE__,Lsig,Fatal,id,"yalloc: signal %u\n",sig);
  }
  ip = (size_t)adr;
  minidiag(fln|__LINE__,Lsig,Fatal,id,"yalloc[%lu]: sig%s at adr %zx",ospid(),name,ip);
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
