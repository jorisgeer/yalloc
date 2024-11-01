/* slab.h - regions of fixed-size blocks

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Meetadata is stored separate from the user blocks - aka cells - and layed out as consecutive arrays of one word per cell.

  binset            - one atomic byte for bin allocation. 0 init 1 alloc 2 free. Used for alloced / freed admin and invalid free detect
  bin                 - one 32 bits word dependent on cell count. List of binpos cells, max celcnt. starts at binorg
  userlen          - one 16/32 bits word. requested aka net length. Absent for small cells
  tags                - optional one 16/32 bits word with callsite info.
*/

#define Logfile Fslab

#define Nocel Hi32

// determine suitable size for new slab, given it's sequence in its class. Higher seqs get exponentially larger ones.
static region *newslab(heap *hb,ub4 cellen,ub4 clas,ub4 claseq)
{
  ub4 order,addord,maxord,celord;
  region *reg;
  ub4 rid;
  size_t reglen,xlen;
  size_t metalen,metacnt;
  size_t cnt,acnt;
  static const ub2 addords[18] = { 0,1,1,2,2,3,3,4,4,5,5,6,6,7,8,9,10,11 };

  size_t binlen;
  size_t binorg;
  size_t lenorg,lenlen;
  size_t tagorg,taglen;
  size_t flnorg,flnlen;

  addord = claseq > 17 ? claseq - 6 : addords[claseq];

  celord = 31 - clz(cellen);
  if (cellen & (cellen - 1)) celord++;

  maxord = min(Vmbits - 2,Regorder);
  if (celord < 8) maxord -= (8 - celord); // limit meta and celcnt

  order = max(Minregion + addord,newregorder());
  // order = max(order,celord + 1);

  // if (celord > 12 && celord < 16 && order < 18) order += celord - 12;
  order = min(maxord,order);

  do {
    reglen = 1ul << order;

    if ( (cellen & (cellen - 1)) == 0) { // pwr2
      celord = ctz(cellen);
      cnt = (ub4)(reglen >> celord);
    } else {
      celord = 0;
      cnt = (ub4)(reglen / cellen);
      xlen = (size_t)cnt * cellen;
      if (order < maxord) {
        if (reglen - xlen > 65536) reglen = doalign8(xlen,Page);
      }
    }
    if (cnt < (2u << (claseq >> 2) ) ) {
      if (order >= maxord) break;
      order++;
    } else if (cnt >= Hi32 && order > Minregion) order--;
    else break;
  } while (1);

  ycheck(nil,Lalloc,cnt == 0,"cel cnt 0 for len %u",cellen)

  ydbg2(Fln,Lnone,"new slab clas %u order %u len %zu` seq %u",clas,order,reglen,claseq)

  // cell status is at start
  acnt = doalign8(cnt,4);
  binorg = acnt * sizeof(celset_t) / 4; // local bin

  binlen = doalign8(acnt,L1line / 4);
  lenorg = binorg + binlen;
  if (cellen <= Cel_nolen) lenlen = 0;
  else if (cellen <= Hi16) lenlen = acnt / 2;
  else lenlen = acnt;

  tagorg = lenorg + lenlen;
  taglen = Yal_enable_tag ? acnt : 0;

  flnorg = tagorg + taglen;
  flnlen = Yal_enable_check > 1 ? acnt : 0;

  metacnt = flnorg + flnlen;
  ycheck(nil,Lalloc,metacnt >= Hi30,"len %zu` metacnt %zu`",reglen,metacnt)

  metalen = metacnt * sizeof(ub4); // bytes

  reg = newregion(hb,order,reglen,metalen,cellen,Rslab); // may return a used larger one
  if (reg == nil) return nil;
  rid = reg->id;
  xlen = reg->len;

  ycheck(nil,Lalloc,xlen < reglen,"region %u len %zu` vs %zu`",rid,xlen,reglen)
  ycheck(nil,Lalloc,reg->metalen < metalen,"region %u metalen %zu vs %zu",rid,reg->metalen,metalen)

  ycheck(nil,Lalloc,xlen / cellen < cnt,"region %u cnt %zu vs %zu",rid,xlen / cellen,cnt)

  reg->cellen = cellen; // gross, as allocated
  reg->celcnt = (ub4)cnt;
  reg->celord = celord;
  reg->clas = clas;
  reg->claseq = claseq;

  reg->binorg = binorg;
  reg->lenorg = lenorg;
  reg->tagorg = taglen ? tagorg : 0; //coverity[DEADCODE]
  reg->flnorg = flnlen ? flnorg : 0; //coverity[DEADCODE]

  // reg->metacnt = metacnt;

  setregion(hb,(xregion *)reg,reg->user,xlen,1,Lalloc,Fln);
  return reg;
} // newslab

static ub4 slab_gettag(region *reg,ub4 cel)
{
  size_t tagorg = reg->tagorg;
  ub4 *meta = reg->meta;
  ub4 *tags;
  ub4 tag;

  if (tagorg == 0) return 0;
  tags = meta + tagorg;
  tag = tags[cel];
  return tag;
}

#if Yal_enable_check > 1
static void slab_putfln(region *reg,ub4 cel,ub4 fln)
{
  ub4 *meta = reg->meta;
  ub4 *flns = meta + reg->flnorg;

  if (reg->flnorg == 0) return;
  flns[cel] = fln;
}

