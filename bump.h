/* bump.h - bump allocating region

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   Store initial blocks of not (yet) popular sizes in a bump allocator.
   Metadata is arranged as a list of 16-byte 'cells' similar to slabs, except that the length is stored.
   State is managed with atomics, like slabs, to detect double free.

   metadata:
   ub2 len[cels]
   ub4 tag[cels] optional
   ub1 freebit[cels]
*/

#define Logfile Fbump

// hb may be nil for mini
static bool newbump(heap *hb,ub4 hid,bregion *reg,ub4 len,ub4 regpos,enum Rtype typ,enum Loc loc)
{
  ub4 celcnt,metalen,taglen,tagorg,freorg;
  void *user,*meta;

  len = doalign4(len * (regpos + 1),Pagesize);
  len = min(len, 65536 * Stdalign);

  ycheck(1,loc,len < Pagesize,"%s region size %u page %u",regnames[typ],len,Pagesize);

  celcnt = len / Stdalign;
  tagorg = celcnt / 2; // in ub4
  taglen = Yal_enable_tag ? celcnt  : 0;

  freorg = tagorg + taglen;
  metalen = freorg * 4 + celcnt;

  ycheck(1,loc,celcnt > Hi16,"bump reg %u cels %u",hid,celcnt)

  if (hid < 10) {
    ydbg1(Fln,loc,"new %s region %u heap %u len %u meta %u at %zx",regnames[typ],regpos + 1,hid,len,metalen,(size_t)reg);
  }

  user = osmem(Fln,hid,len,"bumpalloc");
  if (user == nil) return 1;
  vg_mem_noaccess(user,len)
  meta = bootalloc(Fln,hid,loc,metalen);
  if (meta == nil) return 1;

  reg->hb = hb;
  reg->uid = (ub8)hid << 32 | (regpos + 1);
  reg->hid = hid;
  reg->id = regpos + 1;
  reg->user = (size_t)user;
  reg->meta = meta;
  reg->cnt = celcnt;
  reg->freorg = freorg;
  reg->tagorg = Yal_enable_tag ? tagorg : 0;
  reg->len = len;
  reg->typ = typ;

  Atomset(reg->lock,0,Morel);

  return 0;
}

static void *bumpalloc(heap *hb,ub4 hid,bregion *regs,ub4 regcnt,ub4 len,ub4 ulen,ub4 align,enum Loc loc,ub4 tag)
{
  ub4 *meta;
  _Atomic ub2 *lens;
  ub4 *tags;
  size_t ip,base;
  ub4 pos = 0,cel;
  _Atomic ub1 *fres;
  ub4 regpos;
  ub1 zero;
  bool didcas;
  bregion *reg = nil;
  enum Rtype typ = hb ? Rbump : Rmini;

  if (unlikely(align > Stdalign)) {
    if (align >= 512) return nil;
    len += align;
  } else align = Stdalign;

  len = doalign4(len,Stdalign);

  if (regs->typ == Rmini) {
    pos = regs->pos;
    if (pos + len > regs->len) return nil;
    reg = regs;
  } else {
    for (regpos = 0; regpos < regcnt; regpos++) {
      reg = regs + regpos;
      if (unlikely(reg->len == 0)) {
        if (newbump(hb,hid,reg,Bumplen,regpos,typ,loc)) return nil;
        vg_drd_rwlock_init(reg);
        if (hb) setregion(hb,(xregion *)reg,reg->user,reg->len,1,loc,Fln);
      }
      pos = reg->pos;
      if (pos + len + align > reg->len) continue;

      break;
    }
    if (regpos == regcnt) {
      ydbg2(Fln,loc,"bump regions full at regpos %u/%u",regpos,regcnt);
      return nil;
    }
  }
  base = reg->user;
  meta = reg->meta;

  reg->pos = pos + len;

  if (unlikely(loc == Lallocal && pos != 0)) pos = doalign4(pos,align);

  ycheck(nil,loc,pos & (align - 1),"pos %u align %u",pos,align)
  ycheck(nil,loc,pos + len > reg->len,"pos %u + %u > %zu",pos,len,reg->len)
  cel = pos / Stdalign;
  lens = (_Atomic ub2 *)meta;

  fres = (_Atomic ub1 *)(meta + reg->freorg);

  zero = 0;
  didcas = Casa(fres + cel,&zero,1);
  ip = base + pos;
  if (unlikely(didcas == 0)) {
    error(loc,"%s region %.01llu ptr %zx len %u cel %u is not free %.01u state %u",regnames[reg->typ],reg->uid,ip,ulen,cel,tag,zero)
    return nil;
  }

  Atomseta(lens + cel,(ub2)(len / Stdalign),Morel);
  if (reg->tagorg) {
    tags = meta + reg->tagorg;
    tags[cel] = tag;
  }
  ystats(reg->allocs);
  ydbg3(loc,"bumpregion %.01lu ptr %zx len %u cel %u tag %.01u state %u",reg->uid,ip,len,cel,tag,zero)

  if (loc == Lcalloc) { vg_mem_def(ip,ulen) }
  else { vg_mem_undef(ip,ulen) }

  return (void *)ip;
}

