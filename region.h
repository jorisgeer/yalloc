/* region.h - regions

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

    Maintain region directory and creation plus reuse of regions
*/

#define Logfile Fregion

#define Dir1msk (Dir1len - 1)
#define Dir2msk (Dir2len - 1)
#define Dir3msk (Dir3len - 1)

// Global, not per-heap directory
static xregion *** _Atomic global_rootdir[Dir1len];

// Hand out leaf directories. Used for local and global dir.
static xregion **newleafdir(heap *hb)
{
  xregion **dp;
  ub4 pos,len,add = Dir3len;

  pos = hb->ldirmem_pos;
  if (pos + add > hb->ldirmem_top) {
    len = Dirmem * add;

    dp = osmem(Fln,hb->id,len * sizeof(void *),"leaf dir");
    if (dp == nil) return nil;

    hb->leafdirmem = dp;
    hb->ldirmem_top = len;
    pos = 0;
  }
  dp = hb->leafdirmem + pos;
  hb->ldirmem_pos = pos + add;
  return dp;
}

// Hand out intermediate directories. Used for local and global dir.
static xregion ***newdir(heap *hb)
{
  xregion ***dp;
  ub4 pos,len,add = Dir2len;

  pos = hb->dirmem_pos;
  if (pos + add > hb->dirmem_top) {
    len = Dirmem * add;

    dp = osmem(Fln,hb->id,len * sizeof(void *),"dir");
    if (dp == nil) return nil;

    hb->stat.mmaps++;

    hb->dirmem = dp;
    hb->dirmem_top = len;
    pos = 0;
  }
  dp = hb->dirmem + pos;
  hb->dirmem_pos = pos + add;
  return dp;
}

// add region in global directory
// memory for dirs is allocated from local heap as in local dir
static void setgregion(heap *hb,xregion *reg,size_t bas,size_t len,bool add,enum Loc loc,ub4 fln)
{
  xregion *xreg,*from;
  size_t org,end; // in pages
  ub4 pos1,pos2,pos3,posend;
  ub4 shift1,shift2;
  xregion *** _Atomic * dir1, ** _Atomic *dir2, * _Atomic *dir3;
  xregion ***ndir2,**ndir3;
  bool didcas;

  ydbg2(fln,loc,"set global %s region %u.%u @ %zx p %zx len %zu` %u",regname(reg),reg->hid,reg->id,(size_t)reg,bas,len,add);

  if (add) {
    xreg = reg;
  } else {
    xreg = nil;
  }

  org = bas >> Page;
  end = (bas + len) >> Page;

  dir1 = global_rootdir;

  shift1 = Vmbits - Page - Dir1;
  shift2 = shift1 - Dir2;

  do {
    pos1 = (org >> shift1) & Dir1msk;
    dir2 = (xregion ** _Atomic *)Atomgeta(dir1 + pos1,Moacq);
    if (dir2== nil) {
      ndir2 = hb ? newdir(hb) : bootalloc(Fln,reg->hid,Lnone,Dir2len * sizeof(void *));
      if (ndir2 == nil) return;
      didcas = Casa(dir1 + pos1,(xregion ****)&dir2,ndir2);
      if (unlikely(didcas == 0)) {
        ydbg2(fln,Lnone,"dir 2 nil for reg %u",reg->id)
        if (hb) hb->dirmem_pos -= Dir2len;
      } else dir2 = (xregion ** _Atomic *)ndir2; // our new
    }
    pos2 = (org >> shift2) & Dir2msk;

    pos3 = org & Dir3msk;
    posend = (ub4)min(end - org + pos3,Dir3len);
    org += posend - pos3;

    dir3 = (xregion * _Atomic *)Atomgeta(dir2 + pos2,Moacq);
    if (dir3== nil) {
      ndir3 = hb ? newleafdir(hb) : bootalloc(Fln,reg->hid,Lnone,Dir3len * sizeof(void *));
      if (ndir3 == nil) return;

      didcas = Casa(dir2 + pos2,(xregion ***)&dir3,ndir3);
      if (unlikely(didcas == 0)) {
        ydbg2(fln,Lnone,"dir 2 nil for reg %u",reg->id)
        if (hb) hb->ldirmem_pos -= Dir3len;
      } else dir3 = (xregion * _Atomic *)ndir3;
    }

    if (add) {
      do {
        from = nil;
        didcas = Casa(dir3 + pos3,&from,xreg);
        if (unlikely(didcas == 0)) {
          errorctx(fln,loc,"reg %zx base %lx len %lu`",(size_t)reg,bas,len)
          error2(loc,Fln,"heap %u %s region %u still mapped to %zx %u",hb ? hb->id : 0,regname(reg),reg->id,(size_t)from,from->id)
        }
      } while (++pos3 < posend);
    } else {
      do {
        from = reg;
        didcas = Casa(dir3 + pos3,&from,xreg);
        if (unlikely(didcas == 0)) {
          errorctx(fln,loc,"reg %zx base %lx len %lu`",(size_t)reg,bas,len)
          error2(loc,Fln,"heap %u %s region %u was not mapped %zx",hb ? hb->id : 0,regname(reg),reg->id,(size_t)from)
        }
      } while (++pos3 < posend);
    }

  } while (org < end); // -V776 PVS inf loop false positive ?
}

