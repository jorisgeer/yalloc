/*free.h - free() toplevel

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Handle recycling bin and eventually pass on to slab, buddy or mmap region.free()
*/

#undef Logfile
#define Logfile Ffree

static enum Status free_mmap(heap *xhb,heap *hb,xregion *reg,size_t ip,size_t ulen)
{
  size_t len = reg->len;
  size_t ibase = (size_t)reg->user;
  size_t iap = (size_t)reg->meta;
  bool rv;

  if (ulen && ulen != len) { error(hb->errmsg,Lfree,"free(%zx,%zu) mmap block had size %zu",ip,ulen,len) return St_error; }
  if  (ip >= ibase + len) {
    error(hb->errmsg,Lfree,"free(%zx) is %zu`b after mmap region %u %zx .. %zx of len %zu`",ip,ip - ibase - len,reg->dirid,ibase,ibase + len,len)
    return St_error;
  }
  if  (ip > ibase) {
    error(hb->errmsg,Lfree,"free(%zx) is %zu`b within mmap region %u %zx .. %zx of len %zu`",ip,ip - ibase,reg->dirid,ibase,ibase + len,len)
    return St_error;
  }
  if (iap) {
    if (ip != iap) { error(hb->errmsg,Lfree,"free(%zx) is %zu`b in aligned mmap block allocated at %zx from %zx",ip,ip - iap,iap,ibase) return St_error; }
  } else {
    if (ip != ibase) { error(hb->errmsg,Lfree,"free(%zx) is %zu`b in mmap block allocated at %zx",ip,ip - ibase,ibase) return St_error; }
  }

  // if (unlikely(mp == 0)) { error(hb->errmsg,__LINE__,Falloc,"free-mmap(): double free of ptr %z of len %z",ip,len); return; }
  // if (unlikely(ip != mp)) { error(hb->errmsg,__LINE__,Falloc,"free-mmap(): invalid ptr %z",ip); return; }
  if (ip & (Pagesize - 1)) { error(hb->errmsg,Lfree,"free-mmap(): invalid ptr %zx",ip) return St_error; }
  // if (len < hb->mmap_threshold) { error(hb->errmsg,Lfree,"free: ptr %zx len %zu` was not mmap()ed",ip,len); return 0; }
  // if (unlikely(len > (1UL << Vmsize)) { error(hb->errmsg,__LINE__,Falloc,"free: ptr %z len `%Ilu was not mmap()ed",ip,len); return; }

  ystats(hb->stats.munmaps)
  ytrace(Lfree,"ptr+%zx len %zu mmap",ip,len)
  rv = delxregion(xhb,hb,Lfree,reg);
  ytrace(Lfree,"ptr-%zx len %zu mmap %2zu",ip,len,hb->stats.munmaps)
  return rv ?  St_trim : St_ok;
}

// returns len
static ub4 find_mini(mheap *mhb,size_t ip,size_t clen)
{
#if Bumplen
  char *cp = (char *)ip;
  ub4 ofs,len;

  if (mhb == nil) return 0;
  if (cp < mhb->mem || cp > mhb->mem + Bumplen - Stdalign) return 0;
  ofs = (ub4)(cp - mhb->mem);
  if ( (ofs & (Stdalign - 1)) ) { error2(nil,Lfree,Fln,"invalid ptr %zx", ip); return 0; }
  len = mhb->meta[ofs / Stdalign];
  if (len == 0) { error2(nil,Lfree,Fln,"invalid ptr %zx", ip); return 0; }
  len *= Stdalign;
  if (unlikely(clen != 0)) {
    if (clen != len) error2(nil,Lfree,Fln,"block %zx has len %u, not %zu",ip,len,clen);
  }
    return len;
#endif
  return 0;
}

