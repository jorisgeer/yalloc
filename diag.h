/* diag.h - diagnostics

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   Diagnostics printer / formatter for assertions, logging and tracing
   Statistics are separate
*/

#define Logfile Fdiag

#define Fln (__LINE__|(Logfile<<16))

#define Diag_buf 1024

#define Diagcnts 600 // 256

static const ub4 logitems;

enum Diactl { Dianone,Diadis,Diaena,Diaerr };

static enum Diactl diagctls[Diagcnts]; // for yal_diag.cfg suppressions

static ub4 ylog_mask;

static Cold ub4 showdate(char *buf,ub4 pos,ub4 len)
{
  ub4 day = 12,mon = 9,yr = 2024;
  ub4 h = 0,m = 0;

#ifdef  Date
  day = Date % 100;
  mon = (Date / 100) % 100;
  yr = Date / 10000;
#endif
  pos += snprintf_mini(buf,pos,len," %u-%02u-%02u",yr,mon,day);

#ifdef Time
  h = (Time % 10000) / 100;
  m = Time % 100;
#endif
  pos += snprintf_mini(buf,pos,len," %02u:%02u",h,m);
  return pos;
}

static ub4 underline(char *dst,ub4 dlen,cchar *src,ub4 slen)
{
  ub4 dn=0;
  ub4 sn=0;

  while (sn < slen && dn + 4 < dlen && src[sn]) {
#if Yal_log_utf8
    dst[dn]   = (char)0xcc;  // combining macron below u+331
    dst[dn+1] = (char)0xb1;
#endif
    dst[dn+2] = src[sn++];
    dn += 3;
  }
  dst[dn] = 0;
  return dn;
}

// main diags printer
//  file line did tid level caller msg
static Cold Printf(6,7) ub4 do_ylog(ub4 did,enum Loc loc,ub4 fln,enum Loglvl lvl,bool prepend,cchar *fmt,...)
{
  va_list ap;
  int fd;
  ub4 tid = 0;
  unsigned long pid = Atomget(global_pid,Monone);
  enum Diactl ctl;
  ub4 n,pos = 0,upos;
  ub4 check;
  ub4 len = Diag_buf - 2;
  char buf[Diag_buf];
  char headbuf[256];
  cchar *name;
  static _Atomic ub4 exiting;
  ub4 zero = 0;
  ub4 errcnt,msgcnt;
  char *xbuf = nil;

  check = global_check;
  if ( (check & 1) == 0) return pos; // ignore

  heapdesc *hd = thread_heap;

  if (hd) {
    xbuf = hd->errbuf;
    tid = hd->id;
  }

  ctl = (did < Diagcnts) ? diagctls[did] : 0;

  if (lvl != Nolvl) { // disabled ?
    if (lvl > Yal_log_level) { // disabled by default
      if (ctl < Diaena) return 0;
    } else {
      if (ctl == Diadis) return 0;
    }
    if (ctl == Diaerr) lvl = Error;
    if ( (1u << lvl) & ylog_mask) return 0;
    msgcnt = Atomad(g_msgcnt,1,Moacqrel);
  } else msgcnt = Atomget(g_msgcnt,Moacq);

  if (lvl > Error) {
    fd = Yal_log_fd;
    if (fd == -1) fd = Yal_log_fd = newlogfile(Yal_log_file,"",tid,pid);
    if (msgcnt == 0) {
      upos = snprintf_mini(headbuf,0,255,"\n%18s %-4s %-5s %-4s %-3s %-1s %-8s msg\n","file/line","seq","pid","tid","dia","","api");
      pos = underline(buf,len,headbuf,upos);
    }
  } else {
    fd = Yal_err_fd;
    if (fd == -1) fd = Yal_err_fd = newlogfile(Yal_err_file,"",tid,pid);
    errcnt = Atomad(g_errcnt,1,Moacqrel);
    if (errcnt == 0) {
      pos = snprintf_mini(buf,0,len,"\n-- %lu -- yalloc detected error\n",pid);
      pos += snprintf_mini(buf,pos,len,"  yalloc %s",yal_version);
      pos = showdate(buf,pos,len);
      buf[pos++] = '\n';
    }
  }
  if (xbuf && *xbuf) {
    if (prepend) pos += snprintf_mini(buf,pos,len,"%.255s",xbuf);
    *xbuf = 0;
  }

  if (*fmt == '\n') buf[pos++] = *fmt++;

  if (fln) pos = diagfln(buf,pos,len,fln);

  if (lvl != Nolvl) pos += snprintf_mini(buf,pos,len,"%-4u %-5lu %-4u %-3u ",msgcnt,pid,tid,did);
  else pos += snprintf_mini(buf,pos,len,"%20c",' ');

  name = lvlnames[min(lvl,Nolvl)];
  buf[pos++] = *name;
  buf[pos++] = ' ';

  name = locnames[loc & Lmask];
  buf[pos++] = (loc & Lremote) ? *name & (char)0xdf : *name; // uppercase for remote
  pos += snprintf_mini(buf,pos,len,"%-7.8s ",name + 1);

  va_start(ap,fmt);
  pos += mini_vsnprintf(buf,pos,len,fmt,ap);
  va_end(ap);

  if (pos < Diag_buf - 1) {
    buf[pos++] = '\n';
    buf[pos] = 0;
  }

  if (hd && (lvl <= Error || lvl == Nolvl)) {
    if (xbuf == nil) xbuf = hd->errbuf = bootalloc(Fln,hd->id,loc,256);
    n = min(pos,255);
    if (xbuf) { memcpy(xbuf,buf,n); xbuf[n] = 0; } // keep latest error or provide context
    if (lvl <= Error) hd->stat.errors++;
  }

  if ( (check & 2) == 0) return pos; // ignore

  if (lvl == Nolvl) return pos;

  if (lvl > Error && Atomget(exiting,Moacq)) return pos;

  n = oswrite(fd,buf,pos,Fln);

  if (n == 0) {
    if (lvl > Error) Yal_err_fd = 2;
    else Yal_log_fd = 2;
    oswrite(2,buf,pos,Fln);
  }

  if (lvl > Error) {
    return pos;
  }

  if (Yal_err_fd != Yal_Err_fd && Yal_Err_fd >= 0) oswrite(Yal_Err_fd,buf,pos,Fln);

  if ( (check & 4) == 0) return 0;

  if (Cas(exiting,zero,1)) { // let only one thread call exit
    callstack(hd);
    if (global_stats_opt) yal_mstats(nil,global_stats_opt | Yal_stats_print,Fln,"diag-exit");
    minidiag(Fln,loc,Error,tid,"\n--- %.255s exiting ---\n",global_cmdline);
    _Exit(1);
  }
  return 0;
}