static ub4 slab_getfln(region *reg,ub4 cel)
{
  ub4 *meta = reg->meta;
  ub4 *flns = meta + reg->flnorg;

  if (reg->flnorg == 0) return 0;
  return flns[cel];
  return 0;
}

  #define Getfln(reg,cel) slab_getfln(reg,cel)
  #define Putfln(reg,cel,fln) slab_putfln(reg,cel,fln);

#else
  #define Getfln(r,c) 0
  #define Putfln(r,c,f)
#endif

/* mark cel as freed. Possibly called from remote.
   returns 1 on error
 */
static Hot bool markfree(region *reg,ub4 cel,ub4 cellen,celset_t to,ub4 fln,ub4 fretag)
{
  size_t ip;
  ub4 inipos;
  ub4 *meta = reg->meta;
  ub4 altag;
  _Atomic celset_t *binset;
  celset_t from;
  bool didcas;
  ub4 cfln;

  binset = (_Atomic celset_t *)meta;

// check and mark bin
  from = 1;
  didcas = Casa(binset + cel,&from,to);

  if (likely(didcas != 0)) {
    vg_mem_noaccess(reg->user + (size_t)cel * cellen,cellen)
    Putfln(reg,cel,fln)
    return 0;
  }

  cfln = Getfln(reg,cel);
  do_ylog(0,Lfree,cfln,Info,0,"reg %.01llu cel %u",reg->uid,cel);

   // double free (user error) or never allocated (user or yalloc error)
  ip = reg->user + (size_t)cel * cellen;
  inipos = reg->inipos;
  altag = slab_gettag(reg,cel);
  if (unlikely(cel >= inipos)) {
    errorctx(fln,Lfree,"region %.01llu ptr %zx cel %u fretag %.01u 1 -> %u = %u",reg->uid,ip,cel,fretag,to,from)
    error2(Lfree,Fln,"region %.01llu invalid free(%zx) of size %u - never allocated - cel %u above %u altag %.01u",reg->uid,ip,cellen,cel,inipos,altag)
    return 1;
  }
  if (from == 2 || from == 3) {
    errorctx(fln,Lfree,"region %.01llu ptr %zx cel %u is already binned - 1 -> 2 = %u altag %.01u",reg->uid,ip,cel,from,altag)
    free2(Fln,Lfree,(xregion *)reg,ip,cellen,fretag,"slab-bin");
  } else {
    errorctx(fln,Lfree,"gen %u.%u.%u age %u.%u",reg->gen,reg->hid,reg->id,reg->age,reg->aged)
    error2(Lfree,Fln,"region %.01llu cel %u invalid free(%zx) of size %u tag %.01u - expected status 1, found %u",reg->uid,cel,ip,cellen,fretag,from)
  }
  return 1;
}

// as above, just check
static Hot celset_t slab_chkfree(region *reg,ub4 cel)
{
  ub4 *meta = reg->meta;
  _Atomic celset_t *binset;
  celset_t from;

  binset = (_Atomic celset_t *)meta;

// check bin
  from = Atomgeta(binset + cel,Moacq);
  return from;
}

/* Mark cel as used aka allocated. Local only
   returns 1 on error
 */
static Hot bool slab_markused(region *reg,ub4 cel,celset_t from,ub4 fln)
{
  size_t ip;
  ub4 inipos,cellen;
  ub4 *meta = reg->meta;
  celset_t expect = from;
  _Atomic celset_t *binset;
  celset_t to;
  bool didcas;
  ub4 cfln;

  binset = (_Atomic celset_t *)meta;

// check and mark bin
  to = 1;
  didcas = Casa(binset + cel,&expect,to);

  if (likely(didcas != 0)) {
    Putfln(reg,cel,fln)
    return 0;
  }

  cfln = Getfln(reg,cel);
  do_ylog(0,Lfree,cfln,Info,0,"reg %.01llu cel %u",reg->uid,cel);

  // still allocated (yalloc internal error)
  cellen = reg->cellen;
  ip = reg->user + (size_t)cel * cellen;
  inipos = reg->inipos;
  if (cel >= inipos) {
    errorctx(fln,Lalloc,"region %.01llu gen %u.%u ptr %zx cel %u is not freed earlier %u -> %u = %u",reg->uid,reg->gen,reg->id,ip,cel,from,to,expect)
    error2(Lfree,Fln,"region %.01llu invalid alloc(%zx) of size %u - cel %u >= ini %u",reg->uid,ip,cellen,cel,inipos)
    return 1;
  }
  errorctx(fln,Lalloc,"region %u gen %u cel %u/%u is not freed earlier %u -> %u = %u",reg->id,reg->gen,cel,reg->celcnt,from,to,expect)
  error2(Lfree,Fln,"region %.01llu invalid alloc(%zx) of size %u - cel %u/%u bin %u is not free %u",reg->uid,ip,cellen,cel,reg->celcnt,reg->binpos,reg->prvcel)
  return 1;
}

static ub4 slab_getlen(region *reg,ub4 cel,ub4 cellen)
{
  ub4 *meta = reg->meta;
  size_t lenorg = reg->lenorg;
  ub4 *len4;
  ub2 *len2;

  if (cellen <= Hi16) {
    len2 = (ub2 *)(meta + lenorg);
    return len2[cel];
  } else {
    len4 = (ub4 *)(meta + lenorg);
    return len4[cel];
  }
}

