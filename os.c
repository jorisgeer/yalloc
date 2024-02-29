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

#include <unistd.h>

int oswrite(int fd,const char *buf,size_t len)
{
  return write(fd,buf,len);
}

static _Bool reserve = 1;

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))

 #include <sys/mman.h>

void *osmmap(size_t len)
{
  void *p;

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

 #if 0
  p = sbrk(len + Stdalign);
  p = (p + Stdalign) & (Stdalign - 1);
 #endif
  return p;
}

void *osmremap(void *p,size_t orglen,size_t newlen)
{
  void *np;
#ifdef __linux__
  np = mremap(p,orglen,newlen,MREMAP_MAYMOVE);
  if (p == MAP_FAILED) {
    return NULL;
  }
  return p;
#else
  np = osmmap(newlen);
  if (np) memcpy(np,p,orglen);
  osmunmap(p,orglen);
  return np;
#endif
  return np;
}

void osmunmap(void *p,size_t len)
{
  munmap(p,len);
}

#elif defined _WIN32 || defined _WIN64

 #include <memoryapi.h>

void *osmmap(size_t len)
{
  void *p;

  p = VirtualAlloc(nil,len,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);

  return p;
}

void osmunmap(void *p,size_t len)
{
  VirtualFree(p,len);
}

#else
  #error "no mmap"
#endif
