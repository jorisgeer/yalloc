/* alloc.h - malloc() toplevel

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Large blocks are served directly by mmap(2) or equivalent, wrapped in a region for free(3) to find.
  Small blocks are served by fixed-sized slab
  A recycling bin for the latter serves as a cache, forming a fast path
*/

#define Logfile Falloc

// large blocks. For align > 0, len is already adjusted
static mpregion *yal_mmap(heapdesc *hd,heap *hb,size_t len,size_t align,enum Loc loc,ub4 fln)
{
  size_t ip,aip,alen,rlen;
  void *p;
  mpregion *reg;
  ub4 from;
  bool didcas;

  ycheck(nil,Lalloc,len == 0,"heap %u.%u empty type mmap region",hd->id,hb->id)

  if (unlikely(len >= Vmsize)) {
    oom(fln,loc,len,0);
    return nil;
  }

  alen = doalign8(len,Pagesize);

  reg = newmpregion(hb,len);
  if (reg == nil) return oom(fln,loc,len,0);
  rlen = reg->len;

  from = 2;
  didcas = Cas(reg->set,from,1);
  if (didcas == 0) {
    error(loc,"mmap region %u.%u len %zu is not free %u",reg->hid,reg->id,rlen,from);
    return nil;
  }

  if (rlen) { // reused
    ip = reg->user;
    ytrace(loc,"len %zu mmap = %zx seq %zu",len,ip,hb->stat.mapallocs)
    reg->typ = Rmmap;
    aip = align ? doalign8(ip,align) : ip;
    reg->align = aip - ip;
    reg->ulen = len;
    setregion(hb,(xregion *)reg,aip,Pagesize,1,loc,Fln);
    ystats(hb->stat.mapallocs)
    return reg;
  }

  // Atomset(hb->lock,0,Morel); // todo syscall not under lock

  p = osmem(Fln,hb->id,len,"alloc > mmap_max");
  if (p == nil) return nil;
  ip = (size_t)p;
  aip = align ? doalign8(ip,align) : ip;

  vg_mem_noaccess(p,alen)
  vg_mem_undef((vid *)aip,len - align)

#if 0 // todo see above
  zero = 0;
  didcas = Cas(hb->lock,zero,1); // try to relock
  if (didcas == 0) {
    hb = heap_new(hd,loc,Fln);
    if (hb == nil) return nil;
    hb->hd = hb;
  }
#endif

  reg->len = alen;
  reg->ulen = len;
  reg->align = aip - ip;

  reg->user = ip;

  reg->typ = Rmmap;

  ytrace(loc,"len %zu mmap = %zx seq %zu",len,ip,hb->stat.mapallocs)
  ydbg2(loc,"region %2u.%-2u len %7zu mmap = %zx %u",reg->hid,reg->id,len,ip,Atomget(reg->set,Moacq))
  setregion(hb,(xregion *)reg,aip,Pagesize,1,loc,Fln); // only start needed.
  ystats(hb->stat.mapallocs)
  return reg;
}

// small size classes
static const ub1 smalclas[64] = {
  0,0,0,1,
  1,2,2,2,
  2,3,3,3,
  3,4,4,4, // 15

  4,5,5,5,
  5,5,5,5,
  5,6,6,6,
  6,6,6,6, // 31

  6,7,7,7,
  7,7,7,7,
  7,7,7,7,
  7,7,7,7, // 47

  7,8,8,8,
  8,8,8,8,
  8,8,8,8,
  8,8,8,8 }; // 63

static const ub1 smalalen[64] = {
  2,2,2,4,
  4,8,8,8,
  8,16,16,16,
  16,16,16,16, // 15

  16,24,24,24,
  24,24,24,24,
  24,32,32,32,
  32,32,32,32, // 31

  32,48,48,48,
  48,48,48,48,
  48,48,48,48,
  48,48,48,48, // 47

  48,64,64,64,
  64,64,64,64,
  64,64,64,64,
  64,64,64,64 }; // 63

/* determine size class, then popularity of that class.
 unpopular small requests go to a bump allocator
 unpopular large requests go to mmap
 popular requests up to Mmap_max go to fixed-size slabs
 zero len covered
   malloc / calloc - ulen = reqlen = as requested by user
   realloc - ulen is as requested by user, reqlen is as wanted, e.g. +25%
   aligned_alloc - reqlen is as requested by user, ulen = alignment
 */
