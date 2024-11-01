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
static enum Status free_mmap(heapdesc *hd,heap *hb,mpregion *reg,size_t ap,size_t ulen,enum Loc loc,ub4 fln)
{
  mpregion *preg;
  size_t len = reg->len;
  size_t aip,ip = reg->user;
  size_t align = reg->align;
  ub4 order = reg->order;
  ub4 from;
  bool didcas;

  ydbg3(loc,"reg %2u.%-2u len %7zu mmap = %zx %u",reg->hid,reg->id,len,ip,Atomget(reg->set,Moacq))

  ytrace(0,hd,loc,"free(%zx) len %zu` mmap",ap,len)

  from = 1;
  didcas = Cas(reg->set,from,2);
  if (didcas == 0) {
    errorctx(fln,Lfree,"heap %u expected %u got %u",hd->id,1,from)
    free2(Fln,Lfree,(xregion *)reg,ap,len,0,"mmap");
    return St_error;
  }

  if (ulen) {
    if (doalign8(ulen,Pagesize) != len) { error(loc,"free(%zx,%zu) from %u mmap block had size %zu",ap,ulen,hd->id,len) return St_error; }
  }
  if (align) {
    ycheck(St_error,loc,align & Pagesize1,"mmap region %u.%u align %zu",reg->hid,reg->id,align)
    aip = ip + align;
    if (ap < aip) { error(loc,"free(%zx) is %zu bytes before a %zu` mmap block %zx aligned at %zx",ap,aip - ap,len,ip,align) return St_error; } // possible user error
    if (ap > aip) { error(loc,"free(%zx) is %zu bytes within a %zu` mmap region %zx aligned at %zx",ap,ap - aip,len,ip,align) return St_error; } // possible user error
  } else aip = ip;

  if  (aip >= ip + len) {
    error(loc,"free(%zx) is %zu` after mmap region %u %zx .. %zx of len %zu`",aip,aip - ip - len,reg->id,ip,ip + len,len)
    return St_error;
  }

  ytrace(1,hd,loc,"ptr-%zx len %zu mmap",ip,len)

  if (len >= Mmap_retainlimit) { // release directly
    ydbg1(Fln,loc,"unmap region %u",reg->id)
    if (hb) {
      setregion(hb,(xregion *)reg,ip,Pagesize,0,loc,Fln);
      if (align) setregion(hb,(xregion *)reg,aip,Pagesize,0,loc,Fln);
      preg = hb->freemp0regs;
      hb->freemp0regs = reg;
      reg->frenxt = preg;
      reg->freprv = nil;
      if (preg) preg->freprv = reg;
    } else {
      setgregion(hb,(xregion *)reg,ip,Pagesize,0,loc,Fln);
      if (align) setgregion(hb,(xregion *)reg,aip,Pagesize,0,loc,Fln);
    }
    osmunmap((void *)ip,len);
    hd->stat.munmaps++;
    reg->len = 0;
    return St_ok;
  }

  if (hb) { // allow direct recycling
    ydbg2(fln,loc,"free mmap region %u.%u len %zu`",reg->hid,reg->id,len)
    setregion(hb,(xregion *)reg,ip,Pagesize,0,Lfree,Fln);
    if (align) setregion(hb,(xregion *)reg,ip + align,Pagesize,0,Lfree,Fln);
    reg->align = 0;

    preg = hb->freempregs[order - Mmap_threshold];
    hb->freempregs[order - Mmap_threshold] = reg;
    reg->frenxt = preg;
    reg->freprv = nil;
    if (preg) preg->freprv = reg;

    hb->stat.trimregions[5]++;
    Atomset(reg->age,2,Morel);
    reg->aged = 1;
  } else Atomset(reg->age,1,Morel); // start ageing

  return St_ok;
}