#if Yal_inter_thread_free
// caller, owner
static bool free_remote(heap *hb,heap *xhb,xregion *reg,size_t ip,size_t len)
{
  ub4 dulen;
  region *creg;
  void *user;
  bool isempty,iserr;
  enum Rtype typ;
  enum Status rv;
  ylock_t one = 1;
  int locmod;
  int zero = 0;

  ystats(xhb->stats.remotefrees)

  typ = reg->typ;
  if (unlikely(typ >= Rmmap)) {
    // dulen = reg->dulen;
    user = reg->user;
    len = reg->len;
    if (typ == Rmmap) {
      reg->typ = Rmmap_free; // mark free
      rv = free_mmap(hb,xhb,reg,(size_t)user,len);
      if (rv == St_error) xhb->stats.invalid_frees++;
      else return 0;
    } else if (typ == Rmmap_free) {
      xhb->stats.invalid_frees++;
      *hb->errmsg = 0;
      free2(hb,Fln,Lfree,0,(void *)ip,len,"free");
    } // else osunmem(Fln,xhb,user,len,"free-rem");
    return 1;
  }

  if (likely(typ == Rslab)) { // put in recycling bin
    creg = (region *)reg;

    if (unlikely(len != 0)) {
      iserr = (len != creg->ucellen);
    } else iserr = 0;

    if (likely(iserr == 0)) {

      locmod = Atomget(xhb->lockmode)
      if (locmod < 2) {
        if (locmod == 0) Cas(xhb->lockmode,zero,1); // request
        return 0; // no locking active
      }
      Lock(xhb,creg,1024,Lfree,Fln,1,"slab free remote",(ub4)len)

      isempty = slab_bin(xhb,creg,ip); // put in recycling bin

      Unlock(xhb,creg,Lfree,Fln,"free-mmap")

    } else isempty = 0;

    if (unlikely(isempty != 0)) {
      ylog(Lfree,"slab reg %x empty",reg->dirid)
#if 0
      // mark for recycle
      binsum = xhb->regbinsum;
      if (binsum > Regbinsum) {
        delregion(xhb,reg);
        return;
      }
      rid = reg->dirid;
      ord = reg->order;
      rbpos = xhb->regbinpos[ord];
      regbins = xhb->regbins + ord * Regbin;
      regbins[rbpos] = rid;
      xhb->regbinpos[ord] = rbpos + 1;
      xhb->regbinsum += reg->len;
#endif
    } // isempty

    if (iserr) error(hb->errmsg,Lfree,"free(%zx) of size %u has invalid len %zu",ip,creg->ucellen,len)

    return 0;
  } // slab

  if (typ == Rbuddy) {
    creg = (region *)reg;
    if (buddy_free(xhb,creg,ip)) {
      delregion(xhb,hb,reg);
    }
  }

  return 0;
}

// caller, owner
static xregion *findregion_rem(heap *hb,heap *xhb,size_t ip)
{
  xregion *reg;
  size_t v1,v2;
  size_t base,ip1;
  ub4 iter = 5;

  do {
    v1 = atomic_load_explicit(&xhb->dirversion,memory_order_relaxed);
    reg = findregion(xhb,ip);
    v2 = atomic_load_explicit(&xhb->dirversion,memory_order_relaxed);
    if (unlikely(--iter == 0)) return nil;
  } while (v1 != v2 || (v1 & 1));
  if (reg == nil) return reg;

  base = (size_t)reg->user;
#if Yal_enable_check
  if (ip < base) { error(hb->errmsg,Lfree,"reg %x ptr %zx below base %zx",reg->dirid,ip,base) return nil; }
#endif
  if (unlikely(ip >= base + reg->len)) {
    ip1 = ip >> (Vmsize - Dir1);
    if (ip1 & ~Dir1msk) error(hb->errmsg,Lfree,"ptr  %zx is outside %u bit VM space",ip,Vmsize)
    else if (base) error(hb->errmsg,Lfree,"heap %u from %u: ptr %zx is %zu`b beyond region %x %zx + %zu type %u",xhb->id,hb->id,ip,ip - base - reg->len,reg->dirid,base,reg->len,reg->typ)
    else error(hb->errmsg,Lfree,"heap %u from %u empty region %x type %u",xhb->id,hb->id,reg->dirid,reg->typ)
    return nil;
  }

  return reg;
}

static ub4 findmini_rem(size_t ip)
{
  mheap *xmhb = atomic_load_explicit(&global_mheaps,memory_order_relaxed);
  ub4 iter = 50;
  ub4 mlen;

  while (xmhb && --iter) {
    if ( (mlen = find_mini(xmhb,ip,0)) ) return mlen;
    xmhb = xmhb->nxt;
  }
  return 0;
}