#define Diagcode (Yal_diag_count + __COUNTER__ + 32) // reserve 32 for tracing

#define error(loc,fmt,...) do_ylog(Diagcode,loc,Fln,Error,0,fmt,__VA_ARGS__);
#define error2(loc,fln,fmt,...) do_ylog(Diagcode,loc,fln,Error,1,fmt,__VA_ARGS__);

#define errorctx(fln,loc,fmt,...) do_ylog(Diagcode,loc,fln,Nolvl,0,fmt,__VA_ARGS__);

#define ylogx(loc,fmt,...) do_ylog(0,loc,Fln,Info,0,fmt,__VA_ARGS__);

#if Yal_enable_trace
  #define ytrace(lvl,hd,loc,tag,seq,fmt,...) if (unlikely(hd->trace > lvl)) { errorctx((tag),Lnone,"seq %u",(ub4)(seq)) do_ylog(Yal_diag_count + __COUNTER__,loc,Fln,Trace,1,fmt,__VA_ARGS__); }
#else
  #define ytrace(lvl,hd,loc,tag,seq,fmt,...)
#endif

#if Yal_dbg_level > 0
  #define ydbg1(fln,loc,fmt,...) do_ylog(Diagcode,loc,fln,Debug,0,fmt,__VA_ARGS__);
#else
  #define ydbg1(fln,loc,fmt,...)
#endif
#if Yal_dbg_level > 1
  #define ydbg2(fln,loc,fmt,...) do_ylog(Diagcode,loc,fln,Debug,0,fmt,__VA_ARGS__);
#else
  #define ydbg2(fln,loc,fmt,...)
#endif
#if Yal_dbg_level > 2
  #define ydbg3(loc,fmt,...) do_ylog(Diagcode,loc,Fln,Debug,0,fmt,__VA_ARGS__);
#else
  #define ydbg3(loc,fmt,...)
#endif

#if Yal_enable_stats == 2
  #define ystats(var) (var)++;
  #define ystats2(var,inc) (var) += (inc);
  #define ylostats(a,b) if ( (b) < (a) ) (a) = (b);
  #define yhistats(a,b) if ( (b) > (a) ) (a) = (b);
#else
  #define ystats(var)
  #define ystats2(var,inc)
  #define ylostats(a,b)
  #define yhistats(a,b)
#endif