// add region in heap directory
static bool setregion(heap *hb,xregion *reg,size_t bas,size_t len,bool add,enum Loc loc,ub4 fln)
{
  xregion *xreg = add ? reg : nil;
  size_t org,end; // in pages
  ub4 pos1,pos2,pos3,posend;
  ub4 shift1,shift2;
  xregion ****dir1,***dir2,**dir3;
  ub4 hid = hb->id;

  ydbg2(fln,loc,"set %s region %u.%u p %zx len %zu` %u",regname(reg),hb->id,reg->id,bas,len,add);

#if Yal_enable_check

  if (reg->typ != Rmini) {
    if (hid != reg->hid) { do_ylog(Diagcode,loc,fln,Assert,0,"heap %u vs %u for %s region %u",hid,reg->hid,regname(reg),reg->id); return 1; }
    if (reg->typ == Rslab && len < Pagesize) { do_ylog(Diagcode,loc,fln,Assert,0,"heap %u type %s region has len %zu",hid,regname(reg),len); return 1; }
  }
  if (len < Pagesize) { do_ylog(Diagcode,loc,fln,Assert,0,"heap %u type %s region has len %zu",hid,regname(reg),len); return 1; }
  if (bas < Pagesize) { do_ylog(Diagcode,loc,fln,Assert,0,"heap %u type %s region has base %zx",hid,regname(reg),bas); return 1; }
  if (bas >= Vmsize) { do_ylog(Diagcode,loc,fln,Assert,0,"heap %u type %s region has base %zx",hid,regname(reg),bas); return 1; }
#endif

  org = bas >> Page; // todo Minregion ?
  end = (bas + len) >> Page;

  dir1 = hb->rootdir;

  shift1 = Vmbits - Page - Dir1;
  shift2 = shift1 - Dir2;

  do {
    pos1 = (org >> shift1) & Dir1msk;
    dir2 = dir1[pos1];
    if (dir2== nil) {
      dir2 = newdir(hb);
      if (dir2 == nil) return 1;
      dir1[pos1] = dir2;
    }
    pos2 = (org >> shift2) & Dir2msk;

    pos3 = org & Dir3msk;
    posend = (ub4)min(end - org + pos3,Dir3len);
    org += posend - pos3;

    dir3 = dir2[pos2];
    if (dir3== nil) {
      dir3 = newleafdir(hb);
      if (dir3 == nil) return 1;
      dir2[pos2] = dir3;
    }
    do {
      ycheck(1,Lnone,pos3 >= Dir3len,"pos1 %u above %u",pos3,Dir3len)
      dir3[pos3] = xreg;
    } while (++pos3 < posend);
  } while (org < end);

  if (reg->typ != Rmini) setgregion(hb,reg,bas,len,add,loc,fln);

  return 0;
}

