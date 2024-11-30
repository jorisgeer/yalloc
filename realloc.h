/* realloc.h - realloc() toplevel

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   use malloc_usable_size to get size of original, then either leave as-is, shrink or expand
   calls free and alloc internals for the actual steps.
   resizing mmap blocks are done directly, benefitting from mremap on systems that have it
*/

#define Logfile Frealloc

static void real_clear(void Unused * p,size_t Unused oldlen,size_t Unused newlen)
{
#if Realloc_clear
  if (newlen > oldlen) memset(p + oldlen,0,newlen - oldlen);
  else if (newlen < oldlen) memset(p + newlen,0,oldlen - newlen);
#endif
}

// copy org part
static void real_copy(void *p,void *np,size_t ulen,size_t alen)
{
  size_t len = min(alen,ulen);

  vg_mem_def(np,len)
  vg_mem_def(p,len)
  memcpy(np,p,len);
}

static size_t real_mmap(heapdesc *hd,heap *hb,bool local,mpregion *reg,size_t newlen,size_t newulen)
{
  xregion *xreg = (xregion *)reg;
  void *np;
  size_t oldlen = reg->len;
  size_t align = reg->align;
  size_t nip,ip = reg->user;
  size_t naip,aip = ip + align;
  size_t ulen = reg->ulen;

  if (reg->real) newlen += newlen >> 3; // ~10% headroom
  newlen = doalign8(newlen,Pagesize);

  reg->real = 1;

  // c standard does not specify to preserve alignment. Code is sometimes simpler if we do.
  // orglen is gross len minus align gap
  if (align) {
    ydbg1(Fln,Lreal,"reg %u.%u align %zu remap %zu -> %zu,%zu local %u",hb->id,reg->id,align,oldlen,newlen,newulen,local)
    ycheck(0,Lreal,align & Pagesize1,"align %zu",align)

    if (local) {

      np = osmremap((void *)ip,oldlen,ulen,newlen + align);
      nip = (size_t)np;
      if (nip == 0) return 0;
      ycheck(0,Lreal,nip & Pagesize1,"mmap %zx not page aligned",nip)

      reg->len = newlen + align;
      reg->ulen = newulen;

      naip = nip + align;
      if (nip == ip) {
        vg_mem_noaccess(np,newlen + align)
        vg_mem_def((void *)naip,newulen)
        return ip;
      }

      setregion(hb,xreg,ip,Pagesize,0,Lreal,Fln);
      setregion(hb,xreg,aip,Pagesize,0,Lreal,Fln);

      setregion(hb,xreg,nip,Pagesize,1,Lreal,Fln);
      setregion(hb,xreg,naip,Pagesize,1,Lreal,Fln);

      vg_mem_noaccess((void *)nip,newlen + align)
      vg_mem_def((void *)naip,newulen)

      reg->user = nip;

      return naip;

    } else {
      np = alloc_heap(hd,hb,newlen,1,Lreal,Fln);
      nip = (size_t)np;
      if (nip == 0) return 0;

      real_copy((void *)aip,(void *)nip,newulen,ulen);
      free_mmap(hd,nil,reg,ip,0,Lreal,Fln,Fln);
      vg_mem_def((void *)nip,newulen)
      return nip;
    }
  }

  // common - no align

  ydbg2(Fln,Lreal,"reg %u.%u remap %zu -> %zu,%zu local %u",hb->id,reg->id,orglen,newlen,newulen,local)

  if (local) {
    np = osmremap((void *)ip,reg->len,ulen,newlen);
    nip = (size_t)np;
    if (nip == 0) return 0;
    reg->len = newlen;
    reg->ulen = newulen;
    if (nip == ip) {
      vg_mem_noaccess(np,newlen)
      vg_mem_def(np,newulen)
      return ip;
    }
    ycheck(0,Lreal,nip & Pagesize1,"mmap %zx not page aligned",nip)
    setregion(hb,xreg,ip,Pagesize,0,Lreal,Fln);
    setregion(hb,xreg,nip,Pagesize,1,Lreal,Fln);

  } else {
    setgregion(hb,xreg,ip,Pagesize,0,Lreal,Fln);
    np = alloc_heap(hd,hb,newlen,1,Lreal,0);
    nip = naip = (size_t)np;
    if (nip == 0) return 0;
    ycheck(0,Lreal,nip & Pagesize1,"mmap %zx not page aligned",nip)
    real_copy((void *)ip,np,newulen,ulen);
    free_mmap(hd,nil,reg,ip,oldlen,Lreal,Fln,Fln);
  }

  vg_mem_noaccess(np,newlen)
  vg_mem_def(np,newulen)

  reg->user = nip;

  return nip;
}

