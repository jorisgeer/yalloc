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

// static _Atomic ub4 global_dirver;

static Const inline ub4 bucket(size_t x)
{
  return (ub4)murmurmix(x) & (Rembkt - 1);
}

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
static void setgregion(heap *hb,xregion *reg,size_t bas,size_t len,bool add)
{
#if Yal_inter_thread_free
  xregion *xreg;
  size_t org,end; // in pages
  ub4 pos1,pos2,pos3,posend;
  ub4 shift1,shift2;
  xregion *** _Atomic * dir1, ** _Atomic *dir2, * _Atomic *dir3;
  xregion ***ndir2,**ndir3;
  // ub4 dirver;
  bool didcas;

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

  // dirver = Atomad(global_dirver,1,Moacqrel);

  do {
    pos1 = (org >> shift1) & Dir1msk;
    dir2 = (xregion ** _Atomic *)Atomgeta(dir1 + pos1,Moacq);
    if (dir2== nil) {
      ndir2 = hb ? newdir(hb) : bootalloc(Fln,reg->hid,Lnone,Dir2len * sizeof(void *));
      if (ndir2 == nil) return;
      didcas = Casa(dir1 + pos1,(xregion ****)&dir2,ndir2);
      if (unlikely(didcas == 0)) {
        ydbg2(Lnone,"dir 2 nil for reg %u",reg->id)
        dir2 = (xregion ** _Atomic *)Atomgeta(dir1 + pos1,Moacq);
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
        ydbg2(Lnone,"dir 2 nil for reg %u",reg->id)
        dir3 = (xregion * _Atomic *)Atomgeta(dir2 + pos2,Moacq);
        if (hb) hb->ldirmem_pos -= Dir3len;
      } else dir3 = (xregion * _Atomic *)ndir3;
    }
    do {
      Atomseta(dir3 + pos3,xreg,Monone);
    } while (++pos3 < posend);
  } while (org < end); // -V776 PVS inf loop false positive ?

  // dirver = Atomad(global_dirver,1,Moacqrel);

#endif
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

  ydbg3(0,loc,fln,Info,0,"set %s region %u p %zx len %zu` %u",regname(reg),reg->id,bas,len,add);

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
    // ycheck(1,Lnone,pos1 >= Dir1len,"pos1 %u above %u",pos1,Dir1len)
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

    // ycheck(1,Lnone,pos2 >= Dir2len,"pos1 %u above %u",pos2,Dir2len)
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

#if Yal_inter_thread_free
  if (add || reg->typ == Rslab) setgregion(hb,reg,bas,len,add);
#endif
  return 0;
}

// locate region from pointer. First part of free()
static Hot xregion *findregion(heap *hb,size_t ip)
{
  size_t ip1;
  ub4 pos1,pos2,pos3;
  ub4 shift1,shift2;
  xregion ****dir1,***dir2,**dir3,*reg;

  dir1 = hb->rootdir;

  shift1= Vmbits - Dir1;
  ip1 = ip >> shift1;
  pos1 = ip1 & Dir1msk;

  // ycheck(nil,Lnone,pos1 >= Dir1len,"pos1 %u above %u",pos1,Dir1len)
  dir2 = dir1[pos1];

  if (unlikely(dir2 == nil)) return nil;

  shift2= Vmbits - Dir1 - Dir2;
  pos2 = (ip >> shift2) & Dir2msk;
  // ycheck(nil,Lnone,pos2 >= Dir2len,"pos1 %u above %u",pos2,Dir2len)
  dir3 = dir2[pos2];

  if (unlikely(dir3 == nil)) return nil;

  pos3 = (ip >> Page) & Dir3msk;
  // ycheck(nil,Lnone,pos3 >= Dir3len,"pos1 %u above %u",pos3,Dir3len)
  reg = dir3[pos3];

  if (unlikely(reg == nil)) { ydbg2(0,"pos %u,%u,%u.%x %zx %zx %u",pos1,pos2,pos3,pos3,ip,ip & (Pagesize - 1),Pagesize); return nil; }

  ycheck(nil,Lnone,ip < reg->user,"%s region %u p %zx is %zu`b below base %zx",regname(reg),reg->id,ip,reg->user - ip,reg->user)
  ycheck(nil,Lnone,ip > reg->user + reg->len,"region %u p %zx above base %zx + %zu",reg->id,ip,reg->user,reg->len)

  return reg;
}

