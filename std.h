/* std.h - standard library interface aka stdlib.h bindings

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
    p = __je_bootstrap_malloc(len);
    minidiag(Fln,Lalloc,Debug,0,"TLS init %zu = %zx",len,(size_t)p);
    return p;
  }
#endif

  p = yalloc(len,len,Lalloc,Fln);
  return p;
}

void free(void *p)
{
  yfree(p,0,Fln);
}

void *calloc (size_t count, size_t size)
{
  void *p;
  size_t len;

#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    p = __je_bootstrap_calloc(count,size);
    minidiag(Fln,Lcalloc,Debug,0,"TLS init %zu * %zu = %zx",count,size,(size_t)p);
  }
#endif

  if (sizeof(size_t ) < sizeof(long long)) { // e.g. 32-bit system todo change to #if
    unsigned long long nn = (unsigned long long)count * size;
    if (unlikely( nn > Size_max)) return oom(Fln,Lcalloc,count,size);
    len = (size_t)nn;
  } else if (sizeof(size_t ) < sizeof(long long)) { // e.g. long long = 128 (unlikely) or 16-bits system (likely)
    unsigned long long nn = (unsigned long long)count * size;
    if (unlikely(nn > ULONG_MAX)) return oom(Fln,Lcalloc,count,size);
    len = (size_t)nn;
  } else if ( (size | count ) <= Hi16) { // only overflows on 16-bit systems
    len = count * size;
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
  ytrace(Lalloc,"-calloc(%zu,%zu) = %p",count,size,p)

  return p;
}

void *realloc(void *p,size_t newlen)
{
  void *q;

  ytrace(Lreal,"realloc(%p,%zu)",p,newlen)

#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    q = __je_bootstrap_malloc(newlen);
    return q; // no oldlen available, not copied
  }
#endif

  q = yrealloc(p,newlen,0);
  ytrace(Lreal,"realloc(%p,%zu) = %p",p,newlen,q)
  return q;
}

void *aligned_alloc(size_t align, size_t size)
{
#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    return __je_bootstrap_malloc(size);
  }
#endif

  return yalloc_align(align,size);
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

// in contrast with th C23 standard, the pointer passed may have been obtained from any of the allocation functions
void free_aligned_sized(void *ptr, size_t Unused alignment, size_t size)
{
  yfree(ptr,size,Fln);
}
#endif // c23

#undef Logfile
