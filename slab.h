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
  if (cellen > Cel_nolen) lenlen = acnt;
  else lenlen = 0;

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

  reg->rbininc = Rbinbuf;

  reg->cellen = cellen; // gross, as allocated
  reg->celcnt = (ub4)cnt;
  reg->celord = celord;
  reg->clas = clas;
  reg->claseq = claseq;

  reg->binorg = binorg;
  reg->lenorg = lenorg;
  reg->tagorg = Yal_enable_tag ? tagorg : 0;
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
  #define Getfln(r,c) Fln
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

  binset = (_Atomic celset_t *)meta;

// check and mark bin
  from = 1;
  didcas = Casa(binset + cel,&from,to);

  if (likely(didcas != 0)) {
    vg_mem_noaccess(reg->user + (size_t)cel * cellen,cellen)
    Putfln(reg,cel,fln)
    return 0;
  }

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
  error2(Lfree,Fln,"region %.01llu invalid alloc(%zx) of size %u - cel %u/%u bin %u is not free",reg->uid,ip,cellen,cel,reg->celcnt,reg->binpos)
  return 1;
}

// return user aka net len
static Hot ub4 slab_getlen(region *reg,ub4 cel,ub4 cellen)
{
  ub4 *meta = reg->meta;
  ub4 ulen;
  ub4 *lens = meta + reg->lenorg;

  ulen = lens[cel];
  ycheck(0,Lnone,ulen > cellen,"cel %u ulen %u above %u",cel,ulen,cellen)
  return ulen;
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
    ub4 ulen = cellen > Cel_nolen ? slab_getlen(reg,cel,cellen) : cellen;
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

#if Yal_enable_check
  ub4 ref = Atomget(reg->remref,Moacq);
  if (unlikely(ref == 0)) { error(Lalloc,"reg %.01llu ref %u",reg->uid,ref) }
  if (unlikely(rpos >= reg->rbinlen)) { error(Lalloc,"reg %.01llu rbin %u above %u",reg->uid,rpos,reg->rbinlen) }
#endif

  reg->rbinpos = 0; // empty remote bin

  meta = reg->meta;

  bin = meta + reg->binorg;
  pos = reg->binpos;
  celcnt = reg->celcnt;

  ycheck(Nocel,Lalloc,pos + rpos > celcnt,"bin pos %u + %u above %u",pos,rpos,celcnt)
  ycheck(Nocel,Lalloc,rpos > reg->rbinlen,"bin pos %u above %u",rpos,reg->rbinlen)

  // copy all but topmost to local bin
  binset = (_Atomic celset_t *)meta;

  for (c = 0; c < rpos - 1; c++) {
    cel = rbin[c];

    ycheck(Nocel,Lalloc,cel >= celcnt,"bin pos %u + %u cel %u above %u",pos,rpos,cel,celcnt)
    ycheck(Nocel,Lalloc,cel >= reg->inipos,"cel %u above ini %u",cel,reg->inipos)

    from = 3;
    didcas = Casa(binset + cel,&from,2);

    if (unlikely(didcas == 0)) {
      cfln = Getfln(reg,cel);
      errorctx(cfln,Lalloc,"pos %u/%u",c,rpos)
      error2(Lalloc,Fln,"reg %.01llu cel %u is not free %u",reg->uid,cel,from)
      return Nocel;
    }
    Putfln(reg,cel,(Fln))
    ycheck(Nocel,Lalloc,(size_t)(bin + pos) >= reg->metautop,"bin pos %u above meta %zu",pos,reg->metautop)
    bin[pos++] = cel;
    ycheck(Nocel,Lalloc,pos > celcnt,"bin pos %u + %u above %u",pos,rpos,celcnt)
  }
  reg->binpos = pos;
  ystats2(reg->stat.rfrees,rpos)

  // alloc topmost
  c = rpos - 1;
  cel = rbin[c];

  ycheck(Nocel,Lalloc,cel >= celcnt,"bin pos %u + %u cel %u above %u",pos,rpos,cel,celcnt)
  ycheck(Nocel,Lalloc,cel >= reg->inipos,"cel %u above ini %u",cel,reg->inipos)

  ref = Atomsub(reg->remref,1,Moacqrel);
  return cel;
}