// get checked cel from ptr. Can be called from remote.
static Hot ub4 slab_cel(region *reg,size_t ip,ub4 cellen,ub4 celcnt,enum Loc loc)
{
  size_t base,ofs8;
  ub4 ord;
  ub4 cel;

  base = reg->user;
  ord = reg->celord;

  ycheck(Nocel,loc,ip < base,"ptr %zx of size %u outside reg %x",ip,cellen,reg->id)

  ofs8 = ip - base;

  if (ord) cel = (ub4)(ofs8 >> ord);
  else cel = (ub4)(ofs8 / cellen);

  if (unlikely(cel >= celcnt)) {
    error(loc,"ptr %zx of size %u is %u blocks beyond reg %x of %u blocks",ip,reg->cellen,cel - celcnt,reg->id,celcnt)
    return Nocel;
  }

  if (unlikely( (size_t)cel * cellen != ofs8)) { // possible user error: inside block
    ub4 ulen = cellen <= Cel_nolen ? cellen : slab_getlen(reg,cel,cellen);
    if (cel * cellen < ofs8) {
      errorctx(Fln,loc,"ofs8 %zx vs %zx in %zu`/%zu`",ofs8,(size_t)cel * cellen,reg->len,reg->metalen)
      error2(loc,Fln,"ptr %zx of size %u/%u is %zu` b inside block %u/%u` region %.01llu %u",ip,cellen,ulen,(ip - base)  - (size_t)cel * cellen,cel,celcnt,reg->uid,ord)
    } else {
      error(loc,"ptr %zx of size %u/%u is %zu` b inside block %u/%u` region %.01llu %u",ip,cellen,ulen,((size_t)cel * cellen) - (ip - base) ,cel,celcnt,reg->uid,ord)
    }
    return Nocel;
  }

  return cel;
}

// alloc from remote bin
static ub4 slab_remalloc(region *reg)
{
  ub4 pos,rpos;
  ub4 *bin,*rbin;
  ub4 c,cel,celcnt;
  ub4 cfln;
  bool didcas;

  ub4 *meta;
  _Atomic celset_t *binset;
  celset_t from;

  rbin = Atomget(reg->rembin,Moacq);
  if (rbin == nil) return Nocel;

  rpos = reg->rbinpos;
  if (rpos == 0) return Nocel;
  reg->rbinpos = 0; // empty remote bin

  meta = reg->meta;

  bin = meta + reg->binorg;
  pos  = reg->binpos;
  celcnt = reg->celcnt;

  ycheck(Nocel,Lalloc,pos + rpos > celcnt,"bin pos %u + %u above %u",pos,rpos,celcnt)

  // copy all to local bin
  binset = (_Atomic celset_t *)meta;

  for (c = 0; c < rpos; c++) {
    cel = rbin[c];

    ycheck(Nocel,Lalloc,cel >= celcnt,"bin pos %u + %u above %u",pos,rpos,celcnt)
    ycheck(Nocel,Lalloc,cel >= reg->inipos,"cel %u above ini %u",cel,reg->inipos)

    // alloc topmost
    if (c == rpos - 1) {
      from = 3;
      didcas = Casa(binset + cel,&from,1);

      if (likely(didcas != 0)) {
        ystats2(reg->stat.rfrees,rpos)
        ystats(reg->stat.xallocs)
        reg->binpos = pos;
        return cel;
      }
      cfln = Getfln(reg,cel);
      errorctx(cfln,Lalloc,"pos %u/%u",c,rpos)
      error2(Lalloc,Fln,"reg %.01llu cel %u is not free %u",reg->uid,cel,from);
      return Nocel;
    }

    // move others to local bin
    from = 3;
    didcas = Casa(binset + cel,&from,2);

    if (unlikely(didcas == 0)) {
      cfln = Getfln(reg,cel);
      errorctx(cfln,Lalloc,"pos %u/%u",c,rpos)
      error2(Lalloc,Fln,"reg %.01llu cel %u is not free %u",reg->uid,cel,from);
      return Nocel;
    }
    Putfln(reg,cel,(Fln))
    bin[pos++] = cel;
  }
  return Nocel;
}

// Add cel to remote bin. Already marked. Region locked
static ub4 cel2rbin(heap *hb,region *reg,ub4 cel,enum Loc loc)
{
  ub4 celcnt;
  ub4 pos,rpos;
  ub4 *bin,*from;
  celset_t set;
  bool didcas;

  celcnt = reg->celcnt;

  bin = Atomget(reg->rembin,Moacq);
  if (unlikely(bin == nil)) {
    if (hb) bin = getrbinmem(hb,celcnt);
    else bin = bootalloc(Fln,0,Lrfree,celcnt * 4);
    if (bin == nil) return 1;
    from = nil;
    didcas = Cas(reg->rembin,from,bin);
    if (didcas == 0) {
      bin = from;
      if (hb) putrbinmem(hb,celcnt);
    }
  }

  pos = reg->binpos;
  rpos = reg->rbinpos;
  if (unlikely(pos + rpos >= celcnt)) { // overfull, should never occur
    error(loc,"region %.01llu bin cel %u to remote pos %u + %u above %u",reg->uid,cel,pos,rpos,celcnt)
    return 1;
  }

#if Yal_enable_check
  set = slab_chkfree(reg,cel);
  if (set != 3) error(loc,"region %.01llu cel %u/%u is not free %u",reg->uid,cel,celcnt,set);
#endif
  bin[rpos] = cel;

  reg->rbinpos = rpos + 1;

  return 0;
}

