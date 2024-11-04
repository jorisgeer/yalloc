/* std.h - standard library interface aka stdlib.h bindings aka api

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#define Logfile Fstd

void *malloc(size_t len)
{
  void *p;

#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    p =  bootalloc(Fln,0,Lnone,(ub4)len);
    minidiag(Fln,Lalloc,Debug,0,"TLS init %zu = %zx",len,(size_t)p);
    return p;
  }
#endif

  p = ymalloc(len,Fln);

  return p;
}

void free(void *p)
{
#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) return;
#endif

  yfree(p,0,Fln);
}

void *calloc (size_t count, size_t size)
{
  void *p;
  size_t len;

#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    p =  bootalloc(Fln,0,Lnone,(ub4)(count * size));
    minidiag(Fln,Lcalloc,Debug,0,"TLS init %zu * %zu = %zx",count,size,(size_t)p);
  }
#endif

  if (sizeof(size_t ) < sizeof(long long)) { // e.g. 32-bit system
    unsigned long long nn = (unsigned long long)count * size;
    if (unlikely( nn > Size_max)) return oom(Fln,Lcalloc,count,size);
    len = (size_t)nn;
  } else { // common for 64-bit systems
    bool rv = sat_mul(count,size,&len);
    if (unlikely(rv != 0)) return oom(Fln,Lcalloc,count,size);
  }

#if Yal_enable_stats && Yal_trigger_stats
  if (unlikely(len == 0)) { // calloc(0,Yal_magic) prints stats
    yal_trigger_stats(size);
  }
#endif

  p = yalloc(len,len,Lcalloc,Fln);

  return p;
}

void *realloc(void *p,size_t newlen)
{
  void *q;

#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    if (p == nil) p = bootalloc(Fln,0,Lnone,(ub4)newlen);
    else p = nil; // should not occur, cannot locate
    minidiag(Fln,Lalloc,Debug,0,"TLS init %zu = %zx",newlen,(size_t)p);
    return nil;
  }
#endif

  q = yrealloc(p,Nolen,newlen,0);
  return q;
}

void *aligned_alloc(size_t align, size_t size)
{
  void *p;

#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    p =  osmmap(size); // assuming align > Page not done
    minidiag(Fln,Lalloc,Debug,0,"TLS init %zu = %zx",size,(size_t)p);
    return p;
  }
#endif

  p = yalloc_align(align,size,Fln);
  return p;
}

#if Yal_psx_memalign
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

void *memalign(size_t a,size_t n)
{
  return aligned_alloc(a,n);
}

#if Yal_psx_memalign > 1
void *valloc(size_t n) // deprecated
{
  return aligned_alloc(Pagesize,n);
}

void *pvalloc(size_t n) // deprecated
{
  return aligned_alloc(Pagesize,doalign8(n,Pagesize));
}
#endif
#endif // psx_memalign

#if Yal_enable_c23

// https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2699.htm
// In yalloc, the size arg is used for checking only.  If zero, it is equivalent to free(p)
void free_sized(void *ptr,size_t size)
{
  yfree(ptr,size,Fln);
}

// in contrast with the C23 standard, the pointer passed may have been obtained from any of the allocation functions
void free_aligned_sized(void *ptr, size_t Unused alignment, size_t size)
{
  yfree(ptr,size,Fln);
}
#endif // c23

#undef Logfile
