/* slab.h - regions of fixed-size blocks

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Meetadata is stored separate from the user blocks - aka cells - and layed out as consecutive arrays of one word per cell.

  binset            - one atomic byte for bin allocation. 0 init 1 alloc 2 free. Used for alloced / freed admin and invalid free detect
  bin                 - one 16/32 bits word dependent on cell count. List of binpos cells, max celcnt. starts at binorg
  remotebin     - as above, remote bin. atomic access
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
  size_t cnt,acnt,segcnt;
  static const ub2 addords[18] = { 0,1,1,2,2,3,3,4,4,5,5,6,6,7,8,9,10,11 };

  size_t binlen,rbinlen;
  size_t binorg,rbinorg;
  size_t lenorg,lenlen;
  size_t tagorg,taglen;

  addord = claseq > 17 ? claseq - 6 : addords[claseq];

  celord = 31 - clz(cellen);
  if (cellen & (cellen - 1)) celord++;

  maxord = min(Vmbits - 2,Regorder);
  if (celord < 9) maxord -= (9 - celord); // limit meta and celcnt

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

  ydbg2(Lnone,"new slab clas %u order %u seq %u",clas,order,claseq)

  // cell status is at start
  acnt = doalign8(cnt,4);
  binorg = acnt * sizeof(celset_t) / 4; // local bin

  binlen = doalign8(acnt,L1line / 4);

  rbinorg = binorg + binlen; // remote bins todo limit

  segcnt = doalign8(acnt + 2,L1line / 4);

  rbinlen = segcnt * Segment;

  lenorg = rbinorg + rbinlen;
  if (cellen <= Cel_nolen) lenlen = 0;
  else if (cellen <= Hi16) lenlen = acnt / 2;
  else lenlen = acnt;

  tagorg = lenorg + lenlen;
  taglen = Yal_enable_tag ? acnt : 0;
  metacnt = tagorg + taglen;
  ycheck(nil,Lalloc,metacnt >= Hi30,"len %zu` metacnt %zu`",reglen,metacnt)

  metalen = metacnt * sizeof(ub4); // bytes

  reg = newregion(hb,order,reglen,metalen,cellen,Rslab); // may return a used larger one
  if (reg == nil) return nil;
  rid = reg->id;
  xlen = reg->len;

  ycheck(nil,Lalloc,xlen < reglen,"region %u len %zu vs %zu",rid,xlen,reglen)
  ycheck(nil,Lalloc,reg->metalen < metalen,"region %u metalen %zu vs %zu",rid,reg->metalen,metalen)

  ycheck(nil,Lalloc,xlen / cellen < cnt,"region %u cnt %zu vs %zu",rid,xlen / cellen,cnt)

  cnt = (ub4)(xlen / cellen);

  reg->cellen = cellen; // gross, as allocated
  reg->celcnt = (ub4)cnt;
  reg->segcnt = (ub4)segcnt;
  reg->celord = celord;
  reg->clas = clas;
  reg->claseq = claseq;

  reg->binorg = binorg;
  reg->rbinorg = rbinorg;
  reg->lenorg = lenorg;
  reg->tagorg = Yal_enable_tag ? tagorg : 0;
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

/* mark cel as freed. Possibly called from remote.
   returns 1 on error
 */
