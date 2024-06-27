/*alloc.h - alloc() toplevel

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Largest blocks are served directly by mmap(2) or equivalent, yet wrapped in a region for free(3) to find.
  Midsize blocks are served by a buddy heap
  Small blocks are either served by fixed-sized slab or buddy, dependent on usage stats
  A recycling bin for the latter two categories serves as a cache, forming a fast path
*/

#undef Logfile
#define Logfile Falloc

static xregion *yal_mmap(heap *hb,size_t len,enum Loc loc)
{
  size_t alen = doalign(len,Pagesize);
  void *p = osmem(Fln,hb,alen,"block > mmap threshold");
  xregion *reg;

  if (p == nil) return nil;

  ystats(hb->stats.mapallocs)
  ytrace(loc,"Len %zu mmap %zu",len,hb->stats.mmaps)
  reg = newxregion(hb,p,alen,len);
  if (reg == nil) {
    *hb->errmsg = 0;
    if (hb->status != St_error) oom(hb,Fln,loc,len,1);
    return nil;
  }
  reg->typ = Rmmap;
  ytrace(loc,"len %zu mmap = %p",len,p)
  // Bist_add(hb,p,len,loc)
  return p;
}

static const ub2 miniclas_align[9] = { 0,2,2,4, 4,8,8,8, 8 };

static Hot void *heapalloc(heap *hb,size_t ulen,bool clr,enum Loc loc)
{
  uint32_t alen,clen,len,align;
  void *p;
  region *reg,**clasregs;
  ub2 clas;
  ub2 claspos,clastop;
  ub4 ord,cord;
  ub4 iter;
  int locmod;
  ylock_t one = 1;

  if (unlikely(ulen >= (1ul << (Maxorder - Addorder)))) {
    return yal_mmap(hb,ulen,loc);
  }

  len = (uint32_t)ulen;

#if Yal_locking == 3 // c11 threads
  if (unlikely(hb->boot != 0)) {
    return __je_bootstrap_malloc(ulen);
  }
#endif

// size classes: mini, 16, 16 * pwr2, Clasbits
#define Mini 4
#define Base 1
#define Pwr2 16 - Basealign2
#define Fine Clasbits

#define Clasinc (1u << Clasbits)

  // try last used reg
  reg = hb->prvallreg;
  alen = reg->cellen;

  if (len <= alen && len * 2 > alen) { // suitable
    ystats(reg->stats.fastregs)

  } else {

  // find suitable size class
  alen = len;
  switch (len) {

  case 0:
    p = &zeroblock;
    ytrace(loc,"alloc 0 = %p",p);
    vg_mem_noaccess(p,1)
    return p;

  case 1: case 2: alen = 2; clas = 0; break;
  case 3: case 4: alen = 4; clas = 1; break;
  case 5: case 6: case 7: case 8: alen = 8; clas = 2; break;

  case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: alen = 16; clas = 3; break;

  case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24: alen = 24; clas = 4; break;

  case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: alen = 32; clas = 5; break;

  default:
    if (len >= Maxclasslen) {
      // todo buddy
      return yal_mmap(hb,len,loc);
    }
    // create class for in-between pwr2 sizes,e.g. 48, 56
    ord = 32u - clz(len);
    clas = Mini + Base + (ub2)(ord << Clasbits);
    if (len & (len - 1)) {
      cord = ord - Clasbits - 1;
      align = (1u << cord);
      alen = doalign(len,align);
      clen = alen >> cord;
      clas += (ub2)clen;
    } else {
      alen = len;
      clas += (1u << Clasbits);
    }
  } // switch len
  ycheck(nil,Lalloc,clas >= Clascnt,"class %u out of range %u",clas,Clascnt)
  ycheck(nil,Lalloc,alen < len,"alen %u len %u",alen,len)

    // check size classes aka slabs, tentative at first for all sizes
#if Yal_enable_check
  clen = hb->claslens[clas];
  if (clen && clen != alen) { error(hb->errmsg,Lalloc,"clas %u len %u vs %u from %u",clas,clen,alen,len) return nil; }
#endif
  hb->claslens[clas] = alen;

  clasregs = hb->clasregs + clas * Clasregs;
  clastop = hb->clastop[clas];
  if (likely(clastop != 0)) {
    claspos = hb->claspos[clas];
    reg = clasregs[claspos];
  } else {
    reg = newslab(hb,alen,len,clas,0);
    if (unlikely(reg == nil)) return nil;
    hb->clastop[clas] = 1;
    clasregs[0] = reg;
  }
  hb->prvallreg = reg;
  } // prvreg or not

  ytrace(loc,"Len %u clr %u %2zu",len,clr,reg->stats.allocs)
  iter = 1000;
  do {
    reg->clrlen = reg->clen;

    ycheck(nil,Lalloc,reg->cellen != alen,"region %x cel len %u vs %u",reg->id,reg->cellen,alen)

#if Yal_inter_thread_free
    locmod = Atomget(hb->lockmode)
    if (unlikely(locmod != 0)) {
      if (locmod == 1) Atomset(hb->lockmode,2); // ack
      Lock(hb,reg,1024 * 1024,Lalloc,Fln,nil,"slab alloc",len)
    }
#endif

    p = slab_alloc(hb,reg,alen,loc);

#if Yal_inter_thread_free
    if (unlikely(locmod != 0)) {
      Unlock(hb,reg,Lalloc,Fln,"slab alloc")
    }
#endif

    if (likely(p != nil)) {
#if Yal_enable_valgrind
      if (vg_mem_isaccess(p,alen)) error(hb->errmsg,Lalloc,"malloc(%p) was allocated earlier",p)
#endif
      if (unlikely(clr != 0)) {
        if (reg->clrlen) memset(p,0,reg->clrlen);
        vg_mem_def(p,len)
      } else {
        vg_mem_undef(p,len)
      }
      ytrace(loc,"len %u = %p %2zu",len,p,reg->accstats.allocs)
      Bist_add(hb,reg,p,len,loc)
      return p;
    } // p != nil aka have space

    if (unlikely(reg->status == St_error)) {
      ytrace(loc,"len %u = nil %2zu",len,reg->accstats.allocs)
      reg->status = St_ok;
      hb->status = St_error;
      hb->errfln = reg->errfln;
      return nil;
    }
    clas = reg->clas;
    claspos = 0;
    clastop = hb->clastop[clas];
    if (clastop >= Clasregs - 1) {
      hb->status = St_error;
      hb->errfln = Fln;
      error(hb->errmsg,Lalloc,"class %u size %u regions exceed %u",clas,alen,clastop)
      return nil;
    }

    clasregs = hb->clasregs + clas * Clasregs;
    while (claspos <= clastop) {
      reg = clasregs[claspos];
      if (reg == nil) { // create new
        reg = hb->prvallreg = newslab(hb,alen,len,clas,clastop);
        if (unlikely(reg == nil)) return nil;
        clasregs[claspos] = reg;
        hb->clastop[clas] = clastop + 1;
        hb->claspos[clas] = claspos;
        break;
      } else {
        if (reg->binpos || reg->premsk || reg->preofs < reg->runcnt) break;
      }
      claspos++;
    }
  } while (--iter);
  return nil;
}