// Add cels to remote bin. Already marked. have hb
static ub4 cels2rbin(heap *hb,ub4 *bin,region *reg,ub4 cnt,enum Loc loc)
{
  ub4 cel,c,celcnt,rcnt,inc;
  ub4 pos,rpos = reg->rbinpos;
  ub4 *rbin,*rbin2;

  if (rpos == 0) Atomad(reg->remref,1,Moacqrel);

  celcnt = reg->celcnt;

  rbin = Atomget(reg->rembin,Moacq);
  if (unlikely(rbin == nil)) {
    ycheck(1,loc,rpos != 0,"reg %.01llu pos %unil rbin",reg->uid,rpos)
    rcnt = doalign4(cnt + Rbinbuf,Rbinbuf);
  } else {
    rcnt = doalign4(rpos + cnt + Rbinbuf,Rbinbuf);
  }
  if (rbin == nil || rcnt > reg->rbinlen) {
    inc = reg->rbininc ;
    reg->rbininc = inc * 2;
    rcnt = max(rcnt,inc);
    rcnt = doalign4(rcnt + Rbinbuf,Rbinbuf );
    if (cnt > 1u << 16) { ydbg2(Fln,loc,"rbin %u -> %u",reg->rbinlen,rcnt) }
    rbin2 = getrbinmem(hb,rcnt);
    if (rbin2 == nil) {
      return cnt;
    }
    if (rpos && rbin) memcpy(rbin2,rbin,rpos * 4);
    rbin = rbin2;
    Atomset(reg->rembin,rbin,Morel);
    reg->rbinlen = rcnt;
  }

  pos = reg->binpos;

  if (unlikely(pos + rpos + cnt > celcnt)) { // overfull, should never occur
    error(loc,"region %.01llu bin cel %u to remote pos %u + %u + %u above %u",reg->uid,*bin,pos,rpos,cnt,celcnt)
    return cnt;
  }

#if Yal_enable_check
  for (c = 0; c < cnt; c++) {
    cel = bin[c];
    ycheck(cnt,loc,cel >= celcnt,"pos %u cel %u above cnt %u",c,cel,celcnt)
#if Yal_enable_check > 1
    celset_t set = slab_chkfree(reg,cel);
    if (set != 3) {
      cfln = Getfln(reg,cel);
      errorctx(cfln,loc,"pos %u/%u gen %u",c,cnt,reg->gen)
      error2(loc,Fln,"pos %u/%u region %.01llu cel %u is not free %u",c,celcnt,reg->uid,cel,set);
    }
#endif
    rbin[rpos++] = cel;
  }
#else
  memcpy(rbin + rpos,bin,cnt * sizeof(ub4));
  rpos += cnt;
#endif

  reg->rbinpos = rpos;

  return 0;
}