static Hot bool slab_markfree(region *reg,ub4 cel,ub4 cellen,ub4 fln,ub4 fretag)
{
  size_t ip;
  ub4 inipos;
  ub4 *meta;
  ub4 altag;
  _Atomic celset_t *binset;
  celset_t from,to;
  bool didcas;

  meta = reg->meta;
  binset = (_Atomic celset_t *)meta;

// check and mark bin
  from = 1;
  to = 2;
  didcas = Casa(binset + cel,&from,to);

  if (likely(didcas != 0)) {
    vg_mem_noaccess((char *)reg->user + cel * cellen,cellen)
    return 0;
  }

  ydbg3(Lfree,"reg %.01llu cel %u",reg->uid,cel);

   // double free (user error) or never allocated (user or yalloc error)
  ip = reg->user + cel * cellen;
  inipos = reg->inipos;
  altag = slab_gettag(reg,cel);
  if (unlikely(cel >= inipos)) {
    errorctx(fln,Lfree,"region %.01llu ptr %zx cel %u fretag %.01u 1 -> %u = %u",reg->uid,ip,cel,fretag,to,from)
    error2(Lfree,Fln,"region %.01llu invalid free(%zx) of size %u - never allocated - cel %u above %u altag %.01u",reg->uid,ip,cellen,cel,inipos,altag)
    return 1;
  }
  if (from == 2) {
    errorctx(fln,Lfree,"region %.01llu ptr %zx cel %u is already binned - 1 -> 2 = %u altag %.01u",reg->uid,ip,cel,from,altag)
    free2(Fln,Lfree,(xregion *)reg,ip,cellen,fretag,"slab-bin");
  } else {
    error(Lfree,"region %.01llu invalid free(%zx) of size %u tag %.01u - expected status 1, found %u",reg->uid,ip,cellen,fretag,from)
  }
  return 1;
}

/* Mark cel as used aka allocated. Local only
   returns 1 on error
 */
static Hot bool slab_markused(region *reg,ub4 cel,celset_t from,ub4 fln)
{
  size_t ip;
  ub4 inipos,cellen;
  ub4 *meta;
  celset_t expect = from;
  _Atomic celset_t *binset;
  celset_t to;
  bool didcas;

  meta = reg->meta;
  binset = (_Atomic celset_t *)meta;

// check and mark bin
  to = 1;
  didcas = Casa(binset + cel,&expect,to);

  if (likely(didcas != 0)) {
    vg_mem_noaccess((char *)reg->user + cel * reg->cellen,cellen)
    return 0;
  }

  // still allocated (yalloc internal error)
  cellen = reg->cellen;
  ip = reg->user + cel * cellen;
  inipos = reg->inipos;
  if (cel >= inipos) {
    errorctx(fln,Lalloc,"region %.01llu ptr %zx cel %u is not freed earlier %u -> %u = %u",reg->uid,ip,cel,from,to,expect)
    error2(Lfree,Fln,"region %.01llu invalid alloc(%zx) of size %u - cel %u above %u",reg->uid,ip,cellen,cel,inipos)
    return 1;
  }
  errorctx(fln,Lalloc,"region %.01llu ptr %zx cel %u is not freed earlier %u -> %u = %u",reg->uid,ip,cel,from,to,expect)
  error2(Lfree,Fln,"region %.01llu invalid alloc(%zx) of size %u - cel %u/%u is not free",reg->uid,ip,cellen,cel,reg->celcnt)
  return 1;
}

static celset_t slab_chkused(region *reg,ub4 cel)
{
  ub4 *meta;
  _Atomic celset_t *binset;
  celset_t set;

  meta = reg->meta;
  binset = (_Atomic celset_t *)meta;

  set = Atomgeta(binset + cel,Moacq);
  return set;
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

  if (unlikely(cel * cellen != ofs8)) { // possible user error: inside block
    ub4 ulen = cellen <= Cel_nolen ? cellen : slab_getlen(reg,cel,cellen);
    if (cel * cellen < ofs8) {
      error(loc,"ptr %zx of size %u/%u is %zu` b inside block %u/%u` region %u %u",ip,cellen,ulen,(ip - base)  - (size_t)cel * cellen,cel,celcnt,reg->id,ord)
    } else {
      error(loc,"ptr %zx of size %u/%u is %zu` b inside block %u/%u` region %u %u",ip,cellen,ulen,((size_t)cel * cellen) - (ip - base) ,cel,celcnt,reg->id,ord)
    }
    return Nocel;
  }

#if Yal_enable_tag
  ytrace(loc,"slab_free(%zx) seq %zu tag %.01u",ip,reg->stat.frees,slab_gettag(reg,cel))
#endif

  return cel;
}

