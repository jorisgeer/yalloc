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
  vg_mem_def(np,oldlen)
}

static void *real_mmap(heap *hb,bool local,mpregion *reg,void *p,size_t orglen,size_t newlen,size_t newulen)
{
  void *np = osmremap(p,orglen,newlen);
  size_t aip = reg->user + reg->align;

  if (np == nil) {
    return nil;
  }

  if (np != p) {
    if (local) {
      setregion(hb,(xregion *)reg,aip,Pagesize,0,Lreal,Fln); // remove old
    } else {
      setgregion(hb,(xregion *)reg,aip,Pagesize,0);
     }
  }
  if (np != p || newlen < orglen) { vg_mem_noaccess(p,orglen) }

  reg->len = newlen;
  reg->ulen = newulen;
  reg->user = (size_t)np;
  if (local) setregion(hb,(xregion *)reg,(size_t)np,Pagesize,1,Lreal,Fln);
  vg_mem_undef(np,newlen)
  vg_mem_def(np,orglen)

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
  ycheck(nil,Lreal,xreg == nil,"nil reg for %zx len %zu %u",ip,alen,local);
  typ = xreg->typ;

  if (newulen <= alen) { // shrink will fit

    if (likely(typ == Rslab)) {
      reg = (region *)xreg;
      if (newulen < 8192 && newulen * 2 > alen) {
        if (local && alen > Cel_nolen) slab_setlen(reg,pi->cel,(ub4)newulen);
        return p; // not worth
      }
      if (local) reg->stat.reallocles++;

      np = alloc_heap(hd,hb,newlen,newulen,Lreal,tag);
      if (unlikely(np == nil)) return (void *)__LINE__;
      real_copy(p,np,newulen);
      if (likely(local != 0)) {
        flen = slab_frecel(hb,reg,pi->cel,reg->cellen,reg->celcnt,Fln); // put old in recycling bin
        if (unlikely(flen == 0)) return (void *)__LINE__;
      }
      else if (slab_free_remote(hd,reg,ip,0,Lreal) == 0) return (void *)__LINE__;
      return np;

    } else if (typ == Rmmap) {
      if (newulen < Hi20 && newulen + (newulen >> 2) > ulen) return p; // not worth
      if (newulen >= (1ul << Mmap_threshold) / 2) {
        np = real_mmap(hb,local,(mpregion *)xreg,p,ulen,newlen,newulen); // typically mremap
        ystats(hb->stat.mreallocles)
        return np;
      }
      np = alloc_heap(hd,hb,newlen,newulen,Lreal,Fln);
      if (unlikely(np == nil)) return (void *)__LINE__;
      real_copy(p,np,newulen);
      free_mmap(hd,(mpregion *)xreg,ip,ulen,Lreal,Fln);
      return np;
    } else if (typ == Rbump) return p;
    else if (typ == Rmini) return p;
    else return (void *)__LINE__;

  } else { // expand

    newlen = max(doalign8(newulen,Stdalign),64);
    if (newlen < 64) newlen *= 2;
    else newlen += (newlen >> 2); // * 1.25

    if (likely(typ == Rslab)) {
      reg = (region *)xreg;
      np = alloc_heap(hd,hb,newlen,newulen,Lreal,Fln);
      if (unlikely(np == nil)) return (void *)__LINE__;
      real_copy(p,np,ulen);
      if (likely(local != 0)) {
        ystats(reg->stat.reallocgts)
        flen = slab_frecel(hb,reg,pi->cel,reg->cellen,reg->celcnt,Fln); // put old in recycling bin
        if (unlikely(flen == 0)) return (void *)__LINE__;
      } else if (slab_free_remote(hd,reg,ip,0,Lreal | Lremote) == 0) return (void *)__LINE__;
      return np;

    } else if (typ == Rbump || typ == Rmini) {
      ydbg3(Lreal,"len %lu",newlen);
      np = alloc_heap(hd,hb,newlen,newulen,Lreal,Fln);
      if (unlikely(np == nil)) return (void *)__LINE__;
      real_copy(p,np,alen);
      return np;

    } else if (typ == Rmmap) {
      newlen = max(newlen,Pagesize);
      ydbg3(Lreal,"reg %u",xreg->id);
      np = real_mmap(hb,local,(mpregion *)xreg,p,alen,newlen,newulen);
      ystats(hb->stat.mreallocgts)
      return np;
    } else return (void *)__LINE__;
  } // shrink or expand
}

// main realloc().
static void *yrealloc(void *p,size_t newlen,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lreal);
  heap *hb;
  void *np;
  size_t alen;
  ub4 fln;
  struct ptrinfo pi;
  ub4 from;
  bool didcas;

  ypush(hd,Fln)

  if (unlikely(p == nil)) { // realloc(nil,n) = malloc(n)
    np = yal_heapdesc(hd,newlen,newlen,Lreal,tag);
    ytrace(Lreal,"- realloc(nil,%zu) = %p",newlen,np)
    return np;
  }

  // realloc(p,nil) = free(p) - deprecated since c17. see https://open-std.org/JTC1/SC22/WG14/www/docs/n2396.htm#dr_400
  if (unlikely(newlen == 0)) {
    yfree_heap(hd,p,0,nil,Lreal,tag);
    np = (void *)zeroblock;
    ytrace(Lreal,"- realloc(nil,%zu) = %p",newlen,np)
    return np;
  }

  memset(&pi,0,sizeof(pi));

  // find original block and its length
  alen = yfree_heap(hd,p,Nolen,&pi,Lsize,tag);
  if (unlikely(alen == Nolen)) { // not found. Note nil ptr already covered
    return (void *)__LINE__;
  }
  ytrace(Lreal,"+ %p len %zu -> %zu tag %.01u",p,alen,newlen,tag)

  if (unlikely(alen == 0)) { // zero-sized block
    ytrace(Lreal,"<%p len 0 -> %zu",p,newlen)
    newlen = doalign8(newlen,Stdalign);
    return yal_heapdesc(hd,newlen,newlen,Lreal,tag);
  }

  // get or lock heap
  hb = hd->hb;
  if (unlikely(hb == nil)) {
    hb = hd->hb = heap_new(hd,Lreal,Fln);
  } else {
    from = 0;
    didcas = Cas(hb->lock,from,1);
    if (unlikely(didcas == 0)) {
      hb = heap_new(hd,Lreal,Fln);
    }
  }
  if (hb == nil) return oom(Fln,Lreal,newlen,0);

  np = real_heap(hd,hb,p,alen,newlen,&pi,tag);
  ytrace(Lreal,"- realloc(%zx,%zu) = %zx",(size_t)p,newlen,(size_t)np)

  // hb = hd->hb;

  from = 1;
  didcas = Cas(hb->lock,from,0);
  ycheck(nil,Lreal,didcas == 0,"heap %u lock %u",hb->id,from)

  if (likely(np >= (void *)Pagesize)) {
    ytrace(Lreal,"- %p len %zu",np,newlen)
    ypopall(hd)
    return np;
  }
  fln = (ub4)(size_t)np;
  fln |= (Logfile << 16);
  error2(Lreal,fln,"realloc(%p,%zu) failed",p,newlen)
  ypopall(hd)
  return nil;
}
#undef Logfile
