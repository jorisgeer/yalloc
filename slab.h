/* slab.h - regions of fixed-size blocks

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  6  line
  12 A
  18 B
  24 C
  accel A,B,C bit set if allocated
*/

#define Noclass 0xffff

static region *newslab(heap *hb,ub4 cellen,ub4 userlen)
{
  ub4 order = newregorder(hb);
  region *reg;
  size_t reglen = 1ul << order;
  size_t admlen;
  ub4 cnt;
  ub4 a;
  ub4 ord;

  ylog(Fslab,"new slab cel len %u,%u ord %u",cellen,userlen,order);

  if (cellen & (cellen - 1)) {
    cnt = (ub4)(reglen / cellen);
    ord = 0;
  } else {
    ord = ctz(cellen);
    cnt = (ub4)(reglen >> ord);
  }

  a = cnt >> 3; // 1 bit per cel
  a = doalign(a,8u); // in 64-bit ints
  admlen = a << 1; // 1 alloc and 1 freed bitmap

  do { // accels
    a >>= 6;
    a = doalign(a,8u);
    admlen += a;
  } while (a);

  admlen += Accel_cnt * 8;

  ylog(Fslab,"new slab reg len %zu`b meta %zu`b ord %u cnt %u",reglen,admlen,ord,cnt);
  reg = newregion(hb,nil,reglen,admlen,Rslab);

  reg->frecnt = reg->cnt = cnt;
  reg->len = userlen;
  reg->cellen = cellen;
  reg->celcnt = cnt;
  reg->celord = (ub1)ord;

  return reg;
}

static void *slab_oom(ub4 line,heap *hb,region *reg)
{
  error(line,Fslab,"cannot allocate from slab %u,%u",hb->id,reg->id);
  return nil;
}

static void *slab_alloc(heap *hb,region *reg,bool clear)
{
  uint_fast16_t ord;
  ub4 reglen;
  ub4 len;
  ub4 linlen;
  ub4 accAlen;
  ub4 accBlen;
  ub4 accClen;
  ub4 cacc,bacc,aacc;
  ub4 ofs;
  ub4 bit;
  size_t cel;
  ub8 cmask,bmask,amask,lmask,mask;

  ub8 *meta = reg->meta;
  void *p;
  char *user = reg->user;
  ub8 *line = meta;

  mask = reg->linmask;
  len = (ub4)reg->len;

  if (mask != Full) { // fast path: next cel from previous alloc
    ofs = reg->linofs;
    bit = ctzl(~mask);
    mask |= (1ul << bit);
    if (mask != Full) {
      cel = (ofs << 6) + bit;
      line[ofs] = reg->linmask = mask;
      p = user + cel * len;
      if (clear) memset(p,0,len);
      ylog(Fslab,"slab alloc fast cel %zu = %p",cel,p);
      return p;
    }
  }

  ord = reg->cntord;

  if (ord > 24) {
    accClen = ord >> 24;
  }

  reglen = (1u << reg->order);
  linlen = reglen >> 6;
  accAlen = linlen >> 6;
  accBlen = accAlen >> 6;
  accClen = accBlen >> 6;

  unsigned int cbit = 0,bbit,abit;

  ub8 *line2 = meta + linlen * 2;

  ub8 *accA = line + linlen;
  ub8 *accB = accA + accAlen;
  ub8 *accC = accB + accBlen;

  ub2 clas;
  region *xreg;

  ylog(Fslab,"slab alloc reg %u len %u",reg->id,len);

  // search in freemap
  for (cacc = 0; cacc < accClen; cacc++) {
    cmask = accC[cacc];
    if (cmask == hi64) continue; // bit unset if available
    cbit = ctzl(~cmask);
    break;
  }
  if (cacc == accClen) return slab_oom(__LINE__,hb,reg);

  ylog(Fslab,"cacc %u cbit %u",cacc,cbit);

  bacc = (cacc << 6) + cbit;
  bmask = accB[bacc];
  bbit = ctzl(~bmask);

  aacc = (bacc << 6) + bbit;
  amask = accA[aacc];
  abit = ctzl(~amask);

  ofs = (aacc << 6) + abit;
  lmask = line[ofs];
  bit = ctzl(~lmask);

  cel = (ofs << 6) + bit;

  // unmark from free map
  lmask = (1ul << bit);
  line[ofs] = reg->linmask = lmask;
  accA[aacc] |= (1ul << abit);
  accB[bacc] |= (1ul << bbit);
  accC[cacc] |= (1ul << cbit);

  p = user + cel * len;

  if (clear && (line2[ofs] & lmask) ) memset(p,0,len);

  reg->ofs = ofs;

  if (--reg->frecnt == 0) { // full, put next in front
    clas = reg->clas;
    xreg = reg->nxt;
    xreg->nxt = reg;
    hb->clasreg[clas] = xreg;
  }

  return p;
}