// Add cels to remote bin. Already marked.
static ub4 cels2rbin(heap *hb,ub4 *bin,region *reg,ub4 cnt,enum Loc loc)
{
  ub4 cel,c,celcnt;
  ub4 pos,rpos;
  ub4 *rbin,*frbin;
  ub4 cfln;
  celset_t set;
  ub4 from;
  bool didcas;

  from = 0; didcas = Cas(reg->lock,from,1);
  if (didcas == 0) return 1;
  vg_drd_wlock_acq(reg)
  celcnt = reg->celcnt;

  rbin = Atomget(reg->rembin,Moacq);
  if (unlikely(rbin == nil)) {
    rbin = getrbinmem(hb,celcnt);
    if (rbin == nil) {
      Atomset(reg->lock,0,Morel);
      vg_drd_wlock_rel(reg)
      return cnt;
    }
    frbin= nil;
    didcas = Cas(reg->rembin,frbin,rbin);
    if (didcas == 0) {
      rbin = frbin;
      putrbinmem(hb,celcnt);
    }
  }

  pos = reg->binpos;
  rpos = reg->rbinpos;
  if (unlikely(pos + rpos + cnt > celcnt)) { // overfull, should never occur
    error(loc,"region %.01llu bin cel %u to remote pos %u + %u + %u above %u",reg->uid,*bin,pos,rpos,cnt,celcnt)
    Atomset(reg->lock,0,Morel);
    vg_drd_wlock_rel(reg)
    return cnt;
  }

#if Yal_enable_check
  for (c = 0; c < cnt; c++) {
    cel = bin[c];
    set = slab_chkfree(reg,cel);
    if (set != 3) {
      cfln = Getfln(reg,cel);
      errorctx(cfln,loc,"pos %u/%u",c,cnt)
      error2(loc,Fln,"pos %u/%u region %.01llu cel %u is not free %u",c,celcnt,reg->uid,cel,set);
    }
    rbin[rpos++] = cel;
  }
#else
  memcpy(rbin + rpos,bin,cnt * sizeof(ub4));
  rpos += cnt;
#endif

  reg->rbinpos = rpos;

  Atomset(reg->lock,0,Morel);
  vg_drd_wlock_rel(reg)

  return 0;
}

// free from other thread. Returns cel len. Region locked. hb may be nil
static ub4 slab_free_rreg(heapdesc *hd,heap *hb,region *reg,size_t ip,ub4 tag,enum Loc loc)
{
  struct rembuf *rb;
  struct remote *rem,*remp;
  ub4 cel,cellen,celcnt,cnt;
  region *xreg;
  ub4 hid,clas,seq,clasofs;
  Ub8 clasmsk,Clasmsk,clasmsks,seqmsk,Seqmsk,hidmsk,Hidmsk;
  yalstats *hs;
  size_t frees;
  ub4 ref;
  bool rv,all;

  cellen = reg->cellen;
  celcnt = reg->celcnt;

  cel = slab_cel(reg,ip,cellen,celcnt,loc);
  if (unlikely(cel == Nocel)) {
    return 0;
  }

  // mark
  rv = markfree(reg,cel,cellen,3,Fln,tag);
  if (unlikely(rv != 0)) {
    ypush(hd,Fln)
    return 0;
  }

  cel2rbin(hb,reg,cel,loc);

  if (hb == nil) {
    hd->stat.xfreebatch++;
    return cellen;
  }
  hs = &hb->stat;
  hs->xfreebatch1++;

  frees = hs->xfreebuf;
  if ( (frees & 0x3f) != 0x3f) return cellen;
  all = ( (frees & 0x3ff) == 0x3ff);

  // unbuffer earlier cells
  hidmsk = Hidmsk = hb->remask;
  if (hidmsk == 0) return cellen; // none

 do {
  hid = ctzl(hidmsk);
  ycheck(0,loc,hid >= Remhid,"hid %u",hid)
  rb = hb->rembufs[hid];
  rem = rb->rem;

  clasmsks = 0;
  for (clasofs = 0; clasofs <= Clascnt / 64; clasofs++) {
    if ( (clasmsk = Clasmsk = rb->clas[clasofs]) == 0) continue;
    // ydbg1(Fln,loc,"ofs %u clasmsk %lx",clasofs,clasmsk)
    do {
      clas = ctzl(clasmsk) + clasofs * 64;
      ycheck(0,loc,clas >= Clascnt,"ofs %u class %u",clasofs,clas)
      seqmsk = Seqmsk = rb->seq[clas];
      do {
        ycheck(0,loc,seqmsk == 0,"ofs %u class %u mask 0",clasofs,clas)
        seq = ctzl(seqmsk);
        seqmsk &= ~(1ul << seq);
        ycheck(0,loc,seq >= Clasregs,"class %u seq %u",clas,seq)
        remp = rem + clas * Clasregs + seq;
        cnt = remp->cnt;
        ycheck(0,loc,cnt == 0,"class %u seq %u pos 0",clas,seq)
        if (cnt == 1 && !all) continue;
        xreg = remp->reg;
        ycheck(0,loc,xreg->hid != hid,"hid %u vs %u",hid,xreg->hid)
        ycheck(0,loc,xreg->clas != clas,"class %u vs %u",clas,xreg->clas)
        ycheck(0,loc,xreg->claspos != seq,"class %u vs %u",seq,xreg->claspos)
        ref = Atomget(xreg->remref,Moacq);
        ycheck(0,0,ref  == 0,"ref %u %zx",ref,(size_t)remp)
        if (cels2rbin(hb,remp->bin,xreg,cnt,loc)) {
          ydbg2(Fln,loc,"busy region %.01llu from %u gen %u ref %-2u cnt %-2u hid %u clas %u seq %u %zx",xreg->uid,hb->id,xreg->gen,ref,cnt,hid,clas,seq,(size_t)xreg)
          continue;
        }
        ref = Atomsub(xreg->remref,1,Moacqrel);
        hs->xfreebatch += cnt;
        ydbg2(Fln,loc,"use  region %.01llu from %u gen %u ref %u cnt %u hid %u clas %u seq %u %zx",xreg->uid,hb->id,xreg->gen,ref,cnt,hid,clas,seq,(size_t)xreg)
        remp->cnt = 0;
        remp->reg = nil;
        Seqmsk &= ~(1ul << seq);
      } while (seqmsk); // each seq
      rb->seq[clas] = Seqmsk;
      clasmsk &= ~(1ul << clas);
      if (Seqmsk == 0) Clasmsk &= ~(1ul << clas);
    } while (clasmsk); // each clas
    rb->clas[clasofs] = Clasmsk;
    clasmsks |= Clasmsk;
  } // each clasofs
  hidmsk &= ~(1ul << hid);
  if (clasmsks == 0) Hidmsk &= ~(1ul << hid);
 } while (hidmsk); // each hid
  hb->remask = Hidmsk;

  return cellen;
}

