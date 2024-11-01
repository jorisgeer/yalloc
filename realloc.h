/* realloc.h - realloc() toplevel

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   use malloc_usable_size to get size of original, then either leave as-is, shrink or expand
   calls free and alloc internals for the actual steps.
   resizing mmap blocks are done directly, benefitting from mremap on systems that have it
*/

#define Logfile Frealloc

// copy org part
static void real_copy(void *p,void *np,size_t oldlen)
{
  vg_mem_def(np,oldlen)
  vg_mem_def(p,oldlen)
  memcpy(np,p,oldlen);
}

// may be called from remote
static void *real_mmap(heap *hb,bool local,mpregion *reg,size_t orglen,size_t newlen,size_t newulen)
{
  void *np;
  size_t ip = reg->user;
  size_t align = reg->align;

  // remove old
  if (align) {
    if (local) setregion(hb,(xregion *)reg,ip + align,Pagesize,0,Lreal,Fln);
    else setgregion(hb,(xregion *)reg,ip + align,Pagesize,0,Lreal,Fln);
  }
  if (local) setregion(hb,(xregion *)reg,ip,Pagesize,0,Lreal,Fln);
  else setgregion(hb,(xregion *)reg,ip,Pagesize,0,Lreal,Fln);

  vg_mem_noaccess(ip,reg->len)
  np = osmremap((void *)ip,reg->len,orglen,newlen);
  if (np == nil) return nil;
  vg_mem_def(np,newulen)

  reg->len = newlen;
  reg->ulen = newulen;
  reg->user = (size_t)np;
  if (local) setregion(hb,(xregion *)reg,(size_t)np,Pagesize,1,Lreal,Fln); // add new
  else setgregion(hb,(xregion *)reg,(size_t)np,Pagesize,1,Lreal,Fln);

  return np;
}

