/* slab.h - regions of fixed-size blocks

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Layout of metadata :
  bin allocbits - one bit per cell set for bin entry
  allocbits        - one bit per cell set when allocated. Alternating words alloced and freed
  acceleratorA - one bit set per one full word of allocbits
  acceleratorB - one bit set per one full word of accel A
  runs                - one bit per cell set for run. Alternating words for next

  6  line
  12 A
  18 B
  24 C
  accel A,B,C bit set if allocated
*/

#undef Logfile
#define Logfile Fslab

#define Nocel hi32

#define Cel(cel,ofs,bit,msk) ofs = cel >> 6; bit = cel & 63; msk = 1ul << bit;

static region *newslab(heap *hb,ub4 cellen,ub4 userlen,ub2 clas,ub2 addcnt)
{
  ub4 order;
  region *reg;
  size_t reglen;
  size_t metalen;
  bool multi;
  ub4 cnt,hicel,cnt64,runlen,xtra;
  ub4 linlen,lena,lenb;
  ub4 linpos,apos,bpos,rpos,pos;
  ub4 ord,addord,maxord;
  void *user;
  ub8 msk,bit,*meta;
  ub4 rid;
  reg_t dirid,*regbins;
  ub2 regbinpos,rbpos;

  switch (addcnt) {
  case 0: case 1: addord = 0; break;
  case 2: case 3: addord = 1; break;
  case 4: case 5: addord = 2; break;
  case 6: addord = 3; break;
  case 7: addord = 4; break;
  case 8: addord = 5; break;
  case 9: addord = 6; break;
  case 10: addord = 8; break;
  case 11: addord = 10; break;
  case 12: addord = 12; break;
  case 13: addord = 14; break;
  case 14: addord = 15; break;
  default: addord = addcnt;
  }

  ord = ctz(cellen);
  maxord = min(Vmsize - 2,ord + 6 +  6 * Accel_cnt);

  order = max(Minregion + addord,newregorder());
  if (ord > 8) order += ord / 4;
  order = min(maxord,order);

  reglen = 1ul << order;

  if (cellen & (cellen - 1)) {
    cnt = (ub4)(reglen / cellen);
    ord = 0;
  } else { // pwr2
    cnt = (ub4)(reglen >> ord);
  }
  cnt64 = cnt >> 6;
  linlen = cnt64 + 1; // 1 bit per cel, allow overflow

  lena = (linlen >> 6) + 1;
  lenb = (lena >> 6) + 1;

  multi = (cellen > Multilen);
  if (multi) runlen = linlen * 2;
  else runlen = 0;

  linpos = linlen; // bin mask
  apos = linpos + linlen * 2; // even alloc, odd free
  bpos = apos + lena;
  rpos = bpos + lenb;

  metalen = (rpos + runlen) << 3; // bytes

  // increase metalen to accomodate reuse for other cel size
  if (order < 20) {
    if (cellen > 16) metalen *= 2;
  }
  metalen = max(metalen,0x2000);

  // search previously freed one to reuse
  reg = nil;
#if 0
  regbinpos = order < Maxregion ? hb->regbinpos[order] : 0;
  rbpos = regbinpos;
  while (rbpos) {
    regbins = hb->regbins + order * Regbin;
    dirid = regbins[rbpos--];
    ylog(Lalloc,"try reg bin %u ord %u at %u",dirid,order,rbpos);
    reg = hb->regs[dirid];
    rid = reg->id;
    if (reg->metalen >= metalen) { // reuse
      hb->regbinsum -= reglen;

      meta = reg->meta;
      user = reg->user;
      memset(meta,0,metalen);
      memset(reg,0,sizeof(region));
      reg->id = rid;
      reg->dirid = dirid;
      reg->meta = meta;
      reg->user = user;
      reg->hb = hb;

      for (i = rbpos; i < regbinpos; i++) regbins[i] = regbins[i+1];
      hb->regbinpos[order] = regbinpos - 1;
      break;
    }
  }
#endif
  if (reg == nil) {
    reg = newregion(hb,order,reglen,metalen,Rslab);
    if (reg == nil) return nil;
  }
  reg->uid = hb->stats.slab_cnt++;
  reg->frecnt = reg->cnt = cnt;
  reg->ucellen = userlen; // net
  reg->cellen = cellen; // gross, as allocated
  reg->celcnt = cnt;
  reg->runcnt = cnt64;
  reg->celord = (ub2)ord;
  reg->clas = clas;

  reg->lpos = linpos;
  reg->apos = apos;
  reg->bpos = bpos;
  reg->rpos = rpos;

  // length to clear / copy
  if (cellen <= Stdalign) reg->clen = userlen;
  else reg->clen = doalign(userlen,Stdalign);

  meta = reg->meta;
  msk = Full;
  xtra = cnt & 63;
  if (xtra) {
    bit = 1;
    do { msk &= ~bit; bit <<= 1; } while (--xtra);
  }
  meta[linpos + cnt64 * 2] = msk;

  return reg;
} // newslab