// free from other thread. Returns cel len. Region not locked.
static ub4 slab_free_rheap(heapdesc *hd,heap *hb,region *reg,size_t ip,ub4 tag,enum Loc loc)
{
  struct remote *rem,*remp;
  struct rembuf *rb;
  ub4 *binp;
  ub4 pos;
  ub4 cel,cellen,celcnt;
  ub4 clasofs,clasbit;
  ub4 hid,clas,seq;
  ub4 ref;
  bool rv;

  ycheck(0,loc,hb == nil,"reg %u nil heap",reg->id)

  cellen = reg->cellen;
  celcnt = reg->celcnt;
  hid = reg->hid;

  cel = slab_cel(reg,ip,cellen,celcnt,loc);
  if (unlikely(cel == Nocel)) return 0;

  // mark
  rv = markfree(reg,cel,cellen,3,Fln,tag);
  if (unlikely(rv != 0)) {
    ypush(hd,Fln)
    return 0;
  }

  // create a remote buffer if not present
  if (unlikely(hid >= Remhid)) {
    hb->stat.xfreedropped++;
    return cellen;
  }
  if ( (rb = hb->rembufs[hid]) == nil) {
    rb = hb->rembufs[hid] = newrem(hb);
    if (unlikely(rb == nil)) return 0;
  }
  rem = rb->rem;

  hb->stat.xfreebuf++;
  yhistats(hb->stat.xmaxbin,hb->stat.xfreebuf - hb->stat.xfreebatch)

  // add to local buffer, unbuffer at next free with region lock
  clas = reg->clas;
  seq = reg->claspos; // indeed pos
  ycheck(0,loc,clas == 0 || clas >= Clascnt,"reg %u class %u",reg->id,clas)
  ycheck(0,loc,seq >= Clasregs,"reg %u class %u seq %u",reg->id,clas,seq)

  remp = rem + clas * Clasregs + seq;
  if ( (binp = remp->bin) == nil) {
    binp = remp->bin = getrbinmem(hb,celcnt);
  }
  pos = remp->cnt;
  ycheck(0,loc,pos >= celcnt,"reg %u pos %u above %u",reg->id,pos,celcnt)

  ydbg2(Fln,loc,"clas %u seq %u reg %.01llu pos %u hid %u clas %u seq %u",clas,seq,reg->uid,pos,hid,clas,seq)

  if (pos == 0) {
    ycheck(0,loc,remp->reg != nil,"region %.01llu from %u has empty bin reg %.01llu %zx",reg->uid,hb->id,remp->reg->uid,(size_t)remp)
    remp->reg = reg;
    ref = Atomad(reg->remref,1,Moacqrel);
    // ycheck(0,loc,ref != 0,"reg %.01llu from %u ref %u hid %u clas %u pos %u",reg->uid,hb->id,ref,hid,clas,seq)
    ycheck(0,loc,ref >= Remhid,"reg %.01llu.%u ref %u",reg->uid,reg->id,ref)
    ydbg2(Fln,loc,"add  region %.01llu from %u gen %u ref %-2u cnt 0  hid %u clas %u seq %u rem %zx reg %zx",reg->uid,hb->id,reg->gen,ref,hid,clas,seq,(size_t)remp,(size_t)reg)
  } else {
    if (remp->reg != reg) {
      do_ylog(hd->id,loc,Fln,Error,0,"reg %.01llu.%u vs bin reg %.01llu.%u",reg->uid,reg->id,remp->reg->uid,remp->reg->id);
      return cellen;
    }
    ycheck(0,loc,remp->reg != reg,"reg %.01llu.%u vs bin reg %.01llu.%u",reg->uid,reg->id,remp->reg->uid,remp->reg->id)
  }
  binp[pos] = cel;
  remp->cnt = pos + 1;

  // add to masks
  rb->seq[clas] |= (1ul << seq);
  clasofs = clas / 64;
  clasbit = clas & 63;
  rb->clas[clasofs] |= (1ul << clasbit);
  hb->remask |= (1ul << hid);

  return cellen;
}

