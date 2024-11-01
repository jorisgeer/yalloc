/* os.c - operating system bindings

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   Complete for unix-like systems, work in progress for windows.
*/

#ifdef __unix__
 #ifdef __linux__
 #define _GNU_SOURCE // for mmap
 #else
  #define _POSIX_C_SOURCE 200809L
 #endif
#endif

#include <stddef.h> // size_t
#include <fcntl.h> // open modes
#include <unistd.h> // open,read,write,close
#include <errno.h>
#include <sys/stat.h> // stat

#ifdef Inc_os
  #define Vis static
  struct osstat {
    unsigned long len;
    unsigned long mtime;
  };
#else
 #include "os.h"
 #define Vis
#endif

static char *ucnv(char *end,unsigned int x)
{
    do *--end = (char)((x % 10) + '0'); while (x /= 10);
    return end;
}

Vis int osopen(const char *name,struct osstat *sp)
{
  struct stat st;
  int fd = open(name,O_RDONLY);

  if (fd == -1 || sp == NULL) return fd;
  if (fstat(fd,&st)) return fd;
  sp->len = (unsigned long)st.st_size;
  sp->mtime = (unsigned long)st.st_mtime;
  return fd;
}

Vis int oscreate(const char *name)
{
  int fd = open(name,O_CREAT | O_TRUNC | O_WRONLY,0644);
  return fd;
}

Vis void osclose(int fd)
{
  close(fd);
}

Vis long osread(int fd,char *buf,size_t len)
{
  ssize_t nn = read(fd,buf,len);
  return nn;
}

static const unsigned int chkzeros = 1;

static void writeint(int fd,unsigned int x,int sign)
{
  unsigned int len = 30;
  char buf[32];
  char *p;

  buf[--len] = '\n';
  p = ucnv(buf + len,x);
  if (sign) *--p = '-';
  *--p = ' ';
  write(fd,p,(size_t)( buf + 30 - p));
}

Vis unsigned int oswrite(int fd,const char *buf,size_t len,unsigned int fln)
{
  size_t nn;
  ssize_t nw;
  const char *p;
  int ec;

  if (buf == NULL) { buf = "\noswrite: nil buf\n"; len = 17; }
  else if (len == 0) { buf = "\noswrite: nil lenf\n"; len = 17; }

  if (chkzeros) {
    p = buf;
    while (p < buf + len && *p) p++;
    if (p < buf + len) {
      write(2,"\nnil char ",10);
      writeint(2,fln >> 16,0);
      writeint(2,fln & 0xffff,0);
      writeint(2,(unsigned int)(p - buf),0);
      write(2,buf,len > 64 ? 64 : len);
    }
  }

  do {
    nn = len < 65536 ? len : 65536;
    nw = write(fd,buf,nn);
    if (nw < 0) {
      ec = errno;
      write(2,"\ncannot write to fd ",20);
      writeint(2,(unsigned int)fd,0);
      writeint(2,(unsigned int)ec,0);
      return 0;
    }
    nn = (size_t)nw;
    if (len < nn) nn = len;
    len -= nn;
    buf += nn;
    if (len) write(fd,"\n (partial write)\n",18);
    else return (unsigned int)nn;
  } while (1);
}

static const int reserve = 1;

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))

Vis unsigned int ospagesize(void)
{
  long ret = sysconf(_SC_PAGESIZE);

  if (ret == -1) return 0;
  else return (unsigned int)ret;
}

 #include <sys/mman.h>

Vis void *osmmap(size_t len)
{
  void *p;

#ifdef  MAP_ANON
  int prot = PROT_READ | PROT_WRITE;
  int flags = MAP_PRIVATE | MAP_ANON;
  int fd = -1;
  int ofs = 0;

 #ifdef MAP_NORESERVE
  if (reserve == 0) flags |= MAP_NORESERVE;
 #endif

  p = mmap(NULL,len,prot,flags,fd,ofs);
  if (p == MAP_FAILED) {
    return NULL;
  }
#else // fallback
  len = (len + 4095 + 15) & ~4095;
  p = sbrk(len);
  p = (p + 15) & ~15;
#endif
  return p;
}

#ifndef __linux__
 #include <string.h> // memcpy
#endif

Vis void *osmremap(void *p,size_t orglen,size_t ulen,size_t newlen)
{
  void *np;
#ifdef __linux__
  if (ulen == 0) return NULL; // won't occur
  np = mremap(p,orglen,newlen,MREMAP_MAYMOVE);
  if (np == MAP_FAILED) {
    return NULL;
  }
#else // :-(
  if (newlen) {
    np = osmmap(newlen);
    if (np) memcpy(np,p,ulen < newlen ? ulen : newlen);
  } else np = NULL;
  munmap(p,orglen);
#endif
  return np;
}

Vis int osmunmap(void *p,size_t len)
{
  return munmap(p,len);
}

Vis unsigned long ospid(void)
{
  pid_t pid = getpid();
  return (unsigned long)pid;
}

#elif defined _WIN32 || defined _WIN64

 #include <memoryapi.h>

Vis unsigned int ospagesize(void)
{
  SYSTEM_INFO si;

  GetSystemInfo(&si);
  return si.dwPageSize;
}

Vis void *osmmap(size_t len)
{
  void *p;

  p = VirtualAlloc(nil,len,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);

  return p;
}

Vis int osmunmap(void *p,size_t len)
{
  return VirtualFree(p,len);
}

#else

Vis unsigned int ospagesize(void) { return 4096; } // a too-low pagesize never harms

Vis void *osmmap(size_t len) { return nil; }
Vis void *osmremap(void *p,size_t orglen,size_t newlen)
{
  return p;
}

Vis int osmunmap(void *p,size_t len) { return 0; }

  #error "no mmap"

#endif // unix or windows

#if defined (__linux__) || (defined (__APPLE__) && defined (__MACH__))

 #include <sys/time.h>
 #include <sys/resource.h>

Vis int osrusage(struct osrusage *usg)
{
  struct rusage u;
  int rv = getrusage(RUSAGE_SELF,&u);

  if (rv) return rv;

  usg->utime = (size_t)u.ru_utime.tv_sec * 1000u + (size_t)u.ru_utime.tv_usec / 1000u;
  usg->stime = (size_t)u.ru_stime.tv_sec * 1000u + (size_t)u.ru_stime.tv_usec / 1000u;
  usg->maxrss = (size_t)u.ru_maxrss;
  usg->minflt = (size_t)u.ru_minflt;
  usg->maxflt = (size_t)u.ru_majflt;
  usg->volctx = (size_t)u.ru_nvcsw;
  usg->ivolctx = (size_t)u.ru_nivcsw;

  return 0;
}
#else
Vis int osrusage(struct osrusage *usg)
{
  memset(usg,0,sizeof(struct osrusage));
  return 0;
}

#endif
