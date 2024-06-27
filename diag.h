/* diag.h - diagnostics

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

enum File { Falloc,Fbist,Fbuddy,Fdir,Ffree,Fheap,Flock,Fos,Frealloc,Fregion,Fslab,Fstd,Fyalloc,Ftest,Fcount };
static cchar * const filenames[Fcount] = { "alloc.h","bist","buddy.h","dir","free","heap.h","lock","os.h","realloc","region.h","slab.h","std.h","yalloc.c","test.c" };

enum Loc { Lnone,Lalloc,Lallocal,Lcalloc,Lreal,Lfree,Lsize,Ltest,Lcount };
static cchar * const locnames[Lcount + 1] = { "any","malloc","malloc_align","calloc","realloc","free", "size","test","?" };

enum Loglvl { Fatal,Assert,Error,Warn,Info,Trace,Verb,Debug,Nolvl };
static cchar * const lvlnames[Nolvl + 1] = { "Fatal","Assert","Error","Warn","Info","Trace","Verb","Debug"," " };

#define Fln __LINE__|(Logfile<<16)

static ub1 diagcnts[Diag_counts];

static ub4 ylog_mask;

static Cold Printf(6,7) void do_ylog(ub4 counter,enum Loc loc,ub4 fln,enum Loglvl lvl,char *xbuf,cchar *fmt,...)
{
  va_list ap;
  int fd;
  ub2 fn,ln;
  ub1 cnt;
  ub4 pos = 0;
  ub4 len = Diag_buf - 2;
  char buf[Diag_buf];
  char fbuf[64];
  cchar *fnam;
  cchar *lvlnam;

  cnt = diagcnts[min(counter,Diag_counts - 1)];

#if 1
  if (lvl > Yal_log_level && lvl < Nolvl) { // disabled
    if (cnt != 2) return;
  } else {
    if (cnt == 1) return;
  }
  if ( (1u << lvl) & ylog_mask) return;
#endif

  if (lvl > Error) {
    fd = Yal_log_fd;
  } else {
    fd = Yal_error_fd;
    if (xbuf && *xbuf) pos = snprintf_mini(buf,pos,len,"%.256s",xbuf);
  }

  if (fln) {
    fn = fln >> 16; ln = fln & hi16;
    fnam = filenames[min(fn,Fcount)];
    snprintf_mini(fbuf,0,64,"yal/%s:%u",fnam,ln);
    pos += snprintf_mini(buf,pos,len,"%-16s ",fbuf);
  }

  pos += snprintf_mini(buf,pos,len,"%-3u ",counter);

  lvlnam = lvlnames[min(lvl,Nolvl)];
  buf[pos++] = *lvlnam;
  buf[pos++] = ' ';

  loc = min(loc,Lcount);
  pos += snprintf_mini(buf,pos,len,"%-8s ",locnames[loc]);

  va_start(ap,fmt);
  pos += mini_vsnprintf(buf,pos,len,fmt,ap);
  va_end(ap);

  buf[pos++] = '\n';
  buf[pos] = 0;
  len = min(Diag_buf - 2,pos + 1);
  if (xbuf) { memcpy(xbuf,buf,len); xbuf[len] = 0; }
#if Yal_enable_error
  if (lvl != Nolvl) oswrite(fd,buf,pos);
#endif
}

#define error(buf,loc,fmt,...) { *buf = 0; do_ylog(__COUNTER__,loc,Fln,Error,buf,fmt,__VA_ARGS__); }
#define error2(buf,loc,fln,fmt,...) do_ylog(__COUNTER__,loc,fln,Error,buf,fmt,__VA_ARGS__);
#define errorctx(loc,buf,fmt,...) do_ylog(__COUNTER__,loc,Fln,Nolvl,buf,fmt,__VA_ARGS__);

#if Yal_enable_log
  #define ylog(loc,fmt,...) do_ylog(__COUNTER__,loc,Fln,Info,nil,fmt,__VA_ARGS__);
  #define ylog2(loc,fln,fmt,...) do_ylog(__COUNTER__,loc,fln,Info,nil,fmt,__VA_ARGS__);
  #define ylog0(loc,str) do_ylog(__COUNTER__,loc,Fln,Info,nil,"%s",str);

#else
  #define ylog(loc,fmt,...)
  #define ylog2(loc,fln,fmt,...)
  #define ylog0(loc,str)
  #define ytrace(loc,fmt,...)
#endif

#if Yal_enable_trace
  #define ytrace(loc,fmt,...) do_ylog(__COUNTER__,loc,Fln,Trace,nil,fmt,__VA_ARGS__);
#else
  #define ytrace(loc,fmt,...)
#endif

#if Yal_enable_dbg
  #define ydbg(loc,fmt,...) do_ylog(__COUNTER__,loc,Fln,Debug,nil,fmt,__VA_ARGS__);
#else
  #define ydbg(loc,fmt,...)
#endif

#if Yal_enable_stats
  #define ystats(var) (var)++;
  #define ystats2(var,inc) (var) += (inc);
#else
  #define ystats(var)
  #define ystats2(var,inc)
#endif
#define ylostats(a,b) if ( (a) < (b) ) { (b) = (a); }
#define yhistats(a,b) if ( (a) > (b) ) { (b) = (a); }

#if Yal_enable_check == 2
  #define ycheck(rv,loc,expr,fmt,...) if (unlikely (expr) ) { do_ylog(__COUNTER__,loc,Fln,Assert,nil,fmt,__VA_ARGS__); return(rv); }
  #define ycheck0(expr,loc,fmt,...) if (unlikely (expr) ) do_ylog(__COUNTER__,loc,Fln,Assert,nil,fmt,__VA_ARGS__);
#else
  #define ycheck(rv,loc,expr,fmt,...)
  #define ycheck0(loc,expr,fmt,...)
#endif

#if __STDC_VERSION__ < 201112L
static void assert_fail(ub4 fln,const char *msg)
{
  do_ylog(__COUNTER__,Lnone,fln,Assert,nil,"assertion failed - %s",msg);
  // exit(1);
}
#endif

/* diag file has entry for each line to override default for a single diag code or range
   -123   disable
   +123  enable
   =123 keep as-is
   +123-456 enable range inclusive

  enabled with env var '2'

 */
static Cold void diag_init(void)
{
  ub4 len = 1024;
  char buf[1024];
  ub4 x,y;
  ub1 v;
  char c,m;
  int fd = osopen(Yal_diag_init);
  ssize_t nn,n;

  if (fd == -1) return;

  memset(buf,0,len);
  nn = osread(fd,buf,len - 4);
  osclose(fd);
  if (nn <= 0) return;

  n = 0;
  while (n < nn) {
    m = buf[n++];
    x = y = 0; c = '\n';
    while ( (c = buf[n++]) >= '0' && c <= '9') x = x * 10 + (ub4)c - '0';
    if (c == '-') {
      while ( (c = buf[n++]) >= '0' && c <= '9') y = y * 10 + (ub4)c - '0'; // range
    }
    x = min(x,Diag_counts - 1); y = min(y,Diag_counts - 1);
    switch (m) {
      case '-':  v = 1; break;
      case '+':  v = 2; break;
      case '=': v = 0; break;
      default: v = 0;
    }
    if (y == 0) y = x;
    do diagcnts[x++] = v; while(x <= y);
    while (c && c != '\n') c = buf[n++];
  }
}