// preallocate a run of max 64 cells from one line entry. Return inverted mask or 0 if full. unchecked
static Hot ub8 slab_prealloc(heap *hb,region *reg,enum Loc loc)
{
  ub4 lpos,apos,bpos;
  ub4 cacc,bacc,aacc;
  ub4 lenc;
  ub4 ofs,ofs2;
  ub8 dmsk,cmsk,bmsk,amsk,almsk,fremsk;

  ub8 *meta = reg->meta;

  unsigned int dbit,cbit,bbit,abit;

  ystats(reg->stats.preallocs)

  lpos = reg->lpos;
  apos = reg->apos;
  bpos = reg->bpos;

  // search in freemap
  dmsk = reg->dmsk;
  if (unlikely(dmsk == Full)) return 0;
  dbit = ctzl(~dmsk);
  cmsk = reg->accc[dbit];
  cbit = ctzl(~cmsk);

  cacc = (dbit << 6);

  bacc = (cacc << 6) + cbit;
  bmsk = meta[bpos + bacc];
  bbit = ctzl(~bmsk);

  aacc = (bacc << 6) + bbit;
  ycheck(0,loc,apos + aacc >= reg->metacnt,"apos %u acc %u len %zu",apos,aacc,reg->metacnt)
  amsk = meta[apos + aacc];
  abit = ctzl(~amsk);

  ofs = (aacc << 6) + abit;
  ofs2 = ofs * 2;

#if Yal_enable_check
  if (ofs2 >= apos) {
    ylog(Lalloc,"apos %u bpos %u cbit %u bacc %u bbit %u aacc %u abit %u amask %llx bmask %llx",apos,bpos,cbit,bacc,bbit,aacc,abit,(ubl)amsk,(ubl)bmsk)
    error(hb->errmsg,Lalloc,"reg %x cellen %u ofs %u >= %u cnt %u amask %llx free %u",reg->id,reg->cellen,ofs2,apos,reg->cnt,(ubl)amsk,reg->frecnt)
    return 0;
  }
#endif

  // preallocate run of 64 cells by allocating from <ofs>mask>
  reg->preofs = ofs;

  // mark what to clear for calloc()
  if ((sb4)ofs > (sb4)reg->hiofs) {
    reg->hiofs = ofs;
    reg->clrlen = 0;
  }

  almsk = meta[lpos + ofs2];
  if (unlikely(almsk == Full)) {
    if (ofs < reg->runcnt) {
      *hb->errmsg = 0;
      errorctx(loc,hb->errmsg,"accelerators a %u.%u b %u.%u",aacc,abit,bacc,bbit)
      error2(hb->errmsg,loc,Fln,"region %x unexpected full run at pos %u/%u",reg->id,ofs,reg->runcnt)
    }
    return 0;
  }

  meta[lpos + ofs2] = Full;

  fremsk = meta[lpos + ofs2 + 1];
  meta[lpos + ofs2 + 1] = fremsk & ~almsk;

  // update accels
  amsk |= (1ul << abit);
  meta[apos + aacc] = amsk;
  if (unlikely(amsk == Full)) {
    bmsk |= (1ul << bbit);
    meta[bpos + bacc] = bmsk;
    if (bmsk == Full) {
      cmsk |= (1ul << cbit);
      reg->accc[cacc] = cmsk;
      if (cmsk == Full) reg->dmsk |= (1ul << dbit);
    }
  }

  return ~almsk;
} // prealloc

