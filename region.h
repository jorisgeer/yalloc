/* region.h - regions

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

/*
  page gran
  no reg lookup
  1 reg / entry
  40 - 12 = 28 = 7+7+10
  40 - 20 = 20 = 2 x 10
 */

#undef Logfile
#define Logfile Fregion

#define Dir2len (1u << Dir2)

#define Xdir2len (1u << Xdir2)

#define Dir1msk (Dir1len - 1)
#define Dir2msk (Dir2len - 1)
#define Dir3msk (Dir3len - 1)

static reg_t *newleafdir(heap *hb,ub4 cnt)
{
  reg_t *dp;
  ub4 pos,len,add = cnt * sizeof(reg_t);

  pos = hb->dirmem_pos;
  if (pos + add > hb->dirmem_top) {
    len = Dirmem * Dir3len * sizeof(reg_t);

    dp = osmmap(len);
    if (dp == nil) return nil;

    hb->stats.mmaps++;
    hb->dirmem = (char *)dp;
    hb->dirmem_top = len;
    pos = 0;
  }
  dp = (reg_t *)(hb->dirmem + pos);
  hb->dirmem_pos = pos + add;
  return dp;
}

static reg_t **newdir(heap *hb,ub4 cnt)
{
  reg_t **dp;
  ub4 pos,len,add = cnt * sizeof(reg_t *);

  pos = hb->dirmem_pos;
  if (pos + add > hb->dirmem_top) {
    len = Dirmem * Dir2len * sizeof(reg_t *);

    dp = osmmap(len);
    if (dp == nil) return nil;

    hb->stats.mmaps++;

    hb->dirmem = (char *)dp;
    hb->dirmem_top = len;
    pos = 0;
  }
  dp = (reg_t **)(hb->dirmem + pos);
  hb->dirmem_pos = pos + add;
  return dp;
}

// add region in directory
static int setregion(heap *hb,xregion *reg,size_t bas,size_t len)
{
  size_t org,end; // in pages
  ub4 pos1,pos2,pos3,posend;
  ub4 shift1,shift2;
  reg_t ***dir1,**dir2,*dir3;
  reg_t id;
  size_t xid;

  id = reg->dirid;

  hb->regs[id] = reg;

  Atomad(hb->dirversion,1);

  org = bas >> Page;
  end = (bas + len) >> Page;

  dir1 = hb->rootdir;

  shift1 = Vmsize - Page - Dir1;
  shift2 = shift1 - Dir2;

  do {
    pos1 = (org >> shift1) & Dir1msk;
    dir2 = dir1[pos1];
    if (dir2== nil) {
      dir2 = newdir(hb,Dir2len);
      if (dir2 == nil) goto end;
      dir1[pos1] = dir2;
      ylog(Lnone,"reg %x new dir %p at pos %x",id,(void *)dir2,pos1);
    }
    pos2 = (org >> shift2) & Dir2msk;

    pos3 = org & Dir3msk;
    posend = (ub4)min(end - org + pos3,Dir3len);
    org += posend - pos3;

    if (pos3 == 0 && (posend & Dir3msk) == 0) {
      xid = (size_t)id | Bit63;
      dir2[pos2] = (reg_t *)xid;
    } else {
      dir3 = dir2[pos2];
      if (dir3== nil) {
        dir3 = newleafdir(hb,Dir3len);
        if (dir3 == nil) goto end;
        dir2[pos2] = dir3;
      }
      do {
        dir3[pos3] = id;
      } while (++pos3 < posend);
    }
  } while (org < end);

end:

  Atomad(hb->dirversion,1);

  return 0;
}

// del region in directory
static int unsetregion(heap *hb,xregion *reg)
{
  size_t bas = (size_t)reg->user;
  size_t len = reg->len;
  size_t org,end; // in pages
  ub4 pos1,pos2,pos3,posend;
  ub4 shift1,shift2;
  reg_t ***dir1,**dir2,*dir3;

  if (bas == 0 || len == 0) { error(hb->errmsg,Lfree,"heap %u region %x is alredy empty",hb->id,reg->dirid) return 1; }

  Atomad(hb->dirversion,1);

  org = bas >> Page;
  end = (bas + len) >> Page;

  dir1 = hb->rootdir;

  shift1 = Vmsize - Page - Dir1;
  shift2 = shift1 - Dir2;

  do {
    pos1 = (org >> shift1) & Dir1msk;
    dir2 = dir1[pos1];
    if (dir2== nil) return 1;
    pos2 = (org >> shift2) & Dir2msk;

    pos3 = org & Dir3msk;
    posend = (ub4)min(end - org + pos3,Dir3len);
    org += posend - pos3;

    if (pos3 == 0 && (posend & Dir3msk) == 0) {
      dir2[pos2] = 0;
    } else {
      dir3 = dir2[pos2];
      if (dir3== nil) {
        return 0;
      }
      do {
        dir3[pos3] = 0;
      } while (++pos3 < posend);
    }
  } while (org < end);

  Atomad(hb->dirversion,1);

  return 0;
}

