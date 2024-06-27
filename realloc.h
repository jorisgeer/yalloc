/*realloc.h - realloc() toplevel

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#undef Logfile
#define Logfile Frealloc

static void *real_copy(heap *hb,void *p,size_t oldlen,size_t newlen)
{
  void *np;

   np = yal_heap(hb,newlen,0,Lreal);
   if (unlikely(np == nil)) {
#if Free_failed_realloc
    yfree_heap(hb,p,0);
#endif
     return nil;
  }
  memcpy(np,p,oldlen);
  vg_mem_def(np,oldlen)
  return np;
}

static void *real_mmap(heap *hb,xregion *reg,void *p,size_t orglen,size_t newlen,size_t newulen,bool remote)
{
  // ylock_t one = 1;
  void *np = osmremap(p,orglen,newlen);

  // Bist_del(hb,hb,(size_t)p,Lreal)
  if (np == nil) {
#if Free_failed_realloc
    delregion(hb,reg,0);
#endif
    return nil;
  }
  // Bist_add(hb,np,newlen,Lreal)

  if (remote) {
    ylog0(Lreal,"< frealloc-remote lock");
    // Lock(hb,reg,1024,Lreal,Fln,nil,"realloc-mmap",reg->dirid)
    ylog0(Lreal,"> frealloc-remote lock");
  }

  if (np != p || newlen < orglen) {
    setregion(hb,reg,(size_t)p,orglen); // remove old
    vg_mem_noaccess(p,orglen)
  }
  reg->len = newlen;
  // reg->ulen = (ub4)(newlen - newulen);
  reg->user = np;
  setregion(hb,reg,(size_t)np,newlen);
  vg_mem_undef(np,newlen)
  vg_mem_def(np,orglen)

  if (remote) {
    ylog0(Lreal,"< frealloc-remote unlock");
    // Unlock(hb,reg,Lreal,Fln,"realloc-mmap")
    ylog0(Lreal,"> frealloc-remote unlock");
  }

  return np;
}

#if Yal_inter_thread_free
static xregion * real_remote_region(heap *hb,size_t ip)
{
  heap *xhb;
  bool root = 0;
  xregion *reg;

  xhb = hb->prvxheap;

  do {
    if (likely(xhb && xhb != hb)) {
      reg = findregion(xhb,ip);
      if (reg) {
        ystats(xhb->stats.freeremotes)
        hb->prvxheap = xhb;
        return reg;
      }
    }
    if (unlikely(root == 0) || xhb == nil) xhb = atomic_load_explicit(&global_heaps,memory_order_relaxed);
    else xhb = xhb->nxt;
    root = 1;
  } while (xhb);
  return nil;
}
#endif

// main realloc(). nil ptr and nil newlen already covered
static void *yreal_heap(heap *hb,void *p,size_t newlen)
{
  xregion *xreg;
  region *reg,*nreg,**clasregs;
  enum Rtype typ;
  ub4 cellen,ucellen,xlen,mlen;
  ub4 clas,xclas;
  void *newp;
  size_t ip = (size_t)p;
  size_t orglen,oldlen,diflen,newulen = newlen;
  bool isempty,remote;
  enum Status rv;

  xreg = findregion(hb,ip);
  if (unlikely(xreg == nil)) {
    if (p == (void *)&zeroblock) {
      if (zeroblock != 0) { error(hb->errmsg,Lreal,"written to malloc(0) block (%x)",zeroblock) }
      p = yal_heap(hb,newlen,0,Lreal);
      vg_mem_undef(p,newlen)
      return p;
    }

#if Bumplen
    mheap *mhb;
    mhb = getminiheap(0);
    mlen = find_mini(mhb,ip,0);
    if (mlen) {
      oldlen = mlen;
      if (newlen <= oldlen) return p;
      return real_copy(hb,p,oldlen,newlen);
    }
#endif

#if Yal_inter_thread_free
    xreg = real_remote_region(hb,ip);
    if (xreg) {
      typ = xreg->typ;
      if (typ == Rslab) {
        reg = (region *)xreg;
        cellen = reg->cellen;
        ucellen = reg->ucellen;
        oldlen = cellen; // gross, rounded up
        orglen = ucellen;
      } else if (typ == Rmmap) {
        oldlen = xreg->len;
        orglen = oldlen - xreg->len;
      } else if ( (mlen = findmini_rem(ip)) ) {
         oldlen = orglen = mlen;
      } else return nil;
      if (newlen <= oldlen) return p;
      return real_copy(hb,p,orglen,newlen);
      return newp;
    }
#endif
    findregion_rep(Lreal,hb,ip);
    hb->status = St_error;
    error(hb->errmsg,Lreal,"realloc(%p,%zu) was not malloc()ed",p,newlen)
    return nil;
  } else remote = 0; // reg == nil

  typ = xreg->typ;

  if (likely(typ == Rslab)) {
    reg = (region *)xreg;
    cellen = reg->cellen;
    oldlen = cellen; // gross, rounded up

#if Yal_enable_valgrind
    if (vg_mem_isnoaccess(p,reg->ucellen)) { error(hb->errmsg,Lreal,"realloc(%p) was not allocated earlier",p) return nil; }
#endif

    // smaller, copy over if worth
    if (newlen <= oldlen) {
      ystats(reg->stats.reallocles)
      vg_mem_undef( (char *)p + reg->ucellen,oldlen - reg->ucellen)
      if (unlikely(newlen == oldlen)) return p;
      if (oldlen - newlen < 256 || (newlen >= 0x1000 && oldlen * 2 >= newlen)) return p; // not worth to copy over
      if (unlikely(remote == 1)) return p;

      // search smaller class
      clas = reg->clas;
      xclas = clas;
      while (clas--) {
        xlen = hb->claslens[clas];
        if (xlen >= newlen) xclas = clas;
        else if (xlen && xlen < newlen) break;
      }
      if (xclas == clas) return p;
      clasregs = hb->clasregs + clas * Clasregs;
      nreg = *clasregs;
      if  (nreg == nil) return p;
      newp = yal_heap(hb,newlen,0,Lreal);
      vg_mem_undef(newp,newlen)
      ylog(Lreal,"realloc-slab(%p,%zu) from %zu",p,newlen,oldlen);
      newp = real_copy(hb,p,oldlen,newlen);
      isempty = slab_bin(hb,reg,ip); // put old in recycling bin
      Bist_del(hb,reg,ip,Lreal)
      if (isempty) {}
      return newp;
    }

    // larger
    if (likely(newlen < hb->mmap_threshold)) {
      if (reg->hasrun) {
        rv = slab_real(hb,reg,ip,cellen,(ub4)newlen);
        if (unlikely(rv == St_error)) return nil;
        else if (rv == St_ok) {
          Bist_del(hb,reg,ip,Lreal)
          Bist_add(hb,reg,p,newlen,Lreal)
          return p; // enough space
        }
      }
    }

    // - anticipate some headroom
    ystats(reg->stats.reallocgts)
    if (newlen <= 128) newlen = doalign(newlen * 2,16u);
    else if (likely(newlen <= hi32)) {
      newlen += newlen >> 3;
    }

    ylog(Lreal,"realloc-slab(%p,%zu) from %u.%zu",p,newlen,reg->ucellen,oldlen);
    newp = real_copy(hb,p,reg->ucellen,newlen);
    if (likely(remote == 0)) {
      Bist_del(hb,reg,ip,Lreal)
      isempty = slab_bin(hb,reg,ip); // put old in recycling bin
    }
    return newp;

  } else if (typ == Rbuddy) { // not yet
    return buddy_realloc(hb,(region *)xreg,p,newlen);

  } else if (typ == Rmmap) {
    if ( (size_t)p & (Pagesize - 1)) { error(hb->errmsg,Lreal,"realloc: invalid ptr %p",p) return nil; }
    oldlen = xreg->len;
    orglen = oldlen - xreg->len;

#if Yal_enable_valgrind
    if (vg_mem_isnoaccess(p,orglen)) { error(hb->errmsg,Freal,"realloc(%p) was not allocated earlier",p) return nil; }
#endif

    newlen = doalign(newlen,Pagesize);
    vg_mem_undef( (char *)p + xreg->len,orglen - xreg->len)

    if (newlen <= oldlen) { // smaller
      if (newlen == oldlen) return p;
      diflen = oldlen - newlen;
      vg_mem_noaccess( (char *)p + newlen,diflen)
      if (diflen <= 65536) {
        return p; // not worth the savings
      }
      return p; // todo shrink
    } else if (likely(newlen <= hi32)) {
      newlen += (newlen >> 4);
    }
    ylog(Lreal,"realloc-mmap(%p,%zu) from %zu",p,newlen,orglen)
    newp = real_mmap(hb,xreg,p,orglen,newlen,newulen,remote);
    return newp;
  } else return nil;
}

// main realloc(). nil ptr and nil newlen already covered
static void *yrealloc(void *p,size_t newlen)
{
  heap *hb;
  void *np;

  ytrace(Lreal,"<%p len %zu",p,newlen)
  hb = getheap();

  if (likely(hb != nil)) {
    np = yreal_heap(hb,p,newlen);
    ytrace(Lreal,">%p len %zu",np,newlen)
    return np;
  }
  error(hb->errmsg,Lreal,"realloc(%p) failed",p)
  return nil;
}