#if Yal_inter_thread_free
// alloc from remote bin
static ub4 slab_remalloc(region *reg)
{
  ub4 rpos = 0,pos;
  ub4 c,cel = Nocel,seg;
  ub4 *bin,*rbin;
  ub4 *segp;
  _Atomic ub4 *lockp;
  bool didcas,rv;
  ub4 zero;

  ub4 celcnt = reg->celcnt;
  ub4 segcnt = reg->segcnt;
  size_t rbinorg = reg->rbinorg;

  size_t binorg = reg->binorg;
  ub4 *meta = reg->meta;

  bin = meta + binorg;
  pos  = reg->binpos;

  for (seg = 0; seg < Segment; seg++) { // try each segment

    segp = meta + rbinorg + seg * segcnt; // includes lock, pos

    lockp = (_Atomic ub4 *)segp;

    zero = 0;
    didcas = Casa(lockp,&zero,1);
    if (unlikely(didcas == 0)) {
      ystats(reg->stat.rbinskip)
      continue;
    }

#if Yal_enable_check
    rpos = Atomgeta(lockp + 1,Moacq);
#else
    rpos = segp[1];
#endif

    if (rpos == 0) { // common: empty
      Atomseta(lockp,0,Morel);
      continue;
    }

    rbin = segp + 2;
    ycheck(Nocel,Lalloc,pos + rpos > celcnt,"bin pos %u + %u above %u",pos,rpos,celcnt) // todo check >=

    // copy all to local bin

#if Yal_enable_check
    for (c = 0; c < rpos; c++) {
      // cel = rbin[c];
      cel = Atomgeta(lockp + 2 + c,Moacq); // dev

      ycheck(Nocel,Lalloc,cel >= celcnt,"bin pos %u + %u above %u",pos,rpos,celcnt)
      // set = slab_chkused(reg,cel);
      // if (set != 2) error(Lalloc,"reg %.01llu cel %u is not free %u",reg->uid,cel,set);
      bin[pos++] = cel;
    }
    Atomseta(lockp + 1,0,Morel); // empty remote bin segment
#else
    memcpy(bin + pos,rbin,rpos * sizeof(ub4));
    pos += rpos;
    segp[1] = 0;
#endif

    ystats2(reg->stat.rfrees,rpos)

    Atomseta(lockp,0,Morel);

  } // foreach seg
  reg->rbincnt = 0;
  if (pos == reg->binpos) { // none
    return Nocel;
  }

  cel = bin[--pos];
  ystats(reg->stat.xallocs)
  ycheck(Nocel,Lalloc,cel >= reg->celcnt,"cel %u above cnt %u",cel,reg->celcnt)
  ycheck(Nocel,Lalloc,cel >= reg->inipos,"cel %u above ini %u",cel,reg->inipos)
  reg->binpos = pos;

  rv = slab_markused(reg,cel,2,Fln);
  if (unlikely(rv != 0)) return Nocel;

  return cel;
}

// add cels to remote bin. returns number of cels left
static ub4 add2rbin(heapdesc *hd,struct remote *rem,region *reg,ub4 bkt,ub4 cnt)
{
  ub4 b;
  ub4 segcnt,seg;
  ub4 cel;
  ub4 zero;
  ub4 hid = hd->id;
  ub4 pos;
  ub4 *bin;
  ub4 *remp;
  ub4 *meta,*segp;

  _Atomic ub4 *lockp;
  ub4 iter,iters = 5;
  bool didcas;
  struct hdstats *ds = &hd->stat;

  ycheck(cnt,Lfree,cnt > Rembatch,"cnt %u above %u",cnt,Rembatch)

  ycheck(cnt,Lfree,reg == nil,"nil reg cnt %u",cnt)
  ystats(ds->xfreebatch)
  meta = reg->meta;

  seg = (hid & (Segment - 1));
  segcnt = reg->segcnt;
  segp = meta + reg->rbinorg + seg * segcnt; // includes lock, pos
  lockp = (_Atomic ub4 *)segp;
  bin = segp + 2;

  remp = rem->remcels + bkt * Rembatch;

  iter = 0;
  do {

    // -- attempt lock --
    zero = 0;
    didcas = Casa(lockp,&zero,1);

    if (likely(didcas != 0)) { // common uncontended

      pos = Atomgeta(lockp + 1,Moacq);
      if (unlikely(pos + cnt > reg->celcnt)) { // overfull, should not occur
        Atomseta(lockp,0,Morel);
        error(Lfree,"region %.01llu bin cel %u to remote seg %u pos %u + %u above %u",reg->uid,*remp,seg,pos,cnt,reg->celcnt)
        return cnt;
      }

#if Yal_enable_check
      for (b = 0; b < cnt; b++) {
        cel = remp[b];
        // set = slab_chkused(reg,cel);
        // if (set != 2) error(Lalloc,"bkt %u pos %u/%u region %.01llu cel %u is not free %u",bkt,b,cnt,reg->uid,cel,set);

        Atomseta(lockp + 2 + pos + b,cel,Morel); // bin[pos++] = cel;
      }
#else
      memcpy(bin + pos,remp,cnt * sizeof(ub4));
#endif
      pos += cnt;
      ystats2(ds->xfreesum,cnt)
      Atomseta(lockp + 1,pos,Morel); // bump bin pos aka increment (sharded) stack ptr

      // -- unlock --
      Atomseta(lockp,0,Morel);

      if (likely(iter == 0)) {
        ds->locks++; // uncontended
        return 0;
      }
      break;
    } // didcas

    while (Atomgeta(lockp,Monone) && iter++ < 2) { Pause } // inspired by https://rigtorp.se/spinlock

  } while (iter++ < iters);

  ds->clocks++; // contended
  ds->maxspin = max(ds->maxspin,iter);
  ds->spinsum += iter;

  if (iter >= iters) {
    ds->nolocks++;
    ydbg2(Lrfree,"heap %u region %.01llu seg %u no lock in %u iters",hid,reg->uid,seg,iters);
    return cnt;
  }

  return 0;
}