// unlocked
static Hot xregion *findregion(heap *hb,size_t ip)
{
  size_t base,ip1;
  ub4 pos1,pos2,pos3;
  ub4 shift1,shift2,shift3;
  reg_t rid;
  reg_t ***dir1,**dir2,*dir3;
  xregion *reg,**regs;

  dir1 = hb->rootdir;

  shift1= Vmsize - Dir1;
  ip1 = ip >> shift1;
  pos1 = ip1 & Dir1msk;

  dir2 = dir1[pos1];

  if (unlikely(dir2 == nil)) return nil;

  shift2= Vmsize - Dir1 - Dir2;
  pos2 = (ip >> shift2) & Dir2msk;
  dir3 = dir2[pos2];

  if (unlikely(dir3 == nil)) return nil;

  base = (size_t)dir3;
  if (base & Bit63) { // large region todo port
    rid = (reg_t)base;
    pos3 = 0;
  } else {
    shift3= Vmsize - Dir1 - Dir2 - Dir3;
    pos3 = (ip >> shift3) & Dir3msk;
    rid = dir3[pos3];
  }
  if (unlikely(rid == 0)) return nil;

  regs = hb->regs;
  reg = regs[rid];

  if (unlikely(reg == nil)) return nil;
  base = (size_t)reg->user;
  if (unlikely(ip < base)) return nil;
  if (unlikely(ip >= base + reg->len)) return nil;

  return reg;
}

static Cold xregion *findregion_rep(enum Loc loc,heap *hb,size_t ip)
{
  size_t base,ip1;
  ub4 pos1,pos2,pos3;
  ub4 shift1,shift2,shift3;
  reg_t rid;
  reg_t ***dir1,**dir2,*dir3;
  xregion *reg,**regs;

  dir1 = hb->rootdir;

  shift1= Vmsize - Dir1;
  ip1 = ip >> shift1;
  pos1 = ip1 & Dir1msk;

  dir2 = dir1[pos1];

  if (dir2 == nil) {
    error(hb->errmsg,loc,"no page dir at pos %x shift %u for adr %zx",pos1,shift1,ip)
    return nil;
  }

  shift2= Vmsize - Dir1 - Dir2;
  pos2 = (ip >> shift2) & Dir2msk;
  dir3 = dir2[pos2];

  if (dir3 == nil) {
    error(hb->errmsg,loc,"no page dir at pos %x shift %u for adr %zx",pos2,shift2,ip)
    return nil;
  }

  base = (size_t)dir3;
  if (base & Bit63) { // large region todo port
    rid = (reg_t)base;
    pos3 = 0;
  } else {
    shift3= Vmsize - Dir1 - Dir2 - Dir3;
    pos3 = (ip >> shift3) & Dir3msk;
    rid = dir3[pos3];
  }
  if (rid == 0) {
    error(hb->errmsg,loc,"no reg at pos %x,%x,%x for adr %zx base %zx",pos1,pos2,pos3,ip >> Page,base)
    return nil;
  }

  regs = hb->regs;
  reg = regs[rid];

  if (reg == nil) { error(hb->errmsg,loc,"no reg at pos %x,%x for adr %zx",pos1,pos2,ip) return nil; }
  base = (size_t)reg->user;
  if (ip < base) { error(hb->errmsg,loc,"pos %x,%x reg %x for %zx below base %zx",pos1,pos2,rid,ip,base) return nil; }

  if (ip >= base + reg->len) {
    if (ip1 & ~Dir1msk) { error(hb->errmsg,loc,"ptr  %zx is outside %u bit VM space",ip,Vmsize) return nil; }
    else if (base) { error(hb->errmsg,loc,"heap %u ptr %zx is %zu`b beyond region %x - pos %u,%u",hb->id,ip,ip - base - reg->len,rid,pos1,pos2) return nil; }
    else {
      ylog(loc,"heap %u empty region %x",hb->id,rid)
      return nil;
    }
  }
  return reg;
}