#if Yal_inter_thread_free
// as above, global
static xregion *findgregion(enum Loc loc,size_t ip)
{
  size_t ip1;
  ub4 pos1,pos2,pos3;
  ub4 shift1,shift2;
  xregion *** _Atomic *dir1,**_Atomic *dir2,*_Atomic *dir3;
  xregion *reg;
  // ub4 v1,v2;

  shift1= Vmbits - Dir1;
  ip1 = ip >> shift1;
  pos1 = ip1 & Dir1msk;

  shift2= Vmbits - Dir1 - Dir2;
  pos2 = (ip >> shift2) & Dir2msk;
  pos3 = (ip >> Page) & Dir3msk;

  // v1 = Atomget(global_dirver,Moacq);

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

  // v2 = Atomget(global_dirver,Moacq);

  if (unlikely(reg == nil)) {
    if (ip1 & ~Dir1msk) error(loc,"ptr %zx is %zu` outside %u bit VM space",ip,ip - Vmsize,Vmbits);
    errorctx(0,loc,"no region at pos %u,%u,%u",pos1,pos2,pos3)
    return nil;
  }

#if Yal_enable_check
  size_t base = reg->user;
  size_t len = reg->len;

  if (ip < base) { error(loc,"region %u.%u p %zx is %zu` below base %zx",reg->hid,reg->id,ip,base - ip,base); return nil; } // internal error
  if (ip > base + len) { error(loc,"region %u p %zx above base %zx + %zu",reg->id,ip,base,len); return nil; } // possible user error
#endif

  return reg;
}

// Returns region nearby given ptr
static void *region_near(size_t ip,char *buf,ub4 len)
{
  size_t basea = Size_max,baseb = 0,bas,lena,lenb,ip1;
  ub4 b,pos;
  xregion *rega = nil,*regb = nil;
  mpregion *mpreg;
  bregion *breg;
  region *reg;
  heapdesc *xhd = Atomget(global_heapdescs,Moacq);
  heap *hb = Atomget(global_heaps,Moacq);

  *buf = 0;

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
      if ( (bas = reg->user ) > ip && bas < basea) { basea = bas; rega = (xregion *)reg; }
      if ( bas < ip && bas > baseb) { baseb = bas; regb = (xregion *)reg;  }
      reg = reg->nxt;
    }
    mpreg = hb->mpreglst;
    while (mpreg) {
      if ( (bas = mpreg->user )> ip && bas < basea) { basea = bas; rega = (xregion *)mpreg; }
      if ( bas < ip && bas > baseb) { baseb = bas; regb = (xregion *)mpreg;  }
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
    pos = snprintf_mini(buf,0,len,"ptr %zx is %zu`b inside %s region %u.%u len %zu` age %u.%u",ip,ip - baseb,regname(regb),regb->hid,regb->id,lenb,regb->age,regb->aged);
    return regb;
  }
  if (ip > baseb && ip - baseb - lenb < basea - ip) {
    snprintf_mini(buf,0,len,"ptr %zx is %zu`b after %s region %u.%u len %zu` at %zx .. %zx",ip,ip - baseb - lenb,regname(regb),regb->hid,regb->id,lenb,baseb,baseb + lenb);
    return regb;
  }
  snprintf_mini(buf,0,len,"ptr %zx is %zu`b before %s region %u.%u len %zu` at %zx .. %zx",ip,basea - ip,regname(rega),rega->hid,rega->id,lena,basea,basea + lena);
  return rega;
}

#endif // Yal_inter_thread_free

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

