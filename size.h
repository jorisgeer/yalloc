/* size.h - malloc_get_size() toplevel

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   Lookup metadata, and pass on to slab or mmap region.size()
*/

#define Logfile Fsize

struct ptrinfo {
  xregion *reg;
  size_t len;
  ub4 cel; // for realloc subsequent free
  ub4 fln;
  bool local;
};

/* First, find region in page dir. If not found, check remote heaps
   For slab, get size from cel metan.
   For mmap, get size from region
   returns available i.e. allocated size, typically rounded up from requested or Nolen if not found
   If hb is not nil, it is locked beforehand
  */
static Hot size_t Nonnull(1,4) size_heap(heapdesc *hd,heap *hb,size_t ip,struct ptrinfo *pi,enum Loc loc,ub4 fln,ub4 tag)
{
  size_t ulen,alen,rlen;
  ub4 ulen4;
  region *creg;
  mpregion *mpreg;
  bregion *mhb;
  xregion *xreg,*reg;
  enum Rtype typ;
  ub4 cel,cellen,celcnt,len4;
  char buf[256];
  ub4 xpct;
  celset_t set;

  // common: regular local heap
  if (likely(hb != nil)) {
    reg = findregion(hb,ip,loc); // search page dir
  } else {
    reg = nil;
  } // nil hb

  if (unlikely(reg == nil)) {

    ytrace(1,hd,loc,tag,0,"size(%zx) tag %.01u",ip,tag)

    // empty block ?
    if (unlikely(ip == (size_t)zeroblock)) {
 #if Yal_enable_valgrind == 0
      size_t x8 = 0;
      for (ub4 i = 0; i < 8; i++) x8 |= zeroarea[i];
      if (unlikely(x8 != 0)) error(loc,"written to malloc(0) block (%zx) = %zx",ip,x8)
#endif
      ytrace(1,hd,loc,tag,0,"size(%zx) len 0",ip)
      return 0;
    }

    if (unlikely(ip >= Vmsize)) { // out of range
      error(loc,"invalid size(%zx) above max %u bits VM",ip,Vmbits)
      hd->stat.invalid_frees++;
      return Nolen;
    }
    if (unlikely(ip < Pagesize)) { // out of range
      error(loc,"invalid size(%zx) on page 0 of len %u",ip,Pagesize)
      hd->stat.invalid_frees++;
      return Nolen;
    }

    // mini ?
    if (hd->mhb) {
      mhb = hd->mhb;
      if (ip >= mhb->user && ip < mhb->user + mhb->len) {
        alen = bump_free(hd,nil,mhb,ip,Nolen,tag,loc);
        pi->reg = (xregion *)mhb; // todo unused
        pi->len = alen ? alen : Nolen;
        return pi->len;
      }
    }

    // remote ?
    ydbg3(loc,"size(%zx) from heap %u",ip,hd->id)

    reg = findgregion(loc,ip); // locate ptr in global directory

    if (unlikely(reg == nil)) {
      hd->stat.invalid_frees++;
      xreg = region_near(ip,buf,255);
      if (xreg) errorctx(fln,loc,"heap %u %s",hb ? hb->id : 0,buf)
      error2(loc,Fln,"ptr %zx unallocated - not in any heap tag %.01u",ip,tag)
      return Nolen;
    }
    pi->local = 0;
  } else {
    pi->local = 1;
  } // local or remote

  typ = reg->typ;
  if (likely(reg->typ == Rslab)) {
    creg = (region *)reg;
    vg_mem_def(creg,sizeof(region))
    vg_mem_def(creg->meta,creg->metalen)
    cellen = creg->cellen;
    celcnt = creg->celcnt;
    cel = slab_cel(creg,ip,cellen,celcnt,loc);
    if (unlikely(cel == Nocel)) return Nolen;
    set = slab_chkfree(creg,cel);
    if (unlikely(set != 1)) {
      error(loc,"ptr %zx is not allocated: %u",ip,set)
      return Nolen;
    }
    ulen4 = cellen <= Cel_nolen ? cellen : slab_getlen(creg,cel,cellen);
    ycheck(Nolen,loc,ulen4 == 0,"region %u cel %u ulen 0 for %u",creg->id,cel,cellen)
    ycheck(Nolen,loc,ulen4 > cellen,"region %u cel %u ulen %u above %u",creg->id,cel,ulen4,cellen)
    ytrace(0,hd,loc,tag,0,"size(%zx) len %u for %u",ip,cellen,ulen4)
    pi->reg = reg;
    pi->cel = cel;
    pi->len = ulen4;
    return cellen;
  }

  if (unlikely(typ == Rbump || typ == Rmini)) {
    len4 = bump_free(hd,nil,(bregion *)reg,ip,Nolen,tag,loc);
    pi->reg = reg;
    pi->len = len4 ? len4 : Nolen;
    pi->local = 0;
    return len4 ? len4 : Nolen;
  }

  // mmap
  if (unlikely(typ == Rmmap)) {
    mpreg = (mpregion *)reg;
    rlen = reg->len;
    ulen = mpreg->ulen;
    ycheck(Nolen,loc,rlen == 0,"region %u len zero",reg->id)
    ycheck(Nolen,loc,ulen > rlen,"region %u len %zu vs %zu",reg->id,ulen,rlen)
    ycheck(Nolen,loc,mpreg->align > rlen,"region %u align %zu above len %zu",reg->id,mpreg->align,rlen)
    rlen -= mpreg->align;
    xpct = Atomget(mpreg->set,Moacq); // check free2
    if (xpct != 1) {
      errorctx(fln,loc,"expected 1, found %u",xpct)
      free2(Fln,loc,reg,ip,rlen,tag,"getsize");
      return Nolen;
    }
    pi->reg = reg;
    pi->len = ulen;
    pi->local = 0;
    return rlen;
  }

  // unknown type
  hd->stat.invalid_frees++;
  errorctx(Fln,loc,"from heap %u type %s",hd->id,regname(reg))
  error2(loc,Fln,"region %u.%u ptr %zx",reg->hid,reg->id,ip)
  return Nolen;
}