/* check to-be-freed pointer, add to bin mask and split runs
   ptr within block
   not allocated
   doubly freed
   in prealloc
   in bin
   part of multi-cell run
   returns run length, typically 1, 0 on error
 */
static Hot ub4 slab_prefree(heap *hb,region *reg,ub4 cel,ub4 cellen)
{
  size_t ibase,ip;
  ub8 msk,bitmsk,premsk,runmsk;
  ub4 lpos,rpos,pos;
  ub4 binpos;
  ub4 *bin;
  ub4 ofs,ofs2,run;
  ub2 bit;

  ub8 *meta;

#if Yal_enable_check
  if (cel >= reg->cnt) {
    ibase = (size_t)reg->user;
    ip = ibase + cel * cellen;
    ylog(Lfree,"base %p ord %u len %u",reg->user,reg->celord,cellen)
    error(hb->errmsg,Lfree,"ptr %zx of size %u at pos %u outside len %u reg %x",ip,reg->cellen,cel,reg->cnt,reg->id)
    return 0;
  }
#endif

  Cel(cel,ofs,bit,msk)

  if (ofs == reg->preofs) { // in prealloc. common for very short-lived blocks
    premsk = reg->premsk;
    if (unlikely( (premsk & msk) != 0)) {
      ibase = (size_t)reg->user;
      ip = ibase + cel * cellen;
      error(hb->errmsg,Lfree,"invalid free(%zx) of size %u - preallocated",ip,reg->cellen)
      return 0;
    }
  }

  meta = reg->meta;
  lpos = reg->lpos;

  ofs2 = ofs * 2;

  bitmsk = meta[lpos + ofs2];
  if ( unlikely( (bitmsk & msk) == 0)) { // not allocated, find out why
    ibase = (size_t)reg->user;
    ip = ibase + cel * cellen;
    *hb->errmsg = 0;
    errorctx(Lfree,hb->errmsg,"cel %u/%u len %u bit %u",cel,reg->cnt,cellen,bit);
    if (meta[lpos + ofs2 + 1] & msk) {
      free2(hb,Fln,Lfree,reg->id,(void *)ip,cellen,"slab");
    } else { error2(hb->errmsg,Lfree,Fln,"reg %x free(%zx) was not allocated",reg->id,ip) }
    return 0;
  }

  if (unlikely(reg->userun == 1)) {
    rpos = reg->rpos;

    runmsk = meta[rpos + ofs2];
    if (unlikely( (runmsk & msk) != 0)) { // is multi-cel run
      run = 0;
      if (ofs && (meta[rpos + ofs2 - 2] & msk) && (meta[rpos + ofs2 - 1] & msk) == 0) { // not first cel, in run and previous not last
        while (run < 64 && (meta[rpos + ofs2 + run * 2 + 1] & msk) == 0) run++;
        ibase = (size_t)reg->user;
        ip = ibase + cel * cellen;
        error(hb->errmsg,Lfree,"region %x ptr %zx has %u more cells within block",reg->id,ip,run)
        return 0;
      }
      bitmsk = meta[ofs];
      if ( unlikely( (bitmsk & msk) != 0)) {
        ibase = (size_t)reg->user;
        ip = ibase + cel * cellen;
        *hb->errmsg = 0;
        free2(hb,Fln,Lfree,reg->id,(void *)ip,cellen,"slab-bin");
        return 0;
      }
      while (run < 64 && (meta[rpos + ofs2 + run * 2 + 1] & msk) == 0) { // split
        meta[rpos + ofs2 + run * 2]  &= ~msk;
        meta[rpos + ofs2 + run * 2 + 1]  &= ~msk;
        meta[ofs + run] = bitmsk | msk;
        run++;
      }
      vg_mem_noaccess((void *)ip,cellen * run)
      return run;
    }
  } // userun

// check and mark bin
  bitmsk = meta[ofs];
  ylog(Lfree,"cel %u ofs %u bit %u msk %llx,%llx",cel,ofs,bit,(ubl)bitmsk,(ubl)msk);
  if ( unlikely( (bitmsk & msk) != 0)) {

    ibase = (size_t)reg->user;
    ip = ibase + cel * cellen;
    pos = 0;
    binpos = reg->binpos;
    bin = reg->bin;
    while (pos < binpos && bin[pos] != cel) pos++;
    *hb->errmsg = 0;
    errorctx(Lfree,hb->errmsg,"region %x ptr %zx is alredy binned at pos %u/%u",reg->id,ip,pos,binpos)
    free2(hb,Fln,Lfree,reg->id,(void *)ip,cellen,"slab-bin");
    return 0;
  }
  meta[ofs] = bitmsk | msk;
  vg_mem_noaccess((void *)ip,cellen)
  return 1;
} // prefree

