/* free.h - free() toplevel

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   Lookup metadata, and pass on to slab or mmap region.free()
   malloc_usable_size is integrated and performs the first step
*/

#define Logfile Ffree

struct ptrinfo { // malloc_usable_size
  xregion *reg;
  size_t len;
  ub4 cel;
  ub4 local;
};

// free large block
static enum Status free_mmap(heapdesc *hd,mpregion *reg,size_t ap,size_t ulen,enum Loc loc,ub4 fln)
{
  size_t len = reg->len;
  size_t ip,base = reg->user;
  size_t align = reg->align;
  ub4 from;
  bool didcas;

  ydbg3(loc,"reg %2u.%-2u len %7zu mmap = %zx %u",reg->hid,reg->id,len,base,Atomget(reg->set,Moacq))

  from = 1;
  didcas = Cas(reg->set,from,2);
  if (didcas == 0) {
    errorctx(fln,Lfree,"heap %u expected %u got %u",hd->id,1,from)
    free2(Fln,Lfree,(xregion *)reg,ap,len,0,"mmap");
    return St_error;
  }

  if (ulen && ulen != len) { error(loc,"free(%zx,%zu) from %u mmap block had size %zu",ap,ulen,hd->id,len) return St_error; }

  ip = base;
  if (align) {
    ip += align;
    if (ap != base + align) { error(loc,"free(%zx) in mmap block %zx aligned at %zx",ap,base,ip) return St_error; }
  }

  if  (ip >= base + len) {
    error(loc,"free(%zx) is %zu` after mmap region %u %zx .. %zx of len %zu`",ip,ip - base - len,reg->id,base,base + len,len)
    return St_error;
  }

  if (ip & (Pagesize - 1)) { error(loc,"free-mmap(%zx) is not page %u aligned",ip,Pagesize) return St_error; }

  ytrace(loc,"ptr-%zx len %zu mmap",ip,len)

  reg->age = 1; // start aging

  return St_ok;
}