// lock heap if present. nil ptr handled
static Hot Nonnull(1,2,3) size_t ysize_heap(heapdesc *hd,void *p,struct ptrinfo *pi,enum Loc loc,ub4 tag)
{
  size_t ip = (size_t)p;
  size_t retlen;
  heap *hb = hd->hb;
  ub4 from;
  bool didcas = 0;

  // common: regular local heap
  if (likely(hb != nil)) {
    from = 0; didcas = Cas(hb->lock,from,1);
    if (unlikely(didcas == 0)) hb = nil;
    else {
      vg_drd_wlock_acq(hb)
    }
  } // nil hb
  hd->locked = didcas;

  ytrace(0,hd,loc,tag,0,"+ size(%zx) tag %.01u",ip,tag)
  retlen = size_heap(hd,hb,ip,pi,loc,Fln,tag);

  if (hd->locked == 0) return retlen;

  hb = hd->hb; // note: may have changed

#if Yal_enable_check
  if (unlikely(hb == nil)) { error(loc,"size(%zx) - nil heap",ip) return retlen; }
  from = Atomget(hb->lock,Moacq);
  if (unlikely(from != 1)) { error(loc,"heap %u unlock %u",hb->id,from) return retlen; }
#endif

  Atomset(hb->lock,0,Morel);
  return retlen;
}

// main entry
static size_t ysize(void *p,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lsize);
  struct ptrinfo pi;
  size_t len;

  ytrace(0,hd,Lsize,tag,0,"+ size(%zx) tag %.01u",(size_t)p,tag)

  if (unlikely(p == nil)) {
    ytrace(0,hd,Lsize,tag,0,"size(nil) tag %.01u",tag)
    return 0;
  }
  ypush(hd,Fln)
  pi.len = 0;
  pi.local = 0;
  len = ysize_heap(hd,p,&pi,Lsize,tag);

  ytrace(0,hd,Lsize,tag,0,"- size(%zx) = %zu for %zu tag %.01u",(size_t)p,len,pi.len,tag)
  return len;
}
#undef Logfile