// locate region from pointer. First part of free()
static Hot xregion *findregion(heap *hb,size_t ip,enum Loc loc)
{
  size_t ip1;
  ub4 pos1,pos2,pos3;
  ub4 shift1,shift2;
  xregion ****dir1,***dir2,**dir3,*reg;

  dir1 = hb->rootdir;

  shift1= Vmbits - Dir1;
  ip1 = ip >> shift1;
  pos1 = ip1 & Dir1msk;

  dir2 = dir1[pos1];

  if (unlikely(dir2 == nil)) return nil;

  shift2= Vmbits - Dir1 - Dir2;
  pos2 = (ip >> shift2) & Dir2msk;
  dir3 = dir2[pos2];

  if (unlikely(dir3 == nil)) return nil;

  pos3 = (ip >> Page) & Dir3msk;
  reg = dir3[pos3];

  if (unlikely(reg == nil)) { ydbg3(0,"pos %u,%u,%u.%x %zx %zx %u",pos1,pos2,pos3,pos3,ip,ip & (Pagesize - 1),Pagesize); return nil; }

  vg_mem_def((char *)reg,sizeof(region));

#if Yal_enable_check
  size_t base = reg->user;
  size_t len = reg->len;

  if (ip < base) { // internal error
    error(loc,"region %u.%u p %zx is %zu` below base %zx",reg->hid,reg->id,ip,base - ip,base)
    return nil;
  }
  if (ip > base + len) { error(loc,"region %u p %zx above base %zx + %zu",reg->id,ip,base,len) return nil; } // possible user error
#endif

  return reg;
}

// as above, global
static xregion *findgregion(enum Loc loc,size_t ip)
{
  size_t ip1;
  ub4 pos1,pos2,pos3;
  ub4 shift1,shift2;
  xregion *** _Atomic *dir1,**_Atomic *dir2,*_Atomic *dir3;
  xregion *reg;

  shift1= Vmbits - Dir1;
  ip1 = ip >> shift1;
  pos1 = ip1 & Dir1msk;

  shift2= Vmbits - Dir1 - Dir2;
  pos2 = (ip >> shift2) & Dir2msk;
  pos3 = (ip >> Page) & Dir3msk;

  dir1 = global_rootdir;

  reg = nil; dir3 = nil;

  dir2 = (xregion ** _Atomic *)Atomgeta(dir1 + pos1,Moacq);
  if (unlikely(dir2 == nil)) {
    errorctx(0,loc,"no mid page dir at pos %x",pos1)
    return nil;
  }

  dir3 = (xregion * _Atomic *)Atomgeta(dir2 + pos2,Moacq);
  if (unlikely(dir3 == nil)) {
    errorctx(0,loc,"no leaf page dir at pos %x,%x",pos1,pos2)
    return nil;
  }

  reg = Atomgeta(dir3 + pos3,Moacq);

  if (unlikely(reg == nil)) {
    errorctx(0,loc,"no region at pos %u,%u,%u",pos1,pos2,pos3)
    if (ip1 & ~Dir1msk) error(loc,"ptr %zx is %zu` outside %u bit VM space",ip,ip - Vmsize,Vmbits)
    return nil;
  }

#if Yal_enable_check
  vg_mem_def(reg,sizeof(xregion))
  size_t base = reg->user;
  size_t len = reg->len;

  if (ip < base) { error(loc,"region %u.%u p %zx is %zu` below base %zx",reg->hid,reg->id,ip,base - ip,base) return nil; } // internal error
  if (ip > base + len) { error(loc,"region %u p %zx above base %zx + %zu",reg->id,ip,base,len) return nil; } // possible user error
#endif

  return reg;
}