static void delregmem(heap *hb,region *reg)
{
  ub4 mapcnt = 1;
  size_t meta,metalen,ulen = reg->len;

  if (reg->user) {
    reg->len = 0;
    osunmem(Fln,hb,reg->user,ulen,"region user");
  }
  meta = (size_t)reg->meta;
  if (meta) {
    metalen = reg->metacnt * sizeof(ub8) + 2 * Metaguard * Pagesize;
    meta -= Metaguard * Pagesize;
    reg->metacnt = 0;
    osunmem(Fln,hb,(void *)meta,metalen,"region meta");
    mapcnt = 2;
  }
  reg->user = nil;
  reg->meta = nil;
  atomic_fetch_sub_explicit(&global_mapcnt,mapcnt,memory_order_relaxed);
}

// delete regular region e.g. slab
static bool delregion(heap *xhb,heap *hb,xregion *reg)
{
  region *xreg;
  ub4 frecnt = hb->freeregcnt;
  ub4 allcnt = hb->allocregcnt;
  bool last = (allcnt == frecnt + 1);
  reg_t freeid;
  size_t ip;

  hb->stats.region_cnt--;

  ylog(Lfree,"heap %u from %u delete region %x typ %u",hb->id,xhb->id,reg->dirid,reg->typ)

  ip = (size_t)reg->user;

  setregion(hb,reg,ip,reg->len); // unmap from page dir

  freeid = Atomget(hb->freeregs)
  reg->nxt = freeid;
  Cas(hb->freeregs,freeid,reg->dirid);

  return 0; // todo

#if 0
  for (i = 0; i < Regfree_trim && reg; i++) {
    if (last) delregmem(hb,reg);
    reg = reg->regbin;
  }
  if (reg && i == Regfree_trim) delregmem(hb,reg);
  hb->freeregcnt = frecnt + 1;
  if (last) delheap(hb);
  return last;
#endif
  return 0;
}

// delete mmap region. caller,owner
static bool delxregion(heap *xhb,heap *hb,enum Loc  loc,xregion *reg)
{
  ub4 frecnt = hb->freeregcnt;
  ub4 allcnt = hb->allocregcnt;
  bool last = (allcnt == frecnt + 1);
  reg_t freeid,id;
  size_t ip = (size_t)reg->user;
  size_t len = reg->len;

  hb->stats.xregion_cnt--;

  id= reg->dirid;
  ytrace(Lnone,"heap %u from %u delete mmap region %u for %zx",hb->id,xhb->id,id,ip)

  reg->typ = Rmmap_free;
  if (osunmem(Fln,hb,(void *)ip,len,"mmap region user")) error(hb->errmsg,loc,"invalid free heap %u from %u",hb->id,xhb->id)

  freeid = Atomget(hb->freeregs)
  reg->nxt = freeid;
  Cas(hb->freeregs,freeid,id);

  atomic_fetch_sub_explicit(&global_mapcnt,1,memory_order_relaxed);

  return 0; // todo

  hb->freeregcnt = frecnt + 1;
  if (last) delheap(hb);
  return last;
}

static region *newregmem(heap *hb,reg_t id)
{
  ub4 pos = hb->regmem_pos;
  ub4 top = Regmem_inc;
  reg_t regchkpos = hb->regchkpos;
  region *reg;

  static_assert( (sizeof(xregion) & (Stdalign - 1)) == 0,"xregion size aligned 16");

  if (pos == top) {
    hb->stats.mmaps++;
    reg = osmmap(top * sizeof(region));
    if (reg == nil) return nil;
    hb->regmem = reg;
    hb->regmem_pos = 1;
    hb->regchks[regchkpos] = id;
    hb->regchkpos = regchkpos + 1;
    return reg;
  }
  reg = hb->regmem + pos;
  hb->regmem_pos = pos + 1;
  return reg;
}