static void *yal_heap(heap *hb,size_t len,bool clr,enum Loc loc)
{
  void *p;
  enum Status st;

  p = heapalloc(hb,len,clr,loc);

  if (likely(p != nil)) {
    vg_mem_maydef(p,len,clr)
    return p;
  }

  st = hb->status; hb->status = St_ok;
  *hb->errmsg = 0;
  errorctx(loc,hb->errmsg,"heap %u len %zu`",hb->id,len);
  error2(hb->errmsg,loc,hb->errfln,"status %u",st);
  if (st == St_error) return p;

  *hb->errmsg = 0;
  oom(hb,Fln,loc,len,1);

  if (st != St_ok) {
    hb->status = St_ok;
    if (st == St_oom) return osmmap(len);
  }

  // rare: contention
  ylog(Lalloc,"alloc(%zu): new contention heap",len)
  hb = new_heap();
  if (hb == nil) return osmmap(len);
  p = heapalloc(hb,len,clr,loc);
  return p;
}

// main entry.
static void *yalloc(size_t len,bool clr,enum Loc loc)
{
  heap *hb;
  void *p;

  hb = getheap();

  if (unlikely(hb == nil)) {
#if Bumplen
    mheap *mhb;
    ub4 pos;
    ub4 ulen,alen;
    if (len < Bumpmax) {
      ulen = (ub4)len;
      if (ulen == 0) return &zeroblock;
      alen = doalign(ulen,Stdalign);
      mhb = getminiheap(1);
      if (mhb == nil) return nil;
      pos = mhb->pos;
      if (pos + alen <= Bumplen) {
        mhb->meta[pos / Stdalign] = (ub2)(alen / Stdalign);
        mhb->pos = pos + alen;
        return mhb->mem + pos;
      }
    }
#endif
    hb = new_heap();
    if (hb == nil) return osmmap(len);
  }
  p = yal_heap(hb,len,clr,loc);
  return p;
}

static void *yalloc_align(size_t align, size_t len)
{
  void *p,*ap;
  ub4 align4;
  size_t alen;
  heap *hb;
  xregion *reg;
  size_t ip;

  if (len < 8)  align4 = miniclas_align[len] & 0xff;
  else if (len == 8) align4 = 8;
  else align4 = 16;

  hb = getheap();
  if (unlikely(hb == nil)) {
    hb = new_heap();
    if (hb == nil) return osmmap(len);
  }

  if (align <= align4) return yal_heap(hb,len,0,Lallocal); // no adjustment needed

  alen = max(len,align);
  if (align > Pagesize) alen += align;
  if (alen >= hb->mmap_threshold) {
    if (align <= Pagesize) return yal_heap(hb,len,0,Lallocal); // no adjustment needed

    // need to move base
    ylog(Lallocal,"aligned_alloc(%zu,%zu",align,len);
    reg = yal_mmap(hb,alen,Lallocal);
    if (reg == nil) return nil;
    ip = (size_t)reg->user;
    ip = doalign(ip,align);
    ap = (void *)ip;
    reg->meta = ap;
    return ap;
  }
  p = yal_heap(hb,alen,0,Lallocal);
  if (p == nil) return p;
  ip = (size_t)p;
  ip = doalign(ip,align);
  ap = (void *)ip;
  // todo buddy_addref(hb,reg,p,ap);
  return ap;
}
