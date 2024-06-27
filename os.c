/* os.c - operating system bindings

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifdef __unix__
 #ifdef __linux__
 #define _GNU_SOURCE
 #else
  #define _POSIX_C_SOURCE 200809L
 #endif
#endif

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <string.h>

#include "os.h"

static char *ucnv(char *end,unsigned int x)
{
    do *--end = (char)((x % 10) + '0'); while (x /= 10);
    return end;
}

int osopen(const char *name)
{
  int fd = open(name,O_RDONLY);
  return fd;
}

void osclose(int fd)
{
  close(fd);
}

ssize_t osread(int fd,char *buf,size_t len)
{
  ssize_t nn = read(fd,buf,len);
  return nn;
}

void oswrite(int fd,const char *buf,size_t len)
{
  size_t nn;
  ssize_t nw;
  unsigned int blen = 32;
  char ibuf[32];
  char *p;
  int ec;

  while (len) {
    nw = write(fd,buf,len);
    if (nw == -1) {
      ec = errno;
      ibuf[blen-1] = 0;
      write(2,"cannot write to fd ",19);
      p = ucnv(ibuf + blen,(unsigned int)fd);
      write(2,p,(size_t)(ibuf + blen - p));
      write(2," : ",3);
      p = ucnv(ibuf + blen,(unsigned int)ec);
      write(2,p,(size_t)(ibuf + blen - p));
      return;
    }
    nn = (size_t)nw;
    if (len < nn) nn = len;
    len -= nn;
    buf += nn;
  }
}

static _Bool reserve = 1;

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))

unsigned int ospagesize(void)
{
  long ret = sysconf(_SC_PAGESIZE);

  if (ret == -1) return 0;
  else return (unsigned int)ret;
}

 #include <sys/mman.h>

void *osmmap(size_t len)
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
#else
  p = sbrk(len + Stdalign);
  p = (p + Stdalign - 1) & (Stdalign - 1);
#endif
  return p;
}

void *osmremap(void *p,size_t orglen,size_t newlen)
{
  void *np;
#ifdef __linux__
  np = mremap(p,orglen,newlen,MREMAP_MAYMOVE);
  if (np == MAP_FAILED) {
    return NULL;
  }
#else
  np = osmmap(newlen);
  if (np) memcpy(np,p,orglen);
  munmap(p,orglen);
#endif
  return np;
}

int osmunmap(void *p,size_t len)
{
  return munmap(p,len);
}

#elif defined _WIN32 || defined _WIN64

 #include <memoryapi.h>

unsigned int ospagesize(void)
{
  SYSTEM_INFO si;

  GetSystemInfo(&si);
  return si.dwPageSize;
}

void *osmmap(size_t len)
{
  void *p;

  p = VirtualAlloc(nil,len,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);

  return p;
}

int osmunmap(void *p,size_t len)
{
  return VirtualFree(p,len);
}

#else

unsigned int ospagesize(void) { return 4096; } // a too-low pagesize never harms

void *osmmap(size_t len) { return nil; }
void *osmremap(void *p,size_t orglen,size_t newlen)
{
  return p;
}

int osmunmap(void *p,size_t len) { return 0; }

  #error "no mmap"

#endif // unix or windows