static Hot void *alloc_heap(heapdesc *hd,heap *hb,size_t reqlen,size_t ulen,enum Loc loc,ub4 tag)
{
  ub4 alen,clen,len,align;
  void *p;
  size_t ip,aip;
  region *reg,**clasregs,**nxclasregs;
  mpregion *xreg;
  ub4 clas,nxclas,nx;
  ub4 msk;
  ub4 pos,nxpos,claseq;
  ub4 clasmsk;
  ub4 clascnt,threshold;
  ub4 ord,cord;
  ub4 iter;
  bool movbas;

  Atomad(hb->ticker,1,Monone);

  if (reqlen < 64) { // small
    len = (ub4)reqlen;
    clas = smalclas[len];
    alen = smalalen[len];
    ord = clas;

    ydbg3(Lnone,"clas %2u for len %5u ord %u",clas,len,ord);
    ycheck(nil,loc,alen < len,"alen %u len %u",alen,len)

  } else if (likely(reqlen <= Hi30)) { // normal. Above mmap_max still counting popularity
    len = (ub4)reqlen;
    if (len & (len - 1)) { // 0000_0000_1ccx_xxxx  use 2 'cc' bits for 4 size classes per power of two and round 'xxx...' up
      ord = 32 - clz(len);
      cord = ord - 3;
      align = 1u << cord;
      alen = doalign4(len,align);
      clen = (alen >> cord) & 3;
      if (clen == 0) clas = ord + 4; // if clen == 0, we can use clas = ctz(len) as below
      else clas = Maxclass + ord * 3 + clen + 4;
      ydbg3(Lnone,"clas %2u for len %5x ord %u.%u align %x alen %x clen %x",clas,len,ord,cord,align,alen,clen);
    } else { // pwr2
      ord = ctz(len);
      clas = ord + 4; // smal uses 8 and covers 6
      alen = len;
      ydbg3(Lnone,"clas %2u for len %5u ord %u",clas,len,ord);
    }

  } else { // reqlen >= Hi30
    ord = sizeof(size_t) * 8 - clzl(reqlen);
    if (unlikely(ord >= Vmbits)) return nil;
    clas = ord + 4;
    len = alen = (1u << Mmap_max_threshold);
    ydbg3(Lnone,"clas %2u for len %5zu` ord %u",clas,reqlen,ord);
  }

  ycheck(nil,loc,clas >= Xclascnt,"class %u for len  %u out of range %u",clas,alen,Xclascnt)

  clascnt = hb->clascnts[clas];

#if Yal_enable_check
  if (clascnt == 0) {
    hb->claslens[clas] = alen;
    ydbg3(Lnone,"clas %2u for len %5u` ord %u",clas,len,ord);
  }
  else if (hb->claslens[clas]  != alen) { error(loc,"reqlen %zu clas %u alen %u vs %u %zu",reqlen,clas,alen,hb->claslens[clas],ulen) return nil; }
#endif

  hb->clascnts[clas] = (clascnt & Hi31) + 1;

  // mmap ?
  if (unlikely(alen >= mmap_limit)) {
    movbas = (loc == Lallocal && ulen > Pagesize); // aligned_alloc(big,big)
    if (clascnt <= Xclas_threshold || alen >= mmap_max_limit || movbas) {
      xreg = yal_mmap(hd,hb,reqlen,movbas ? ulen : 0,loc,Fln);
      if (xreg == nil) return  nil;
      ip = xreg->user;
      if (movbas) {
        aip = ip + xreg->align;
        ytrace(loc,"-mallocal(%zu`,%zu`) mmap = %zx,%zx seq %zu tag %.01u",reqlen,ulen,ip,aip,hb->stat.mapallocs,tag)
        return (void *)aip;
      } else {
        ytrace(loc,"-malloc(%zu`) mmap = %zx seq %zu tag %.01u",reqlen,ip,hb->stat.mapallocs,tag)
        return (void *)ip;
      }
    }
  } // mmap thresh

  ycheck(nil,loc,clas >= Clascnt,"class %u for len  %u out of range %u",clas,alen,Clascnt)

  // bump ?
  threshold = max(Clas_threshold >> (ord / 2),3);
  if (unlikely(clascnt < threshold && len < min(Bumplen,Bumpmax) )) { // size class not popular yet
    p = bump_alloc(hb,len,(ub4)ulen,loc,tag);
    if (likely(p != nil)) {
      ytrace(Lalloc,"-malloc(%zu`) = %zx (bump) tag %.01u",loc == Lalloc ? ulen : len,(size_t)p,tag)
      return p;
    }
  }

  // regular slab
  clasregs = hb->clasregs + clas * Clasregs;
  pos = hb->claspos[clas];

  ycheck(nil,loc,pos >= 32,"clas %u pos %u",clas,pos);

  iter = Clasregs + 2;
  do {
    reg = clasregs[pos];
    if (unlikely(reg == nil)) { // need to create new

      // next class ?
      if (clascnt < threshold && clas < Clascnt - 3) { // size class not popular yet
        for (nx = 1; nx < 3; nx++) {
          nxclas = clas + nx;
          // ycheck(nil,loc,nxclas >= Clascnt,"class %u for len  %u out of range %u",nxclas,alen,Clascnt)
          if (hb->claslens[nxclas] < alen) continue;
          nxclasregs = hb->clasregs + nxclas * Clasregs;
          nxpos = hb->claspos[nxclas];
          ycheck(nil,loc,nxpos >= 32,"clas %u pos %u",clas,nxpos);
          reg = nxclasregs[nxpos];
          if (reg) { // use a larger one
            clas = nxclas;
            pos = nxpos;
            alen = reg->cellen;
            clasregs = hb->clasregs + clas * Clasregs;
            ydbg3(Lnone,"reg %.01llu clas %u use %u len %u,%u for %u",reg->uid,clas,nxclas,hb->claslens[nxclas],reg->cellen,alen);
            ycheck(nil,loc,clas != reg->clas,"region %zx %.01llu clas %u len %u vs %u %u",(size_t)reg,reg->uid,reg->clas,reg->cellen,clas,alen)
            break;
          }
        }
      } // threshold for next

      if (unlikely(reg == nil)) { // get new region
        claseq = hb->clasregcnt[clas];
        reg = newslab(hb,alen,clas,claseq);
        if (unlikely(reg == nil)) {
          xreg = osmmap(len); // fallback
          return xreg ? (void *)xreg->user : nil;
        }
        reg->claseq = claseq;
        reg->claspos = pos;
        hb->clasregcnt[clas] = (ub2)(claseq + 1);
        clasmsk = hb->clasmsk[clas];
        msk = (1u << pos);
        clasregs[pos] = reg;
        clasmsk |= msk;
        hb->clasmsk[clas] = clasmsk;
        ydbg3(loc,"reg %.01lu clas %u pos %u msk %x",reg->uid,clas,pos,clasmsk);
      } // havereg
    } // have reg or not

    ycheck(nil,loc,clas != reg->clas,"region %zx %.01llu clas %u len %u vs %u %u pos %u",(size_t)reg,reg->uid,reg->clas,reg->cellen,clas,alen,pos)
    ycheck(nil,loc,reg->cellen < alen,"region %.01llu clas %u cellen %u len %u.%u tag %.01u",reg->uid,clas,reg->cellen,alen,len,tag)

    p = slab_alloc(reg,(ub4)ulen,loc,tag);

    if (likely(p != nil)) {
      ytrace(Lalloc,"-malloc(%zu`) = %zx tag %.01u",loc == Lalloc ? ulen : len,(size_t)p,tag)
      return p;
    } // p != nil aka have space

    if (hd->status == St_error) {
      return nil;
    }

    claseq = hb->clasregcnt[clas];

    // full: try next region in class
    clasmsk = hb->clasmsk[clas];
    clasmsk &= ~(1u << pos); // disable full one
    hb->clasmsk[clas] = clasmsk;

    // pick first non-full region
    if (clasmsk == 0) {
      clasmsk = Hi32;
      hb->clasmsk[clas] = clasmsk;
    }
    pos = ctz(clasmsk);
    ydbg3(loc,"clas %u pos %u msk %x",clas,pos,clasmsk);
    if (pos >= Clasregs) {
      ydbg3(loc,"clas %u wrap pos mask %x",clas,clasmsk);
      pos = 0;
    }
    hb->claspos[clas] = (ub2)pos;
  } while (likely(--iter));
  error(loc,"class %u size %u regions exceed %u mask %x",clas,alen,claseq,clasmsk)
  hd->status = St_oom; // should never occur due to region size growth
  hd->errfln = Fln;
  return nil;
}