// unbuffer remote frees. Returns cells left
static size_t slab_unbuffer(heap *hb,enum Loc loc,ub4 frees)
{
  heap *xhb;
  struct rembuf *rb;
  struct remote *rem,*remp;
  ub4 cnt,pos;
  region *reg;
  ub4 hid,clas,seq,clasofs;
  Ub8 clasmsk,Clasmsk,clasmsks,seqmsk,Seqmsk,hidmsk,Hidmsk;
  yalstats *hs = &hb->stat;
  size_t bufs = hs->xfreebuf;
  size_t batch = hs->xfreebatch;
  size_t Unused left;
  size_t nocas=0;
  ub4 urv;
  ub4 from;
  bool didcas;
  bool effort = sometimes(frees,0xfff) || bufs - batch > 1024;

  ycheck(0,loc,bufs < batch,"frees %zu` batch %zu`",bufs,batch)
  left = bufs - batch;

  hidmsk = Hidmsk = hb->remask;
  ycheck(0,loc,hidmsk == 0,"no hid mask for frees %zu` - %zu` = %zu",bufs,batch,bufs - batch)

 do {
  hid = ctzl(hidmsk);
  ycheck(0,loc,hid >= Remhid,"hid %u",hid)

  rb = hb->rembufs[hid];
  ycheck(0,loc,rb == nil || rb->rem == nil,"hid %u nil rembuf for mask %lx",hid,hidmsk)
  rem = rb->rem;

  xhb = hb->remhbs[hid];
  ycheck(0,loc,xhb == nil,"hid %u nil heap",hid)
  ycheck(0,loc,xhb == hb,"hid %u equals heap",hid)

  from = 0; didcas = Cas(xhb->lock,from,1);
  if (didcas) rb->nocas = 0;
  else {
    nocas = ++rb->nocas;
    if (nocas > 100) { ydbg2(reg->fln,loc,"busy heap  %.u from %u no %zu",hid,hb->id,nocas) }
  }

  clasmsks = 0;
  for (clasofs = 0; clasofs <= Clascnt / 64; clasofs++) {
    clasmsk = Clasmsk = rb->clas[clasofs];
    if (clasmsk == 0) continue;
    // ydbg1(Fln,loc,"ofs %u clasmsk %lx",clasofs,clasmsk)
    do {
      clas = ctzl(clasmsk) + clasofs * 64;
      ycheck(0,loc,clas >= Clascnt,"ofs %u class %u",clasofs,clas)
      seqmsk = Seqmsk = rb->seq[clas];
      ycheck(0,loc,seqmsk == 0,"ofs %u class %u mask 0",clasofs,clas)
      do {
        seq = ctzl(seqmsk);
        seqmsk &= ~(1ul << seq);
        ycheck(0,loc,seq >= Clasregs,"class %u seq %u",clas,seq)
        remp = rem + clas * Clasregs + seq;
        pos = remp->pos;
        cnt = remp->cnt;
        ycheck(0,loc,pos == 0,"class %u seq %u pos 0 nil cnt",clas,seq)
        ycheck(0,loc,pos >= cnt,"class %u seq %u pos %u above %u",clas,seq,pos,cnt)
        reg = remp->reg;
        ycheck1(0,loc,remp->celcnt != reg->celcnt,"class %u seq %u cellen %u vs %u",clas,seq,remp->celcnt,reg->celcnt)
        ycheck(0,loc,pos > reg->celcnt,"class %u seq %u pos 0 nil cnt",clas,seq)
        ycheck1(0,loc,reg->hid != hid,"hid %u vs %u",hid,reg->hid)
        ycheck(0,loc,reg->clas != clas,"class %u vs %u",clas,reg->clas)
        ycheck(0,loc,reg->claspos != seq,"class %u vs %u",seq,reg->claspos)
        ycheck1(0,loc,reg->uid != remp->uid,"reg %.01llu.%u vs %.01llu",reg->uid,reg->id,remp->uid)
        if (pos < 4 && effort == 0) continue;

        if (didcas) {

          urv = cels2rbin(hb,remp->bin,reg,pos,loc);

          if (unlikely(urv)) {
            ydbg2(reg->fln,loc,"skip region %.01llu from %u gen %u cnt %-2u hid %u clas %u seq %u",reg->uid,hb->id,reg->gen,cnt,hid,clas,seq)
          }

        } else {
          if (nocas < Private_drop_threshold) {
            continue;
          } else {
            hb->stat.xfreedropped += pos; // Private heap may be abandoned -> drop
          }
        } // cas or not

        // done, empty
        batch += pos;
        ycheck(0,loc,bufs < batch,"frees %zu` batch %zu`",bufs,batch)
        remp->pos = 0;
        remp->reg = nil;
        remp->inc = Rbinbuf;
        Seqmsk &= ~(1ul << seq);
      } while (seqmsk); // each seq

      rb->seq[clas] = Seqmsk;
      clasmsk &= ~(1ul << clas);
      if (Seqmsk == 0) Clasmsk &= ~(1ul << clas);
    } while (clasmsk); // each clas
    rb->clas[clasofs] = Clasmsk;
    clasmsks |= Clasmsk;
  } // each clasofs

  if (didcas) Atomset(xhb->lock,0,Morel);
  hidmsk &= ~(1ul << hid);
  if (clasmsks == 0) Hidmsk &= ~(1ul << hid);

 } while (hidmsk); // each hid

  hb->remask = Hidmsk;
  hs->xfreebatch = batch;

  if (bufs - batch > 1ul << 16) { ydbg1(Fln,0,"left %zu from %zu nocas %zu",bufs - batch,left,nocas) }
  return bufs - batch;
}