// create new region with user and meta blocks
static region *newregion(heap *hb,ub4 order,size_t len,size_t metaulen,ub4 cellen,enum Rtype typ)
{
  void *user,*ouser;
  void *meta,*ometa;
  size_t mlen,ulen,olen,omlen,loadr,hiadr;
  ub8 uid;
  region *reg,*preg,*trimreg,*trimpreg,*nxt;
  ub4 rid,hid;
  ub4 claseq;
  ub4 ohid = hid = hb->id;
  ub4 bkt;
  ub4 shift;
  ub4 iter;
  yalstats *sp = &hb->stat;

  ycheck(nil,Lalloc,len < Pagesize,"heap %u type %s region has len %zu",hid,regnames[typ],len)
  ycheck(nil,Lalloc,len >= Vmsize,"heap %u type %s region has len %zu`",hid,regnames[typ],len)

  ycheck(nil,Lnone,order > Regorder,"region len %zu` order %u",len,order)

  // recycle ?
  reg = hb->freeregs[order];
  preg = trimreg = trimpreg = nil;
  ulen = len;
  iter = 100;
  while (reg && --iter) {
    if (ulen <= reg->len && metaulen <= reg->metalen) break; // suitable size
    else if (reg->len == 0 && trimreg == nil) { trimreg = reg; trimpreg = preg; }
    preg = reg;
    reg = reg->frenxt;
  }
  if (reg == nil && trimreg) {
    reg = trimreg; // reuse trimmed region
    preg = trimpreg;
  }
  if (reg) { // reuse

    ycheck(nil,0,reg->hb != hb,"region %u heap %zx vs %zx",reg->id,(size_t)reg->hb,(size_t)hb);
    ycheck(nil,0,reg->typ != Rslab,"region %u typ %s",reg->id,regnames[reg->typ]);

    uid = ++sp->useregions + sp->newregions;
    olen = reg->len;
    ouser = (void *)reg->user;

    if (olen) {
      if (olen == ulen) setgregion(hb,(xregion *)reg,(size_t)ouser,olen,0); // remains identical
      else setregion(hb,(xregion *)reg,(size_t)ouser,olen,0,Lalloc,Fln); // unmap old one
    }
    reg->typ = Rnone;

    // remove from free reg list
    if (preg) {
      ycheck(nil,0,preg->typ != Rslab,"region %u typ %s",preg->id,regnames[preg->typ]);
      preg->frenxt = reg->frenxt;
    } else hb->freeregs[order] = reg->frenxt;

    omlen = reg->metalen;
    ometa = (void *)reg->meta;

    // preserve
    rid = reg->id;
    ohid = reg->hid;
    nxt = reg->nxt;
    claseq = reg->claseq;

    slabstats(reg,&hb->stat,nil,0,0,0,0,0); // accumulate stats from previous user

    ydbg2(Lnone,"use region %.01lu len %zu` cel %u for %zu`,%u",uid,olen,reg->cellen,len,cellen);

    memset(reg,0,sizeof(region));

    reg->claseq = claseq;
    reg->nxt = nxt;
    reg->clr = 1; // if set, calloc() needs to clear.

  } else { // new
    olen = omlen = 0;
    ouser = nil; ometa = nil;
    rid = (ub4)++sp->newregions;
    uid = rid + sp->useregions;
    reg = newregmem(hb);
    ydbg2(Lnone,"new region %zx %u.%lu len %zu` for size %u",(size_t)reg,hid,uid,ulen,cellen);
    if (reg == nil) {
      return nil;
    }

    if ( (size_t)reg & 15) {
      error(Lalloc,"region %u at %p unaligned",rid,(void *)reg)
      return nil;
    }

    // maintain list for aging and stats
    if (hb->reglst == nil) hb->reglst = hb->regtrim = hb->regprv = reg;
    else {
      preg = hb->regprv;
      ycheck(nil,0,preg->typ != Rslab,"region %u typ %s",preg->id,regnames[preg->typ]);
      preg->nxt = reg;
      hb->regprv = reg;
    }
  } // new or not

  reg->hb = hb;

  reg->typ = typ;

  uid |= (ub8)hid << 32;
  bkt = bucket(uid);

  reg->hid = ohid;
  reg->id = rid;
  reg->uid = uid;
  reg->bucket = bkt;

  if (olen == 0) {
    ulen = doalign8(len,Pagesize);
    user = osmem(Fln,hid,ulen,"region base");
    if (user == nil) {
      return nil;
    }
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

  reg->order = (ub1)order;

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
static mpregion *newmpregion(heap *hb,size_t len)
{
  mpregion *reg,*trimreg,*preg,*trimpreg;
  ub4 rid;
  ub4 hid = hb->id;
  ub4 order,iter;
  size_t user,ouser,olen;
  void *p;
  yalstats *sp = &hb->stat;

  order = sizeof(size_t) * 8 - clzl(len) - Mmap_threshold;

  ycheck(nil,Lnone,order >=Vmbits - Mmap_threshold,"region len %zu` order %u",len,order)

  // reuse ?
  reg = hb->freempregs[order];
  preg = trimreg = trimpreg = nil;
  iter = 50;
  while (reg && --iter) {
    ycheck(nil,0,reg->typ != Rmmap,"region %u typ %s",reg->id,regnames[reg->typ]);
    if (len <= reg->len && 4 * len > reg->len) break; // suitable size
    else if (reg->len == 0 && trimreg == nil) { trimreg = reg; trimpreg = preg; }
    preg = reg;
    reg = reg->frenxt;
  }
  if (reg == nil && trimreg) {
    reg = trimreg; // reuse trimmed region
    preg = trimpreg;
  }

  if (reg == nil) { // new
    rid = (ub4)++hb->stat.newmpregions;

    reg = newmpregmem(hb);
    if (reg == nil) return nil;

    ydbg2(Lnone,"new xregion %zx %u.%u for size %zu from %zu",(size_t)reg,hid,rid,len,reg->len);

    if (hb->mpreglst == nil) { // maintain for stats and trim
      hb->mpreglst = hb->mpregtrim = hb->mpregprv = reg;
    } else {
      preg = hb->mpregprv;
      ycheck(nil,0,preg->typ != Rmmap,"region %u typ %s",preg->id,regnames[preg->typ]);
      preg->nxt = reg;
      hb->mpregprv = reg;
    }

    reg->hb = hb;
    reg->id = rid;
    reg->hid = hid;
    reg->order = order;
    reg->age = reg->aged = 0;
    Atomset(reg->set,2,Morel); // give it freed
    return reg;
  } else rid = reg->id;

  // reuse
  ycheck(nil,0,reg->hb != hb,"mpregion %u heap %zx vs %zx",rid,(size_t)reg->hb,(size_t)hb);
  sp->usempregions++;
  reg->age = reg->aged = 0;
  olen = reg->len;
  ouser = reg->user;
  ydbg2(Lnone,"use xregion %zx %u.%u for size %zu from %zu",(size_t)reg,hid,reg->id,len,olen);

  if (olen) {
    if (olen == len) setgregion(hb,(xregion *)reg,(size_t)ouser,olen,0); // remains identical
    else {
      setregion(hb,(xregion *)reg,(size_t)ouser,olen,0,Lalloc,Fln); // unmap old one
    }
  } else {
    Atomad(global_mapadd,1,Monone);
    p = osmmap(len);
    user = (size_t)p;
    if (user == 0) return nil;
    reg->user = user;
    reg->len = len;
    reg->order = order;
    Atomset(reg->set,2,Morel); // give it freed
  }
  reg->typ = Rnone;

  // remove from free reg list
  if (preg) {
    ycheck(nil,0,preg->typ != Rmmap,"region %u typ %s",preg->id,regnames[preg->typ]);
    preg->frenxt = reg->frenxt;
  } else hb->freempregs[order] = reg->frenxt;
  reg->frenxt = nil;

  return reg;
}

#undef Logfile