// main realloc(). nil ptr and nil newlen already covered
static void *real_heap(heapdesc *hd,heap *hb,void *p,size_t alen,size_t newulen, struct ptrinfo *pi,ub4 tag)
{
  xregion *xreg;
  region *reg;
  enum Rtype typ;
  void *np;
  size_t ip = (size_t)p;
  size_t ulen,flen,newlen = newulen;
  bool local;

  xreg = pi->reg;
  local = pi->local;
  ulen = pi->len;

  ycheck( (void *)__LINE__,Lreal,xreg == nil,"realloc(%zx,%zu) nil region",ip,newulen)

  vg_mem_def(xreg,sizeof(xregion))
  typ = xreg->typ;
  ycheck( (void *)__LINE__,Lreal,typ == Rnone,"realloc(%zx,%zu)  region %u has type none",ip,newulen,xreg->id)

  if (typ == Rmmap) alen += ((mpregion  *)xreg)->align;

  if (newulen <= alen) { // shrink will fit

    if (likely(typ == Rslab)) {
      reg = (region *)xreg;
      openreg(reg)
      if (newulen < 8192 && newulen * 2 > alen) {
        if (local && alen > Cel_nolen) slab_setlen(reg,pi->cel,(ub4)newulen);
        closereg(reg)
        return p; // not worth
      }
      if (local) reg->stat.reallocles++;

      np = alloc_heap(hd,hb,newlen,newulen,1,Lreal,tag);
      if (unlikely(np == nil)) return (void *)__LINE__;
      openreg(reg) // newregion may close it
      real_copy(p,np,newulen);

      if (likely(local != 0)) {
        flen = slab_frecel(hb,reg,pi->cel,reg->cellen,reg->celcnt,Fln); // put old in recycling bin
        if (likely(flen != 0)) {
          closereg(reg)
          return np;
        }
        hd->stat.invalid_frees++;
        error(Lreal,"invalid free(%zx) tag %.01u",ip,tag)
        return (void *)__LINE__;
      } else {
        flen = slab_free_rheap(hd,hb,reg,ip,tag,Lreal);
        if (likely(flen != 0)) {
          closereg(reg)
          return np;
        }
        hd->stat.invalid_frees++;
        error(Lreal,"invalid free(%zx) tag %.01u",ip,tag)
        return (void *)__LINE__;
      }

    } else if (typ == Rmmap) {
      if (newulen < Hi20 && newulen + (newulen >> 2) > ulen) return p; // not worth
      if (newulen >= (1ul << Mmap_threshold) / 2) {
        np = real_mmap(hb,local,(mpregion *)xreg,ulen,newlen,newulen); // typically mremap
        ystats(hb->stat.mreallocles)
        return np;
      }
      np = alloc_heap(hd,hb,newlen,newulen,1,Lreal,Fln);
      if (unlikely(np == nil)) return (void *)__LINE__;
      real_copy(p,np,newulen);
      free_mmap(hd,local ? hb : nil,(mpregion *)xreg,ip,ulen,Lreal,Fln);
      return np;
    } else if (typ == Rbump) return p;
    else if (typ == Rmini) return p;
    // else if (typ == Rnone) return p; // todo check
    else return (void *)__LINE__;

  } else { // expand

    newlen = max(doalign8(newulen,Stdalign),64);
    if (newlen < 64) newlen *= 2;
    else newlen += (newlen >> 2); // * 1.25

    if (likely(typ == Rslab || typ == Rnone)) {
      np = alloc_heap(hd,hb,newlen,newulen,1,Lreal,tag);
      if (unlikely(np == nil)) return (void *)__LINE__;
      if (unlikely(alen == 0)) return np;
      reg = (region *)xreg;
      openreg(reg)
      real_copy(p,np,ulen);

      if (likely(local != 0)) {
        ystats(reg->stat.reallocgts)
        flen = slab_frecel(hb,reg,pi->cel,reg->cellen,reg->celcnt,tag); // put old in recycling bin
        if (likely(flen != 0)) {
          closereg(reg)
          return np;
        }
        hd->stat.invalid_frees++;
        error(Lreal,"invalid free(%zx) tag %.01u",ip,tag)
        return (void *)__LINE__;
      } else {
        flen = slab_free_rheap(hd,hb,reg,ip,tag,Lreal);
        if (likely(flen != 0)) {
          closereg(reg)
          return np;
        }
        hd->stat.invalid_frees++;
        error(Lreal,"invalid free(%zx) tag %.01u",ip,tag)
        return (void *)__LINE__;
      }

    } else if (typ == Rbump || typ == Rmini) {
      ydbg1(Fln,Lreal,"len %lu",newlen);
      np = alloc_heap(hd,hb,newlen,newulen,1,Lreal,tag);
      if (unlikely(np == nil)) return (void *)__LINE__;
      if (likely(alen != 0)) real_copy(p,np,alen);
      return np;

    } else if (typ == Rmmap) {
      newlen = max(newlen,Pagesize);
      ydbg3(Lreal,"reg %u",xreg->id);
      np = real_mmap(hb,local,(mpregion *)xreg,alen,newlen,newulen);
      ystats(hb->stat.mreallocgts)
      return np;
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

  ypush(hd,Fln)

  ytrace(0,hd,Lreal,"+ realloc(%zx,%zu) tag %.01u",(size_t)p,newlen,tag)

  if (unlikely(p == nil)) { // realloc(nil,n) = malloc(n)
    np = yal_heapdesc(hd,newlen,newlen,1,Lreal,tag);
    ytrace(0,hd,Lreal,"- realloc(nil,%zu) = %zx",newlen,(size_t)np)
    return np;
  }

  // realloc(p,0) = free(p) - deprecated since c17. see https://open-std.org/JTC1/SC22/WG14/www/docs/n2396.htm#dr_400
  if (unlikely(newlen == 0)) {
    yfree_heap(hd,p,0,nil,Lreal,tag);
    np = (void *)zeroblock;
    ytrace(0,hd,Lreal,"- realloc(nil,%zu) = %p",newlen,np)
    return np;
  }

  if (unlikely(oldlen && oldlen != Nolen)) { // extension: size of org passed
    np = yal_heapdesc(hd,newlen,newlen,1,Lreal,tag);
    if (np == nil) return nil;
    real_copy(p,np,min(oldlen,newlen));
    ytrace(0,hd,Lreal,"- realloc(%zx,%zu) = %zx",(size_t)p,newlen,(size_t)np)
    // todo free orig
    return np;
  }

  // get or lock heap
  hb = hd->hb;
  if (unlikely(hb == nil)) {
    hb = hd->hb = heap_new(hd,Lreal,Fln);
  } else {
    from = 0;
    didcas = Cas(hb->lock,from,1);
    if (unlikely(didcas == 0)) {
      hb = hd->hb = heap_new(hd,Lreal,Fln);
    } else {
      vg_drd_wlock_acq(hb)
      // Atomset(hb->locfln,Fln,Morel);
    }
  }
  if (hb == nil) return oom(Fln,Lreal,newlen,0);

  memset(&pi,0,sizeof(pi));

  // find original block and its length
  alen = free_heap(hd,hb,p,Nolen,&pi,Lsize,Fln,tag);
  if (unlikely(alen == Nolen)) { // not found. Note nil ptr already covered
    Atomset(hb->lock,0,Morel);
    vg_drd_wlock_rel(hb)
    return (void *)__LINE__;
  }
  ytrace(1,hd,Lsize," %p len %zu -> %zu tag %.01u",p,pi.len,newlen,tag)

  if (likely(alen != 0)) {
    np = real_heap(hd,hb,p,alen,newlen,&pi,tag);
  } else {
    np = alloc_heap(hd,hb,doalign8(newlen,Stdalign),newlen,1,Lreal,tag);
  }

  //coverity[pass_freed_arg]
  ytrace(0,hd,Lreal,"- realloc(%zx,%zu) from %zu = %zx",(size_t)p,newlen,alen,(size_t)np)

  Atomset(hb->lock,0,Morel);
  vg_drd_wlock_rel(hb)

  ip = (size_t)np;

  if (likely( (ub4)ip >= Pagesize)) {
    ytrace(1,hd,Lreal,"- %p len %zu",np,newlen)
    return np;
  }
  fln = (ub4)(size_t)np;
  fln |= (Logfile << 16);
  error2(Lreal,fln,"realloc(%p,%zu) failed",p,newlen)
  return nil;
}
#undef Logfile