// Returns region nearby given ptr
static void *region_near(size_t ip,char *buf,ub4 len)
{
  size_t basea = Size_max,baseb = 0,bas,lena,lenb,ip1;
  ub4 b,age = 0,aged = 0;
  xregion *rega = nil,*regb = nil;
  mpregion *mpreg;
  bregion *breg;
  region *reg;
  heapdesc *xhd = Atomget(global_heapdescs,Moacq);
  heap *hb = Atomget(global_heaps,Moacq);

  *buf = 0;

  if (ip == (size_t)zeroblock) {
    snprintf_mini(buf,0,len,"ptr  %zx is a zero-len block",ip);
    return nil;
  }

  ip1 = ip >> (Vmbits - Dir1);
  if (ip1 & ~Dir1msk) { snprintf_mini(buf,0,len,"ptr  %zx is %zu` outside %u bit VM space",ip,ip - Vmsize,Vmbits ); return nil; }

  while (xhd) {
    if ( (breg = xhd->mhb) ) { // mini
      if ( (bas = breg->user ) > ip && bas < basea) { basea = bas; rega = (xregion *)breg; }
      if ( bas < ip && bas > baseb) { baseb = bas; regb = (xregion *)breg;  }
    }
    xhd = xhd->nxt;
  }

  while (hb) {
    for (b = 0; b < Bumpregions; b++) {
      breg = hb->bumpregs + b;
      if (breg->len == 0) continue;
      if ( (bas = breg->user ) > ip && bas < basea) { basea = bas; rega = (xregion *)breg; }
      if ( bas < ip && bas > baseb) { baseb = bas; regb = (xregion *)breg;  }
    }
    reg = hb->reglst;
    while (reg) {
      vg_mem_def(reg,sizeof(region))
      vg_mem_def(reg->meta,reg->metalen)
      if ( (bas = reg->user ) > ip && bas < basea) { basea = bas; rega = (xregion *)reg; }
      if ( bas < ip && bas > baseb) { baseb = bas; regb = (xregion *)reg; age = reg->age; aged = reg->aged; }
      reg = reg->nxt;
    }
    mpreg = hb->mpreglst;
    while (mpreg) {
      if ( (bas = mpreg->user )> ip && bas < basea) { basea = bas; rega = (xregion *)mpreg; }
      if ( bas < ip && bas > baseb) { baseb = bas; regb = (xregion *)mpreg; age = Atomget(mpreg->age,Moacq); aged = mpreg->aged; }
      mpreg = mpreg->nxt;
    }
    hb = hb->nxt;
  }

  if (rega == nil && regb == nil) return nil;
  if (rega == nil) rega = regb;
  else if (regb == nil) regb = rega;
  // if (rega == nil || regb == nil) return nil;

  lena = rega->len; // -V522 PVS nil ptr deref
  lenb = regb->len; // -V522 PVS nil ptr deref

  if (ip > baseb && ip < baseb + lenb) {
    snprintf_mini(buf,0,len,"ptr %zx is %zu`b inside %s region %u.%u len %zu` age %u.%u",ip,ip - baseb,regname(regb),regb->hid,regb->id,lenb,age,aged);
    return regb;
  }
  if (ip > baseb && ip - baseb - lenb < basea - ip) {
    snprintf_mini(buf,0,len,"ptr %zx is %zu`b after %s region %u.%u len %zu` at %zx .. %zx",ip,ip - baseb - lenb,regname(regb),regb->hid,regb->id,lenb,baseb,baseb + lenb);
    return regb;
  }
  snprintf_mini(buf,0,len,"ptr %zx is %zu`b before %s region %u.%u len %zu` at %zx .. %zx",ip,basea - ip,regname(rega),rega->hid,rega->id,lena,basea,basea + lena);
  return rega;
}

#define Metaguard 0

static region *newregmem(heap *hb)
{
  ub4 pos = hb->regmem_pos;
  ub4 top = Regmem_inc;
  region *reg;

  if (pos == top) {
    hb->stat.mmaps++;
    reg = osmem(Fln,hb->id,top * sizeof(region),"region pool");
    if (reg == nil) return nil;
    hb->regmem = reg;
    hb->regmem_pos = 1;
    return reg;
  }
  reg = hb->regmem + pos;
  hb->regmem_pos = pos + 1;
  return reg;
}

static mpregion *newmpregmem(heap *hb)
{
  ub4 pos = hb->xregmem_pos;
  ub4 top = Xregmem_inc;
  mpregion *reg;

  if (pos == top) {
    hb->stat.mmaps++;
    reg = osmem(Fln,hb->id,top * sizeof(mpregion),"xregion pool");
    if (reg == nil) return nil;
    hb->xregmem = reg;
    hb->xregmem_pos = 1;
    return reg;
  }
  reg = hb->xregmem + pos;
  hb->xregmem_pos = pos + 1;
  return reg;
}