// Mark empty regions for reuse, and free after a certain 'time'
static bool free_trim(heapdesc *hd,heap *hb,ub4 slabeffort,ub4 mapeffort)
{
  region *reg,*startreg,*xreg,*nxreg,**clasregs;
  mpregion *mreg,*mpstartreg,*mpxreg,*mpnxreg;
  ub4 hid;
  ub4 rid;
  ub4 order;
  ub4 claspos,claseq;
  ub4 clas;
  bool isempty;
  ub4 rbincnt;
  ub4 age,aged;
  ub4 set;
  ub4 clasmsk;
  ub4 iter,rbpos=0,i;
  ub4 zero,from;
  bool didcas;
  size_t base;
  size_t bases[Trim_scan];
  ub4 *metas[Trim_scan];
  size_t lens[Trim_scan];
  size_t metalens[Trim_scan];
  ub4 *ages = Trim_ages;
  static ub4 effort_ages[] = { 2,3,4 };

  if (unlikely(hb == nil)) { error(Lnone,"nil heap for trim effort %u,%u",slabeffort,mapeffort) return 1; }
  hid = hb->id;
  ydbg2(Lnone,"heap %u trim effort %u,%u",hid,slabeffort,mapeffort)

  memset(metalens,0,sizeof(metalens));
  memset(lens,0,sizeof(lens));

  if (unlikely(slabeffort == 3)) ages = effort_ages;
  else if (unlikely(slabeffort == 2)) ages = Trim_Ages;

 if (slabeffort) {

  reg = startreg = hb->regtrim;

  iter = Trim_scan;

  while (likely(reg != nil)) {
    ycheck(1,Lfree,reg->typ != Rslab,"region %u typ %s",reg->id,regnames[reg->typ])
    if (reg == startreg && iter < Trim_scan) break;
    if (--iter == 0) break;
    nxreg = reg->nxt;
    if (nxreg == nil) nxreg = hb->reglst; // loop

    age = reg->age;
    aged = reg->aged;
    if (likely(age == 0 || aged == 3)) { reg = nxreg; continue; } // not empty or already aged

    if (age == 1) { // to check
      isempty = (reg->rbinpos + reg->binpos == reg->inipos); // quick pessimistic assessment
      if (isempty == 0) {
        rbincnt = slab_rbincnt(reg);
        isempty = (reg->binpos + rbincnt == reg->inipos); // verify
      }
      if (isempty) reg->age = 2; // next time aging starts

      reg = nxreg;
      continue;
    }

    reg->age = age + 1;
    rid = reg->id;

    if (aged == 0 && age >= ages[0]) { // arrange for recycling
      ydbg2(Lfree,"recycle slab region %u",rid);
      hb->mrufrereg = &dummyreg;
      order = reg->order;
      ycheck(1,Lnone,order > Regorder,"region %u order %u",rid,order)
      xreg = hb->freeregs[order];
      hb->freeregs[order] = reg;
      reg->frenxt = xreg;
      clas = reg->clas;
      claspos = reg->claspos;
      clasregs = hb->clasregs + clas * Clasregs;
      xreg = clasregs[claspos];
      ycheck(1,Lnone,xreg != reg,"empty region %u vs %u clas %u pos %u",reg->id,xreg ? xreg->id : 0,clas,claspos)

      clasregs[claspos] = nil; // remove from list
      clasmsk = hb->clasmsk[clas];
      clasmsk &= ~(1u << claspos);
      if (claspos == hb->claspos[clas]) { // active ?
        claspos = clasmsk ? ctz(clasmsk) : 0;
       ycheck(1,0,claspos >= 32,"reg %u clas %u pos %u",xreg->id,clas,claspos);
        hb->claspos[clas] = (ub2)claspos;
      }
      ydbg3(Lnone,"reg %.01lu clas %u pos %u msk %x",reg->uid,clas,claspos,clasmsk);
      hb->clasmsk[clas] = clasmsk;
      claseq = hb->clasregcnt[clas];
      if (claseq) hb->clasregcnt[clas] = (ub2)(claseq - 1);
      reg->aged = 1;
    }

    if (age >= ages[1] && aged == 1) { // remove from dir
      ydbg2(Lfree,"undir slab region %u",rid);
      ycheck(1,Lnone,reg->aged < 1,"region %.01llu age %u not  recycling %u",reg->uid,age,reg->aged)
      setregion(hb,(xregion *)reg,reg->user,reg->len,0,Lfree,Fln);
      reg->aged = 2;
    }

    if (age >= ages[2] && aged == 2) { // trim : delete user and meta
      ydbg2(Lfree,"trim slab region %u",rid);
      ycheck(1,Lnone,reg->aged < 2,"region %.01llu age %u not  recycling %u",reg->uid,age,reg->aged)
      zero = 0;
      didcas = Cas(reg->refcnt,zero,Hi24); // other threads may still try double free
      if (didcas) {
        bases[rbpos] = reg->user;
        metas[rbpos] = reg->meta;
        lens[rbpos] = reg->len;
        metalens[rbpos++] = reg->metalen;
        reg->len = reg->metalen = 0;
        reg->aged = 3;
      }
    }

    reg = nxreg;
  } // each reg

  hb->regtrim = reg ? reg : hb->reglst;
  hb->stat.delregions += rbpos;
 } // slabeffort

  /*mmap regions
    age 0 - never used
            1 - just freed
  */

  if (likely(mapeffort == 1)) ages = Trim_ages;
  else if (mapeffort == 2) ages = Trim_Ages;
  else ages = effort_ages;

 if (mapeffort) {

  mreg = mpstartreg = hb->mpregtrim;

  iter = Trim_scan;

  while (likely(mreg != nil)) {
    rid = mreg->id;
    if (mreg == mpstartreg && iter < Trim_scan) {
      break;
    }
    if (--iter == 0) break;
    mpnxreg = mreg->nxt;
    if (mpnxreg == nil) mpnxreg = hb->mpreglst; // loop

    age = mreg->age;
    aged = mreg->aged;
    if (age == 0 || aged == 3) { mreg = mpnxreg; continue; } // unavailable or aged

    ycheck(1,Lfree,mreg->typ != Rmmap,"region %u typ %s",mreg->id,regnames[mreg->typ])

    if (age == 1) { // not empty ?
      set = Atomget(mreg->set,Moacq);
      ydbg2(Lfree,"region %u set %u",rid,set)
      if (set == 1) { mreg = mpnxreg; continue; } // allocated
      if (set != 2) { error(Lfree,"region %u set %u",rid,set); return 1; }
    }

    mreg->age = age + 1;
    base = mreg->user;

    if (aged ==  0 && age >= ages[0]) { // arrange for recycling, add to freelist
      if (mreg->align) { // simplify reuse
        setregion(hb,(xregion *)mreg,base + mreg->align,Pagesize,0,Lfree,Fln);
        mreg->align = 0;
      }
      ydbg2(Lfree,"recycle mmap region %u",rid);
      hb->mrufrereg = &dummyreg;
      order = mreg->order;
      ycheck(1,Lnone,order >=Vmbits - Mmap_threshold,"region %u order %u",rid,order)
      mpxreg = hb->freempregs[order];
      hb->freempregs[order] = mreg;
      mreg->frenxt = mpxreg;
      ycheck(1,Lnone,mpxreg == mreg,"empty region %u vs %u",rid,mpxreg->id)
      mreg->aged = 1;
    }

    if (aged == 1 && age >= ages[1]) { // undir
      ydbg2(Lfree,"undir mmap region %u",rid);
      setregion(hb,(xregion *)mreg,base,Pagesize,0,Lfree,Fln);
      mreg->aged = 2;
    }

    if (aged == 2 && age >= ages[2]) { // trim
      ydbg2(Lfree,"trim mmap region %u",rid);
      from = 2;
      didcas = Cas(mreg->set,from,0);
      if (didcas == 0) {
        error(Lfree,"mmap region %u.%u set %u",hid,rid,from)
        return 1;
      }
      bases[rbpos] = base;
      lens[rbpos++] = mreg->len;
      hb->stat.delmpregions++;
      mreg->len = 0;
      // mreg->typ = Rnone;
      mreg->aged = 3;
    }

    mreg = mpnxreg;
  } // for each mreg

  hb->mpregtrim = mreg ? mreg : hb->mpreglst;
 } //mapeffort

  from = 1;
  didcas = Cas(hb->lock,from,0); // unlock
  ycheck(0,Lfree,didcas == 0,"heap %u lock %u",hb->id,from)

  for (i = 0; i < rbpos; i++) {
    if (lens[i]) osunmem(Fln,hd,(void *)bases[i],lens[i],"trim");
    //coverity[uninit_use_in_call]
    if (metalens[i]) osunmem(Fln,hd,metas[i],metalens[i],"trim");
  }

  return 0;
}