static Hot void *slab_alloc(heap *hb,region *reg,ub4 acellen,enum Loc loc)
{
  void *p;
  ub4 ofs,lpos,runcnt,cel,celcnt,cellen = reg->cellen;
  ub4 frecnt;
  ub2 pos;
  ub8 premsk,binmsk,bitmsk,msk;
  ub4 bit;
  size_t ofs8,ip,ibase = (size_t)reg->user;
  ub8 *meta;

#if Yal_enable_check
  if (acellen > cellen) { error(hb->errmsg,Lalloc,"reg %u clas %u len %u below requested %u",reg->id,reg->clas,acellen,cellen) return nil; }
#endif

  pos = reg->binpos;
  if (pos) { // from bin
    cel = reg->bin[--pos];
    reg->binpos = pos;
    ycheck(nil,loc,cel >= reg->celcnt,"reg %x cel %u >= cnt %u",reg->id,cel,reg->celcnt)
    ip = ibase + cel * cellen;
    ystats(reg->stats.binallocs)

    Cel(cel,ofs,bit,msk)

    meta = reg->meta;
    binmsk = meta[ofs];
    ylog(loc,"msk %llx,%llx",(ubl)binmsk,(ubl)binmsk & ~msk);
    if (unlikely( (binmsk & msk) == 0)) {
      error(hb->errmsg,loc,"reg %x cel %u ofs %u bit %u bin %u",reg->id,cel,ofs,bit,pos+1)
      reg->errfln = Fln;
      reg->status = St_error;
      return nil;
    }
    meta[ofs] = binmsk & ~msk;

    ylog(loc,"reg %x alloc bin cel %u pos %u %zx ofs %u bit %u",reg->id,cel,pos+1,ip,ofs,bit);
    return (void *)ip;
  } // bin

  premsk = reg->premsk;
  frecnt = reg->frecnt;

  if (premsk == 0) {
    ofs = reg->preofs;
    runcnt = reg->runcnt;
    if (unlikely(ofs >= runcnt)) {
      lpos = reg->lpos;
      meta = reg->meta;
      if (meta[lpos + ofs * 2] == Full) {
        ylog(Lalloc,"reg %x binpos %u fre %u pre %llx",reg->id,pos,reg->frecnt,(ubl)premsk);
        return nil; // yalloc_heap() will retry at next region.
      }
    }
    premsk = slab_prealloc(hb,reg,loc); // preallocate one run of at most 64 cells if needed
    if (unlikely(premsk == 0)) {
      ofs = reg->preofs;
      if (ofs >= runcnt) {
        ylog(Lalloc,"reg %x binpos %u fre %u pre %llx",reg->id,pos,reg->frecnt,(ubl)premsk);
        return nil; // yalloc_heap() will retry at next region.
      }
      *hb->errmsg = 0;
      errorctx(loc,hb->errmsg,"free %u /%u",frecnt,reg->celcnt);
      error2(hb->errmsg,loc,Fln,"region %x unexpected full run at pos %u/%u",reg->id,ofs,reg->runcnt)
      reg->errfln = Fln;
      reg->status = St_error;
      return nil;
    }
  }

  ofs = reg->preofs;

#if Yal_enable_check
  if (premsk == 0) {
    error(hb->errmsg,loc,"region %x empty cell run at freecnt %u/%u pos %u/%u",reg->id,frecnt,reg->celcnt,ofs,reg->runcnt)
   reg->errfln = Fln;
   reg->status = St_error;
    return nil;
  }
#endif
  ystats(reg->stats.allocs)

  // return one cell from preallocated run
  bit = ctzl(premsk);
  bitmsk = 1ul << bit;

  cel = (ofs << 6) | bit;

#if Yal_enable_check
  celcnt = reg->celcnt;
  if (cel >= celcnt) {
    ofs8 = cel * cellen;
    *hb->errmsg = 0;
    errorctx(loc,hb->errmsg,"ofs %u,%u/%u ofs8 %zu/%zu",ofs,reg->preofs,reg->runcnt,ofs8,(size_t)celcnt * cellen);
    error2(hb->errmsg,loc,Fln,"reg %x cel %u >= cnt %u msk %llx bit %u",reg->id,cel,celcnt,(ubl)premsk,bit)
    reg->errfln = Fln;
    reg->status = St_error;
    ip = ibase + ofs8;
    p = (void *)ip;
    return p;
  }
#endif

  ofs8 = cel * cellen;
  reg->premsk = premsk & ~bitmsk; // clear
  ip = ibase + ofs8;
  p = (void *)ip;

  reg->frecnt = frecnt - 1;

#if Yal_enable_check
  if (ip < ibase || ip + cellen > ibase + reg->len) { error(hb->errmsg,Lalloc,"ptr %zx of size %u outside reg %u",ip,cellen,reg->id) return p; }
#endif

  return p;
}