// Mark empty regions for reuse, and free after a certain 'time'
static bool free_trim(heapdesc *hd,heap *hb,ub4 tick)
{
  region *reg,*startreg,*xreg,*nxreg,*nreg,*preg,**clasregs;
  mpregion *mreg,*mpstartreg,*mpnxreg,*nmreg,*pmreg;
  ub4 hid;
  ub4 rid;
  ub8 uid;
  ub4 order;
  ub4 claspos,claseq;
  ub4 clas;
  bool isempty,small;
  ub4 age,aged,lim;
  ub4 set;
  Ub8 clasmsk,msk;
  ub4 iter,rbpos=0,i;
  yalstats *sp = &hb->stat;
  ub4 curregs;
  ub4 from;
  ub4 ref;
  bool didcas;
  size_t base;
  size_t bases[Trim_scan+1];
  ub4 *metas[Trim_scan+1];
  size_t lens[Trim_scan+1];
  size_t metalens[Trim_scan+1];
  ub4 *ages;
  static ub4 effort_ages[] = { 2,3,4 };

  hid = hb->id;

  // trim empty regions 'periodically'
  if (sometimes(tick,0xffff)) ages = effort_ages;
  else ages = Trim_ages;

  openregs(hb)

  reg = startreg = hb->regtrim;

  iter = Trim_scan;

  while (likely(reg != nil)) {
    ycheck(1,Lfree,reg->typ != Rslab,"region %u typ %s",reg->id,regnames[reg->typ])
    if (reg == startreg && iter < Trim_scan) break; // full ircle
    if (iter == 0) break;
    iter--;
    nxreg = reg->nxt;
    if (nxreg == nil) nxreg = hb->reglst; // loop

    age = reg->age;
    aged = reg->aged;
    if (likely(age == 0 || aged == 3)) { reg = nxreg; continue; } // not empty or already aged

    from = 0; didcas = Cas(reg->lock,from,1);
    if (didcas == 0) { reg = nxreg; continue; }
    vg_drd_wlock_acq(reg)

    uid = reg->uid;
    rid = reg->id;

    if (age == 1) { // to check
      isempty = (reg->rbinpos + reg->binpos == reg->inipos);
      if (isempty) {
        ref = Atomget(reg->remref,Moacq);
        ycheck(1,Lfree,ref != 0,"reg %.01llu ref %u",uid,ref)
        hb->stat.trimregions[0]++;
        reg->age = 2; // next time ageing starts
      }
      from = 1; didcas = Cas(reg->lock,from,0);
      vg_drd_wlock_rel(reg)
      reg = nxreg;
      continue;
    }

    ref = Atomget(reg->remref,Moacq);
    ycheck(1,Lfree,ref != 0,"reg %.01llu ref %u",uid,ref)

    reg->age = age + 1;
    order = reg->order;

    if (aged == 0 && age >= ages[0]) { // arrange for recycling
      ydbg2(Fln,Lfree,"recycle slab region %.01llu gen %u.%u.%u len %zu ",uid,reg->gen,hid,rid,reg->len);
      if (reg == hb->mrureg) hb->mrulen = 0;

      setregion(hb,(xregion *)reg,reg->user,reg->len,0,Lfree,Fln);

      ycheck(nil,0,reg->inuse == 0,"region %.01llu not in use",reg->uid)
      reg->inuse = 0;

      // add to sized list
      ycheck(1,Lnone,order > Regorder,"region %u order %u",rid,order)
      preg = hb->freeregs[order];
      hb->freeregs[order] = reg;
      reg->frenxt = preg;
      reg->freprv = nil;
      if (preg) preg->freprv = reg;

      // remove from class list
      clas = reg->clas;
      claspos = reg->claspos;
      clasregs = hb->clasregs + clas * Clasregs;
      xreg = clasregs[claspos];
      ycheck(1,Lnone,xreg != reg,"empty region %.01llu vs %u clas %u pos %u",uid,xreg ? xreg->id : 0,clas,claspos)
      clasregs[claspos] = nil;
      clasmsk = hb->clasmsk[clas];
      msk = 1ul << claspos;
      clasmsk &= ~msk;
      if (claspos == hb->claspos[clas]) { // active ?
        claspos = clasmsk ? ctzl(clasmsk) : 0;
       ycheck(1,0,claspos >= Clasregs,"reg %u clas %u pos %u",xreg->id,clas,claspos);
        hb->claspos[clas] = (ub2)claspos;
      }
      ydbg3(Lnone,"reg %.01lu clas %u pos %u msk %lx",uid,clas,claspos,clasmsk);
      hb->clasmsk[clas] = clasmsk;
      hb->cfremsk[clas] |= msk;
      claseq = hb->clasregcnt[clas];
      if (claseq) hb->clasregcnt[clas] = (ub2)(claseq - 1);
      hb->stat.trimregions[1]++;
      reg->aged = 1;
    }

    if (age >= ages[1] && aged == 1) { // todo
      hb->stat.trimregions[2]++;
      reg->aged = 2;
    }

    lim = ages[2];
    small = (reg->len <= 0x10000 && reg->metalen <= 0x8000);
    if (small) lim *= 4;
    curregs = (ub4)(sp->useregions + sp->noregions);
    if (sometimes(curregs,Region_interval)) sp->curnoregions = 0;
    if (sp->curnoregions > Region_alloc) lim = 1024; // reduce trim if too much redo happens
    if (age >= lim && aged == 2) { // trim : delete user and meta
      if (sp->noregions > Region_alloc) { // todo check
        from = 1; didcas = Cas(reg->lock,from,0);
        vg_drd_wlock_rel(reg)
        ycheck(1,Lnone,didcas == 0,"region %.01llu age %u locked %u",uid,age,from)
        break;
      }
      ydbg1(Fln,Lfree,"trim slab region %.01llu gen %u.%u.%u len %zu",uid,reg->gen,hid,rid,reg->len);
      ycheck(1,Lnone,reg->aged < 2,"region %.01llu age %u not  recycling %u",uid,age,reg->aged)

      // prepare to unmap
      bases[rbpos] = reg->user;
      metas[rbpos] = reg->meta;
      lens[rbpos] = reg->len;
      metalens[rbpos++] = reg->metalen;
      reg->prvlen = reg->len;
      reg->prvmetalen = reg->metalen;
      reg->len = reg->metalen = 0;
      reg->user = 0;
      reg->meta = nil;

      // move from sized to zerosized list
      preg = reg->freprv;
      nreg = reg->frenxt;
      if (preg) {
        preg->frenxt = nreg;
        if (nreg) nreg->freprv = preg;
      } else {
        hb->freeregs[order] = nreg;
        if (nreg) nreg->freprv = nil;
      }
      preg = hb->freeregs[0];
      hb->freeregs[0] = reg;
      reg->frenxt = preg;
      reg->freprv = nil;
      if (preg) preg->freprv = reg;

      hb->stat.trimregions[3]++;
      reg->aged = 3;
    }

    from = 1; didcas = Cas(reg->lock,from,0);
    vg_drd_wlock_rel(reg)
    ycheck(1,Lnone,didcas == 0,"region %.01llu age %u locked %u",uid,age,from)
    reg = nxreg;
  } // each reg

  hb->regtrim = reg ? reg : hb->reglst;
  hb->stat.delregions += rbpos;

  closeregs(hb)

  // as above for mmap regions

  /*mmap regions
    age 0 - never used
            1 - just freed
  */

  if (sometimes(tick,0xffff)) ages = effort_ages;
  else ages = Trim_Ages;

  mreg = mpstartreg = hb->mpregtrim;

  iter = Trim_scan;

  while (likely(mreg != nil)) {
    rid = mreg->id;
    if (mreg == mpstartreg && iter < Trim_scan) { // full circle
      break;
    }
    if (iter == 0) break;
    iter--;
    mpnxreg = mreg->nxt;
    if (mpnxreg == nil) mpnxreg = hb->mpreglst; // loop

    age = Atomget(mreg->age,Moacq);
    aged = mreg->aged;
    if (age == 0 || aged == 3) { mreg = mpnxreg; continue; } // unavailable or aged

    ycheck(1,Lfree,mreg->typ != Rmmap,"region %u typ %s",mreg->id,regnames[mreg->typ])

    if (age == 1) { // not empty ?
      set = Atomget(mreg->set,Moacq);
      ydbg2(Fln,Lfree,"region %u set %u",rid,set)
      // if (set == 1) { mreg = mpnxreg; continue; } // allocated
      hb->stat.trimregions[4]++;
      if (set != 2) { error(Lfree,"region %u set %u",rid,set); return 1; }
    }

    Atomset(mreg->age,age + 1,Morel);
    base = mreg->user;
    order = mreg->order;
    ycheck(1,Lnone,order >= Vmbits,"region %u order %u",rid,order)
    ycheck(1,Lnone,order < Mmap_threshold,"region %u order %u",rid,order)

    if (aged == 0 && age >= ages[0]) { // arrange for recycling, add to freelist
      ydbg1(Fln,Lfree,"recycle mmap region %u ord %u",rid,order);

      setregion(hb,(xregion *)mreg,base,Pagesize,0,Lfree,Fln);
      if (mreg->align) setregion(hb,(xregion *)mreg,base + mreg->align,Pagesize,0,Lfree,Fln);
      mreg->align = 0;

      // add to sized list
      pmreg = hb->freempregs[order - Mmap_threshold];
      hb->freempregs[order - Mmap_threshold] = mreg;
      mreg->frenxt = pmreg;
      mreg->freprv = nil;
      if (pmreg) pmreg->freprv = mreg;

      hb->stat.trimregions[5]++;
      mreg->aged = 1;
    }

    if (aged == 1 && age >= ages[1]) { // todo
      hb->stat.trimregions[6]++;
      mreg->aged = 2;
    }

    if (aged == 2 && age >= ages[2]) { // trim
      ydbg1(Fln,Lfree,"trim mmap region %u.%u ord %u",hid,rid,order);
      from = 2;
      didcas = Cas(mreg->set,from,0);
      if (didcas == 0) {
        error(Lfree,"mmap region %u.%u set %u",hid,rid,from)
        return 1;
      }
      // prepare unmap
      bases[rbpos] = base;
      metas[rbpos] = nil;
      lens[rbpos] = mreg->len;
      metalens[rbpos++] = 0;
      hb->stat.delmpregions++;
      mreg->prvlen = mreg->len;
      mreg->len = 0;

      // move from sized to zerosized list
      pmreg = mreg->freprv;
      nmreg = mreg->frenxt;
      if (pmreg) {
        pmreg->frenxt = nmreg;
        if (nmreg) nmreg->freprv = pmreg;
      } else {
        hb->freempregs[order - Mmap_threshold] = nmreg;
        if (nmreg) nmreg->freprv = nil;
      }
      mreg->frenxt = mreg->freprv = nil;

      pmreg = hb->freemp0regs;
      hb->freemp0regs = mreg;
      mreg->frenxt = pmreg;
      mreg->freprv = nil;
      if (pmreg) pmreg->freprv = mreg;

      // mreg->typ = Rnone;
      hb->stat.trimregions[7]++;
      mreg->aged = 3;
    }

    mreg = mpnxreg;
  } // for each mreg

  hb->mpregtrim = mreg ? mreg : hb->mpreglst;

  from = 1;
  didcas = Cas(hb->lock,from,0); // actual munmap is unlocked
  vg_drd_wlock_rel(hb)

  ycheck(0,Lfree,didcas == 0,"heap %u lock %u",hb->id,from)

  for (i = 0; i < rbpos; i++) {
    if (lens[i]) osunmem(Fln,hd,(void *)bases[i],lens[i],"trim");
    //coverity[uninit_use_in_call]
    if (metalens[i]) osunmem(Fln,hd,metas[i],metalens[i],"trim");
  }

  return 0;
}