// returns len if found, zero if not, Nolen if cancelled
static size_t free_remote_slab(heapdesc *hd,heap *hb,xregion *reg,size_t ip,size_t reqlen,ub4 tag,enum Loc loc)
{
  region *creg;
  ub4 len;
  ub4 refcnt;

  creg = (region *)reg;

  refcnt = Atomad(creg->refcnt,1,Moacqrel); // prevent trim from recycling
  if (unlikely(refcnt > Hi20)) return 0; // is recycled and must be empty

  len = slab_free_remote(hd,hb,creg,ip,reqlen,tag,loc);
  Atomsub(creg->refcnt,1,Moacqrel);
  return len ? len : Nolen;
}

/* First, find region. If not found, check remote heaps
   For slab, put in bin.
   For mmap, directly delete
   If reqlen == Nolen, return usaable size only and stores info
   else if reqlen > 0, check with actual size
   returns available i.e. allocated size, typically rounded up from requested or Nolen if not found
   If hb is not nil, it is locked
  */
static Hot size_t free_heap(heapdesc *hd,heap *hb,void *p,size_t reqlen,struct ptrinfo *pi,enum Loc loc,ub4 fln,ub4 tag)
{
  size_t ulen,alen,rlen,ip = (size_t)p;
  size_t x8;
  heap *xhb;
  region *creg;
  mpregion *mpreg;
  bregion *mhb;
  xregion *xreg,*reg;
  enum Rtype typ;
  ub4 cel,cellen,celcnt,bincnt,len4;
  ub4 clas,claspos;
  ub4 clasmsk;
  ub4 i;
  char buf[256];
  enum Status rv;
  ub4 xpct;
  bool didcas,reglocked;

  // common: regular local heap
  if (likely(hb != nil)) {
//    reg = hb->mrufrereg; // start trying last used region todo reenable ?
//    if (ip < reg->user || ip >= reg->user + reg->len) {
      reg = findregion(hb,ip,loc); // need to search page dir
//    }
  } else reg = nil;
  // nil hb

  if (unlikely(reg == nil)) {

    ytrace(loc,"free(%zx) tag %.01u",ip,tag)

    // empty block ?
    if (unlikely(p == nil)) {
      ystats(hd->stat.freenils)
      return 0;
    }
    if (unlikely(p == (void *)zeroblock)) {
      x8 = 0;
      for (i = 0; i < 8; i++) x8 |= zeroarea[i];
      if (unlikely(x8 != 0)) error(loc,"written to malloc(0) block (%p) = %zx",p,x8)
      ytrace(loc,"free(%zx) len 0",ip)
      ystats(hd->stat.free0s)
      return 0;
    }

    if (unlikely(ip >= Vmsize)) { // oversized
      error(loc,"invalid free(%zx) above max %u bits VM",ip,Vmbits)
      return Nolen;
    }

    // mini
    if (hd->mhb) {
      mhb = hd->mhb;
      xpct = 0; didcas = Cas(mhb->lock,xpct,1);
      if (likely(didcas != 0)) {
        vg_drd_wlock_acq(reg);
        if (ip >= mhb->user && ip < mhb->user + mhb->len) {
          alen = bump_free(hd,nil,mhb,ip,reqlen,tag,loc);
          xpct = 1; Cas(mhb->lock,xpct,0); vg_drd_wlock_rel(reg);

          if (unlikely(reqlen == Nolen)) { // get_usable_size()
            pi->reg = (xregion *)mhb; // todo pi unused
            pi->len = alen ? alen : Nolen;
            pi->local = 1;
          }
          return alen ? alen : Nolen;
        }
      }
    }

    // remote ?
    ydbg3(loc,"+free(%zx) from heap %u",ip,hd->id)

    reg = findgregion(loc,ip); // locate ptr in global directory

    if (unlikely(reg == nil)) {
      hd->stat.invalid_frees++;
      xreg = region_near(ip,buf,255);
      if (xreg) errorctx(fln,loc,"%s region %u.%u %s",regname(xreg),xreg->hid,xreg->id,buf);
      error2(loc,Fln,"ptr %zx unallocated - not in any heap tag %.01u",ip,tag)
      return Nolen;
    }

    xpct = 0; reglocked = Cas(reg->lock,xpct,1); vg_drd_wlock_acq(reg);

    typ = reg->typ;
    xhb = reg->hb;
    ycheck(Nolen,loc,hb && xhb == hb,"xhb %u == hb for %s region %u",xhb ? xhb->id : 0,regnames[typ],reg->id);

    if (xhb == nil || typ == Rbump || typ == Rmini) { // mini or bump
      alen = bump_free(hd,nil,(bregion *)reg,ip,reqlen,tag,loc);
      if (unlikely(reqlen == Nolen)) { // get_usable_size()
        pi->reg = reg;
        pi->len = alen ? alen : Nolen;;
        pi->local = 0;
      }
      xpct = 1; Cas(reg->lock,xpct,0); vg_drd_wlock_rel(reg);
      return alen ? alen : Nolen;
    }

    if (typ == Rslab && reqlen != Nolen) { // try twice to lock owner heap
      xpct = 0;
      didcas = Cas(xhb->lock,xpct,1);
      if (didcas == 0) {
        xpct = 0;
        Pause
        didcas = Cas(xhb->lock,xpct,1);
      }
      if (didcas == 0) { // cannot obtain owner heap, free remote
        if (reglocked) {
          xpct = 1; Cas(reg->lock,xpct,0);
        }
        alen = free_remote_slab(hd,hb,reg,ip,reqlen,tag,loc | Lremote);

        if (likely(alen != 0)) return alen;

        hd->stat.invalid_frees++;
        xreg = region_near(ip,buf,255);
        if (xreg) errorctx(fln,loc,"%s region %u.%u %s",regname(xreg),xreg->hid,xreg->id,buf);
        error2(loc,Fln,"cannot free ptr %zx tag %.01u",ip,tag)
        return Nolen;
      } // locked remote heap
      xpct = 1; Cas(reg->lock,xpct,0);

      // local free from owner heap
      if (hb && hb != xhb && hd->locked) { // release orig
        xpct = 1;
        didcas = Cas(hb->lock,xpct,0);
        ycheck(Nolen,loc,didcas == 0,"heap %u lock %u",hb->id,xpct)
      }
      hb = hd->hb = xhb;
      hd->locked = 1;

#if Yal_enable_check
      xreg = findregion(hb,ip,loc);
      if (xreg != reg) {
        error(loc,"region %u.%u versus %u.%u for ptr %zx",reg->hid,reg->id,xreg ? xreg->hid : 0,xreg ? xreg->id : 0,ip)
        return Nolen;
      }
#endif

    } // slab

  } // local reg nil or not

  // local possibly from remote above, or mmap

  typ = reg->typ;

  ytrace(loc,"free(%zx) %s tag %.01u",ip,regnames[typ],tag)

  if (likely(typ == Rslab)) {
    creg = (region *)reg;
    // ycheck(0, Lnone,reg->hid != hb->id,"region %u in heap %u",creg->hid,hb->id)
    cellen = creg->cellen;
    celcnt = creg->celcnt;
    if (unlikely(reqlen != 0)) {
      if (reqlen == Nolen) { // get_usable_size()
        cel = slab_cel(creg,ip,cellen,celcnt,loc);
        if (unlikely(cel == Nocel)) return Nolen;
        if (unlikely(slab_chkfree(creg,cel) != 1)) {
          error(loc,"ptr %zx is not allocated",ip);
          return Nolen;
        }
        ulen = cellen <= Cel_nolen ? cellen : slab_getlen(creg,cel,cellen);
        ycheck(Nolen,loc,ulen == 0,"region %u cel %u ulen %zu/%u",creg->id,cel,ulen,cellen)
        ycheck(Nolen,loc,ulen > cellen,"region %u cel %u ulen %zu above %u",creg->id,cel,ulen,cellen)
        pi->reg = reg;
        pi->cel = cel;
        pi->len = ulen;
        pi->local = 1;
        return cellen;
      }
      if (reqlen > Hi32) {
        error(loc,"free_sized(%zx,%zx) has len %u",ip,reqlen,cellen)
        return Nolen;
      }
      len4 = (ub4)reqlen;
    } else len4 = 0; // reqlen

    ycheck(Nolen,loc,hb == nil,"nil heap for %u",hd->id)

    ytrace(loc,"ptr+%zx len %u %2zu",ip,cellen,creg->stat.frees)

    bincnt = slab_free(hb,creg,ip,len4,cellen,celcnt,tag); // put in recycling bin
    if (unlikely(bincnt == 0)) {
      ytrace(loc,"ptr+%zx len %u %2zu",ip,cellen,creg->stat.frees)
      return 0; // error
    }
    if (unlikely(bincnt == 1) && creg->inipos == celcnt) { // was probably full
      clas = creg->clas;
      claspos = creg->claspos;
      ycheck(Nolen,loc,claspos >= 32,"reg %u clas %u pos %u",creg->id,clas,claspos);
      clasmsk = hb->clasmsk[clas];
      clasmsk |= (1u << claspos); // re-include in alloc candidate list
      ydbg3(loc,"reg %.01lu clas %u pos %u msk %x",creg->uid,clas,claspos,clasmsk);
      hb->clasmsk[clas] = clasmsk;
      hb->claspos[clas] = (ub2)claspos;
    }
    hb->mrufrereg = reg;
    return creg->cellen;
  } // local slab

  if (typ == Rmmap) {
    mpreg = (mpregion *)reg;
    rlen = reg->len;
    if (unlikely(reqlen == Nolen)) {
      xpct = Atomget(mpreg->set,Moacq); // check free2
      if (xpct != 1) {
        errorctx(fln,loc,"expected 1, found %u",xpct);
        free2(Fln,loc,reg,ip,rlen,tag,"getsize");
        return Nolen;
      }
      pi->reg = reg;
      pi->len = mpreg->ulen;
      pi->local = 1;
      return rlen;
    }

    // free mmap directly
    if (hb) {
      hb->stat.mapfrees++;
      hb->stat.munmaps++;
    }
    mpreg->age = 1;
    rv = free_mmap(hd,mpreg,ip,reqlen,loc,Fln);
    if (rv == St_error) {
      ypush(hd,Fln)
      if (hb) hd->stat.invalid_frees++;
      return Nolen;
    }
    return rlen;
  } // mmap

  if (typ == Rbump || typ == Rmini) {
    len4 = bump_free(hd,hb,(bregion *)reg,ip,reqlen,tag,loc);
    if (unlikely(reqlen == Nolen)) {
      pi->reg = reg;
      pi->len = len4 ? len4 : Nolen;
      pi->local = 1;
      return len4;
    }
    return len4;
  }

  hd->stat.invalid_frees++;
  errorctx(Fln,loc,"from heap %u type %s",hd->id,regname(reg))
  error2(loc,Fln,"region %u.%u ptr %zx",reg->hid,reg->id,ip)
  return 0;
} // yfree_heap