// free from other thread. Returns cel len.
static ub4 slab_free_rheap(heapdesc *hd,heap *hb,region *reg,size_t ip,ub4 tag,enum Loc loc)
{
  heap *xhb;
  struct remote *rem,*remp;
  struct rembuf *rb;
  ub4 *binp,*bin2;
  ub4 pos;
  ub4 cel,cellen,celcnt,cnt,inc;
  ub4 clasofs,clasbit;
  ub4 hid,clas,seq;
  ub4 ref;
  size_t bufs,batch;
  bool rv;

  ycheck(0,loc,hb == nil,"reg %u nil heap",reg->id)

  cellen = reg->cellen;
  celcnt = reg->celcnt;
  hid = reg->hid;
  xhb = reg->hb;
  ycheck(0,loc,xhb->id != hid,"reg %u hid %u vs %u",reg->id,xhb->id,hid)

  cel = slab_cel(reg,ip,cellen,celcnt,loc);
  if (unlikely(cel == Nocel)) return 0;

  // mark
  rv = markfree(reg,cel,cellen,3,Fln,tag);
  if (unlikely(rv != 0)) {
    ypush(hd,loc,Fln)
    return 0;
  }

  // create a remote buffer if not present
  if (unlikely(hid >= Remhid)) {
    hb->stat.xfreedropped++; // should be very rare
    return cellen;
  }
  if ( (rb = hb->rembufs[hid]) == nil) {
    rb = hb->rembufs[hid] = newrem(hb);
    if (unlikely(rb == nil)) return 0;
  }
  rem = rb->rem;

  bufs = hb->stat.xfreebuf + 1;
  batch = hb->stat.xfreebatch;
  ycheck(0,loc,bufs < batch,"buffered %zu` batch %zu`",bufs,batch)
  yhistats(hb->stat.xmaxbin,bufs - batch)
  hb->stat.xfreebuf = bufs;

  // add to local buffer, unbuffer with trim
  clas = reg->clas;
  seq = reg->claspos; // indeed pos
  ycheck(0,loc,clas == 0 || clas >= Clascnt,"reg %u class %u",reg->id,clas)
  ycheck(0,loc,seq >= Clasregs,"reg %u class %u seq %u",reg->id,clas,seq)

  remp = rem + clas * Clasregs + seq;
  pos = remp->pos;
  ycheck(0,loc,pos >= celcnt,"reg %u pos %u above %u",reg->id,pos,celcnt)

  if ( (binp = remp->bin) == nil) {
    ycheck(0,loc,pos != 0,"reg %u pos %u",reg->id,pos)
    cnt = remp->inc = Rbinbuf;
  } else {
    cnt = doalign4(pos + Rbinbuf,Rbinbuf);
    ycheck(0,loc,pos > cnt,"reg %u pos %u above %u",reg->id,pos,cnt)
  }
  if (cnt > remp->cnt) {
    inc = remp->inc;
    remp->inc = inc * 2;
    cnt = max(inc,cnt);
    cnt = doalign4(cnt,Rbinbuf);
    if (cnt > 1u << 16) { ydbg1(Fln,loc,"rbin %u -> %u for celcnt %u frees %zu` batch %zu`",remp->cnt,cnt,celcnt,bufs,batch) }
    bin2 = getrbinmem(hb,cnt);
    if (bin2 == nil) return 0;
    if (pos && binp) memcpy(bin2,binp,pos * 4);
    binp = remp->bin = bin2;
    remp->cnt = cnt;
  }
  if (binp == nil) return 0;

  ycheck(0,loc,pos >= celcnt,"reg %u pos %u above %u",reg->id,pos,celcnt)

  ydbg2(Fln,loc,"clas %u seq %u reg %.01llu pos %u hid %u clas %u seq %u",clas,seq,reg->uid,pos,hid,clas,seq)

  if (pos == 0) {
    ycheck(0,loc,remp->reg != nil,"region %.01llu from %u has empty bin reg %.01llu %zx",reg->uid,hb->id,remp->reg->uid,(size_t)remp)
    remp->reg = reg;
    remp->uid = reg->uid;
    remp->celcnt = celcnt;
    ref = Atomget(reg->remref,Moacq);
    ywarn(loc,ref >= Remhid,"reg %.01llu.%u ref %u",reg->uid,reg->id,ref)
    ydbg2(Fln,loc,"add  region %.01llu from %u gen %u ref %-2u cnt 0  hid %u clas %u seq %u rem %zx reg %zx",reg->uid,hb->id,reg->gen,ref,hid,clas,seq,(size_t)remp,(size_t)reg)
  } else {
    if (unlikely(remp->reg != reg)) {
      if (remp->reg) do_ylog(hd->id,loc,Fln,Assert,0,"reg %.01llu.%u vs bin reg %.01llu.%u",reg->uid,reg->id,remp->reg->uid,remp->reg->id);
      else do_ylog(hd->id,loc,Fln,Assert,0,"reg %.01llu.%u nil bin",reg->uid,reg->id);
      return cellen;
    }
    ycheck1(0,loc,remp->celcnt != celcnt,"reg %.01llu.%u cellen %u vs %u",reg->uid,reg->id,celcnt,remp->celcnt)
    ycheck1(0,loc,remp->uid != reg->uid,"reg %.01llu.%u vs %.01llu",reg->uid,reg->id,remp->uid)
  }
  binp[pos] = cel;
  remp->pos = pos + 1;

  // add to masks
  rb->seq[clas] |= (1ul << seq);
  clasofs = clas / 64;
  clasbit = clas & 63;
  rb->clas[clasofs] |= (1ul << clasbit);
  hb->remask |= (1ul << hid);
  hb->remhbs[hid] = reg->hb;

#if 0
  if (bufs - batch < 256) return cellen;

  iter = 3;
  do {
    left = slab_unbuffer(hb,loc);
    if (left > 1ul << 21) ydbg1(Fln,loc,"unbuffer %zu",left)
  } while (left > 256 && --iter);
#endif

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
  ub4 inipos = reg->inipos;
  ub4 binpos = reg->binpos;
  ub4 *meta;
  ub4 *binp;
  bool rv;
  ub4 *len4;

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

  if (cellen <= Cel_nolen) return ip;

  len4 = meta + reg->lenorg;
  len4[cel] = ulen;

  return ip;
}