// Actual free of list of cels, typically a chunk from recycling bin
static enum Status slab_free(heap *hb,region *reg,ub4 *cels)
{
  ub4 binpos,binp2;
  size_t base,ip;
  ub8 msk,amsk,fmsk,cmsk;
  ub4 cellen;
  ub4 frecnt = Binful;
  ub4 cel;
  ub4 ofs,ofs1,ofs2;
  ub4 bit;

  ub8 *meta;

  unsigned int cbit,bbit,abit;
  ub4 lpos,apos,bpos,cpos;
  ub4 cacc,bacc,aacc;
  ub8 cmask,bmask,amask;
  enum Status rv = St_ok;

  base = (size_t)reg->user;

  cellen = reg->cellen;

  meta = reg->meta;

  lpos = reg->lpos;
  apos = reg->apos;

  cel = cels[0];

  Cel(cel,ofs,bit,cmsk)

  for (binpos = 1; binpos < Binful; binpos++) {
    cel = cels[binpos];

#if Yal_enable_check
    if (cel >= reg->cnt) {
      ylog(Lfree,"base %zx ord %u len %u",base,reg->celord,cellen)
      error(hb->errmsg,Lfree,"free(%zx) of size %u at pos %u outside len %u reg %x",base + cel * cellen,reg->cellen,cel,reg->cnt,reg->id)
      reg->errfln = Fln;
      return St_error;
    }
#endif

    Cel(cel,ofs1,bit,msk)

    ylog(Lfree,"free(%zx)  size %u cel %u",base + cel * cellen,reg->cellen,cel)

    if (ofs == ofs1 && binpos < Binful - 1) { // handle all bits in the same line run
      cmsk |= msk;
      continue;
    }

    ofs2 = ofs * 2;
    amsk = meta[lpos + ofs2];
    fmsk = meta[lpos + ofs2 + 1];
    if (unlikely( (amsk & cmsk) != cmsk)) { // not allocated. report

      for (bit = 0; bit < 63; bit++) {
        msk = (1ul << bit);
        if ( (msk & cmsk) == 0 || (msk & amsk) ) continue;
        cel = ofs * 2 + bit;
        ip = base + cel * cellen;
        if (fmsk & msk) {
          *hb->errmsg = 0;
          free2(hb,Fln,Lfree,reg->id,(void *)ip,reg->ucellen,"slab");
        } else error(hb->errmsg,Lfree,"ptr %zx of size %u was not allocated in region %x",ip,reg->ucellen,reg->id)
        reg->stats.invalidfrees++;
        reg->errfln = Fln;
        rv = St_error;
        cmsk &= ~msk;
      }
    }

    ycheck(0,Lfree,reg->frecnt + frecnt >= reg->cnt,"reg %u clas %u len %u frecnt %u",reg->id,reg->clas,reg->cellen,reg->cnt)

    // actual free
    meta[ofs] &= ~cmsk;
#if 0
    if (unlikely(reg->premsk == 0)) { // convert to prealloc if none
      reg->preofs = ofs;
      reg->premsk = ~amsk | cmsk;
      continue;
    }
#endif
    meta[lpos + ofs2] = amsk & ~cmsk;
    meta[lpos + ofs2 + 1] = fmsk | cmsk;

    // reg->linofs = ofs; // next alloc uses this block

    cmsk = msk;

    if (likely(amsk != Full) || cmsk == 0) {
      ofs = ofs1;
      continue;
    }

    // if full initially, update accels
    aacc = ofs >> 6;
    abit = ofs & 63;
    amask = meta[apos + aacc];
    meta[apos + aacc] = amask & ~ (1ul << abit);
    if (amask == Full) {
      bpos = reg->bpos;
      bacc = aacc >> 6;
      bbit = aacc & 63;
      ylog(Lfree,"bacc %u bbit %u",bacc,bbit);
      bmask = meta[bpos + bacc];
      meta[bpos + bacc] = bmask & ~ (1ul << bbit);
      if (bmask == Full) {
       cpos = reg->cpos;
        cacc = bacc >> 6;
        cbit = bacc & 63;
        cmask = meta[cpos + cacc];
        meta[cpos + cacc] = cmask & ~ (1ul << cbit);
      }
    }
    ofs = ofs1;
  } // each cel from bin