// free from other thread. Returns cel len
static ub4 slab_free_remote(heapdesc *hd,region *reg,size_t ip,size_t len,enum Loc loc)
{
  struct remote *rem;
  ub4 hid = hd->id;
  ub8 uid,ouid,ruid;
  ub4 pos,xpos;
  ub4 cel,ocel,cellen,celcnt,cnt,rcnt;
  ub4 ovflen,xovflen,blen;
  struct overflow *ovf,*xovf,*novf;
  ub4 *remp;
  size_t frees;
  Ub8 bktmsk,bkt1msk,msk,bmsk;
  ub4 lo,lo0,bkt;
  celset_t set;
  region *oreg,*rreg;
  bool rv;

  cellen = reg->cellen;

  if (unlikely(len != 0)) {
    if (len > cellen) {
      error(loc,"free(%zx) of size %u has invalid len %zu",ip,cellen,len)
      return 0;
    }
  }

  celcnt = reg->celcnt;
  cel = slab_cel(reg,ip,cellen,celcnt,loc);
  if (unlikely(cel == Nocel)) {
    return 0;
  }

  // mark
  rv = slab_markfree(reg,cel,cellen,Fln,Fln);
  if (unlikely(rv != 0)) {
    ypush(hd,Fln)
    return 0;
  }

  // create a remote buffer if not present
  if ( (rem = hd->rem) == nil) {
    rem = hd->rem = newrem(hd);
    if (rem == nil) return 0;
  }

  frees = hd->stat.xfreebuf++;

  // common case: direct add
  uid = reg->uid;
  bktmsk = rem->bktmsk;
  bkt1msk = rem->bkt1msk;

  yhistats(hd->stat.xmaxbin,(ub4)(hd->stat.xfreebin - hd->stat.xfreesum))

  pos = rem->ovfpos;

  // expand ?
  ovflen = rem->ovflen;
  ycheck(Hi32,loc,pos > ovflen,"pos %u above %u",pos,ovflen)
  if (unlikely(pos == ovflen)) {
    if (pos == Ovfmax) {
      hd->stat.remote_dropped++;
      hd->stat.remote_dropbytes += reg->cellen;
      error(loc,"heap %u overflow bin full for reg %.01llu",hid,reg->uid)
      return cellen;
    }
    ovf = rem->ovfbin;
    blen = ovflen * sizeof(struct overflow);
    if (ovf == rem->ovfmem) {
      novf = osmmap(blen * 2);
      memcpy(novf,ovf,blen);
      Atomad(global_mapadd,1,Monone);
    } else  novf = osmremap(ovf,blen,blen * 2);
    rem->ovfbin = novf;
    rem->ovflen = ovflen * 2;
  }

  // first add to overflow

  ovf = rem->ovfbin + pos;
  ycheck(Hi32,Lnone,pos >= rem->ovflen,"pos %u above %u",pos,rem->ovflen)
  ovf->reg = reg;
  ovf->cel = cel;
  rem->ovfpos = ++pos;

  set = slab_chkused(reg,cel); // dbg
  if (set != 2) error(loc,"reg %.01llu cel %-3u is not free %u",uid,cel,set);

  yhistats(hd->stat.xmaxovf,pos)

  if (sometimes(frees,0x0f) ) { // then process overflow in batches
    xpos = 0;
    xovf = rem->xovfbin;
    if (pos <= Rembatch) lo = 0;
    else lo = pos - Rembatch;
    lo0 = lo;
    ovf = rem->ovfbin + lo;
    do {
      oreg = ovf->reg;
      if (oreg == nil) { ovf++; continue; }
      ocel = ovf->cel;
      ouid = oreg->uid;

      set = slab_chkused(oreg,ocel); // dbg
      if (set != 2) error(loc,"reg %.01llu.cel %-3u is not free %u",oreg->uid,ocel,set);

      bkt = oreg->bucket;
      msk = 1ul << bkt;
      bkt1msk |= msk;
      cnt = rem->remcnts[bkt];
      remp = rem->remcels + bkt * Rembatch;
      rreg = rem->remregs[bkt];
      ruid = rem->remuids[bkt];

      // empty: add
      if (cnt == 0) {
        rem->remregs[bkt] = oreg;
        rem->remuids[bkt] = ouid;
        remp[0] = ocel;
        rem->remcnts[bkt] = 1;

      // collide or full: postpone
      } else if (ouid != ruid || cnt == Rembatch) {
        ycheck(0,loc,rreg == nil,"nil reg at bkt %u",bkt)
        if (cnt == Rembatch) bktmsk |= msk;
        xovflen = rem->xovflen;
        xovf = rem->xovfmem;
        if (xpos >= xovflen) {
          if (rem->xovfbin == rem->xovfmem) {
            Atomad(global_mapadd,1,Monone);
            xovf = osmmap(xovflen * 2);
          } else  xovf = osmremap(rem->xovfbin,xovflen,xovflen * 2);
          rem->xovfbin = xovf;
          rem->xovflen = xovflen * 2;
        }
        xovf[xpos].reg = oreg;
        xovf[xpos].cel = ocel;
        xpos++;

      // add
      } else {
        ycheck(0,loc,rreg == nil,"nil reg at bkt %u",bkt)
        remp[cnt] = ocel;
        rem->remcnts[bkt] = (ub2)(cnt + 1);
        if (cnt == Rembatch - 1) bktmsk |= msk;
      }
      ovf++;
    } while (++lo < pos);

    if (xpos) { // leftovers
      ovf = rem->ovfbin + lo0;
      memcpy(ovf,xovf,xpos * sizeof(struct overflow));
      lo0 += xpos;
    }
    ycheck(0,loc,lo0 > pos,"lo0 %u above pos %u",lo0,pos)
    ystats2(hd->stat.xfreebin,pos - lo0);
    yhistats(hd->stat.xmaxbin,(ub4)(hd->stat.xfreebin - hd->stat.xfreesum))
    rem->ovfpos = lo0;

    if (lo0 > 200 || sometimes(frees,0x3f)) msk = bkt1msk;
    else msk =  bktmsk;
    while (msk) {
      bkt = ctzl(msk);
      bmsk = (1ul << bkt);
      msk &= ~bmsk;
      cnt = rem->remcnts[bkt];
      rreg = rem->remregs[bkt];
      ycheck(0,loc,rreg == nil,"nil reg at bkt %u uid %.01llu",bkt,rem->remuids[bkt])
      rcnt = add2rbin(hd,rem,rreg,bkt,cnt);
      rem->remcnts[bkt] = (ub2)rcnt;
      if (rcnt != Rembatch) bktmsk &= ~bmsk;
      if (likely(rcnt == 0)) { // common: emptied
        hd->stat.xslabfrees += cnt;
        rem->remregs[bkt] = nil;
        bkt1msk &= ~bmsk;
      }
    }
  }
  rem->bkt1msk = bkt1msk;
  rem->bktmsk = bktmsk;
  return cellen;
}

