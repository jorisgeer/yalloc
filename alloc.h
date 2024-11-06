/* alloc.h - malloc() toplevel

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Large blocks are served directly by mmap(2) or equivalent, wrapped in a tiny region for free() to find.
  Small blocks are served by fixed-sized slabs
  Inintially small blocks are served by a bump allocator.
  Suitable size classes are determined and their popularity used to determine where it goes
*/

#define Logfile Falloc

// large blocks. For align > Page, len is already adjusted
static mpregion *yal_mmap(heapdesc *hd,heap *hb,size_t len,size_t ulen,size_t align,enum Loc loc,ub4 fln)
{
  size_t ip,aip,alen,rlen;
  void *p;
  mpregion *reg;
  ub4 from;
  bool didcas;

  ycheck(nil,Lalloc,len < Pagesize,"heap %u.%u mmap region has len %zu",hd->id,hb->id,len)
  ycheck(nil,Lalloc,align == 0 || (align & (align - 1)) != 0 ,"heap %u.%u mmap region has align %zu",hd->id,hb->id,align)

  if (unlikely(len >= Vmsize)) {
    oom(fln,loc,len,0);
    return nil;
  }

  alen = doalign8(len,Pagesize);

  reg = newmpregion(hb,alen,loc,fln);
  if (reg == nil) return oom(fln,loc,ulen,0);
  rlen = reg->len;

  from = 2;
  didcas = Cas(reg->set,from,1);
  if (didcas == 0) {
    error(loc,"mmap region %u.%u len %zu gen %u is not free %u",reg->hid,reg->id,rlen,reg->gen,from);
    return nil;
  }

  if (rlen) { // reused
    ip = reg->user;
    reg->typ = Rmmap;
  } else {

    // Atomset(hb->lock,0,Morel); // todo syscall not under lock

    p = osmem(Fln,hb->id,len,"alloc > mmap_max");
    if (p == nil) return nil;
    ip = (size_t)p;
    reg->len = alen;
    reg->user = ip;
  }

  aip = doalign8(ip,align);

  vg_mem_noaccess(ip,alen)
  if (loc == Lcalloc) {
    vg_mem_def(aip,ulen)
    if (reg->clr) memset((void *)aip,0,ulen);
  } else {
    vg_mem_undef(aip,ulen)
  }

#if 0 // todo see above
  zero = 0;
  didcas = Cas(hb->lock,zero,1); // try to relock
  if (didcas == 0) {
    hb = heap_new(hd,loc,Fln);
    if (hb == nil) return nil;
    hb->hd = hb;
  }
#endif

  reg->ulen = len;
  reg->align = aip - ip;

  reg->typ = Rmmap;

  ydbg2(fln,loc,"region %2u.%-2u len %7zu mmap = %zx %u",reg->hid,reg->id,len,ip,Atomget(reg->set,Moacq))
  if (aip != ip) setregion(hb,(xregion *)reg,aip,Pagesize,1,loc,Fln);
  setregion(hb,(xregion *)reg,ip,Pagesize,1,loc,Fln); // only start needed.
  ystats(hb->stat.mapallocs)
  return reg;
}

/* determine size class, then popularity of that class.
 unpopular small requests go to a bump allocator
 unpopular large requests go to mmap
 popular requests up to Mmap_max go to fixed-size slabs
 zero len covered
   malloc / calloc - ulen = reqlen = as requested by user
   realloc - ulen is as requested by user, reqlen is as wanted, e.g. +25%
   aligned_alloc - large alignment and mmap already covered
 */