// nil len handled
static void *yal_heap(heapdesc *hd,heap *hb,size_t len,size_t ulen,enum Loc loc,ub4 tag)
{
  void *p;
  enum Status st;

  p = alloc_heap(hd,hb,len,ulen,loc,tag);  // Note: hb may change
  if (likely(p != nil)) return p;

  st = hd->status; hd->status = St_ok;
  error(loc,"status %d",st)
  if (st == St_error) return p;

  if (st == St_oom) {
    oom(Fln,loc,len,0);
    return osmmap(len); // fallback
  }

  return p;
}

// main entry.
// If no heap yet and small request, use mini bumpallocator
static Hot void *yal_heapdesc(heapdesc *hd,size_t len,size_t ulen,enum Loc loc,ub4 tag)
{
  heap *hb = hd->hb;
  void *p;
  size_t heaps,cheaps;
  bool didcas;
  ub4 from;
  bregion *breg;

  // trivia: malloc(0)
  if (unlikely(ulen == 0)) {
    p = (void *)zeroblock;
    ytrace(loc,"alloc 0 = %p",p)
    ystats(hd->stat.alloc0s)
    return p;
  }

  if (unlikely(hb == nil)) { // no heap yet
    if (len <= min(Minilen,Minimax)) {
      p = mini_alloc(hd,(ub4)len,(ub4)ulen,loc,tag); // initial bump allocator
      if (p) {
        ytrace(loc,"+malloc(%u) mini = %zx tag %.01u",(ub4)len,(size_t)p,tag)
        return p;
      }
    }
    didcas = 0;
  } else {
    from = 0;
    ydbg3(loc,"try heap %u",hb->id);
    didcas = Cas(hb->lock,from,1);
  }

  heaps = hd->stat.getheaps;
  hd->stat.getheaps = heaps + 1;
  heaps = hd->getheaps;
  hd->getheaps = heaps + 1;

  if (unlikely(didcas == 0)) {
    cheaps = hd->stat.nogetheaps; // true satts
    hd->stat.nogetheaps = cheaps + 1;
    cheaps = hd->nogetheaps; // local contention
    hd->nogetheaps = cheaps + 1;

    if (heaps > 100 && cheaps * 1 > heaps) { // private heap if too much contention
      hd->getheaps = hd->nogetheaps = 0;
      hb = newheap(hd,loc,Fln);
    } else {
      hb = heap_new(hd,loc,Fln);
    }
    if (hb == nil) return osmmap(len); // fallback
    ydbg3(loc,"heap %u",hb->id);

    if (unlikely(hd->minidir == 0 && hd->mhb != nil)) {
      hd->minidir = 1;
      breg = hd->mhb;
      setregion(hb,(xregion *)breg,breg->user,breg->len,1,loc,Fln); // add mini to dir
    }
    hd->hb = hb;
  } else {
    ydbg3(loc,"locked heap %u",hb->id);
  } // heap or not

  ypush(hd,Fln)
  ytrace(loc,"+malloc(%zu`)   tag %.01u",loc == Lalloc ? ulen : len,tag)

  heaps = hd->stat.getheaps;
  hd->stat.getheaps = heaps + 1;

  p = yal_heap(hd,hb,len,ulen,loc,tag); // regular
  hb = hd->hb;  // hb may have changed

  from = 1;
  didcas = Cas(hb->lock,from,0);
  ycheck(nil,loc,didcas == 0,"heap %u lock %u",hb->id,from)

  ypopall(hd)
  ycheck(nil,loc,p == nil,"p nil for len %zu",len)
  return p;
}