/* returns ptr or 0 if no space
   aligned_alloc selects a cel from the never allocated pool that may leave a gap
   This gap is moved to the bin
 */
static Hot size_t slab_newalcel(region *reg,ub4 ulen,ub4 align,ub4 cellen Tagargt(tag) )
{
  size_t ip,base;
  ub4 c,cel,celcnt,ofs;
  size_t lenorg;
  ub4 inipos = reg->inipos;
  ub4 binpos = reg->binpos;
  ub4 *meta;
  ub4 *binp;
  bool rv;
  ub4 *len4;
  ub2 *len2;

  meta = reg->meta;
  base = reg->user;

  celcnt = reg->celcnt;
  cel = inipos;
  if (cel == celcnt) {
    reg->fln = Fln;
    return 0; // common if no cels are never allocated. Searching in the bin is too expensive
  }
  ip = base + (size_t)cel * cellen;
  ip = doalign8(ip,align);
  ofs = (ub4)(ip - base);
  ycheck(0,Lallocal,reg->celord == 0,"region %u cellen %u,cellen",reg->id,cellen)
  cel = ofs >> reg->celord;

  if (cel >= celcnt) {
    reg->fln = Fln;
    return 0;
  }
  ydbg3(Lallocal,"cel %u ini %u",cel,inipos);
  rv = slab_markused(reg,cel,0,Fln);
  reg->prvcel = cel;
  if (unlikely(rv != 0)) {
    reg->fln = Fln;
    return 0;
  }
  if (cel + 1 == celcnt) { ydbg3(Lallocal,"cel %u ini %u",cel,inipos) }

  reg->inipos = cel + 1;

  if (inipos < cel) { // -V1051 PVS-assign-check
    // move preceding cels in bin
    binp = meta + reg->binorg;
    for (c = inipos; c < cel; c++) {
      rv = slab_markused(reg,c,0,Fln);
      if (unlikely(rv != 0)) return Nocel;
      rv = markfree(reg,c,cellen,2,Fln,0);
      if (unlikely(rv != 0)) return Nocel;
      binp[binpos++] = c;
    }
    reg->binpos = binpos;
  }
  ystats(reg->stat.iniallocs)

#if Yal_enable_tag
  size_t tagorg = reg->tagorg;
  ub4 *tags = meta + tagorg;

  tags[cel] = tag;
#endif

  if (cellen <= Cel_nolen) return ip; // todo merge

  lenorg = reg->lenorg;
  if (cellen <= Hi16) {
    len2 = (ub2 *)(meta + lenorg);
    len2[cel] = (ub2)ulen;
  } else {
    len4 = meta + lenorg;
    len4[cel] = ulen;
  }

  return ip;
}

// returns cel or Nocel if full
static Hot ub4 slab_newcel(region *reg,enum Loc loc)
{
  ub4 cel,celcnt;
  ub4 pos,c;
  ub4 allocs;
  ub4 *meta;
  ub4 *bin;
  bool rv;
  size_t binallocs = reg->stat.binallocs;
  ub4 from;
  bool some,didcas;

  meta = reg->meta;
  celcnt = reg->celcnt;

  // remote bin

  // If local bin, fresh or other regions heve cels, only check remote periodically
  allocs = (ub4)(reg->stat.iniallocs + binallocs);
  some = sometimes(allocs,0x1f);
  if (unlikely(some)) {
    from = 0; didcas = Cas(reg->lock,from,1);
    if (didcas) {
      vg_drd_wlock_acq(reg)
      cel = slab_remalloc(reg);
      Atomset(reg->lock,0,Morel);
      vg_drd_wlock_rel(reg)
      if (cel != Nocel) return cel;
    }
  }

  pos = reg->binpos;
  // ycheck(0,Lnone,pos != 0,"region %.01llu len %u cnt %u bin %u above ini %u %u %u",reg->uid,reg->cellen,celcnt,pos,reg->inipos,reg->bpchk1,reg->bpchk2)
  if (pos) { // from bin
    ycheck(0,loc,pos > reg->inipos,"region %.01llu len %u cnt %u bin %u above %u",reg->uid,reg->cellen,celcnt,pos,reg->inipos)

    pos--;

    bin = meta + reg->binorg;
    ycheck(0,loc,(size_t)(bin + pos) >= reg->metautop,"bin pos %u above meta %zu",pos,reg->metautop)
    cel = bin[pos];

    if (cel == Nocel) {
      for (c = 0; c <= pos; c++) {
        cel = bin[c];
        do_ylog(0,loc,Fln,Info,0,"bin %u cel %u",c,cel);
      }
      error(loc,"reg %.01llu bin %u",reg->uid,pos)
    }
    reg->binpos = pos;
    bin[pos] = Nocel;

    ycheck(Nocel,loc,cel >= celcnt,"region %.01llu cel %u >= cnt %u",reg->uid,cel,reg->celcnt)
    reg->stat.binallocs = binallocs + 1;

    rv = slab_markused(reg,cel,2,Fln);
    if (likely(rv == 0)) return cel;
  } // bin

  // from initial slab
  cel = reg->inipos;
  if (unlikely(cel == celcnt)) {
    ydbg3(loc,"reg %u ini %u == cnt seq %zu",reg->id,celcnt,reg->stat.iniallocs)
    reg->fln = Fln;
    return Nocel;
  }

  rv = slab_markused(reg,cel,0,Fln);
  if (unlikely(rv != 0)) {
    ydbg2(Fln,loc,"region %u no mark",reg->id)
    reg->fln = Fln;
    return Nocel;
  }

  reg->inipos = cel + 1;
  reg->stat.iniallocs++;

  return cel;
}