static Hot void *alloc_heap(heapdesc *hd,heap *hb,size_t reqlen,size_t ulen,ub4 align,enum Loc loc,ub4 tag)
{
  ub4 alen,clen,len,ulen4,clasal;
  void *p;
  size_t ip,aip;
  region *reg,**clasregs,**nxclasregs;
  mpregion *xreg;
  ub4 clas,nxclas,nx;
  ub4 pos,nxpos,claseq;
  Ub8 clasmsk,fremsk,msk;
  ub4 clascnt,threshold;
  ub4 ord,cord;
  ub4 grain = class_grain;
  ub4 iter;
  ub4 xpct;

  if (reqlen < 64) { // small
    len = (ub4)reqlen;
    clas = len2clas[len];
    alen = clas2len[clas];
    ord = clas;

    ydbg3(Lnone,"clas %2u for len %5u",clas,len);
    ycheck(nil,loc,alen < len,"alen %u len %u",alen,len)

  } else if (likely(reqlen <= Hi30)) { // normal. Above mmap_max still counting popularity
    len = (ub4)reqlen;
    if (len & (len - 1)) { // 0000_0000_1ccx_xxxx  use 2 'cc' bits for 4 size classes per power of two and round 'xxx...' up
      ord = 32 - clz(len);
      cord = ord - grain;
      clasal = 1u << cord;
      alen = doalign4(len,clasal);
      clen = (alen >> cord) & grain;
      if (clen == 0) clas = ord + Baseclass - 6; // if clen == 0, we can use clas = ctz(len) as below
      else clas = Maxclass + ord * grain + clen + 4; // VMbits - 6 + ...  ?
      ycheck(nil,loc,len < Smalclas && clas != len2clas[len],"len %u clas %u vs %u clen %u",len,clas,len2clas[len],clen)
      ydbg3(Lnone,"clas %2u for len %5x ord %u.%u align %x alen %x clen %x",clas,len,ord,cord,align,alen,clen);
    } else { // pwr2
      ord = ctz(len);
      clas = ord + Baseclass - 6; // ctz(64) = 6
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

  clascnt = hb->clascnts[clas] & Hi31;

  if (clascnt == 0) {
    hb->claslens[clas] = alen;
    hb->cfremsk[clas] = 0xfffffffful;
    ydbg2(Fln,Lnone,"clas %2u for len %5u`",clas,len);
  }
#if Yal_enable_check
  else if (hb->claslens[clas]  != alen) { error(loc,"reqlen %zu clas %u alen %u vs %u %zu",reqlen,clas,alen,hb->claslens[clas],ulen) return nil; }
#endif

  hb->clascnts[clas] = clascnt + 1;

  // mmap ?
  if (unlikely(reqlen >= mmap_limit)) {
    if (clascnt <= Xclas_threshold || alen >= mmap_max_limit) {
      // ycheck(nil,loc,align > Pagesize,"len %zu` align %u",alen,align)
      ypush(hd,Fln)
      xreg = yal_mmap(hd,hb,reqlen,ulen,align,loc,Fln);
      if (xreg == nil) return  nil;
      ip = xreg->user;
      aip = ip + xreg->align;
      if (ip == aip) { ytrace(0,hd,loc,"-malloc(%zu`) mmap = %zx seq %zu tag %.01u",ulen,ip,hb->stat.mapallocs,tag) }
      else { ytrace(0,hd,loc,"-mallocal(%zu`,%u) mmap = %zx (%zx) seq %zu tag %.01u",ulen,align,aip,ip,hb->stat.mapallocs,tag) }
      return (void *)ip;
    }
  } // mmap thresh
  ulen4 = (ub4)ulen;

  ycheck(nil,loc,clas >= Clascnt,"class %u for len  %u out of range %u",clas,alen,Clascnt)

  // bump ?
  threshold = max(Clas_threshold >> (ord / 2),3);
  if (unlikely(clascnt < threshold && len <= min(Bumplen,Bumpmax) )) { // size class not popular yet
    ypush(hd,Fln)
    p = bump_alloc(hb,len,ulen4,align,loc,tag);
    if (likely(p != nil)) {
#if Yal_enable_trace
      if (loc == Lallocal) { ytrace(0,hd,loc,"-allocal(%u,%u) = %zx (bump) tag %.01u",ulen4,align,(size_t)p,tag) }
      else if (loc == Lcalloc) { ytrace(0,hd,loc,"-calloc(%u) = %zx (bump) tag %.01u",ulen4,(size_t)p,tag) }
      else { ytrace(0,hd,loc,"-malloc(%u) = %zx (bump) tag %.01u",ulen4,(size_t)p,tag) }
#endif
      return p;
    }
  }

  // regular slab
  clasregs = hb->clasregs + clas * Clasregs;
  fremsk = hb->cfremsk[clas];

#if Yal_enable_check > 1
  Ub8 trymsk = fremsk;
  while (trymsk) {
    pos = ctzl(trymsk);
    trymsk &= ~(1ul << pos);
    reg = clasregs[pos];
    ycheck(nil,loc,reg != nil,"clas %u pos %u reg %.01llu msk %lx",clas,pos,reg->uid,fremsk)
  }
#endif

  pos = hb->claspos[clas];
  ycheck(nil,loc,pos >= Clasregs,"clas %u pos %u",clas,pos)
  ydbg2(Fln,loc,"clas %u pos %u msk %lx %lx",clas,pos,hb->clasmsk[clas],fremsk);

  iter = Clasregs * 2 + 2;
  do {
    reg = clasregs[pos];
    if (unlikely(reg == nil)) { // need to create new

      if (unlikely(len == 0)) {
        ystats(hb->stat.alloc0s)
        return zeroblock;
      }
      // next class ?
      if (clascnt < threshold && iter < 4 && clas < Clascnt - 3 && loc != Lallocal) { // size class not popular yet
        for (nx = 1; nx < 3; nx++) {
          nxclas = clas + nx;
          ycheck(nil,loc,nxclas >= Clascnt,"class %u for len  %u out of range %u",nxclas,alen,Clascnt)
          if (hb->claslens[nxclas] < alen) continue;
          nxclasregs = hb->clasregs + nxclas * Clasregs;
          nxpos = hb->claspos[nxclas];
          ycheck(nil,loc,nxpos >= Clasregs,"clas %u pos %u",clas,nxpos)
          reg = nxclasregs[nxpos];
          if (reg) { // use a larger one
            vg_mem_def(reg,sizeof(region))
            vg_mem_def(reg->meta,reg->metalen)
            ydbg2(Fln,Lnone,"reg %.01llu clas %u use %u len %u,%u for %u",reg->uid,clas,nxclas,hb->claslens[nxclas],reg->cellen,alen);
            clas = nxclas;
            pos = nxpos;
            alen = reg->cellen;
            clasregs = hb->clasregs + clas * Clasregs;
            fremsk = hb->cfremsk[clas];
            ycheck(nil,loc,clas != reg->clas,"region %zx %.01llu clas %u len %u vs %u %u",(size_t)reg,reg->uid,reg->clas,reg->cellen,clas,alen)
            ycheck(nil,loc,reg->inuse != 1,"region %zx %.01llu clas %u len %u vs %u %u",(size_t)reg,reg->uid,reg->clas,reg->cellen,clas,alen)
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
        reg->claspos = pos;
        hb->clasregcnt[clas] = (ub2)(claseq + 1);
        clasmsk = hb->clasmsk[clas];
        msk = (1ul << pos);
        clasregs[pos] = reg;
        clasmsk |= msk;
        fremsk &= ~msk;
        hb->clasmsk[clas] = clasmsk;
        hb->cfremsk[clas] = fremsk;
        ydbg2(Fln,loc,"reg %.01llu clas %u pos %u msk %lx %lx",reg->uid,clas,pos,clasmsk,fremsk);
        xpct = Atomget(reg->lock,Moacq);
        ycheck(nil,loc,xpct != 0,"new reg %u lock %u",reg->id,xpct)
        hb->smalclas[clas] = reg;
        ydbg2(Fln,loc,"reg %.01llu clas %u len %u",reg->uid,clas,len);
      }
    } else { // havereg
      vg_mem_def(reg,sizeof(region))
      vg_mem_def(reg->meta,reg->metalen)
    } // have reg or not

    ycheck(nil,loc,clas != reg->clas,"region %.01llu gen %u.%u clas %u len %u vs %u %u pos %u",reg->uid,reg->gen,reg->id,reg->clas,reg->cellen,clas,alen,pos)

    ycheck(nil,loc,reg->cellen < alen,"region %.01llu clas %u cellen %u len %u.%u tag %.01u",reg->uid,clas,reg->cellen,alen,len,tag)
    ycheck(nil,loc,reg->inuse != 1,"region %.01llu clas %u cellen %u len %u.%u tag %.01u",reg->uid,clas,reg->cellen,alen,len,tag)
    ydbg2(Fln,loc,"clas %u pos %u msk %lx",clas,pos,fremsk);

    ypush(hd,Fln)
    p = slab_alloc(hd,reg,(ub4)ulen,(ub4)align,loc,tag);

    if (likely(p != nil)) {
      ytrace(0,hd,loc,"-malloc(%zu`) = %zx tag %.01u",loc == Lalloc ? ulen : len,(size_t)p,tag)
      ydbg2(Fln,loc,"clas %u pos %u msk %lx",clas,pos,fremsk);
      vg_mem_noaccess(reg->meta,reg->metalen)
      vg_mem_noaccess(reg,sizeof(region))
      return p;
    } // p != nil aka have space

    if (hd->status == St_error) {
      return nil;
    }
    clasmsk = hb->clasmsk[clas];
    clasmsk &= ~(1ul << pos); // disable full one
    hb->clasmsk[clas] = clasmsk;
    if (clasmsk == 0) {
        if (fremsk == 0) {
         ydbg1(Fln,loc,"clas %u pos %u msk %lx",clas,pos,fremsk);
          fremsk = 0xfffffffful; // should not occur
        }
        pos = ctzl(fremsk); // new reg in empty slots
       ydbg2(Fln,loc,"clas %u pos %u msk %lx",clas,pos,fremsk);
    } else { // pick first non-full region
      pos = ctzl(clasmsk);
      ydbg2(Fln,loc,"clas %u pos %u msk %lx",clas,pos,clasmsk);
    }

    claseq = hb->clasregcnt[clas];

    if (pos >= Clasregs) {
      ydbg1(Fln,loc,"clas %u pos %u msk %lx",clas,pos,fremsk);
      ydbg3(loc,"clas %u wrap pos mask %x",clas,hb->clasmsk[clas]);
      pos = 0;
    }
    hb->claspos[clas] = (ub2)pos;
    reg = clasregs[pos];
    if (reg) {
      ycheck(nil,loc,clas != reg->clas,"region %.01llu clas %u len %u vs %u %u",reg->uid,reg->clas,reg->cellen,clas,alen)
      ycheck(nil,loc,reg->cellen < len,"region %.01llu clas %u len %u vs %u",reg->uid,clas,reg->cellen,len)
    }
    hb->smalclas[clas] = reg; // may be nil
    ydbg2(Fln,loc,"reg %.01llu clas %u len %u",reg->uid,clas,len);

  } while (likely(--iter));

  if (reg) errorctx(reg->fln,loc,"reg %u msk %lx",reg->id,fremsk)
  error2(loc,Fln,"class %u size %u regions exceed %u mask %lx,%lx",clas,alen,claseq,hb->clasmsk[clas],fremsk)
  hd->status = St_oom; // should never occur due to region size growth
  hd->errfln = Fln;
  return nil;
}

// nil len handled
static void *yal_heap(heapdesc *hd,heap *hb,size_t len,size_t ulen,ub4 align,enum Loc loc,ub4 tag)
{
  void *p;
  enum Status st;

  p = alloc_heap(hd,hb,len,ulen,align,loc,tag);  // Note: hb may change
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

// main entry for malloc, calloc, realloc, alloc_align.
// If no heap yet and small request, use mini bumpallocator
static Hot void *yal_heapdesc(heapdesc *hd,size_t len,size_t ulen,ub4 align,enum Loc loc,ub4 tag)
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
    ytrace(0,hd,loc,"alloc 0 = %zx tag %.01u",(size_t)p,tag)
    ystats(hd->stat.alloc0s)
    return p;
  }

  if (unlikely(hb == nil)) { // no heap yet
    if (len <= min(Minilen,Minimax) && align <= Minimax) {
      ypush(hd,Fln)
      p = mini_alloc(hd,(ub4)len,(ub4)ulen,align,loc,tag); // initial bump allocator
      if (p) {
        ytrace(0,hd,loc,"-malloc(%u) mini = %zx tag %.01u",(ub4)len,(size_t)p,tag)
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
    cheaps = hd->stat.nogetheaps;
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
    vg_drd_wlock_acq(hb)
    // Atomset(hb->locfln,Fln,Morel);
    ydbg3(loc,"locked heap %u",hb->id);
  } // heap or not

  ypush(hd,Fln);
  ytrace(0,hd,loc,"+malloc(%zu`)   Tag %.01u",loc == Lallocal ? len : ulen,tag)

  p = yal_heap(hd,hb,len,ulen,align,loc,tag); // regular
  // hb = hd->hb;  // hb may have changed

  Atomset(hb->lock,0,Morel);
  vg_drd_wlock_rel(hb)

  ycheck(nil,loc,p == nil,"p nil for len %zu",len)
  return p;
}

// calloc
static void *yalloc(size_t len,size_t ulen,enum Loc loc,ub4 tag)
{
  heapdesc *hd = getheapdesc(loc);
  void *p;

  p = yal_heapdesc(hd,len,ulen,1,loc,tag);

#if Yal_enable_check > 1
  if (unlikely(chkalign(p,len,Stdalign) != 0)) error(Lalloc,"alloc(%zu) = %zx not aligns",len,(size_t)p)
#endif

  return p;
}

// malloc
static void *ymalloc(size_t len,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lalloc);
  heap *hb = hd->hb;
  region *reg;
  void *p;
  ub4 clas;
  ub4 clascnt;
  ub4 from;
  bool didcas;

  if (likely(hb != nil)) { // simplified case
    from = 0; didcas = Cas(hb->lock,from,1);

    if (likely(didcas != 0)) {
      ystats(hd->stat.getheaps)
      // Atomset(hb->locfln,Fln,Morel);

      if (likely(len < Smalclas)) {
        clas = len2clas[len];
        reg = hb->smalclas[clas];
        if (likely(reg != nil)) {
          clascnt = hb->clascnts[clas] & Hi31;
          ycheck(nil,Lalloc,clascnt == 0,"clas %u count 0",clas)
          ystats(hb->clascnts[clas])
          vg_mem_def(reg,sizeof(region))
          vg_mem_def(reg->meta,reg->metalen)
          ycheck(nil,Lalloc,reg->clas != clas,"region %.01llu clas %u len %zu vs %u %u",reg->uid,clas,len,reg->clas,reg->cellen)
          ycheck(nil,Lalloc,reg->cellen < len,"region %.01llu clas %u len %u vs %zu",reg->uid,clas,reg->cellen,len)
          reg->age = 0; // todo replace with re-check at trim
          ydbg3(Fln,Lalloc,"len %zu",len)

          p = slab_malloc(reg,(ub4)len,tag);

          if (likely(p != nil)) {
#if Yal_enable_check > 1
            if (unlikely(chkalign(p,len,Stdalign) != 0)) error(Lalloc,"alloc(%zu) = %zx not aligns",len,(size_t)p)
#endif
            Atomset(hb->lock,0,Morel);
            vg_mem_noaccess(reg->meta,reg->metalen)
            vg_mem_noaccess(reg,sizeof(region))
            return p;
          }
          hb->smalclas[clas] = nil; // e.g. full
        } // havereg

        if (unlikely(len == 0)) {
          p = (void *)zeroblock;
          ytrace(0,hd,Lalloc,"alloc 0 = %zx  tag %.01u",(size_t)p,tag)
          ystats(hb->stat.alloc0s)
          Atomset(hb->lock,0,Morel);
          return p;
        }

      } // small
      p = alloc_heap(hd,hb,len,len,1,Lalloc,tag);
      Atomset(hb->lock,0,Morel);
      return p;
    } // locked
    ydbg2(Fln,Lalloc,"len %zu",len)
    ystats(hd->stat.nogetheaps)
  } // heap
  ydbg3(Fln,Lalloc,"len %zu",len)
  return yal_heapdesc(hd,len,len,1,Lalloc,tag);
}

// in contrast with c11, any size and pwr2 alignment is accepted
static void *yalloc_align(size_t align, size_t len,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lallocal);
  heap *hb;
  mpregion *reg;
  void *p;
  size_t alen,align2;
  size_t ip,aip;
  ub4 ord;
  bool didcas;
  ub4 from;

  ytrace(0,hd,Lallocal,"+ mallocal(%zu`,%zu) tag %.01u",len,align,tag);

  if (unlikely(align == 0)) align = 1;

  align2 = align & (align - 1); // pwr2 ?
  if (unlikely(align2 != 0)) {
    Einval
    return nil;
  }
  if (unlikely(align >= Vmsize / 2)) {
    Einval
    return nil;
  }

  if (unlikely(len >= mmap_limit || align >= mmap_limit / 4)) { // slab won't accomodate
    ytrace(0,hd,Lallocal,"+ mallocal(%zu`,%zu) tag %.01u",len,align,tag);
    hb = hd->hb;
    if (hb) {
      from = 0; didcas = Cas(hb->lock,from,1);
      if (didcas == 0) hb = nil;
      else {
        vg_drd_wlock_acq(hb)
        // Atomset(hb->locfln,Fln,Morel);
      }
    }
    if (hb == nil) {
      hb = heap_new(hd,Lallocal,Fln);
      if (hb == nil) return nil;
      hd->hb = hb;
    }
    if (align > Pagesize) {
      alen = len + align; // may need to move base
    } else alen = len;
    alen = max(alen,mmap_limit);
    reg = yal_mmap(hd,hb,alen,len,align,Lallocal,Fln);
    if (reg == nil) return nil;
    aip = reg->user + reg->align;
    Atomset(hb->lock,0,Morel);
    vg_drd_wlock_rel(hb)
    ytrace(0,hd,Lallocal,"-mallocal(%zu`,%zu) = %zx tag %.01u",len,align,aip,tag);
    return (void *)aip;
  } // len or align large

  // no special provisions needed
  if (unlikely(len == 0)) return nil;

  ypush(hd,Fln)

  ytrace(0,hd,Lallocal,"+ mallocal(%zu`,%zu) tag %.01u",len,align,tag);
  if (len & (len - 1)) {
    ord = 32 - clz((ub4)len);
    len = 1ul << ord;
  }
  p = yal_heapdesc(hd,len,len,(ub4)align,Lallocal,tag);

#if Yal_enable_check
  if (p == nil) return p;
  ip = (size_t)p;
  aip = doalign8(ip,align);
  ycheck(nil,Lallocal,ip != aip,"p %zx vs %zx",ip,aip)
#endif

  return p;
}
#undef Logfile
