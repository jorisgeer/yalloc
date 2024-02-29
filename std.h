/*std.h - standard library

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

static size_t zeroblock;

void *malloc(size_t n)
{
  void *p;

  // oswrite(2,"malloc\n",7);

  if (likely(n != 0)) {
    if (unlikely(n > (Maxvmsiz >> 2) )) return oom(__LINE__,Fstd,n,1);
    p = yalloc(n,0);
#ifdef VALGRIND
    if (p) {
      vg_mem_undef(p,n)
    }
#endif
  } else {
    p = &zeroblock;
    ylog(Fstd,"alloc 0 = %p",p);
    vg_mem_noaccess(p,sizeof(zeroblock))
  }
  return p;
}

void free(void *p)
{
  if (p == nil) return;
  if (p == &zeroblock) {
    if (zeroblock != 0) error(__LINE__,Fstd,"written to malloc(0) block (%zx)",zeroblock);
    return;
  }
  yfree(p,0);
}

void free_sized(void *p,Unused size_t n)
{
  free(p);
}

void *calloc (size_t count, size_t size)
{
  void *p;
  size_t n;

  if ( (count | size) == 0) return malloc(0);

#if SIZE_T_MAX == hi32
  unsigned long long nn = (unsigned long long)count * size;
  if (nn >= hi32) return oom(__LINE__,Fstd,count,size);
  n = (size_t)n;
#else
  if (sat_mul(count,size,&n))  return oom(__LINE__,Fstd,count,size);
  if (n > (Maxvmsiz >> 2) ) return oom(__LINE__,Fstd,count,size);
#endif

  p = yalloc( (size_t)n,1);
  if (p) {
    vg_mem_def(p,n)
  }
  return p;
}

void *realloc(void *p,size_t newlen)
{
  if (p == nil) return malloc(newlen);

  if (newlen == 0) {
    free(p);
    return nil;
  }
  if (newlen > (Maxvmsiz >> 2) ) return oom(__LINE__,Fstd,newlen,1);

  return yrealloc(p,newlen);
}

void *aligned_alloc(size_t align, size_t size)
{
  ub4 ord;

  if (size > (Maxvmsiz >> 1) || align > (Maxvmsiz >> 2) ) return oom(__LINE__,Fstd,size,1);
  else if (size == 0) return malloc(0);
  return yalloc_align(align,size);
}

#ifdef _Yal_enable_psx_memalign
#include <errno.h>
int posix_memalign(void **memptr, size_t align, size_t size)
{
  void *p;

  // if (align < sizeof(void *)) return EINVAL;
  p = aligned_alloc(align,size);
  *memptr = p;

  if (p == nil) return ENOMEM;
  return 0;
}
#endif

#ifdef _Yal_enable_c23

void free_sized(void *ptr, Unused size_t size)
{
  free(ptr,size);
}

void free_aligned_sized(void *ptr, size_t alignment, size_t size)
{
  free(ptr,size);
}

#endif
