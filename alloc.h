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
static mpregion *yal_mmap(heapdesc *hd,heap *hb,size_t len,size_t align,enum Loc loc,ub4 fln)
{
  size_t ip,aip,alen,rlen;
  void *p;
  mpregion *reg;
  ub4 from;
  bool didcas;

  ycheck(nil,loc,len < Pagesize,"heap %u.%u mmap region has len %zu",hd->id,hb->id,len)
  ycheck(nil,loc,align == 0 || (align & (align - 1)) != 0 ,"heap %u.%u mmap region has align %zu",hd->id,hb->id,align)

  if (unlikely(len >= Vmsize)) {
    oom(hb,fln,loc,len,0);
    return nil;
  }

  alen = doalign8(len,Pagesize);

  reg = newmpregion(hb,alen,loc,fln);
  if (reg == nil) return oom(hb,fln,loc,len,0);
  rlen = reg->len;

  from = 2;
  didcas = Cas(reg->set,from,1);
  if (didcas == 0) {
    error(loc,"mmap region %u.%u len %zu gen %u is not free %u",reg->hid,reg->id,rlen,reg->gen,from)
    return nil;
  }

  if (rlen) { // reused
    ip = reg->user;
    reg->typ = Rmmap;
    ycheck(nil,loc,ip < Pagesize ,"heap %u.%u mmap region of len %zu` gen %u has nil base %zx",hd->id,hb->id,rlen,reg->gen,ip)
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
    vg_mem_def(aip,len)
    if (reg->clr) memset((void *)aip,0,len);
  } else {
    vg_mem_undef(aip,len)
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
  if (loc == Lallocal) {
    ystats(hb->stat.mapAllocs)
  } else {
    ystats(hb->stat.mapallocs)
  }

  if (aip != ip) setregion(hb,(xregion *)reg,aip,Pagesize,1,loc,Fln);
  setregion(hb,(xregion *)reg,ip,Pagesize,1,loc,Fln); // only start needed.

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
static Hot void *alloc_heap(heapdesc *hd,heap *hb,size_t ulen,ub4 align,enum Loc loc,ub4 tag)
{
  ub4 alen,clen,len,ulen4,clasal;
  void *p;
  size_t ip,aip;
  region *reg,*nxreg,**clasregs,**nxclasregs;
  mpregion *xreg;
  ub4 clas,nxclas,nx;
  ub4 pos,nxpos,claseq;
  Ub8 clasmsk,fremsk,msk;
  ub4 clascnt,threshold;
  ub4 ord,cord;
  ub4 iter;
  ub4 xpct;

  if (ulen < 64) { // small
    len = (ub4)ulen;
    clas = len2clas[len];
    alen = clas2len[clas];
    ord = clas;
    clen = 4;

    ydbg2(Fln,Lnone,"clas %2u for len %5u",clas,len);
    ycheck1(nil,loc,alen < len,"alen %u len %u",alen,len)

  } else if (likely(ulen <= Hi30)) { // normal. Above mmap_max still counting popularity
    len = (ub4)ulen;

    if (len & (len - 1)) { // 0000_0000_1ccx_xxxx  use 2 'cc' bits for 4 size classes per power of two and round 'xxx...' up
      ord = 32 - clz(len);
      cord = ord - class_grain;
      clasal = 1u << cord;
      alen = doalign4(len,clasal);
      clen = (alen >> cord) & class_grain;
      if (clen == 0) clen = 4;
      clas = ord * class_grain1 + clen;
      ypush(hd,loc,Fln)
    } else { // pwr2
      ord = ctz(len);
      clas = (ord + 1) * class_grain1;
      alen = len;
      clen = 0;
      ypush(hd,loc,Fln)
    }
    clas += Baseclass - 7 * class_grain1; // smal uses 8 and covers 6 * (grain + 1)
    ydbg2(Fln,Lnone,"clas %2u for len %5u` ord %u align %u alen %u`",clas,len,ord,align,alen);
    ycheck1(nil,loc,alen < len,"clas %u aen %u len %u.%u tag %.01u",clas,alen,len,len,tag)
    ycheck1(nil,loc,len < Smalclas && clas != len2clas[len],"len %u clas %u vs %u",len,clas,len2clas[len])

  } else { // ulen >= Hi30
    ord = sizeof(size_t) * 8 - clzl(ulen);
    if (ulen & (ulen - 1)) ord++;
    if (unlikely(ord >= Vmbits)) return oom(hb,Fln,loc,ulen,0);
    clas = 31 * class_grain1 + ord - 30 + Baseclass - 7 * class_grain1;
    len = alen = (1u << Mmap_max_threshold); // unused
    clen = 0;
    ydbg2(Fln,Lnone,"clas %2u for len %5zu` ord %u",clas,ulen,ord);
  }
  ycheck(nil,loc,clas >= Xclascnt,"class %u for len  %zu` out of range %u",clas,ulen,Xclascnt)

  clascnt = hb->clascnts[clas] & Hi31;

  if (unlikely(clascnt == 0)) {
    hb->claslens[clas] = alen;
    hb->cfremsk[clas] = 0xfffffffful;
    ydbg2(Fln,Lnone,"clas %2u for len %5u`",clas,len);
  }
#if Yal_enable_check
  else if (hb->claslens[clas] != alen) { error(loc,"ulen %zu clas %u alen %u vs %u",ulen,clas,alen,hb->claslens[clas]) return nil; }
#endif

  hb->clascnts[clas] = clascnt + 1;

  // mmap ?
  if (unlikely(ulen >= mmap_limit)) {
    if (clascnt <= Xclas_threshold || alen >= mmap_max_limit) {
      // ycheck(nil,loc,align > Pagesize,"len %zu` align %u",alen,align)
      ypush(hd,loc,Fln)
      xreg = yal_mmap(hd,hb,ulen,align,loc,Fln);
      if (xreg == nil) return  nil;
      ip = xreg->user;
      aip = ip + xreg->align;
      if (ip == aip) { ytrace(0,hd,loc,tag,hb->stat.mapallocs,"-malloc(%zu`) mmap = %zx",ulen,ip) }
      else { ytrace(0,hd,loc,tag,hb->stat.mapAllocs,"-mallocal(%zu`,%u) mmap = %zx (%zx)",ulen,align,aip,ip) }
      return (void *)ip;
    }
  } // mmap thresh

  ulen4 = (ub4)ulen;
  ycheck(nil,loc,clas >= Clascnt,"class %u for len  %zu` out of range %u",clas,ulen,Clascnt)

  // bump ?
  threshold = max(Clas_threshold >> (ord / 2),3);
  if (unlikely(clascnt < threshold && len <= min(Bumplen,Bumpmax) )) { // size class not popular yet
    ypush(hd,loc,Fln)
    if (likely(loc != Lreal)) len = ulen4;
    else len = ulen4 + (ulen4 >> 2); // plus ~25% headroom
    p = bump_alloc(hd,hb,len,align,loc,tag);
    if (likely(p != nil)) {
#if Yal_enable_trace
      if (loc == Lallocal) { ytrace(0,hd,loc,tag,0,"-allocal(%u,%u) = %zx (bump)",ulen4,align,(size_t)p) }
      else { ytrace(0,hd,loc,tag,0,"-%calloc(%u) = %zx (bump)",loc == Lcalloc ? 'c' : 'm',ulen4,(size_t)p) }
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
    if (unlikely(loc == Lreal && clen > 1)) { // try headroom if available
      nxclasregs = clasregs + Clasregs;
      nxclas = clas + 1;
      nxpos = hb->claspos[nxclas];
      nxreg = nxclasregs[nxpos];
      if (nxreg && (nxreg->binpos || nxreg->inipos < nxreg->celcnt) ) { // has space
        ypush(hd,loc,Fln)
        reg = nxreg;
        ycheck(nil,loc,reg->cellen < len,"region %.01llu clas %u cellen %u len %u.%u tag %.01u",reg->uid,clas,reg->cellen,len,len,tag)
        clas = nxclas;
        pos = nxpos;
        clasregs = nxclasregs;
        alen = reg->cellen;
        fremsk = hb->cfremsk[clas];
      }
    }

    if (unlikely(reg == nil)) { // need to create new

      if (unlikely(len == 0)) {
        ystats(hb->stat.alloc0s)
        return zeroblock;
      }
      // next class ?
      if (clascnt < threshold && iter < 4 && clas < Clascnt - 4 && loc != Lallocal) { // size class not popular yet
        for (nx = 1; nx < 3; nx++) {
          nxclas = clas + nx;
          nxclasregs = hb->clasregs + nxclas * Clasregs;
          nxpos = hb->claspos[nxclas];
          ycheck(nil,loc,nxpos >= Clasregs,"clas %u pos %u",clas,nxpos)
          nxreg = nxclasregs[nxpos];
          if (nxreg && (nxreg->binpos || nxreg->inipos < nxreg->celcnt)) { // use a larger one
            reg = nxreg;
            ypush(hd,loc,Fln)
            ycheck(nil,loc,reg->cellen < len,"region %.01llu clas %u cellen %u len %u.%u tag %.01u",reg->uid,clas,reg->cellen,len,len,tag)
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
        ypush(hd,loc,Fln)
        claseq = hb->clasregcnt[clas];
        ycheck(nil,loc,alen < len,"clas %u aen %u len %u.%u tag %.01u",clas,alen,len,len,tag)
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

    ycheck(nil,loc,reg->cellen < len,"region %.01llu.%u clas %u cellen %u len %u.%zu tag %.01u",reg->uid,reg->id,clas,reg->cellen,len,ulen,tag)
    ycheck(nil,loc,reg->inuse != 1,"region %.01llu clas %u cellen %u len %u.%zu tag %.01u",reg->uid,clas,reg->cellen,len,ulen,tag)
    ydbg2(Fln,loc,"clas %u pos %u msk %lx",clas,pos,fremsk);

    ypush(hd,loc,Fln)
    p = slab_alloc(hd,reg,(ub4)ulen,(ub4)align,loc,tag);

    if (likely(p != nil)) {
      ytrace(0,hd,loc,tag,0,"-malloc(%zu`) = %zx",loc == Lalloc ? ulen : len,(size_t)p)
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
         do_ylog(Diagcode,loc,Fln,Warn,0,"clas %u pos %u msk %lx",clas,pos,fremsk);
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
static void *yal_heap(heapdesc *hd,heap *hb,size_t len,ub4 align,enum Loc loc,ub4 tag)
{
  void *p;
  enum Status st;

  p = alloc_heap(hd,hb,len,align,loc,tag);  // Note: hb may change
  if (likely(p != nil)) return p;

  st = hd->status; hd->status = St_ok;
  error(loc,"status %d",st)
  if (st == St_error) return p;

  if (st == St_oom) {
    oom(hb,Fln,loc,len,0);
    return osmmap(len); // fallback
  }

  return p;
}

// main entry for malloc, calloc, realloc, alloc_align.
// If no heap yet and small request, use mini bumpallocator
static Hot void *yal_heapdesc(heapdesc *hd,size_t len,ub4 align,enum Loc loc,ub4 tag)
{
  heap *hb = hd->hb;
  void *p;
  size_t heaps,cheaps;
  bool didcas;
  ub4 from;
  bregion *breg;
  enum Tidstate tidstate = hd->tidstate;

  // trivia: malloc(0)
  if (unlikely(len == 0)) {
    p = (void *)zeroblock;
    ytrace(0,hd,loc,tag,0,"alloc 0 = %zx",(size_t)p)
    ystats(hd->stat.alloc0s)
    return p;
  }

  if (unlikely(hb == nil)) { // no heap yet
    if (len <= min(Minilen,Minimax) && align <= Minimax) {
      ypush(hd,loc,Fln)
      p = mini_alloc(hd,(ub4)len,align,loc,tag); // initial bump allocator
      if (p) {
        ytrace(0,hd,loc,tag,0,"-malloc(%u) mini = %zx",(ub4)len,(size_t)p)
        return p;
      }
    }
    didcas = 0;
  } else {
    ydbg3(loc,"try heap %u",hb->id);

    if (tidstate == Ts_mt) {
      from = 0; didcas = Cas(hb->lock,from,1);
    } else {
      didcas = 1;
    }
    ydbg2(Fln,loc,"try heap %u cas %u",hb->id,didcas)
  } // hb or not

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

  ypush(hd,loc,Fln);
  if (loc == Lallocal) {
    ytrace(0,hd,loc,tag,0,"+malloc(%zu`)",len)
  } else {
    ytrace(0,hd,loc,tag,0,"+mallocal(%u,%zu`)",align,len)
  }
  p = yal_heap(hd,hb,len,align,loc,tag); // regular
  // hb = hd->hb;  // hb may have changed

  if (tidstate != Ts_private) {
    Atomset(hb->lock,0,Morel);
    vg_drd_wlock_rel(hb)
  }

  ycheck(nil,loc,p == nil,"p nil for len %zu",len)
  return p;
}

// calloc
static void *yalloc(size_t len,enum Loc loc,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lcalloc);
  void *p;

  ypush(hd,Lalloc | Lapi,Fln);

  p = yal_heapdesc(hd,len,1,loc,tag);

#if Yal_enable_check > 1
  if (unlikely(chkalign(p,len,Stdalign) != 0)) error(Lalloc,"alloc(%zu) = %zx not aligns",len,(size_t)p)
#endif

  ypush(hd,Lalloc | Lapi,Fln);

  return p;
}

// malloc
static void *ymalloc(size_t len,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lalloc);
  heap *hb = hd->hb;
  region *reg;
  void *p;
  ub4 clas,clascnt;
  ub4 len4;
  ub4 from;
  bool didcas;
  enum Tidstate tidstate;

  ypush(hd,Lalloc | Lapi,Fln);

  if (likely(hb != nil)) { // simplified case
    tidstate = hd->tidstate;
    if (tidstate == Ts_mt) {
      from = 0; didcas = Cas(hb->lock,from,1);
#if Yal_enable_stats > 1
      if (likely(didcas != 0)) hd->stat.getheaps++;
      else hd->stat.nogetheaps++;
#endif
    } else {
      didcas = 1;
    }

    if (likely(didcas != 0)) {
      ycheck1(nil,Lalloc,Atomget(hb->lock,Moacq) == 0,"heap %u is unlocked",hb->id)

      if (likely(len < Smalclas)) {
        len4 = (ub4)len;
        clas = len2clas[len4];
        ydbg2(Fln,Lnone,"clas %2u for len %5u tag %.01u",clas,len4,tag);
        reg = hb->smalclas[clas];
        if (likely(reg != nil)) {
          clascnt = hb->clascnts[clas] & Hi31;
          ycheck(nil,Lalloc,clascnt == 0,"clas %u count 0",clas)
          hb->clascnts[clas] = clascnt + 1;
          vg_mem_def(reg,sizeof(region))
          vg_mem_def(reg->meta,reg->metalen)
          ycheck(nil,Lalloc,reg->clas != clas,"region %.01llu clas %u len %u vs %u %u",reg->uid,clas,len4,reg->clas,reg->cellen)
          ycheck1(nil,Lalloc,reg->cellen < len,"region %.01llu clas %u len %u vs %u",reg->uid,clas,reg->cellen,len4)
          reg->age = 0; // todo replace with re-check at trim

          p = slab_malloc(reg,len4,tag);

          if (likely(p != nil)) {
#if Yal_enable_check > 1
            if (unlikely(len >= Stdalign && chkalign(p,len,Stdalign) != 0)) error(Lalloc,"alloc(%u) = %zx not aligns",len4,(size_t)p)
#endif
            if (tidstate != Ts_private) {
              Atomset(hb->lock,0,Morel);
              vg_drd_wlock_rel(hb)
            }
            vg_mem_noaccess(reg->meta,reg->metalen)
            vg_mem_noaccess(reg,sizeof(region))
            ypush(hd,Lalloc | Lapi,Fln);
            return p;
          }
          hb->smalclas[clas] = nil; // e.g. full
        } // havereg

        if (unlikely(len == 0)) {
          p = (void *)zeroblock;
          ytrace(0,hd,Lalloc,tag,0,"alloc 0 = %zx",(size_t)p)
          ystats(hb->stat.alloc0s)
          if (tidstate != Ts_private) {
            Atomset(hb->lock,0,Morel);
            vg_drd_wlock_rel(hb)
          }
          return p;
        }

      } // small

      p = alloc_heap(hd,hb,len,1,Lalloc,tag);

      if (tidstate != Ts_private) {
        Atomset(hb->lock,0,Morel);
        vg_drd_wlock_rel(hb)
      }
      ypush(hd,Lalloc | Lapi,Fln);
      return p;
    } // locked
    ydbg2(Fln,Lalloc,"len %zu",len)
  } // heap
  ydbg3(Fln,Lalloc,"len %zu",len)
  p = yal_heapdesc(hd,len,1,Lalloc,tag);
  ypush(hd,Lalloc | Lapi,Fln);
  return p;
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
  enum Tidstate tidstate = hd->tidstate;

  ypush(hd,Lallocal | Lapi,Fln)

  ytrace(0,hd,Lallocal,tag,0,"+ mallocal(%zu`,%zu)",len,align)

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
    ytrace(0,hd,Lallocal,tag,0,"+ mallocal(%zu`,%zu)",len,align)
    hb = hd->hb;
    if (hb) {
      if (tidstate == Ts_mt) {
        from = 0; didcas = Cas(hb->lock,from,1);
        if (likely(didcas != 0)) hd->stat.getheaps++;
        else {
          hd->stat.nogetheaps++;
          hb = nil;
        }
      } else {
        didcas = 1;
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
    reg = yal_mmap(hd,hb,alen,align,Lallocal,Fln);
    if (reg == nil) return nil;
    reg->ulen = len;
    aip = reg->user + reg->align;

    if (tidstate != Ts_private) {
      Atomset(hb->lock,0,Morel);
      vg_drd_wlock_rel(hb)
    }

    ytrace(0,hd,Lallocal,tag,0,"-mallocal(%zu`,%zu) = %zx",len,align,aip)
    ypush(hd,Lallocal | Lapi,Fln)
    return (void *)aip;
  } // len or align large

  // no special provisions needed
  if (unlikely(len == 0)) return nil;

  ypush(hd,Lallocal,Fln)

  ytrace(0,hd,Lallocal,tag,0,"+ mallocal(%zu`,%zu)",len,align)
  if (len & (len - 1)) {
    ord = 32 - clz((ub4)len);
    len = 1ul << ord;
  }
  p = yal_heapdesc(hd,len,(ub4)align,Lallocal,tag);

#if Yal_enable_check
  if (p == nil) return p;
  ip = (size_t)p;
  aip = doalign8(ip,align);
  ycheck(nil,Lallocal,ip != aip,"p %zx vs %zx",ip,aip)
#endif
  ypush(hd,Lallocal | Lapi,Fln)

  return p;
}
#undef Logfile