// generic for malloc,calloc,aligned_alloc
static Hot void *slab_alloc(heapdesc *hd,region *reg,ub4 ulen,ub4 align,enum Loc loc,ub4 tag)
{
  ub4 cel,cellen;
  ub4 inipos;
  ub4 *meta;
  size_t lenorg;
  ub4 *len4;
  ub2 *len2;
  size_t ip;
  void *p;

  ypush(hd,Fln)

  ycheck(nil,loc,reg == nil,"nil reg len %u tag %.01u",ulen,tag)
  ypush(hd,Fln)
  cellen = reg->cellen;
  inipos = reg->inipos;
  ycheck(nil,loc,ulen == 0,"len %u tag %.01u",ulen,tag)
  ycheck(nil,loc,ulen > cellen,"len %u above %u",ulen,cellen)

  ycheck(nil,loc,reg->aged != 0,"region %.01llu age %u",reg->uid,reg->aged)

  ypush(hd,Fln)

  reg->age = 0;
  if (unlikely(loc == Lallocal && align > cellen )) {
    ystats(reg->stat.Allocs)
#if Yal_enable_stats >= 2
    ub4 abit = ctz(ulen);
    sat_inc(reg->stat.aligns + abit);
#endif
    ypush(hd,Fln)
    ip = slab_newalcel(reg,ulen,align,cellen Tagarg(tag) );
    vg_mem_undef(ip,ulen)
    return (void *)ip;
  } // malloc / calloc / aligned_alloc

  ypush(hd,Fln)
  cel = slab_newcel(reg,loc);
  if (unlikely(cel == Nocel)) {
    ydbg3(loc,"no cel in region %u seq %zu",reg->id,reg->stat.iniallocs)
    return nil;
  }

#if Yal_enable_tag
  meta = reg->meta;

  size_t tagorg = reg->tagorg;
  ub4 *tags = meta + tagorg;

  tags[cel] = tag;
#endif

  if (cellen > Cel_nolen) {
    meta = reg->meta;
    lenorg = reg->lenorg;
    if (cellen <= Hi16) {
      len2 = (ub2 *)(meta + lenorg);
      len2[cel] = (ub2)cellen;
    } else {
      len4 = meta + lenorg;
      len4[cel] = cellen;
    }
  }

  ip = reg->user + (size_t)cel * cellen;
  p = (void *)ip;

  ypush(hd,Fln)

#if 0 // no longer works with drd
  vg_p = vg_mem_isaccess( (char *)p,ulen);
  if (unlikely(vg_p == 0)) {
    cchar *state = vg_mem_isdef( (char *)p,ulen) ? "defined" : "undefined";
    error(loc,"vg: region %u %zx = malloc(%u) is %s ofs %zu` %x",reg->id,(size_t)p,ulen,state,vg_p - ip,*(char *)p)
    return nil;
  }
#endif

  if (likely(loc != Lcalloc)) {
    vg_mem_undef(p,ulen)
    return p;
  }

  // calloc
  ystats(reg->stat.callocs)
  ydbg2(Fln,loc,"calloc(%u) cel %u/%u ini %u seq %zu",ulen,cel,reg->celcnt,inipos,reg->stat.callocs)
  vg_mem_def(p,ulen)
  if (inipos != reg->inipos && reg->clr == 0) {

#if Yal_enable_check > 1
    char *cp = (char *)p;
    for (ub4 i = 0; i < ulen; i++) {
      if (cp[i]) {
        do_ylog(0,0,Fln,Warn,0,"reg %.01llu gen %u cel %u ofs %u '%x' clr %u ini %u/%u",reg->uid,reg->gen,cel,i,cp[i],reg->clr,inipos,reg->inipos);
        for (ub4 j = 0; j < min(ulen,256); j++) {
          do_ylog(0,0,Fln,Info,0,"reg %.01llu gen %u cel %u ofs %u '%x' clr %u ini %u/%u",reg->uid,reg->gen,cel,i,cp[j],reg->clr,inipos,reg->inipos);
        }
         error(loc,"reg %.01llu gen %u cel %u ofs %u '%x' clr %u ini %u/%u",reg->uid,reg->gen,cel,i,cp[i],reg->clr,inipos,reg->inipos)
       }
    }
#endif
    return p; // is already 0
  }
  ydbg2(Fln,loc,"calloc clear seq %zu",reg->stat.callocs)

  memset(p,0,ulen);
  return p;
}