static xregion *newxregmem(heap *hb,reg_t id)
{
  ub4 pos = hb->xregmem_pos;
  ub4 top = Xregmem_inc;
  reg_t regchkpos = hb->xregchkpos;
  xregion *reg;

  static_assert( (sizeof(xregion) & (Stdalign - 1)) == 0,"xregion size aligned 16");

  if (pos == top) {
    hb->stats.mmaps++;
    reg = osmmap(top * sizeof(xregion));
    if (reg == nil) return nil;
    hb->xregmem = reg;
    hb->xregmem_pos = 1;
    hb->xregchks[regchkpos] = id;
    hb->xregchkpos = regchkpos + 1;
    return reg;
  }
  reg = hb->xregmem + pos;
  hb->xregmem_pos = pos + 1;
  return reg;
}

//
static region *newregion(heap *hb,ub4 order,size_t len,size_t metalen,enum Rtype typ)
{
  void *user;
  size_t loadr,hiadr;
  region *reg = nil;
  region **map;
  reg_t freeid,freenxt;
  char *meta,*guardmeta;
  ub4 rid;
  reg_t dirid;
  ub4 hid = hb->id;
  size_t guardlen;
  ub4 mapcnt;

  hb->status = St_oom; // default

  hb->stats.region_cnt++;

  ycheck(nil,Lalloc,len == 0,"heap %u empty type %u region",hb->id,typ)

  user = osmem(Fln,hb,len,"region base");
  if (user == nil) return nil;
  vg_mem_noaccess(user,len)
  loadr = (size_t)user;
  hiadr = loadr + len;
  ylostats(loadr,hb->stats.loadr)
  yhistats(hiadr,hb->stats.hiadr)

  rid = hb->allocregcnt;
  if (rid + 1 == hb->regmaplen) {
    hb->stats.mmaps++;
    map = osmmap(Regions * sizeof(size_t));
    memcpy(map,hb->regs,rid * sizeof(size_t));
    hb->regmaplen = Regions;
  }
  if (++rid == Regions - 1) return nil;
  dirid = (reg_t)rid;
  if (rid > Regions / 4) { // reuse
    freeid = Atomget(hb->freeregs)
    if (freeid && freeid != hi16) {
      reg = (region *)hb->regs[freeid];
      freenxt = reg->nxt;
      if (Cas(hb->freeregs,freeid,freenxt) == 0) reg = nil;
    }
  }
  if (reg == nil) {
    hb->allocregcnt = rid;
    rid |= (hid << 16);
    reg = newregmem(hb,dirid);
    if (reg == nil) return nil;
    hb->regs[dirid] = (xregion *)reg;
    reg->dirid = (reg_t)dirid;
    reg->id = rid;
  } else {
    rid = reg->id;
    memset(reg,0,offsetof(region,bin));
  }
  if ( (size_t)reg & 7) {
    error(hb->errmsg,Lalloc,"region %x at %p unaligned",rid,(void *)reg)
    hb->status = St_error;
    return nil;
  }
  reg->typ = typ;
  reg->user = user;
  reg->len = len;
  reg->order = (ub1)order;

  ylog(Lalloc,"new region %x %p %p .. %zx len %zu`b meta %zu`b",rid,(void *)reg,user,(size_t)user+len,len,metalen)

  switch(typ) {
    case Rslab:
    case Rbuddy:
      ycheck(nil,Lalloc,metalen == 0,"reg %u nil meta",rid)
      metalen = doalign(metalen,Pagesize);
      guardlen = metalen + 2 * Metaguard * Pagesize;
      guardmeta = osmem(Fln,hb,guardlen,"region meta");
      if (guardmeta == nil) return nil;
      meta = guardmeta + Metaguard * Pagesize;
      reg->meta = (ub8 *)meta;
      reg->metacnt = metalen / sizeof(ub8);
      mapcnt = 2;
      break;
    case Rmmap: case Rmmap_free: mapcnt = 1; break;
    case Rnone: return nil;
  }
  if (setregion(hb,(xregion *)reg,(size_t)user,len)) return nil;
  Atomad(global_mapcnt,mapcnt)
  return reg;
}