#endif // Yal_inter_thread_free

/* returns ptr or 0 if no space
   aligned_alloc selects a cel from the never allocated pool that may leave a gap
   This gap is moved to the bin
 */
static Hot size_t slab_newalcel(region *reg,ub4 align,ub4 cellen,ub4 tag)
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
  if (cel == celcnt) return 0; // common if no cels are never allocated. Searching in the bin is too expensive

  ip = base + cel * cellen;
  ip = doalign8(ip,align);
  ofs = (ub4)(ip - base);
  if (reg->celord) cel = ofs >> reg->celord;
  else cel = ofs / cellen;

  if (cel >= celcnt) return 0;

  ydbg3(Lallocal,"cel %u ini %u",cel,inipos);
  rv = slab_markused(reg,cel,0,Fln);
  if (unlikely(rv != 0)) return Nocel;

  if (cel + 1 == celcnt) { ydbg3(Lallocal,"cel %u ini %u",cel,inipos) }

  reg->inipos = cel + 1;

  if (inipos < cel) { // -V1051 PVS-assign-check
    // move preceding cels in bin
    binp = meta + reg->binorg;
    for (c = inipos; c < cel; c++) {
      rv = slab_markused(reg,c,0,Fln);
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
    len2[cel] = (ub2)cellen;
  } else {
    len4 = meta + lenorg;
    len4[cel] = cellen;
  }

  return ip;
}

