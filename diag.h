/* diag.h - diagnostics

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

enum File { Falloc,Fbuddy,Ffree,Fheap,Fos,Frealloc,Fregion,Fslab,Fstd,Fyalloc,Ftest,Fcount };

static cchar *fnames[Fcount] = { "alloc.h","buddy.h","free","heap.h","os.h","realloc","region.h","slab.h","std.h","yalloc.c","test.c" };

static void _Printf(3,4) error(ub4 line,enum File file,cchar *fmt,...)
{
  va_list ap;
  ub4 n = 0;
  char buf[256];
  cchar *fname;

  if (file >= Fcount) fname = "?";
  else fname = fnames[file];

  if (line) n = mini_snprintf(buf,0,250,"Error %s:%u - ",fname,line);

  va_start(ap,fmt);
  n += mini_vsnprintf(buf,n,250,fmt,ap);
  va_end(ap);

  buf[n] = '\n';
  oswrite(2,buf,n+1);
}

#if Yal_enable_log
  #define ylog(f,fmt,...) do_ylog(__LINE__,f,fmt,__VA_ARGS__);
static void _Printf(3,4) do_ylog(ub4 line,enum File file,cchar *fmt,...)
{
  va_list ap;
  ub4 n;
  char buf[512];
  cchar *fname;

  if (file >= Fcount) fname = "?";
  else fname = fnames[file];

  n = mini_snprintf(buf,0,510,"%s:%u - ",fname,line);

  va_start(ap,fmt);
  n += mini_vsnprintf(buf,n,510,fmt,ap);
  va_end(ap);

  buf[n] = '\n';
  oswrite(diag_fd,buf,n+1);
}
#else
  #define ylog(f,fmt,...)
  #define do_ylog(fl,,fmt,...)
#endif

static void *oom(ub4 line,ub4 file,size_t n1,size_t n2)
{
  error(line,file,"out of memory allocating %zu` * %zu`b",n1,n2);
  return nil;
}

static void free2(ub4 line,ub4 file,cchar *p,size_t len,cchar *msg)
{
}

static void assert_fail(ub4 line,cchar *file,const char *msg)
{
  error(0,Fcount,"%s.%u: assertion failed - %s",file,line,msg);
  // exit(1);
}