// main realloc(). nil ptr and nil newlen already covered
static void *real_heap(heapdesc *hd,heap *hb,void *p,size_t alen,size_t newulen, struct ptrinfo *pi,ub4 tag)
{
  xregion *xreg;
  region *reg;
  mpregion *mreg;
  enum Rtype typ;
  void *np;
  size_t aip,ip = (size_t)p;
  size_t ulen,flen,newlen = newulen;
  ub4 cellen;
  bool local;

  xreg = pi->reg;
  local = pi->local;

  ycheck( (void *)__LINE__,Lreal,xreg == nil,"realloc(%zx,%zu) nil region",ip,newulen)

  vg_mem_def(xreg,sizeof(xregion))
  typ = xreg->typ;
  ycheck( (void *)__LINE__,Lreal,typ == Rnone,"realloc(%zx,%zu)  region %u has type none",ip,newulen,xreg->id)

  if (newulen <= alen && local) { // will fit
    hb->stat.reallocles++;

    vg_mem_def(p,max(alen,newulen))
    real_clear(p,alen,newulen);

    if (likely(typ == Rslab)) {
      reg = (region *)xreg;
      openreg(reg)
      cellen = reg->cellen;
      if (alen - newulen < 32 || newulen + (newulen >> 2) > alen) { // not worth
        if (cellen > Cel_nolen) slab_setlen(reg,pi->cel,(ub4)newulen); // may be larger than ulen
        closereg(reg)
        pi->fln = Fln;
        return p;
      }

      ulen = cellen > Cel_nolen ? slab_getlen(reg,pi->cel,cellen) : cellen;

      if (ulen >= newulen) {
        if (ulen > newulen && cellen > Cel_nolen) slab_setlen(reg,pi->cel,(ub4)newulen);
        return p;
      }

      // shrink
      hb->stat.Reallocles++;
      np = alloc_heap(hd,hb,newulen,1,Lreal,tag);
      if (unlikely(np == nil)) return (void *)__LINE__;
      openreg(reg) // newregion may close it
      ydbg2(Fln,Lreal,"alen %zu ulen %zu newulen %zu",alen,ulen,newulen)
      real_copy(p,np,ulen,newulen);

      flen = slab_frecel(hb,reg,pi->cel,reg->cellen,reg->celcnt,Fln); // put old in recycling bin
      if (likely(flen != 0)) {
        closereg(reg)
        pi->fln = Fln;
        return np;
      }
      hd->stat.invalid_frees++;
      error(Lreal,"invalid free(%zx) tag %.01u",ip,tag)
      return (void *)__LINE__;

    } else if (typ == Rmmap) {
      mreg = (mpregion *)xreg;
      ulen = pi->len;
      if (mreg->align) {
        mreg->ulen = newulen;
        return p; // keep alignment
      }
      if (alen - newulen <= Pagesize || newulen + (newulen >> 3) > ulen) {
        mreg->ulen = newulen;
        mreg->real = 1;
        return p; // not worth
      }
      if (newulen >= (1ul << Mmap_threshold) / 2) {
        aip = real_mmap(hd,hb,local,mreg,newlen,newulen); // typically mremap
        ystats(hb->stat.mreallocles)
        pi->fln = Fln;
        return (void *)aip;
      }
      np = alloc_heap(hd,hb,newulen,1,Lreal,Fln);
      if (unlikely(np == nil)) return (void *)__LINE__;
      real_copy(p,np,ulen,newulen);
      free_mmap(hd,hb,mreg,ip,ulen,Lreal,Fln,tag);
      pi->fln = Fln;
      return np;
    } else if (typ == Rbump) {
      pi->fln = Fln;
      return p;
    } else if (typ == Rmini) {
      pi->fln = Fln;
      return p;
    // else if (typ == Rnone) return p; // todo check
    } else return (void *)__LINE__;

  } else { // expand or remote

    hb->stat.reallocgts++;
    newlen = doalign8(newulen,Stdalign);

    if (likely(typ == Rslab)) {
      np = alloc_heap(hd,hb,newulen,1,Lreal,tag);
      if (unlikely(np == nil)) return (void *)__LINE__;
      reg = (region *)xreg;
      openreg(reg)
      cellen = reg->cellen;
      ulen = cellen > Cel_nolen ? slab_getlen(reg,pi->cel,cellen) : cellen;
      ycheck((void *)__LINE__,Lreal,ulen == 0,"region %u cel %u ulen 0 for %u",reg->id,pi->cel,cellen)
      ycheck((void *)__LINE__,Lreal,ulen > cellen,"region %u cel %u ulen %zu above %u",reg->id,pi->cel,ulen,cellen)
      real_copy(p,np,ulen,newulen);

      if (likely(local != 0)) {
        flen = slab_frecel(hb,reg,pi->cel,reg->cellen,reg->celcnt,tag); // put old in recycling bin
        if (likely(flen != 0)) {
          closereg(reg)
          pi->fln = Fln;
          return np;
        }
        hd->stat.invalid_frees++;
        error(Lreal,"invalid free(%zx) tag %.01u",ip,tag)
        return (void *)__LINE__;
      } else {
        flen = slab_free_rheap(hd,hb,reg,ip,tag,Lreal);
        if (likely(flen != 0)) {
          closereg(reg)
          pi->fln = Fln;
          return np;
        }
        hd->stat.invalid_frees++;
        error(Lreal,"invalid free(%zx) tag %.01u",ip,tag)
        return (void *)__LINE__;
      }

    } else if (typ == Rbump || typ == Rmini) {
      ydbg2(Fln,Lreal,"len %lu",newlen);
      if (newulen <= alen) return p;
      np = alloc_heap(hd,hb,newlen,1,Lreal,tag);
      if (unlikely(np == nil)) return (void *)__LINE__;
      if (likely(alen != 0)) real_copy(p,np,alen,newulen);
      pi->fln = Fln;
      return np;

    } else if (typ == Rmmap) {
      newlen = max(newlen,Pagesize);
      ydbg3(Lreal,"reg %u",xreg->id);
      aip = real_mmap(hd,hb,local,(mpregion *)xreg,newlen,newulen);
      ystats(hb->stat.mreallocgts)
      pi->fln = Fln;
      return (void *)aip;
    } else return (void *)__LINE__;
  } // shrink or expand
}