/* First, find region. If not found, check remote heaps
   For slab, put in bin.
   For mmap, age or directly delete
   If reqlen == Nolen, return usaable size only and store info
   else if reqlen > 0, check with actual size
   returns available i.e. allocated size, typically rounded up from requested or Nolen if not found
   If hb is not nil, it is locked
  */
static Hot size_t free_heap(heapdesc *hd,heap *hb,void *p,size_t reqlen,struct ptrinfo *pi,enum Loc loc,ub4 fln,ub4 tag)
{
  heap *xhb;
  size_t ulen,alen,rlen,ip = (size_t)p;
  region *creg;
  mpregion *mpreg;
  bregion *mhb;
  xregion *xreg,*reg;
  enum Rtype typ;
  ub4 cel,cellen,celcnt,bincnt,len4;
  ub4 clas,claspos;
  Ub8 clasmsk;
  char buf[256];
  enum Status rv;
  ub4 xpct;
  celset_t set;
  ub4 from;
  bool didcas,reglocked,local;

  // common: regular local heap
  if (likely(hb != nil)) {
    reg = findregion(hb,ip,loc); // search page dir
  } else {
    reg = nil;
  } // nil hb

  if (unlikely(reg == nil)) {

    ytrace(0,hd,loc,"free(%zx) tag %.01u",ip,tag)

    // empty block ?
    if (unlikely(p == (void *)zeroblock)) {
 #if Yal_enable_valgrind == 0
      size_t x8 = 0;
      for (ub4 i = 0; i < 8; i++) x8 |= zeroarea[i];
      if (unlikely(x8 != 0)) error(loc,"written to malloc(0) block (%p) = %zx",p,x8)
#endif
      ytrace(1,hd,loc,"free(%zx) len 0",ip)
      ystats(hd->stat.free0s)
      return 0;
    }

    if (unlikely(ip >= Vmsize)) { // out of range
      error(loc,"invalid free(%zx) above max %u bits VM",ip,Vmbits)
      return Nolen;
    }
    if (unlikely(ip < Pagesize)) { // out of range
      error(loc,"invalid free(%zx) on page 0 of len %u",ip,Pagesize)
      return Nolen;
    }

    // mini ?
    if (hd->mhb) {
      mhb = hd->mhb;
      if (ip >= mhb->user && ip < mhb->user + mhb->len) {
       ytrace(1,hd,loc,"ptr+%zx",ip)
        alen = bump_free(hd,nil,mhb,ip,reqlen,tag,loc);
        if (unlikely(reqlen == Nolen)) { // get_usable_size()
          pi->reg = (xregion *)mhb; // todo unused
          pi->len = alen ? alen : Nolen;
          pi->local = 1;
        }
        return alen ? alen : Nolen;
      }
    }

    // remote ?
    ydbg3(loc,"+free(%zx) from heap %u",ip,hd->id)

    reg = findgregion(loc,ip); // locate ptr in global directory

    if (unlikely(reg == nil)) {
      hd->stat.invalid_frees++;
      xreg = region_near(ip,buf,255);
      if (xreg) errorctx(fln,loc,"heap %u %s",hb ? hb->id : 0,buf);
      error2(loc,Fln,"ptr %zx unallocated - not in any heap tag %.01u",ip,tag)
      return Nolen;
    }

    if (unlikely(reqlen == Nolen)) { // get size todo reorganise

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
          error(loc,"ptr %zx is not allocated: %u",ip,set);
          return Nolen;
        }
        ulen = cellen <= Cel_nolen ? cellen : slab_getlen(creg,cel,cellen);
        ycheck(Nolen,loc,ulen == 0,"region %u cel %u ulen %zu/%u",creg->id,cel,ulen,cellen)
        ycheck(Nolen,loc,ulen > cellen,"region %u cel %u ulen %zu above %u",creg->id,cel,ulen,cellen)
        pi->reg = reg;
        pi->cel = cel;
        pi->len = ulen;
        pi->local = 0;
        return cellen;
      }

      if (unlikely(typ == Rbump || typ == Rmini)) {
        len4 = bump_free(hd,nil,(bregion *)reg,ip,reqlen,tag,loc);
        pi->reg = reg;
        pi->len = len4 ? len4 : Nolen;
        pi->local = 0;
        return len4 ? len4 : Nolen;
      }

      // mmap
      if (unlikely(typ == Rmmap)) {
        mpreg = (mpregion *)reg;
        rlen = reg->len - mpreg->align;
        xpct = Atomget(mpreg->set,Moacq); // check free2
        if (xpct != 1) {
          errorctx(fln,loc,"expected 1, found %u",xpct);
          free2(Fln,loc,reg,ip,rlen,tag,"getsize");
          return Nolen;
        }
        pi->reg = reg;
        pi->len = mpreg->ulen;
        pi->local = 0;
        return rlen;
      }
    } // size

    // try to acquire owner heap
    local = 0;
    xhb = reg->hb;
    if (xhb) { // no mini
      from = 0; didcas = Cas(xhb->lock,from,1);
      if (didcas) {
        vg_drd_wlock_acq(xhb)
        local = 1;
        if (hb) {
          Atomset(hb->lock,0,Morel); // release orig
          vg_drd_wlock_rel(hb)
        }
        hd->hb = hb = xhb;
        hd->locked = 1;
      }
    }
  } else {
    local = 1; // nil hb or reg not locked
  }

  typ = reg->typ;
  if (likely(reg->typ == Rslab)) {
    creg = (region *)reg;
    vg_mem_def(creg,sizeof(region))
    vg_mem_def(creg->meta,creg->metalen)
    ycheck(Nolen,loc,reg->hb == nil,"region %zx has no hb",(size_t)reg)
    cellen = creg->cellen;
    celcnt = creg->celcnt;
    if (likely(local && reqlen != Nolen)) {
      ycheck(Nolen,loc,hb == nil,"nil hb for reg %u",reg->id)
      ytrace(1,hd,loc,"ptr+%zx len %u %2zu",ip,cellen,creg->stat.frees)

      bincnt = slab_free(hb,creg,ip,cellen,celcnt,tag); // put in recycling bin
      if (unlikely(bincnt == 0)) {
        return Nolen; // error
      }

      if (unlikely(bincnt == 1) && creg->inipos == celcnt) { // was probably full
        clas = creg->clas;
        claspos = creg->claspos;
        ycheck(Nolen,loc,claspos >= 32,"reg %u clas %u pos %u",creg->id,clas,claspos);
        clasmsk = hb->clasmsk[clas];
        clasmsk |= (1ul << claspos); // re-include in alloc candidate list
        ydbg3(loc,"reg %.01lu clas %u pos %u msk %lx",creg->uid,clas,claspos,clasmsk);
        hb->clasmsk[clas] = clasmsk;
        hb->claspos[clas] = (ub2)claspos;
      }
      vg_mem_noaccess(creg->meta,creg->metalen)
      vg_mem_noaccess(creg,sizeof(region))
      return cellen;

    } else if (reqlen == Nolen) { // get_usable_size()
      cel = slab_cel(creg,ip,cellen,celcnt,loc);
      if (unlikely(cel == Nocel)) return Nolen;
      set = slab_chkfree(creg,cel);
      if (unlikely(set != 1)) {
        error(loc,"ptr %zx is not allocated: set %u tag %.01u",ip,set,tag);
        return Nolen;
      }
      ulen = cellen <= Cel_nolen ? cellen : slab_getlen(creg,cel,cellen);
      ycheck(Nolen,loc,ulen == 0,"region %u cel %u ulen %zu/%u",creg->id,cel,ulen,cellen)
      ycheck(Nolen,loc,ulen > cellen,"region %u cel %u ulen %zu above %u",creg->id,cel,ulen,cellen)
      pi->reg = reg;
      pi->cel = cel;
      pi->len = ulen;
      pi->local = local;
      vg_mem_noaccess(creg->meta,creg->metalen)
      vg_mem_noaccess(creg,sizeof(region))
      return cellen;
    } // size

    // remote
    xpct = 0; reglocked = Cas(reg->lock,xpct,1);

    if (unlikely(reglocked == 0)) { // buffer, needs heap
      if (hb == nil) {
        hb = heap_new(hd,loc,Fln);
        if (hb == nil) return Nolen;
        hd->hb = hb;
        hd->locked = 1;
      }
      ytrace(1,hd,loc,"ptr+%zx len %u %2zu",ip,cellen,creg->stat.frees)
      len4 = slab_free_rheap(hd,hb,creg,ip,tag,loc);
      if (likely(len4 != 0)) return len4;

      hd->stat.invalid_frees++;
      xreg = region_near(ip,buf,255);
      if (xreg) errorctx(fln,loc,"%s region %u.%u %s",regname(xreg),xreg->hid,xreg->id,buf);
      error2(loc,Fln,"invalid free(%zx) tag %.01u",ip,tag)
      return Nolen;
    } // reg not locked

    vg_drd_wlock_acq(reg)

    ytrace(1,hd,loc,"ptr+%zx len %u %2zu",ip,cellen,creg->stat.frees)
    len4 = slab_free_rreg(hd,hb,creg,ip,tag,loc);
    Atomset(reg->lock,0,Morel);
    vg_drd_wlock_rel(reg)
    if (likely(len4 != 0)) return len4;

    hd->stat.invalid_frees++;
    xreg = region_near(ip,buf,255);
    if (xreg) errorctx(fln,loc,"%s region %u.%u %s",regname(xreg),xreg->hid,xreg->id,buf);
    error2(loc,Fln,"invalid free(%zx) tag %.01u",ip,tag)
    return Nolen;
  } // slab

  // mini or bump
  if (unlikely(typ == Rbump || typ == Rmini)) {
    ytrace(1,hd,loc,"ptr+%zx",ip)
    len4 = bump_free(hd,nil,(bregion *)reg,ip,reqlen,tag,loc);
    if (unlikely(reqlen == Nolen)) { // get_usable_size()
      pi->reg = reg;
      pi->len = len4 ? len4 : Nolen;
      pi->local = 0;
    }
    return len4 ? len4 : Nolen;
  }

  // mmap
  if (unlikely(typ == Rmmap)) {
    mpreg = (mpregion *)reg;
    ytrace(1,hd,loc,"ptr+%zx len %zu %2zu",ip,mpreg->len,hb ? hb->stat.mapfrees : 0)
    rlen = reg->len - mpreg->align;
    if (unlikely(reqlen == Nolen)) {
      xpct = Atomget(mpreg->set,Moacq); // check free2
      if (xpct != 1) {
        errorctx(fln,loc,"expected 1, found %u",xpct);
        free2(Fln,loc,reg,ip,rlen,tag,"getsize");
        return Nolen;
      }
      pi->reg = reg;
      pi->len = mpreg->ulen;
      pi->local = local;
      return rlen;
    }

    // free mmap directly
    if (hb) {
      hb->stat.mapfrees++;
      hb->stat.munmaps++;
    }
    rv = free_mmap(hd,local ? hb : nil,mpreg,ip,reqlen,loc,Fln);
    if (rv == St_error) {
      ypush(hd,Fln)
      if (hb) hd->stat.invalid_frees++;
      return Nolen;
    }
    return rlen;
  } // mmap

  hd->stat.invalid_frees++;
  errorctx(Fln,loc,"from heap %u type %s",hd->id,regname(reg))
  error2(loc,Fln,"region %u.%u ptr %zx",reg->hid,reg->id,ip)
  return Nolen;
} // free_heap