#if Yal_enable_check

 #if Isgcc || Isclang
  #define ycheck(rv,loc,expr,fmt,...) if (__builtin_expect ( (expr),0) ) { do_ylog(Diagcode,loc,Fln,Assert,0,fmt,__VA_ARGS__); return(rv); }
  #define ywarn(loc,expr,fmt,...) if (__builtin_expect ( (expr),0) ) do_ylog(Diagcode,loc,Fln,Warn,0,fmt,__VA_ARGS__);
 #else
  #define ycheck(rv,loc,expr,fmt,...) if ( (expr) ) { do_ylog(Diagcode,loc,Fln,Assert,0,fmt,__VA_ARGS__); return(rv); }
  #define ywarn(loc,expr,fmt,...) if ( (expr) ) do_ylog(Diagcode,loc,Fln,Warn,0,fmt,__VA_ARGS__);
 #endif

#else
  #define ycheck(rv,loc,expr,fmt,...)
  #define ywarn(loc,expr,fmt,...)
#endif

#if Yal_enable_check > 1
 #define ycheck1(rv,loc,expr,fmt,...) if ( (expr) ) { do_ylog(Diagcode,loc,Fln,Assert,0,fmt,__VA_ARGS__); return(rv); }
#else
 #define ycheck1(rv,loc,expr,fmt,...)
#endif

#if Yal_enable_stack
 #define ypush(hd,fln) do_ypush(hd,(fln));
static inline void do_ypush(heapdesc*hd,ub4 fln)
{
  ub4 pos;

  if (hd == nil) { minidiag(fln,0,Info,0,"no push %x",fln); return; }

  pos = hd->flnpos;
  hd->flnstack[pos] = fln;
  hd->flnpos = pos < 15 ? pos + 1 : 0;
}

#else
 #define ypush(hb,fln)
#endif

/* diag file has an entry per line to override default for a single diag code or range
   -123   disable
   +123  enable
   =123 keep as-is
   +123-456 enable range inclusive
   - 200 but enable 200
 */
static Cold void diag_initrace(void)
{
  ub4 len = 4096;
  char buf[4096];
  ub4 x,y;
  ub1 v;
  char c,m;
  int fd = osopen(Yal_trace_ctl,nil);
  long nw;
  ub4 nn,n;

  minidiag(Fln,Lnone,Debug,0,"diag fd %d",fd);

  if (fd == -1) return;

  memset(buf,0,len);
  nw = osread(fd,buf,len - 2);
  osclose(fd);
  if (nw <= 0 || nw >= len) return;

  nn = (ub4)nw;
  n = 0;
  while (n < nn) {
    m = buf[n++];
    x = y = 0;
    c = 0;
    while (n < len &&  (c = buf[n++]) >= '0' && c <= '9' && x < Diagcnts) x = x * 10 + (ub4)c - '0';
    if (c == '-') {
      while (n < len && (c = buf[n++]) >= '0' && c <= '9' && y < Diagcnts) y = y * 10 + (ub4)c - '0'; // range
    }
    if (x >= Diagcnts - 1) {
      minidiag(Fln,Lnone,Warn,0,"invalid diag code %u",x);
      x = Diagcnts - 2;
    }
    if (y >= Diagcnts - 1) {
      minidiag(Fln,Lnone,Warn,0,"invalid diag cocd %u",y);
      y = 0;
    }
    switch (m) {
      case '-':  v = Diadis; break;
      case '+':  v = Diaena; break;
      case '!': v = Diaerr; break;
      case '=': v = 0; break;
      default: v = 0;
    }
    if (y == 0) y = x;
    if (Yal_dbg_level > 1) minidiag(Fln,Lnone,Debug,0,"diag %u-%u = %u",x,y,v);
    do diagctls[x++] = v; while(x <= y);
    while (n < len && c && c != '\n') c = buf[n++];
  }
}

static ub4 trace_enable(ub4 ena)
{
  ub4 rv = global_trace;
  heapdesc *hd = getheapdesc(Lnone);

  minidiag(Fln,Lnone,Vrb,0,"trace %u -> %u",rv,ena);
  hd->trace = ena;
  global_trace = ena | 8;
  return rv;
}

static ub4 trace_name(ub4 id,cchar *name)
{
  if (id >= Trcnames) return __LINE__;
  trcnames[id] = name;
  return 0;
}

static ub4 diag_enable(size_t dia,ub4 ena)
{
  ub4 rv;

  if (dia >= Diagcnts) return __LINE__;
  rv = diagctls[dia];
  diagctls[dia] = ena ? Diaena : Diadis;
  return rv;
}

#undef Logfile