// returns cel or Nocel if full
static Hot ub4 slab_newcel(region *reg,enum Loc loc)
{
  ub4 cel,celcnt;
  ub4 pos,c;
  ub4 *meta;
  ub4 *bin;
  bool rv;
  size_t binallocs = reg->stat.binallocs;
  celset_t set;
  ub4 from;
  bool didcas;

  meta = reg->meta;
  celcnt = reg->celcnt;

  pos = reg->binpos;

  if (pos) { // from bin
    ycheck(0,loc,pos > reg->inipos,"region %.01llu len %u cnt %u bin %u above %u",reg->uid,reg->cellen,celcnt,pos,reg->inipos)

    pos--;

    bin = meta + reg->binorg;
    ycheck(0,loc,(size_t)(bin + pos) >= reg->metautop,"bin pos %u above meta %zu",pos,reg->metautop)
    cel = bin[pos];

    if (unlikely(cel == Nocel)) {
      for (c = 0; c <= min(pos,64); c++) {
        cel = bin[c];
        do_ylog(0,loc,Fln,Info,0,"bin %u cel %u",c,cel);
      }
      error(loc,"reg %.01llu bin %u",reg->uid,pos)
    }
    reg->binpos = pos;
    bin[pos] = Nocel;

    ycheck(Nocel,loc,cel >= celcnt,"region %.01llu cel %u >= cnt %u",reg->uid,cel,reg->celcnt)
    reg->stat.binallocs = binallocs + 1;
    set = 2;
  } else { // from ini
    cel = reg->inipos;
    if (unlikely(cel == celcnt)) { // full
      ydbg3(loc,"reg %u ini %u == cnt seq %zu",reg->id,celcnt,reg->stat.iniallocs)

      // check remote bin
      from = 0; didcas = Cas(reg->lock,from,1);
      if (didcas) {
        vg_drd_wlock_acq(reg)
        reg->fln = Fln;

        cel = slab_remalloc(reg);

        Atomset(reg->lock,0,Morel);
        vg_drd_wlock_rel(reg)
        if (cel == Nocel) {
          return Nocel;
        }
        ystats(reg->stat.xallocs)
        set = 3;
      } else { // nocas
        return Nocel;
      }
    } else { // ini full
      reg->inipos = cel + 1;
      reg->stat.iniallocs++;
      set = 0;
    }
  }

  rv = slab_markused(reg,cel,set,Fln);
  if (unlikely(rv != 0)) {
    ydbg2(Fln,loc,"region %u no mark",reg->id)
    reg->fln = Fln;
    return Nocel;
  }
  return cel;
}