// new mini region for mmap block
static xregion *newxregion(heap *hb,void *user,size_t len,size_t ulen)
{
  xregion *reg = nil;
  reg_t freeid,freenxt;
  ub4 rid;
  reg_t dirid;
  ub4 hid = hb->id;
  size_t dulen;

  hb->stats.xregion_cnt++;

  ycheck(nil,Lalloc,len == 0,"heap %u empty type mmap region",hb->id)
  ycheck(nil,Lalloc,ulen > len,"heap %u userlen %zu above len %zu",hb->id,ulen,len)

  dulen = len - ulen;

  vg_mem_undef(user,ulen)
  if (dulen) { vg_mem_noaccess( (char *)user + ulen,dulen) }

  rid = hb->allocregcnt + 1;
  if (rid >= Regions - 1) return nil;
  dirid = (reg_t)rid;
  if (rid > Regions / 2) { // reuse
    freeid = Atomget(hb->freexregs)
    if (freeid && freeid != hi16) {
      reg = hb->regs[freeid];
      freenxt = reg->nxt;
      if (Cas(hb->freexregs,freeid,freenxt) == 0) reg = nil;
      else {
        unsetregion(hb,reg); // delregion does not unset to allow dblfree detection
      }
    }
  }
  if (reg == nil) { // get a new region
    hb->allocregcnt = rid;
    rid |= (hid << 16);
    reg = newxregmem(hb,dirid);
    if (reg == nil) return nil;
    hb->regs[dirid] = reg;
    reg->dirid = dirid;
  }
  if ( (size_t)reg & 15) {
    error(hb->errmsg,Lalloc,"xregion %p unaligned - %zu",(void *)reg,sizeof(xregion))
    hb->status = St_error;
    return nil;
  }
  reg->user = user;
  reg->len = len;

  ylog(Lalloc,"new reg %x %p .. %zx len %zu`b",rid,user,(size_t)user+len,len)

  if (setregion(hb,reg,(size_t)user,len)) return nil;
  Atomad(global_mapcnt,1)
  return reg;
}

#if Yal_enable_stats

#define Statbuf 8192

static ub4 Cold slabstats(region *reg,struct yal_stats *sp,char *buf,ub4 pos,ub4 len,bool print,bool clear)
{
  ub4 cellen = reg->cellen;
  ub4 ulen = reg->ucellen;
  ub4 rid = reg->id;
  char lenbuf[64];
  char realbuf[64];

  struct regstat *rp = &reg->stats,*arp = &reg->accstats;

  // differentials
  size_t allocs = rp->allocs;
  size_t binallocs = rp->binallocs;
  size_t reallocles = rp->reallocles;
  size_t reallocgts = rp->reallocgts;
  size_t frees = rp->frees;
  size_t binned = rp->binned;

  size_t locks = rp->locks;
  size_t oslocks = rp->oslocks;
  size_t oslocktmos = rp->oslocktmos;

  // add to per-heap
  sp->allocs += allocs + binallocs;
  sp->frees += frees;
  sp->binned += binned;
  sp->reallocles += reallocles;
  sp->reallocgts += reallocgts;
  sp->minlen = min(sp->minlen,ulen);
  sp->maxlen = max(sp->maxlen,ulen);

  sp->locks += locks;
  sp->oslocks += oslocks;
  sp->oslocktimeouts += oslocktmos;

  memset(rp,0,sizeof(struct regstat));

  // add in accumulated
  allocs += arp->allocs;
  binallocs += arp->binallocs;
  reallocles += arp->reallocles;
  reallocgts += arp->reallocgts;
  frees += arp->frees;
  binned += arp->binned;

  locks += arp->locks;
  oslocks += arp->oslocks;
  oslocktmos += arp->oslocktmos;

  arp->allocs = allocs;
  arp->binallocs = binallocs;
  arp->reallocles = reallocles;
  arp->reallocgts = reallocgts;
  arp->frees = frees;
  arp->binned = binned;

  arp->locks = locks;
  arp->oslocks = oslocks;
  arp->oslocktmos = oslocktmos;

  if (clear) memset(arp,0,sizeof(struct regstat));

  if (print) {
    if ( (rid & 0xf) == 1) {
      pos += snprintf_mini(buf,pos,len,"%-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s",
        "region","len","cellen","alloc ","fast ","pre","bin","free","binned\n");
    }
    snprintf_mini(lenbuf,0,64,"%-6u` / %-6u`",ulen,cellen);
    if (reallocles | reallocgts) snprintf_mini(realbuf,0,64," realloc < %-6zu` > %-6zu`",reallocles,reallocgts);
    else *realbuf = 0;
    pos += snprintf_mini(buf,pos,len,"%-4x %-7zu` %-14s %-7zu`%s %-7zu` %-6zu` %-7zu` %-7zu` %zu`\n",
      reg->id,reg->len,lenbuf,allocs + binallocs,realbuf,rp->fastregs,rp->preallocs,rp->binallocs,frees,binned);
    if (locks) pos += snprintf_mini(buf,pos,len,"         locks %zu oslocks %zu timeout %zu\n",locks,oslocks,oslocktmos);
  }

  return pos;
}