  reg->frecnt += frecnt;
  ystats2(reg->stats.frees,frecnt)

  return rv;
}

// get checked cel from ptr
static Hot ub4 slab_cel(heap *hb,region *reg,size_t ip,ub4 cellen,enum Loc loc)
{
  size_t base;
  ub2 ord;
  ub4 cel,celcnt;
  ub4 ofs8;

  base = (size_t)reg->user;
  ord = reg->celord;

  ycheck(Nocel,loc,ip < base,"ptr %zx of size %u outside reg %u",ip,reg->cellen,reg->id)

  ofs8 = (ub4)( ip - base);

  if (ord) cel = ofs8 >> ord;
  else cel = ofs8 / cellen;
  celcnt = reg->celcnt;

  if (unlikely(cel >= celcnt)) {
    error(hb->errmsg,loc,"ptr %zx of size %u is %u blocks beyond reg %u of %u blocks",ip,reg->cellen,cel - celcnt,reg->id,celcnt)
    return Nocel;
  }

  if (unlikely(cel * cellen != ofs8)) { // possible user error: inside block
    error(hb->errmsg,loc,"ptr %zx of size %u is %u bytes inside block %u region %x",ip,cellen,ofs8 - cel * cellen,cel,reg->id)
    return Nocel;
  }
  return cel;
}

// returns zero on error
static enum Status slab_real(heap *hb,region *reg,size_t ip,ub4 cellen,size_t newlen)
{
  ub4 cel;
  ub4 ofs,ofs2,bit;
  ub8 msk,bitmsk,runmsk,premsk;
  ub8 *meta = reg->meta;
  size_t ibase;
  ub4 run,havecnt,needcnt;
  ub4 rpos,lpos;

  cel = slab_cel(hb,reg,ip,cellen,Lreal);

  if (unlikely(cel == Nocel)) {
    hb->stats.invalid_reallocs++;
    reg->errfln = Fln;
    return St_error;
  }

#if Yal_enable_check
  if (cel >= reg->cnt) {
    ibase = (size_t)reg->user;
    ip = ibase + cel * cellen;
    ylog(Lreal,"base %p ord %u len %u",reg->user,reg->celord,cellen)
    error(hb->errmsg,Lreal,"ptr %zx of size %u at pos %u outside len %u reg %x",ip,reg->cellen,cel,reg->cnt,reg->id)
    reg->errfln = Fln;
    return St_error;
  }
#endif

  Cel(cel,ofs,bit,msk)

  if (ofs == reg->preofs) { // in prealloc. common for very short-lived blocks
    premsk = reg->premsk;
    if (unlikely( (premsk & msk) != 0)) {
      error(hb->errmsg,Lreal,"invalid ptr %zx of size %u - preallocated",ip,reg->cellen)
      reg->errfln = Fln;
      return St_error;
    }
  }

  meta = reg->meta;