// generic for malloc,calloc,aligned_alloc
static Hot void *slab_alloc( Unused heapdesc *hd,region *reg,ub4 ulen,ub4 align,enum Loc loc,ub4 tag)
{
  ub4 cel,cellen;
  ub4 inipos;
  ub4 *meta;
  ub4 *len4;
  size_t ip;
  void *p;

  ypush(hd,loc,Fln)

  ycheck(nil,loc,reg == nil,"nil reg len %u tag %.01u",ulen,tag)
  ypush(hd,loc,Fln)
  cellen = reg->cellen;
  inipos = reg->inipos;

  ycheck(nil,loc,ulen == 0,"len %u tag %.01u",ulen,tag)
  ycheck(nil,loc,ulen > cellen,"len %u above %u",ulen,cellen)
  ycheck(nil,loc,reg->aged != 0,"region %.01llu age %u.%u",reg->uid,reg->age,reg->aged)

  reg->age = 0;
  if (unlikely(loc == Lallocal && align > cellen )) {
    ystats(reg->stat.Allocs)
#if Yal_enable_stats >= 2
    ub4 abit = ctz(ulen);
    ub4 acnt = reg->stat.aligns[abit] & Hi31;
    reg->stat.aligns[abit] = acnt + 1;
#endif
    ypush(hd,loc,Fln)
    ip = slab_newalcel(reg,ulen,align,cellen Tagarg(tag) );
    vg_mem_undef(ip,ulen)
    return (void *)ip;
  } // aligned_alloc

  ypush(hd,loc,Fln)
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

    len4 = meta + reg->lenorg;
    len4[cel] = ulen;
  }

  ip = reg->user + (size_t)cel * cellen;
  p = (void *)ip;

  ypush(hd,loc,Fln)

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
    ub4 i,j;

    for (i = 0; i < ulen; i++) {
      if (cp[i]) {
        do_ylog(0,0,Fln,Warn,0,"reg %.01llu gen %u cel %u ofs %u '%x' clr %u ini %u/%u",reg->uid,reg->gen,cel,i,cp[i],reg->clr,inipos,reg->inipos);
        for (j = 0; j < min(ulen,256); j++) {
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
  ub4 *len4;
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

    len4 = meta + reg->lenorg;
    len4[cel] = ulen;
  }

  ip = reg->user + (size_t)cel * cellen;
  p = (void *)ip;

  vg_mem_undef(p,ulen)
  return p;
}

static bool slab_setlen(region *reg,ub4 cel,ub4 len)
{
  ub4 cellen = reg->cellen;
  ub4 *meta = reg->meta;
  ub4 *len4;

  ycheck(1,Lnone,len == 0,"ulen %u",len)
  ycheck(1,Lnone,len > cellen,"ulen %u above %u",len,cellen)

  len4 = meta + reg->lenorg;
  len4[cel] = len;

  return 0;
}

// check and add to bin. local only
// returns bin size, thus 0 at error
static Hot ub4 slab_frecel(heap *hb,region *reg,ub4 cel,ub4 cellen,ub4 celcnt,ub4 tag)
{
  ub4 pos;
  ub4 *meta,*bin;
  bool rv;

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

#if Yal_enable_check > 1
  celset_t set = slab_chkfree(reg,cel);
  ycheck(0, Lfree,set != 2,"region %.01llu gen %u.%u.%u cel %u set %u",reg->uid,reg->gen,hb->id,reg->id,cel,set)
#endif

  ystats(reg->stat.frees)
  if (pos == reg->inipos) {
    reg->age = 1; // becomes empty
    ydbg3(Fln,Lnone,"empty region %.01llu gen %u.%u.%u cellen %u %zu %zu",reg->uid,reg->gen,hb->id,reg->id,cellen,hb->stat.xfreebuf,hb->stat.xfreebatch);
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

static bool slab_reset(region *reg)
{
  ycheck(1,Lnone,reg->uid == 0,"region %.01llu",reg->uid)
  return 0;
}

#undef Logfile