// returns true when found
static bool free_remote_region(heap *hb,size_t ip,size_t len)
{
  heap *xhb;
  ub4 iter = 64;
  xregion *reg;

  xhb = hb->prvxheap;
  if (likely(xhb != nil)) reg = findregion_rem(hb,xhb,ip);
  else reg = nil;

  if (reg == nil) {
    xhb = atomic_load_explicit(&global_heaps,memory_order_relaxed);
    do {
      if (xhb != hb) reg = findregion_rem(hb,xhb,ip);
      if (reg) {
        hb->prvxheap = xhb;
        ystats(xhb->stats.freeremotes)
        if (unlikely(free_remote(hb,xhb,reg,ip,len) != 0)) error(hb->errmsg,Lfree,"cannot free(%zx) in heap %u from heap %u",ip,xhb->id,hb->id)
        return 1;
      }
    } while (--iter && (xhb = xhb->nxt) != nil);
  }

  // not found: check mini
  if (findmini_rem(ip)) return 1;
  // bist_del(hb,xhb,ip,Lfree)
  return 0;
}
#endif

/* First, find region. If not found, check remote heaps
 For slab, put in bin. If bin full, free a set
 For mmap, directly delete
 */
static Hot void *yfree_heap(heap *hb,void *p,size_t len)
{
  size_t ibase,ip = (size_t)p;
  mheap *mhb;
  ub4 mlen;
  region *creg;
  xregion *reg;
  enum Rtype typ;
  ub2 clas;
  size_t binsum;
  reg_t rid,*regbins;
  ub4 order;
  ub2 rbpos;
  bool isempty;
  enum Status rv;
  ylock_t one = 1;
  int locmod;

#if Yal_locking == 3 // c11 threads
  if (unlikely(hb->boot != 0)) {
    return nil;
  }
#endif

  // start trying last used region
  reg = hb->mrufrereg;
  ibase = (size_t)reg->user;
  if (ip < ibase || ip >= ibase + reg->len) { // need to search page dir

    // find region. If none, either not allocated, bump alloc or zero block
    reg = findregion(hb,ip);
    if (unlikely(reg == nil)) {
      mhb = getminiheap(0);
      if ( (mlen = find_mini(mhb,ip,len)) ) return p;

      if (ip >= (1ul << Vmsize)) {
        error(hb->errmsg,Lfree,"invalid free(%zx) above max %u bits VM",ip,Vmsize)
        return nil;
      }

      if (p == (void *)&zeroblock) {
        if (zeroblock != 0) error(hb->errmsg,Lfree,"written to malloc(0) block (%x)",zeroblock)
        return p;
      }

#if Yal_inter_thread_free
      if (likely(free_remote_region(hb,ip,len) != 0)) return p;
#endif

      findregion_rep(Lfree,hb,ip);
      hb->stats.invalid_frees++;
      error(hb->errmsg,Lfree,"ptr %p unallocated - not in any of %u region(s) in heap %u",p,hb->allocregcnt - hb->freeregcnt,hb->id)
      return nil;
    } // reg == nil aka not found in page dir

#if Yal_enable_check
    ibase = (size_t)reg->user;
    size_t xlen = reg->len;
    if  (ip < ibase) {
      error(hb->errmsg,Lfree,"ptr %zx is %zu`b before region %u %zx .. %zx",ip,ibase - ip,reg->dirid,ibase,ibase + xlen)
      return nil;
    } else if  (ip >= ibase + xlen) {
      error(hb->errmsg,Lfree,"ptr %zx is %zu`b after region %u %zx .. %zx of len %zu`",ip,ip - ibase - xlen,reg->dirid,ibase,ibase + xlen,xlen)
      return nil;
    }
#endif
    hb->mrufrereg = reg;
  } // not last reg

  typ = reg->typ;
  if (likely(typ == Rslab)) { // put in recycling bin
    creg = (region *)reg;
    clas = creg->clas;

    if (unlikely(len != 0)) {
      if (len != creg->ucellen) {
        error(hb->errmsg,Lfree,"free(%zx) of size %u has invalid len %zu",ip,creg->ucellen,len)
        return nil;
      }
    }
    ytrace(Lfree,"ptr+%zx len %u %2zu",ip,creg->ucellen,creg->accstats.frees)
    Bist_del(hb,creg,ip,Lfree)

#if Yal_inter_thread_free
    locmod = Atomget(hb->lockmode)
    if (unlikely(locmod != 0)) {
      if (locmod == 1) Atomset(hb->lockmode,2); // ack
      Lock(hb,creg,1024,Lfree,Fln,nil,"free",0)
    }
#endif

    isempty = slab_bin(hb,creg,ip); // put in recycling bin

    if (unlikely(isempty != 0)) {
#if 0
      // mark for recycle
      order = reg->order;
      rbpos = order < Maxregion ? hb->regbinpos[order] : Regbin;
      binsum = hb->regbinsum;
      if (binsum > Regbinsum || rbpos == Regbin) {
        delregion(hb,reg,0);
      } else {
        rid = reg->dirid;
        regbins = hb->regbins + order * Regbin;
        regbins[rbpos] = rid;
        ylog(Lfree,"bin reg %x ord %u at %u",reg->id,order,rbpos)
        hb->regbinpos[order] = rbpos + 1;
        hb->regbinsum += reg->len;
    /*
        claspos = hb->claspos[clas];
        clasregs = hb->clasregs + clas * Clasregs;
        clasregs[claspos] = nil;
     */
      }
#endif
    }

#if Yal_inter_thread_free
    if (unlikely(locmod != 0)) { // no acq if req
      Unlock(hb,creg,Lfree,Fln,"free")
    }
#endif
    ytrace(Lfree,"ptr-%zx len %u %2zu",ip,creg->ucellen,creg->accstats.frees)
    return p;
  } // slab

  if (reg->typ == Rbuddy) {
    if (buddy_free(hb,(region *)reg,ip)) {
      delregion(hb,hb,reg);
    }
    return 0;

  // free mmap directly
  } else if (reg->typ == Rmmap) {
    // Bist_del(hb,hb,ip,Lfree)
    rv = free_mmap(hb,hb,reg,ip,len);
    if (rv == St_error) { hb->stats.invalid_frees++; return nil; }
    else if (rv == St_trim) ytrim();
    return p;
  } else if (unlikely(reg->typ == Rmmap_free)) {
    hb->stats.invalid_frees++;
    *hb->errmsg = 0;
    free2(hb,Fln,Lfree,reg->dirid,p,reg->len,"mmap");
    return nil;
  } else {
    error(hb->errmsg,Lfree,"invalid region %u",reg->dirid)
    return nil;
  }
}