// main realloc().
static void *yrealloc(void *p,size_t oldlen,size_t newlen,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lreal);
  heap *hb;
  void *np;
  size_t alen,ip;
  ub4 fln;
  struct ptrinfo pi;
  ub4 from;
  bool didcas;
  enum Tidstate tidstate = hd->tidstate;

  ypush(hd,Lreal | Lapi,Fln)

  ytrace(0,hd,Lreal,tag,0,"+ realloc(%zx,%zu)",(size_t)p,newlen)

  if (unlikely(p == nil)) { // realloc(nil,n) = malloc(n)
    np = yal_heapdesc(hd,newlen,1,Lreal,tag);
    ytrace(0,hd,Lreal,tag,0,"- realloc(nil,%zu) = %zx",newlen,(size_t)np)
    ypush(hd,Lreal | Lapi,Fln)
    return np;
  }

  // realloc(p,0) = free(p) - deprecated since c17. see https://open-std.org/JTC1/SC22/WG14/www/docs/n2396.htm#dr_400
  if (unlikely(newlen == 0)) {
    yfree_heap(hd,p,0,Lreal,tag);
    np = (void *)global_zeroblock;
    ytrace(0,hd,Lreal,tag,0,"- realloc(nil,%zu) = %p",newlen,np)
    ypush(hd,Lreal | Lapi,Fln)
    return np;
  }

  if (unlikely(oldlen && oldlen != Nolen)) { // extension: size of org passed
    np = yal_heapdesc(hd,newlen,1,Lreal,tag);
    if (unlikely(np == nil)) return nil;
    real_copy(p,np,oldlen,newlen);
    ytrace(0,hd,Lreal,tag,0,"- realloc(%zx,%zu) = %zx",(size_t)p,newlen,(size_t)np)
    // todo free orig
    ypush(hd,Lreal | Lapi,Fln)
    return np;
  }

  // get or lock heap
  hb = hd->hb;
  if (unlikely(hb == nil)) {
    hb = hd->hb = heap_new(hd,Lreal,Fln);
  } else {
    if (tidstate == Ts_mt) {
      from = 0; didcas = Cas(hb->lock,from,1);
#if Yal_enable_stats > 1
      if (likely(didcas != 0)) hd->stat.getheaps++;
      else hd->stat.nogetheaps++;
#endif
    } else {
      didcas = 1;
    }
    if (unlikely(didcas == 0)) {
      hb = hd->hb = heap_new(hd,Lreal,Fln);
    } else {
      vg_drd_wlock_acq(hb)
      // Atomset(hb->locfln,Fln,Morel);
    }
  }
  if (unlikely(hb == nil)) return oom(hb,Fln,Lreal,newlen,0);

  memset(&pi,0,sizeof(pi));

  // find original block and its length
  ydbg2(Fln,0,"size %p len %zu",p,newlen)
  alen = size_heap(hd,hb,(size_t)p,&pi,Lsize,Fln,tag);
  if (likely(alen != Nolen)) { // Note: nil ptr already covered
    ytrace(1,hd,Lsize,tag,0," %p len %zu -> %zu local %u",p,pi.len,newlen,pi.local)

    if (likely(alen != 0)) {
      ylostats(hb->stat.minrelen,newlen)
      yhistats(hb->stat.maxrelen,newlen)

      np = real_heap(hd,hb,p,alen,newlen,&pi,tag);

      //coverity[pass_freed_arg]
       ytrace(0,hd,Lreal,tag,0,"- realloc(%zx,%zu) from %zu = %zx loc %.01u",(size_t)p,newlen,alen,(size_t)np,pi.fln)
    } else { // from zero len

      np = alloc_heap(hd,hb,doalign8(newlen,Stdalign),1,Lreal,tag);
      real_clear(np,0,newlen);
      vg_mem_def(np,newlen)

      //coverity[pass_freed_arg]
      ytrace(0,hd,Lreal,tag,0,"- realloc(%zx,%zu) from %zu = %zx",(size_t)p,newlen,alen,(size_t)np)
    }
  } else {
    np = nil;
  } // found or not

  if (tidstate != Ts_private) {
    Atomset(hb->lock,0,Morel);
    vg_drd_wlock_rel(hb)
  }

  ip = (size_t)np;

  if (likely( (ub4)ip >= Pagesize)) {
    ytrace(1,hd,Lreal,tag,0,"- %p len %zu",np,newlen)
    ypush(hd,Lreal | Lapi,Fln)
    return np;
  }
  fln = (ub4)(size_t)np;
  fln |= (Logfile << 16);
  error2(Lreal,fln,"realloc(%zx,%zu) failed",ip,newlen)
  return nil;
}
#undef Logfile