// lock heap if present. nil ptr handled
static Hot size_t yfree_heap(heapdesc *hd,void *p,size_t reqlen,struct ptrinfo *pi,enum Loc loc,ub4 tag)
{
  size_t retlen;
  heap *hb = hd->hb;
  ub4 from;
  bool didcas = 0;
  bool totrim;
  ub4 seffort,meffort;
  size_t frees;

  // common: regular local heap
  if (likely(hb != nil)) {
    from = 0; didcas = Cas(hb->lock,from,1);
    if (unlikely(didcas == 0)) hb = nil;
  } // nil hb
  hd->locked = didcas;

  // ytrace(loc,"free(%zx) tag %.01u",(size_t)p,tag)
  retlen = free_heap(hd,hb,p,reqlen,pi,loc,Fln,tag);

  hb = hd->hb; // note: may have changed
  if (hb == nil || hd->locked == 0) {
    return retlen;
  }

  frees = hb->stat.frees;

  // trim empty regions 'periodically'
  if (likely(retlen < mmap_limit)) {
    totrim = sometimes(frees,regfree_interval);
    seffort = meffort = 1;
  } else {
    if (retlen < mmap_max_limit) {
      totrim = sometimes(frees,0xf);
      seffort = 1; meffort = 2;
    } else {
      totrim = 1;
      seffort = 0; meffort = 3;
    }
  }

  hb->stat.frees = frees + 1;

  if (likely(totrim == 0)) {

    from = 1;
    didcas = Cas(hb->lock,from,0);
    ycheck(0,loc,didcas == 0,"heap %u lock %u",hb->id,from)
    return retlen;
  }
  free_trim(hd,hb,seffort,meffort); // unlocks
  return retlen;
}

// main entry
static Hot inline void yfree(void *p,size_t len,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lfree);

  if (unlikely(p == nil)) {
    ystats(hd->stat.freenils)
    return;
  }
  ypush(hd,Fln)
  yfree_heap(hd,p,len,nil,Lfree,tag); // note hb may be nil
  ypopall(hd)
}

static size_t yal_getsize(void *p,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lsize);
  struct ptrinfo pi;
  size_t len;

  if (unlikely(p == nil)) {
    ytrace(Lsize,"size(nil) tag %.01u",tag)
    return 0;
  }
  ypush(hd,Fln)
  memset(&pi,0,sizeof(pi));
  len = yfree_heap(hd,p,Nolen,&pi,Lsize,tag);
  ypopall(hd)
  return len;
}
#undef Logfile