static void yfree(void *p,size_t len)
{
  heap *hb;
  mheap *mhb;
  size_t ip = (size_t)p;
  ub4 mlen;

  hb = getheap();
  if (unlikely(hb == nil)) {
    mhb = getminiheap(0);
    if ( (mlen = find_mini(mhb,ip,len)) ) {
      if (len && len != mlen) error2(nil,Lfree,Fln,"ptr %zx len %zu vs %u",ip,len,mlen);
      return;
    }
    if (p == &zeroblock) return;
    error2(nil,Lfree,Fln,"pointer %zx was not malloc()ed: empty heap",ip)
  } else yfree_heap(hb,p,len);
}

static size_t yalloc_getsize(void *p)
{
  heap *hb;
  mheap *mhb;
  xregion *reg;
  region *creg;
  size_t ip = (size_t)p;
  ub4 cel,cellen;
  ub4 mlen;

  hb = getheap();
  if (unlikely(hb == nil)) {
    mhb = getminiheap(0);
    if ( (mlen = find_mini(mhb,ip,0)) ) return mlen;
    if (p == &zeroblock) return 0;
  }
  reg = findregion(hb,ip);
  if (unlikely(reg == nil)) {
    findregion_rep(Lsize,hb,ip);
    return 0;
  }

  switch (reg->typ) {
  case Rmmap: ylog(Lfree,"usable_size mmap reg %x: %zx",reg->dirid,reg->len) return reg->len;
  case Rmmap_free: return 0;
  case Rslab:
    creg = (region *)reg;
    cellen = creg->cellen;
    ylog(Lfree,"usable_size slab reg %x: %u",creg->id,cellen)
    cel = slab_cel(hb,creg,ip,cellen,Lsize);
    return cel == Nocel ? 0 : cellen;
  case Rbuddy: return 0;
  case Rnone: return 0;
  }
  return 0;
}