static bool slab_chk4free(heap *hb,region *reg,size_t ip)
{
  ub8 *meta = reg->meta;
  size_t ibase = (size_t)reg->user;
  ub4 ofs8;
  ub8 msk;
  ub4 reglen;
  ub4 linlen;
  ub4 cel,ofs;
  ub4 bit;
  ub8 *line = meta;
  ub8 *line2;
  ub2 ord;

  ofs8 = (ub4)(ip - ibase);
  if (ofs8  >= reg->cnt) { error(__LINE__,Fslab,"heap %u invalid free of ptr %lx of size %lu",hb->id,ip,reg->len); return 1; }

  ord = reg->celord;
  if (ord) cel = ofs8 >> ord;
  else cel = ofs8 / (ub4)reg->len;

  ofs = cel >> 6;
  bit = cel - (ofs << 6);
  msk = 1ul << bit;
  if ((line[ofs] & msk) == 0) { // not allocated, find out why
    reglen = (1u << reg->order);
    linlen = reglen >> 6;
    line2 = reg->meta + linlen * 2;
    if (line2[ofs] & msk) error(__LINE__,Fslab,"double free of ptr %lx",ip);
    else error(__LINE__,Fslab,"invalid free of ptr %lx",ip);
    return 1;
  }
  return 0;
}

static bool slab_free(heap *hb,region *reg,size_t ip)
{
  size_t ibase = (size_t)reg->user;
  ub4 ofs8;
  ub8 msk;
  ub8 len = reg->len;
  ub2 order = reg->order;
  ub2 ord = reg->minorder;
  ub4 reglen = (1u << order);
  ub4 linlen = reglen >> 6;
  // ub4 accAlen = linlen >> 6;
  // ub4 accBlen = accAlen >> 6;
  // ? ub4 accClen = accBlen >> 6; ?
  ub4 cel,ofs;
  ub4 bit;

  ub8 *meta = reg->meta;
  ub8 *line = meta;
  ub8 *line2 = meta + linlen * 2;

  ub2 clas;
  region *areg,*xreg,*yreg;

  ofs8 =(ub4)( ip - ibase);

  if (ord) cel = ofs8 >> ord;
  else cel = ofs8 / len;

  ofs = cel >> 6;
  bit = cel - (ofs << 6);
  msk = 1ul << bit;

  line[ofs] &= ~msk;
  line2[ofs] |= bit;
  // todo accels

  // A - x - B - y ->  B - A - x - y
  if (reg->frecnt++ == 0) { // was full, put in front if needed
    clas = reg->clas;
    areg = hb->clasreg[clas];
    // if (areg->frecnt  ) return 1;
    xreg = areg->nxt;
    yreg = reg->nxt;
    if (xreg == reg) {
      areg->nxt = yreg;
    } else {
      areg->nxt = xreg;
      xreg->nxt = yreg;
    }
    reg->nxt = areg;
    hb->clasreg[clas] = reg;
  }
  return (reg->frecnt == reg->cnt);
}