  // check if allocated
  lpos = reg->lpos;
  ofs2 = ofs * 2;
  bitmsk = meta[lpos + ofs2];
  if ( unlikely( (bitmsk & msk) == 0)) { // not allocated, find out why
    char xbuf[256];

    ibase = (size_t)reg->user;
    ip = ibase + cel * cellen;
    *hb->errmsg = 0;
    errorctx(Lreal,hb->errmsg,"cel %u/%u len %u bit %u",cel,reg->cnt,cellen,bit)
    if (meta[lpos + ofs2 + 1] & msk) {
      free2(hb,Fln,Lfree,reg->id,(void *)ip,cellen,"slab");
    } else error2(hb->errmsg,Lreal,Fln,"region %x ptr %zx was not allocated",reg->id,ip)
    reg->errfln = Fln;
    return St_error;
  }

  // check bin
  bitmsk = meta[ofs];
  if ( unlikely( (bitmsk & msk) != 0)) {
    *hb->errmsg = 0;
    free2(hb,Fln,Lfree,reg->id,(void *)ip,cellen,"slab-bin");
    reg->errfln = Fln;
    return St_error;
  }

  rpos = reg->rpos;

  runmsk = meta[rpos + ofs2];
  if (unlikely( (runmsk & msk) != 0)) { // is multi-cel run
    run = 0;
    while (run < 64 && (meta[rpos + ofs2 + run * 2 + 1] & msk) == 0) run++;

    if (ofs && (meta[rpos + ofs2 - 2] & msk) && (meta[rpos + ofs2 - 1] & msk) == 0) { // not first cel, in run and previous not last
      error(hb->errmsg,Lfree,"region %x ptr %zx has %u more cells within block",reg->id,ip,run)
      reg->errfln = Fln;
      return St_error;
    }
  } else run = 1;

  needcnt = doalign(newlen,cellen);
  needcnt /= cellen;
  if (needcnt <= run) return St_ok; // large enough

  havecnt = run;
  do {
    if (meta[rpos + ofs2 + run * 2] & msk) return St_oom;
  } while (++havecnt < needcnt);

  // allocate run
  do {
    meta[rpos + ofs2 + run * 2] |= msk;
    meta[lpos + ofs2 + run * 2]  |= msk;
    meta[lpos + ofs2 + run * 2 + 1]  &= ~msk;
  } while (++run < needcnt);
  meta[rpos + ofs2 + run * 2 - 3] |= msk;
  return St_ok; // expanded in-place
}

// check and add to bin. free part of bin if full
// returns nonzero if empty
static Hot bool slab_bin(heap *hb,region *reg,size_t ip)
{
  ub4 cellen;
  ub2 binpos;
  ub4 *bins;
  ub4 cel,frecnt;
  ub4 run,cnt;
  bool empty;
  enum  Status rv;

  cellen = reg->cellen;
  cel = slab_cel(hb,reg,ip,cellen,Lfree);

  if (unlikely(cel == Nocel)) {
    hb->stats.invalid_frees++;
    ylog(Lfree,"slab bin cel %u len %u",cel,cellen);
    return 0;
  }

  run = slab_prefree(hb,reg,cel,cellen);
  ylog(Lfree,"slab bin cel %u len %u run %u",cel,cellen,run);

  if (unlikely(run == 0)) {
    hb->stats.invalid_frees++;
    return 0;
  }
  ystats2(reg->stats.binned,run)
  reg->stats.binned++;

  binpos = reg->binpos;
  bins = reg->bin;

  do {
    if (binpos == Bin) { // full, delete oldest set
      ylog(Lfree,"slab free bin 0 to %u frecnt %u",Binful,reg->frecnt);
      // if (reg->dirid == 2 && cel == 193) do_ylog(0,Lfree,Fln,Info,nil,"reg %u empty bin",reg->dirid);
      slab_free(hb,reg,bins);
      binpos = Bin - Binful;
      memmove(bins,bins + Binful,binpos * sizeof(ub4));
    }
    ylog(Lfree,"slab bin cel %u len %u pos %u",cel,cellen,binpos);
    // if (reg->dirid == 2 && cel == 193) do_ylog(0,Lfree,Fln,Info,nil,"reg %u bin cel %u pos %u",reg->dirid,cel,binpos);
    bins[binpos++] = cel++;
    run--;
  } while (unlikely(run != 0)); // split run

  reg->binpos = binpos;

  frecnt = reg->frecnt;

  empty = (reg->celcnt == frecnt + binpos);
  return empty;
}