static void *bump_alloc(heap *hb,ub4 len,ub4 ulen,ub4 align,enum Loc loc,ub4 tag)
{
  return bumpalloc(hb,hb->id,hb->bumpregs,Bumpregions,len,ulen,align,loc,tag);
}

// returns len. hb may be nil. Region not locked
static ub4 bump_free(heapdesc *hd,heap *hb,bregion *reg,size_t ip,size_t reqlen,ub4 fretag,enum Loc loc)
{
  size_t base = reg->user;
  ub4 *meta;
  _Atomic ub2 *lens;
  ub4 *tags;
  _Atomic ub1 *fres;
  ub1 one;
  ub4 cel,len,ofs,cnt;
  ub4 frees;
  ub4 altag;
  bool didcas;
  enum Rtype typ = reg->typ;
  char buf[256];
  void *p;

#if Yal_enable_check
  if (hb && typ != Rmini) {
    if (unlikely(reg < hb->bumpregs)) {
      p = region_near(ip,buf,255);
      errorctx(Fln,loc,"near %s %p",buf,p);
      return error2(loc,Fln,"%s region %.01llu (%zx) not in heap %u (%zx)",regnames[typ],reg->uid,(size_t)reg,hb->id,(size_t)hb->bumpregs)
    }
    if (unlikely(reg > hb->bumpregs + Bumpregions)) {
      p = region_near(ip,buf,255);
      errorctx(Fln,loc,"near %s %p",buf,p);
      return error2(loc,Fln,"%s region %.01llu not in heap %u",regnames[typ],reg->uid,hb->id)
    }
  }
#endif

  if (ip < base || ip > base + reg->len - Stdalign) {
    error(loc,"invalid ptr %zx", ip)
    return 0;
  }

  // locate cel to mark
  ofs = (ub4)(ip - base);
  if ( (ofs & Stdalign1) ) { error(loc,"invalid ptr %zx", ip) return 0; }

  cel = ofs / Stdalign;
  cnt = reg->cnt;

  if (unlikely(cel >= cnt)) {
    error(loc,"invalid ptr %zx cel %u above %u", ip,cel,cnt)
    return 0;
  }

  meta = reg->meta;
  lens = (_Atomic ub2 *)meta;
  len = Atomget(lens[cel],Moacq) * Stdalign;
  frees = Atomget(reg->frees,Moacq);
  ytrace(1,hd,loc,"ptr-%zx len %u bump %u",ip,len,frees)

  fres = (_Atomic ub1 *)(meta + reg->freorg);
  if (unlikely(reqlen == Nolen)) {
    one = Atomgeta(fres + cel,Moacq);
    if (one == 1) {
      return len;
    }
    ypush(hd,Fln)
    if (one == 0) return error(loc,"bumpregion %.01llu ptr %zx len %u never allocated tag %.01u",reg->uid,ip,len,fretag)
    else  return error(loc,"bumpregion %.01llu ptr %zx len %u tag %.01u state %u",reg->uid,ip,len,fretag,one)
  }

  one = 1;
  didcas = Casa(fres + cel,&one,2); // check double free
  if (unlikely(didcas == 0)) {
    ypush(hd,Fln)
    if (reg->tagorg) {
      tags = meta + reg->tagorg;
      altag = tags[cel];
    } else altag = Fln;
    if (one == 2) {
      errorctx(Fln,loc,"region %.01llu ptr %zx cel %u is already freed - 1 -> 2 = 2 altag %.01u",reg->uid,ip,cel,altag)
      free2(Fln,loc,(xregion *)reg,ip,len,fretag,"slab-bin");
    } else {
      errorctx(Fln,loc,"from heap %u cel %u",hd->id,cel)
      error2(loc,Fln,"region %.01llu invalid free(%zx) of size %u cel %u/%u tag %.01u - expected status 1, found %u",reg->uid,ip,len,cel,cnt,fretag,one)
    }
    return 0;
  }

  ydbg3(loc,"bumpregion %.01llu ptr %zx len %u cel %u tag %.01u state %u",reg->uid,ip,len,cel,fretag,Atomgeta(fres + cel,Moacq))
  Atomset(reg->frees,frees + 1,Morel);

#if 0 // recycle todo requires sync
  if (loc & Lremote) return len;

  allocs = reg->allocs;
  if (unlikely(frees + 1 == allocs)) { // empty, reset
    ydbg2(Fln,loc,"bumpfree region %u.%u reset at ptr %zx len %u cel %u",reg->hid,reg->id,ip,len,cel);
    reg->pos = 0;
    memset(meta + reg->freorg,0,reg->len / Stdalign); // clear state todo sync with remote
    reg->gen++;
  }
#endif
  return len;
}
#undef Logfile