static void *yalloc(size_t len,size_t ulen,enum Loc loc,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lalloc);

  return yal_heapdesc(hd,len,ulen,loc,tag);
}

// in contrast with c11, any size and pwr2 alignment is accepted
static void *yalloc_align(size_t align, size_t len)
{
  heapdesc *hd = getheapdesc(Lalloc);
  void *p;
  ub4 align4;
  size_t alen,align2;
  size_t ip,aip;

  if (unlikely(len <= 8))  {
    align4 = smalalen[len];
  } else align4 = Stdalign;

  if (align <= align4) return yal_heapdesc(hd,len,len,Lalloc,Fln); // no adjustment needed
  ytrace(Lallocal,"Alloc %zu`b @%zu",len,align);

  align2 = align & (align - 1); // pwr2 ?
  if (unlikely(align2 != 0)) {
    Einval
    return nil;
  }
  if (unlikely(len == 0)) {
    ystats(hd->stat.alloc0s)
    return (void *)zeroblock;
  }

  ypush(hd,Fln)
  alen = max(len,align);

  if (unlikely(alen >= mmap_limit && align > Pagesize)) { // mmap
    len += align; // mmap base is not more than page aligned
    ytrace(Lallocal,"Alloc %zu`b @%zu",len,align);
  }

  p = yal_heapdesc(hd,len,align,Lallocal,Fln);
  ypopall(hd)

#if Yal_enable_check
  if (p == nil) return p;
  ip = (size_t)p;
  aip = doalign8(ip,align);
  ycheck(nil,Lallocal,ip != aip,"p %zx vs %zx",ip,aip);
#endif

  return p;
}
#undef Logfile