// simpler for malloc only
static Hot void *slab_malloc(region *reg,ub4 ulen,ub4 tag)
{
  ub4 cel,cellen = reg->cellen;
  ub4 *meta;
  size_t lenorg;
  ub4 *len4;
  ub2 *len2;
  size_t ip;
  void *p;

  ycheck(nil,Lalloc,ulen == 0,"ulen %u tag %.01u",ulen,tag)
  ycheck(nil,Lalloc,ulen > cellen,"ulen %u above %u",ulen,cellen)

  ycheck(nil,Lalloc,reg->aged != 0,"region %.01llu age %u",reg->uid,reg->aged)

  reg->age = 0;

  cel = slab_newcel(reg,Lalloc);
  if (unlikely(cel == Nocel)) {
    ydbg3(Lalloc,"no cel in region %u seq %zu",reg->id,reg->stat.iniallocs)
    return nil;
  }

#if Yal_enable_tag
  meta = reg->meta;

  size_t tagorg = reg->tagorg;
  ub4 *tags = meta + tagorg;

  tags[cel] = tag;
#endif

  if (cellen > Cel_nolen) {
    meta = reg->meta;
    lenorg = reg->lenorg;
    if (cellen <= Hi16) {
      len2 = (ub2 *)(meta + lenorg);
      len2[cel] = (ub2)cellen;
    } else {
      len4 = meta + lenorg;
      len4[cel] = cellen;
    }
  }

  ip = reg->user + (size_t)cel * cellen;
  p = (void *)ip;

  vg_mem_undef(p,ulen)
  return p;
}

static bool slab_setlen(region *reg,ub4 cel,ub4 len)
{
  ub4 cellen = reg->cellen;
  size_t lenorg = reg->lenorg;
  ub4 *meta = reg->meta;
  ub4 *len4;
  ub2 *len2;

  ycheck(1,Lnone,len == 0,"ulen %u",len)
  ycheck(1,Lnone,len > cellen,"ulen %u above %u",len,cellen)

  if (reg->cellen <= Hi16) {
    len2 = (ub2 *)(meta + lenorg);
    len2[cel] = (ub2)len;
  } else {
    len4 = (ub4 *)(meta + lenorg);
    len4[cel] = len;
  }
  return 0;
}

// check and add to bin. local only
// returns bin size, thus 0 at error
static Hot ub4 slab_frecel(heap *hb,region *reg,ub4 cel,ub4 cellen,ub4 celcnt,ub4 tag)
{
  ub4 pos;
  ub4 *meta,*bin;
  bool rv;
  celset_t set;

  ycheck(0,Lfree,cel >= celcnt,"region %u cel %u above %u",reg->id,cel,celcnt)

  rv = markfree(reg,cel,cellen,2,Fln,tag);

  if (unlikely(rv != 0)) {
    ydbg1(Fln,Lfree,"reg %.01llu cel %u",reg->uid,cel);
    hb->stat.invalid_frees++; // error
    return 0;
  }

  pos = reg->binpos;
  ycheck(0,Lfree,pos >= celcnt,"region %u bin %u above %u",reg->id,pos,celcnt)

  meta = reg->meta;
  bin = meta + reg->binorg;
  ycheck(0,Lfree,(size_t)(bin + pos) >= reg->metautop,"bin pos %u above meta %zu",pos,reg->metautop)
  bin[pos] = cel;

  pos++;
  reg->binpos = pos;

  set = slab_chkfree(reg,cel);
  ycheck(0, Lfree,set != 2,"region %.01llu gen %u.%u.%u cel %u set %u",reg->uid,reg->gen,hb->id,reg->id,cel,set)
  reg->prvcel = cel;

  ystats(reg->stat.frees);
  if (pos == reg->inipos) {
    reg->age = 1; // becomes empty
    ydbg2(Fln,Lnone,"empty region %.01llu gen %u.%u.%u cellen %u",reg->uid,reg->gen,hb->id,reg->id,cellen);
  }

#if 0
  if (pos == 1) return pos;

  for (c = 0; c < pos; c++) {
    cel = bin[c];
    set = slab_chkfree(reg,cel);
    ycheck(0, Lfree,set != 2,"reg %u cel %u set %u",reg->id,cel,set)
  }
#endif

  return pos;
}

// check and add to bin. local only
// returns bin size, thus zero on error
static Hot ub4 slab_free(heap *hb,region *reg,size_t ip,ub4 cellen,ub4 celcnt,ub4 tag)
{
  ub4 cel,bincnt;

  cel = slab_cel(reg,ip,cellen,celcnt,Lfree);

  if (unlikely(cel == Nocel)) {
    ydbg1(Fln,Lfree,"reg %.01llu ptr %zx no cel",reg->uid,ip);
    hb->stat.invalid_frees++; // error
    return 0;
  }
  bincnt = slab_frecel(hb,reg,cel,cellen,celcnt,tag);

  return bincnt;
}

static void slab_reset(region *reg)
{
  ycheck0(Lnone,reg->uid == 0,"region %.01llu",reg->uid);
}

#undef Logfile