// buf is only allocated for print
static Cold ub4 regstats(heap *hb,char *buf,ub4 pos,ub4 blen,bool print,bool clear)
{
  reg_t rid;
  xregion *xreg;
  region *reg;
  enum Rtype typ;
  ub4 rpos = hb->regmem_pos;
  reg_t regchkpos = hb->regchkpos;
  struct yal_stats *sp = &hb->stats;
  struct regstat *rp;
  ub4 cnt = 0;
  ub4 cid;
  size_t ip,len;

  if (print) pos += snprintf_mini(buf,pos,blen,"\nyal/region.h:%u - yalloc region stats for heap %x\n",__LINE__,hb->id);

  for (cid = 0; cid < hb->regchkpos; cid++) {
    rid = hb->regchks[cid];
    if (rid == 0) continue;
    xreg = hb->regs[rid];
    if (xreg == nil) continue;
    typ = xreg->typ;
    if (typ != Rslab) continue;

    rpos = 0;
    reg = (region *)xreg;
    do {
      ip = (size_t)reg->user;
      len = reg->len;
      if (len == 0) continue; // deleted
      sp->loadr = min(sp->loadr,ip);
      sp->hiadr = max(sp->hiadr,ip + len);

      pos = slabstats(reg,sp,buf,pos,blen,print,clear);
      reg++;
    } while (reg->id && ++rpos < Regmem_inc);
  }
  return pos;
}
#else

static ub4 regstats(heap *hb,char *buf,ub4 pos,ub4 blen,bool print,bool clear)
{
  if (print) hb->stats.minlen = 0;
  return pos;
}
#endif // Yal_enable_stats

#if Yal_enable_test

static Cold bool showdir(heap *hb,xregion *reg,size_t bas,size_t len,bool xtra)
{
  size_t o = bas >> Page,e = (bas + len) >> Page;
  ub4 n = 0,rid = reg->dirid;
  xregion *r = reg;
  bool rv = 0;

  ylog(Lnone," show reg %x bas %zx len %zx",rid,bas,len);

  while (o < e && r) {
    r = findregion(Lnone,hb,o << Page,1);
    ylog(Lnone,"< %3u reg %x/%x %zx",n++,rid,r ? r->dirid : hi32,o);
    o++;
  }
  if (r == nil) return 1;
  if (xtra == 0) return 0;

  e = (bas + 2 * len) >> Page;
  r = nil;
  while (o < e) {
    r = findregion(Lnone,hb,o << Page,1);
    ylog(Lnone,"> %3u reg %x/%x %zx",n++,rid,r ? r->dirid : hi32,o);
    o++;
    if (r) rv = 1;
  }
  if (rv) {
    r = findregion(Lnone,hb,o << Page,1);
    ylog(Lnone,"> reg %x/%x %zx",rid,r ? r->dirid : hi32,o);
  }
  return rv;
}

static bool testreg(heap *hb,xregion *reg,size_t bas,size_t len)
{
  reg->user = (void *)bas;
  reg->len = len;
  setregion(hb,reg,bas,len);
  return showdir(hb,reg,bas,len,1);
}

int test_region(unsigned int regno,size_t arg1,size_t arg2) // in pages
{
  static xregion reg1,reg2,reg3,reg4;
  static heap tstheap,*hb = &tstheap;
  static char dirmem[Dir3len * 64];
  xregion *reg = &reg1;

  hb->dirmem = dirmem;
  hb->dirmem_top = sizeof(dirmem);

  reg1.dirid = 1; reg2.dirid = 2; reg3.dirid = 3; reg4.dirid = 4;

  switch (regno) {
    case 1: reg = &reg1; break;
    case 2: reg = &reg2; break;
    case 3: reg = &reg3; break;
    case 4: reg = &reg4; break;
  }

  if (arg1) {
    if (testreg(hb,reg,arg1 << Page,arg2 << Page)) return __LINE__;
  }

  return hb->stats.errors != 0;
}
#endif
