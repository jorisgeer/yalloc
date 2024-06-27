/*std.h - standard library interface aka stdlib.h bindings

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#undef Logfile
#define Logfile Fstd

void *malloc(size_t len)
{
  void *p;

#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    ylog(Lalloc,"boot malloc %zu",len)
    return __je_bootstrap_malloc(len);
  }
#endif

  p = yalloc(len,0,Lalloc);

  return p;
}

void free(void *p)
{
  if (likely(p != nil)) yfree(p,0);
}

void *calloc (size_t count, size_t size)
{
  void *p;
  size_t len,maxlen;
  bool rv;

#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    ylog(Lfree,"boot calloc %zu * %zu",count,size)
    return __je_bootstrap_calloc(count,size);
  }
#endif

#if SIZE_MAX == hi32
  ub8 nn = (ub8)count * size;
  if (unlikely(nn >= hi32)) return oom(nil,Fln,Lcalloc,count,size);
  len = (size_t)len;
#else
  maxlen = 1ul << (Vmsize - 2);
  rv = sat_mul(count,size,&len);
  if (unlikely(rv != 0 || len > maxlen)) return oom(nil,Fln,Lcalloc,count,size);
#endif

#if Yal_enable_stats
  if (unlikely(len == 0)) {
    if (size == Yal_trigger_stats || size == Yal_trigger_stats_clear) {
      yal_mstats(nil,1,0,size == Yal_trigger_stats_clear,"calloc(x)");
      return &zeroblock;
    }
  }
#endif

  p = yalloc(len,1,Lcalloc);

#if Yal_enable_valgrind
  if (len && p && vg_mem_isaccess(p,len)) { error(hb->errmsg,"calloc(%p) was previously allocated",p) return nil; }
  vg_mem_def(p,len)
#endif

  return p;
}

void *realloc(void *p,size_t newlen)
{
  size_t maxlen;
  void *q;

  ylog(Lreal,"realloc(%p,%zu)",p,newlen)

#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    void *q = __je_bootstrap_malloc(newlen);
    return q; // no oldlen available, not copied
  }
#endif

  if (p == nil) {
    p = yalloc(newlen,0,Lreal);
    return p;
  }

  if (unlikely(newlen == 0)) {
    yfree(p,0);
    return &zeroblock;
  }
  maxlen = 1ul << (Vmsize - 2);
  if (unlikely(newlen >= maxlen)) return oom(nil,Fln,Lreal,newlen,1);

  q = yrealloc(p,newlen);
  ylog(Lreal,"realloc(%p,%zu) = %p",p,newlen,q)
  // if (unlikely(q == nil)) { _yal_mstats(nil,1,0); exit(1); }
  return q;
}

void *aligned_alloc(size_t align, size_t size)
{
  size_t maxlen;

#if Yal_prep_TLS
  if (unlikely(yal_tls_inited == 0)) {
    ylog(Lallocal,"boot aligned_alloc %zu",len)
    return __je_bootstrap_malloc(len);
  }
#endif

  if (size == 0) return yalloc(0,0,Lallocal);
  maxlen = 1ul << (Vmsize - 2);
  if (unlikely(size >= maxlen || align > maxlen)) return oom(nil,Fln,Lallocal,size,1);
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

void free_sized(void *ptr,size_t size)
{
  if (likely(ptr != nil)) yfree(ptr,size);
}

void free_aligned_sized(void *ptr, size_t alignment, size_t size)
{
  f (likely(ptr != nil)) yfree(ptr,size);
}

#endif