// lock heap if present. nil ptr handled
static Hot size_t yfree_heap(heapdesc *hd,void *p,size_t reqlen,struct ptrinfo *pi,enum Loc loc,ub4 tag)
{
  size_t retlen;
  heap *hb = hd->hb;
  ub4 from;
  bool didcas = 0;
  bool totrim;
  ub4 frees;

  // common: regular local heap
  if (likely(hb != nil)) {
    from = 0; didcas = Cas(hb->lock,from,1);
    if (unlikely(didcas == 0)) hb = nil;
    else {
      vg_drd_wlock_acq(hb)
      // Atomset(hb->locfln,Fln,Morel);
    }
  } // nil hb
  hd->locked = didcas;

  ytrace(0,hd,loc,"+ free(%zx) tag %.01u",(size_t)p,tag)
  retlen = free_heap(hd,hb,p,reqlen,pi,loc,Fln,tag);

  if (hd->locked == 0) {
    return retlen;
  }
  hb = hd->hb; // note: may have changed

  frees = (ub4)hb->stat.frees++;

  totrim = sometimes(frees,regfree_interval);

  if (likely(totrim == 0)) {
    from = 1; didcas = Cas(hb->lock,from,0);
    ycheck(Nolen,loc,didcas == 0,"heap %u unlock %u",hb->id,from)
    // Atomset(hb->lock,0,Morel);
    vg_drd_wlock_rel(hb)
    return retlen;
  }

  from = Atomget(hb->lock,Moacq);
  ycheck(Nolen,loc,from != 1,"heap %u unlock %u",hb->id,from)

  free_trim(hd,hb,frees); // unlocks
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
  yfree_heap(hd,p,len,nil,Lfree,tag);
}

size_t yal_getsize(void *p,ub4 tag)
{
  heapdesc *hd = getheapdesc(Lsize);
  struct ptrinfo pi;
  size_t len;

  ytrace(0,hd,Lsize,"+ size(%zx) tag %.01u",(size_t)p,tag)

  if (unlikely(p == nil)) {
    ytrace(0,hd,Lsize,"size(nil) tag %.01u",tag)
    return 0;
  }
  ypush(hd,Fln)
  memset(&pi,0,sizeof(pi));
  len = yfree_heap(hd,p,Nolen,&pi,Lsize,tag);

  ytrace(0,hd,Lsize,"- size(%zx) = %zu tag %.01u",(size_t)p,len,tag)
  return len;
}
#undef Logfile