// returns cel or Nocel if full
static Hot ub4 slab_newcel(region *reg)
{
  ub4 cel,celcnt;
  ub4 pos;
  ub4 *meta;
  ub4 *bin;
  bool rv;
  size_t binallocs = reg->stat.binallocs;
  bool some;

  meta = reg->meta;
  celcnt = reg->celcnt;

  // remote bin
#if Yal_inter_thread_free

  // If local bin, fresh or other regions heve cels, only check remote periodically
  some = sometimes( /* reg->stat.iniallocs + */ binallocs,0x1f);
  if (unlikely(some)) {
    cel = slab_remalloc(reg);
    if (cel != Nocel) {
      return cel;
    }
  }
#endif

  pos = reg->binpos;
  if (pos) { // from bin
    ycheck(0,Lnone,pos > reg->inipos,"region %.01llu len %u cnt %u free %u",reg->uid,reg->cellen,reg->celcnt,pos)

    pos--;

    bin = meta + reg->binorg;
    ycheck(0,Lfree,(size_t)(bin + pos) >= reg->metautop,"bin pos %u above meta %zu",pos,reg->metautop)
    cel = bin[pos];

    reg->binpos = pos;

    ycheck(Nocel,Lalloc,cel >= celcnt,"region %.01llu cel %u >= cnt %u",reg->uid,cel,reg->celcnt)
    reg->stat.binallocs = binallocs + 1;

    rv = slab_markused(reg,cel,2,Fln);
    if (likely(rv == 0)) return cel;
  } // bin

  cel = reg->inipos;
  if (unlikely(cel == celcnt)) {
    ydbg3(0,"reg %u ini %u == cnt seq %zu",reg->id,celcnt,reg->stat.iniallocs)
    return Nocel;
  }

  // from initial slab

  rv = slab_markused(reg,cel,0,Fln);
  if (unlikely(rv != 0)) {
    ydbg2(0,"region %u no mark",reg->id)
    return Nocel;
  }

  reg->inipos = cel + 1;

  ystats(reg->stat.iniallocs);

  return cel;
}