#if Yal_enable_valgrind
#define openreg(reg) vg_openreg(reg);
#define closereg(reg) vg_closereg(reg);
#define openregs(hb) vg_openregs(hb);
#define closeregs(hb) vg_closeregs(hb);

static void vg_openreg(region *reg)
{
  vg_mem_def(reg,sizeof(region))
  vg_mem_def(reg->meta,reg->metalen)
}

static void vg_openregs(heap *hb)
{
  region *reg = hb->reglst;
  while (reg) {
    vg_mem_def(reg,sizeof(region))
    vg_mem_def(reg->meta,reg->metalen)
    reg = reg->nxt;
  }
}

static void vg_closereg(region *reg)
{
  vg_mem_noaccess(reg->meta,reg->metalen)
  vg_mem_noaccess(reg,sizeof(region))
}

static void vg_closeregs(heap *hb)
{
  region *nxreg,*reg = hb->reglst;
  while (reg) {
    nxreg = reg->nxt;
    vg_mem_noaccess(reg->meta,reg->metalen)
    vg_mem_noaccess(reg,sizeof(region))
    reg = nxreg;
  }
}

#else
#define openreg(reg)
#define closereg(reg)
#define openregs(hb)
#define closeregs(hb)
#endif

// create new region with user and meta blocks
static region *newregion(heap *hb,ub4 order,size_t len,size_t metaulen,ub4 cellen,enum Rtype typ)
{
  void *user,*ouser;
  void *meta,*ometa;
  size_t mlen,ulen,olen,omlen,loadr,hiadr;
  ub8 uid = 0;
  region *reg = nil,*ureg,*preg,*nreg,*nxt,*nxureg;
  ub4 rid,hid = hb->id;
  ub4 ord = order;
  ub4 claseq,gen;
  ub4 ohid = hid;
  ub4 rbinlen;
  ub4 *rembin;
  ub4 shift;
  ub4 iter;
  ub4 from;
  bool didcas;
  yalstats *sp = &hb->stat;

  ycheck(nil,Lalloc,len < Pagesize,"heap %u type %s region has len %zu",hid,regnames[typ],len)
  ycheck(nil,Lalloc,len >= Vmsize,"heap %u type %s region has len %zu`",hid,regnames[typ],len)

  ycheck(nil,Lnone,order > Regorder,"region len %zu` order %u",len,order)

  openregs(hb)

  // recycle ?
  iter = 40 + 4 * order;
  do {
    ureg = hb->freeregs[ord];
    while (ureg && --iter) {
      vg_mem_def(ureg,sizeof(region))
      vg_mem_def(ureg->meta,ureg->metalen)
      nxureg = ureg->frenxt;
      if (len <= ureg->len && metaulen <= ureg->metalen) { // suitable size
        reg = ureg;
        nreg = reg->frenxt; // unlist
        preg = reg->freprv;
        if (preg) preg->frenxt = nreg;
        else hb->freeregs[ord] = nreg;
        if (nreg) nreg->freprv = preg;
        reg->frenxt = reg->freprv = nil;
        sp->useregions++;
        uid = (sp->useregions + sp->newregions + sp->noregions) * 2;
        ydbg2(Fln,Lnone,"use region %.01llu -> %u.%llu len %zu` gen %u.%u.%u cel %u for %zu`,%u",reg->uid,hid,uid,reg->len,reg->gen,hid,reg->id,reg->cellen,len,cellen);
        break;
      }
      ureg = nxureg;
    }
  } while (reg == nil && ++ord <= min(Regorder,order + 3));

  if (reg == nil) { // use trimmed
    iter = 50;
    ureg = hb->freeregs[0];
    while (ureg && --iter) {
      if (ureg->order >= order - 1 && ureg->order <= order + 3 && len <= ureg->prvlen && metaulen <= ureg->prvmetalen) { // would have reused
        reg = ureg;
        sp->noregions++;
        sp->curnoregions++;
        uid = (sp->useregions + sp->newregions + sp->noregions) * 2;
        ydbg2(Fln,Lnone,"use region %.01llu -> %u.%llu len %zu` gen %u.%u.%u cel %u for %zu`,%u",reg->uid,hid,uid,reg->len,reg->gen,hid,reg->id,reg->cellen,len,cellen);
        break;
      }
      ureg = ureg->frenxt;
    }

    if (reg == nil) {
      reg = hb->freeregs[0];
      if (reg) {
        nreg = reg->frenxt;
        hb->freeregs[0] = nreg;
        if (nreg) nreg->freprv = nil;
        sp->useregions++;
        uid = (sp->useregions + sp->newregions + sp->noregions) * 2;
        ycheck(nil,0,reg->len != 0,"region %.01llu len %zu`",reg->uid,reg->len)
        ydbg1(Fln,Lnone,"use region %.01llu -> %u.%llu len %zu` gen %u.%u.%u cel %u for %zu`,%u",reg->uid,hid,uid,reg->len,reg->gen,hid,reg->id,reg->cellen,len,cellen);
      }
    } else {
      nreg = reg->frenxt;
      preg = reg->freprv;

      if (preg) preg->frenxt = nreg;
      else hb->freeregs[0] = nreg;
      if (nreg) nreg->freprv = preg;
      sp->useregions++;
      uid = (sp->useregions + sp->newregions + sp->noregions) * 2;
      ycheck(nil,0,reg->len != 0,"region %u len %zu`",reg->id,reg->len)
      ydbg2(Fln,Lnone,"use region %.01llu -> %u.%llu len %zu` gen %u.%u.%u cel %u for %zu`,%u",reg->uid,hid,uid,reg->len,hid,reg->gen,reg->id,reg->cellen,len,cellen);
    }
  }

  closeregs(hb)

  if (reg) { // reuse

    vg_mem_def(reg,sizeof(region))
    vg_mem_def(reg->meta,reg->metalen)

    ycheck(nil,0,reg->aged == 0,"region %.01llu not aged",reg->uid)
    ycheck(nil,0,reg->inuse != 0,"region %.01llu in use",reg->uid)
    ycheck(nil,0,reg->hb == nil,"region %.01llu nil hb",reg->uid)
    ycheck(nil,0,reg->hb != hb,"region %.01llu hb %u vs %u",reg->uid,hb->id,reg->hb->id)

#if Yal_enable_check > 2
    for (ord = 0; ord <= Regorder; ord++) {
      ureg = hb->freeregs[ord];
      iter = 0;
      while (ureg && iter < 1000) {
        ycheck(nil,0,ureg == reg,"ord %u iter %u reg %u.%u present",ord,iter,reg->gen,reg->id)
        ureg = ureg->frenxt; iter++;
      }
      ycheck(nil,0,iter == 1000,"ord %u iter %u",ord,iter)
    }
#endif

    from = 0; didcas = Cas(reg->lock,from,1);
    ycheck(nil,0,didcas == 0,"region %.01llu from %u",reg->uid,from)
    vg_drd_wlock_acq(reg)

    ycheck(nil,0,reg->typ != Rslab,"region %u typ %s",reg->id,regnames[reg->typ])

    olen = reg->len;
    ouser = (void *)reg->user;

    omlen = reg->metalen;
    ometa = (void *)reg->meta;

    // preserve
    rid = reg->id;
    ohid = reg->hid;
    nxt = reg->nxt;
    claseq = reg->claseq;
    gen = reg->gen;
    rbinlen = reg->rbinlen;
    rembin = Atomget(reg->rembin,Moacq);

    slabstats(reg,&hb->stat,nil,0,0,0,0,0); // accumulate stats from previous user

    Atomset(reg->lock,0,Morel);
    vg_drd_wlock_rel(reg)

    memset(reg,0,sizeof(region));

    reg->fln = Fln;

    reg->typ = Rnone;
    reg->gen = gen + 1;
    reg->claseq = claseq;
    reg->nxt = nxt;
    reg->clr = 1; // if set, calloc() needs to clear.
    reg->rbinlen = rbinlen;
    Atomset(reg->rembin,rembin,Morel);

  } else { // new
    olen = omlen = 0;
    ouser = nil; ometa = nil;
    rid = (ub4)++sp->newregions;
    rid *= 2;
    uid = (sp->newregions + sp->useregions + sp->noregions) * 2;
    reg = newregmem(hb);
    ydbg1(Fln,Lnone,"new region %zx %u.%llu gen 0.%u.%u len %zu` for cellen %u",(size_t)reg,hid,uid,hid,rid,len,cellen);
    if (reg == nil) {
      return nil;
    }
    vg_mem_name(reg,sizeof(region),"'slab region'")
    vg_drd_rwlock_init(reg)

    if ( (size_t)reg & 15) {
      error(Lalloc,"region %u at %p unaligned",rid,(void *)reg)
      return nil;
    }

    // maintain list for ageing and stats
    if (hb->reglst == nil) hb->reglst = hb->regtrim = hb->regprv = reg;
    else {
      preg = hb->regprv;
      vg_mem_def(preg,sizeof(region))
      ycheck(nil,0,preg->typ != Rslab,"region %u typ %s",preg->id,regnames[preg->typ])
      preg->nxt = reg;
      vg_mem_noaccess(preg,sizeof(region))
      hb->regprv = reg;
    }
  } // new or not

  reg->inuse = 1;

  reg->hb = hb;
  reg->typ = typ;

  uid |= (ub8)hid << 32;

  reg->hid = ohid;
  reg->id = rid;
  reg->uid = uid;

  if (olen == 0) {
    ulen = doalign8(len,Pagesize);
    user = osmem(Fln,hid,ulen,"region base");
    if (user == nil) {
      return nil;
    }
    vg_mem_name(user,ulen,"'slab user'")
  } else {
    user = ouser;
    ulen = olen;
  }
  reg->user = (size_t)user;
  reg->len = ulen;

  vg_mem_noaccess(user,ulen)
  loadr = (size_t)user;
  hiadr = loadr + len;
  ylostats(hb->stat.loadr,loadr)
  yhistats(hb->stat.hiadr,hiadr)

  reg->order = order;

  if (omlen == 0) {
    mlen = doalign8(metaulen,256);
    if (order < 24) { // expand to accomodate reuse for smaller cel size
      if (cellen > 16) mlen *= 2;
    } else if (cellen >= 32) {
      shift = order - 24 + 1;
      mlen += (mlen >> shift);
    }
    mlen = max(mlen,max(Pagesize,8192));
    meta = osmem(Fln,hid,mlen,"region meta");
    if (meta == nil) {
      return nil;
    }
    vg_mem_name(meta,mlen,"'slab meta'")
  } else {
    ycheck(nil,0,ometa == nil,"nil meta for len %zu",omlen)
    meta = ometa;
    mlen = omlen;
    memset(ometa,0,min(metaulen,omlen));
    ydbg3(Lnone,"use meta %zu",omlen);
  }
  reg->meta = (ub4 *)meta;
  reg->metalen = (ub4)mlen;
  reg->metautop = (size_t)meta + metaulen;
  Atomfence(Morel);
  return reg;
}