static Hot void *slab_alloc(region *reg,ub4 ulen,enum Loc loc,ub4 tag)
{
  ub4 cel,cellen = reg->cellen;
  ub4 inipos = reg->inipos;
  ub4 *meta;
  size_t lenorg;
  ub4 *len4;
  ub2 *len2;
  size_t ip;
  void *p;

  ycheck(nil,Lnone,ulen == 0,"ulen %u tag %.01u",ulen,tag)

  reg->age = 0;
  if (unlikely(loc != Lalloc)) {
    if (loc == Lcalloc) {
      ycheck(nil,Lnone,ulen > cellen,"ulen %u above %u",ulen,cellen)
      ystats(reg->stat.callocs)
    } else if (loc == Lallocal) {

#if Yal_enable_stats >= 2
      ub4 abit = ctz(ulen);
      reg->stat.Allocs++;
      sat_inc(reg->stat.aligns + abit);
#endif
      ip = slab_newalcel(reg,ulen,cellen,tag);
      return (void *)ip;
    } else if (loc == Lreal) {
    }
  } // malloc / calloc / aligned_alloc

  cel = slab_newcel(reg);
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

  ip = reg->user + cel * cellen;
  p = (void *)ip;

  ycheck(nil,Lalloc,ulen > cellen,"ulen %u above %u",ulen,cellen)
  if (vg_mem_isaccess(p,ulen)) {
    error(Lalloc,"vg: region %u %zx = malloc(%u) was allocated earlier",reg->id,(size_t)p,ulen)
    return nil;
  }

  vg_mem_undef(p,ulen)

  if (likely(loc != Lcalloc)) return p;

  if (inipos != reg->inipos && reg->clr == 0) return p; // is already 0

  vg_mem_def(p,ulen)
  memset(p,0,ulen);   // calloc()
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

  if (cel >= celcnt) return 0;

  rv = slab_markfree(reg,cel,cellen,Fln,tag);

  if (unlikely(rv != 0)) {
    ydbg1(Lfree,"reg %.01llu cel %u",reg->uid,cel);
    hb->stat.invalid_frees++; // error
    return 0;
  }

  pos = reg->binpos;
  ycheck(0,Lfree,pos >= celcnt,"region %u bin %u above %u",reg->id,pos,celcnt)

  meta = reg->meta;
  bin = meta + reg->binorg;
  ycheck(0,Lfree,(size_t)(bin + pos) >= reg->metautop,"bin pos %u above meta %zu",pos,reg->metautop)
  bin[pos] = cel;

  reg->binpos = ++pos;

  ystats(reg->stat.frees);
  if (pos == reg->inipos) reg->age = 1; // becomes empty
  return pos ;
}

// check and add to bin. local only
// returns bin size, thus zero on error
static Hot ub4 slab_free(heap *hb,region *reg,size_t ip,ub4 cellen,ub4 celcnt,ub4 tag)
{
  ub4 cel,bincnt;

  cel = slab_cel(reg,ip,cellen,celcnt,Lfree);

  if (unlikely(cel == Nocel)) {
    ydbg1(Lfree,"reg %.01llu ptr %zx no cel",reg->uid,ip);
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

// return remote bin size
static ub4 slab_rbincnt(region *reg)
{
  ub4 rid = reg->id;
  ub4 cnt = reg->celcnt;
  ub4 binpos = reg->binpos;
  ub4 pos,rbincnt = 0,bincnt;
  ub4 seg;
  ub4 *segp;
  _Atomic ub4 *lockp;
  ub4 segcnt = reg->segcnt;
  size_t rbinorg = reg->rbinorg;
  bool didcas;
  ub4 zero;

  ub4 *meta = reg->meta;

  ycheck(0,Lnone,binpos > reg->inipos,"region %u len %u cnt %u free %u/%u",rid,reg->cellen,cnt,reg->inipos,binpos)
  ycheck(0,Lnone,reg->metalen == 0,"region %u len %u cnt %u no meta",rid,reg->cellen,cnt)

  for (seg = 0; seg < Segment; seg++) { // try each segment

    segp = meta + rbinorg + seg * segcnt; // includes lock, pos

    lockp = (_Atomic ub4 *)segp;

    zero = 0;
    didcas = Casa(lockp,&zero,1);
    if (unlikely(didcas == 0)) {
      continue;
    }

    pos = segp[1];

    if (pos) {
      rbincnt += pos;
    }
    Atomseta(lockp,0,Morel);
  }

  bincnt = binpos + rbincnt;
  ycheck(0,Lnone,bincnt > reg->inipos,"region %u len %u bin %u+%u free %u",rid,reg->cellen,binpos,rbincnt,reg->inipos)
  return rbincnt;
}

#undef Logfile