// new region for mmap block
static mpregion *newmpregion(heap *hb,size_t len,enum Loc loc,ub4 fln)
{
  mpregion *ureg,*reg = nil,*preg,*nreg;
  ub4 rid;
  ub4 hid = hb->id;
  ub4 order,ord,iter;
  size_t user,olen;
  void *p;
  yalstats *sp = &hb->stat;

  if (len < mmap_limit) {
    do_ylog(Diagcode,loc,fln,Assert,0,"mmap region len %zu",len);
    return nil;
  }

  order = ord = sizeof(size_t) * 8 - clzl(len);

  ycheck(nil,Lnone,order < Mmap_threshold,"region len %zu` order %u below %u",len,order,Mmap_threshold)
  ycheck(nil,Lnone,order >= Vmbits,"region len %zu` order %u above %u",len,order,Vmbits)

  // recycle ?
  iter = 80;
  do {
    ureg = hb->freempregs[ord - Mmap_threshold];
    while (ureg && --iter) {
      ydbg2(Fln,loc,"try xreg %u len %zu` for %zu` ord %u/%u",ureg->id,ureg->len,len,ord,order)
      if (len <= ureg->len) {
        ydbg2(Fln,loc,"use xregion %u.%u len %zu` gen %u for %zu` ord %u/%u",ureg->hid,ureg->id,ureg->len,ureg->gen,len,ord,order)
        reg = ureg;

        nreg = reg->frenxt; // unlist
        preg = reg->freprv;
        if (preg) preg->frenxt = nreg;
        else hb->freempregs[ord - Mmap_threshold] = nreg;
        if (nreg) nreg->freprv = preg;
        sp->usempregions++;
        break; // suitable size
      }
      ureg = ureg->frenxt;
    }
  } while (reg == nil && ++ord < min(Vmbits,order + 3));

  if (reg == nil) { // use empty one
    iter = 100;
    ureg = hb->freemp0regs;
    while (ureg && --iter) {
      if (len <= ureg->prvlen && len * 2 >= ureg->prvlen) { // would have reused
        reg = ureg;
        ydbg1(Fln,loc,"use xregion %u.%u len %zu` gen %u for %zu` ord %u/%u",ureg->hid,ureg->id,ureg->len,ureg->gen,len,ord,order)
        sp->nompregions++;
        sp->curnompregions++;
        break;
      }
      ureg = ureg->frenxt;
    }
    // unlist
    if (reg == nil) {
      reg = hb->freemp0regs;
      if (reg) {
        ydbg1(Fln,loc,"use xregion %u.%u len %zu` for %zu` ord %u/%u",reg->hid,reg->id,reg->len,len,ord,order)
        nreg = reg->frenxt;
        hb->freemp0regs = nreg;
        if (nreg) nreg->freprv = nil;
      }
    } else {
      nreg = reg->frenxt;
      if (reg == hb->freemp0regs) {
        hb->freemp0regs = nreg;
        if (nreg) nreg->freprv = nil;
      } else {
        preg = reg->freprv;
        if (preg) preg->frenxt = nreg;
        if (nreg) nreg->freprv = preg;
      }
    }
  }

#if Yal_enable_check > 2
  ureg = hb->freemp0regs;
  iter = 0;
  while (ureg && iter < 100) {
     ydbg1(Fln,loc,"%u has xregion %u.%u len %zu`",iter++,ureg->hid,ureg->id,ureg->len)
     ureg = ureg->frenxt;
  }
#endif

  if (reg == nil) { // new
    hb->stat.newmpregions++;
    rid = (ub4)hb->stat.newmpregions;
    rid = rid * 2 + 1;

    reg = newmpregmem(hb);
    if (reg == nil) return nil;

    ydbg1(Fln,Lnone,"new xregion %zx %u.%u for size %zu` from %zu`",(size_t)reg,hid,rid,len,reg->len);

    if (hb->mpreglst == nil) { // maintain for stats and trim
      hb->mpreglst = hb->mpregtrim = hb->mpregprv = reg;
    } else {
      preg = hb->mpregprv;
      ycheck(nil,0,preg->typ != Rmmap,"region %u typ %s",preg->id,regnames[preg->typ])
      preg->nxt = reg;
      hb->mpregprv = reg;
    }

    reg->hb = hb;
    reg->id = rid;
    reg->hid = hid;
    reg->order = order;
    Atomset(reg->age,0,Morel);
    reg->aged = 0;
    Atomset(reg->set,2,Morel); // give it freed
    return reg;
  } else rid = reg->id;

  reg->frenxt = reg->freprv = nil;

  // reuse
  ycheck(nil,0,reg->hb != hb,"mpregion %u heap %zx vs %zx",rid,(size_t)reg->hb,(size_t)hb)
  sp->usempregions++;
  olen = reg->len;
  ydbg2(Fln,Lnone,"use xregion %zx %u.%u for size %zu` from %zu`",(size_t)reg,hid,reg->id,len,olen);

  if (olen) {
    reg->gen++;
    reg->clr = 1;
  } else {
    Atomad(global_mapadd,1,Monone);
    p = osmmap(len);
    user = (size_t)p;
    if (user == 0) return nil;
    reg->user = user;
    reg->len = len;
    reg->order = order;
    reg->clr = 0;
    Atomset(reg->set,2,Morel); // give it freed
  }
  Atomset(reg->age,0,Morel);
  reg->aged = 0;
  reg->typ = Rnone;

  return reg;
}

#undef Logfile
