/* heap.h - generic heap admin

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   A heap is the base structure that holds all admin. They are created on demand and never freed as such.
*/

#define Logfile Fheap

static bool slab_reset(region *reg);

static void heap_init(heap *hb)
{
  static_assert(Clascnt < 65536,"Clascnt < 64K");
  static_assert(Page + Dir1 + Dir2 + Dir3 == Vmbits,"VM size not covered by dir");
  static_assert( (Rmeminc & (Rmeminc - 1)) == 0,"Rmeminc not power of two");

  hb->rmeminc = max(Rmeminc,Pagesize);

  hb->stat.id = hb->id;
  hb->stat.minrelen = Hi64;
}

static void heap_reset(heap *hb)
{
  region *reg;

  reg = hb->reglst;
  while (reg) {
    if (reg->typ == Rslab) slab_reset(reg);
    reg = reg->nxt;
  }
}

// create heap for new thread
static heap *newheap(heapdesc *hd,enum Loc loc,ub4 fln)
{
  ub4 id = hd->id;
  ub4 hid = Atomad(global_hid,1,Moacqrel);
  ub4 tidcnt = Atomget(global_tid,Moacq) - 1;
  void *vbase;
  size_t base;
  heap *hb,*orghb;
  ub4 iter,zero;
  bool didcas;

  ub4 len;
  ub4 hlen = sizeof(heap);
  ub4 rsiz = sizeof(region);
  ub4 xrsiz = sizeof(xregion);
  ub4 rlen = Regmem_inc * rsiz;
  ub4 rxlen = Xregmem_inc * xrsiz;
  ub4 dlen = Dirmem_init * Dir2len;
  ub4 llen = Dirmem_init * Dir3len;

  static_assert( (Stdalign & Stdalign1) == 0,"Stdalign must be a power of 2");
  static_assert(Stdalign > 0,"Stdalign > 0");
  static_assert(Stdalign < Pagesize,"Stdalign < Pagesize");
  static_assert(Mmap_max_threshold < 31,"Mmap_max_threshold < 31");
  static_assert(Mmap_threshold <= Mmap_max_threshold,"Mmap_threshold < max");

  len = hlen + rlen + rxlen;
  len += (dlen + llen) * sizeof(void *);
  len = doalign4(len,16u);

  if (id < 3) { // first heaps
    ydbg1(fln,loc,"page %u clas %u zero %zx",Page,Clascnt,(size_t)zeroblock)
    ydbg1(Fln,loc,"sizes: region %u clasregs %u rootdir %u",rsiz,(ub4)sizeof(hb->clasregs),(ub4)sizeof(hb->rootdir))
  }
  ydbg2(fln,loc,"new heap %u for %u base %u regs %u+%u dir %up + %up = %u` tag %.01u",hid,id,hlen,rlen,rxlen,dlen,llen,len,fln);

  if (hid > tidcnt + 1) {
    errorctx(Fln,Lnone,"base %u",id);
    ydbg1(fln,loc,"heap %u above tidcnt %u",hid,tidcnt)
  }
  vbase = osmmap(len);
  if (vbase == nil) return nil;
  base = (size_t)vbase;
  hb = (heap *)base;

  base += hlen;
  ycheck(nil,loc,(base & 15),"regmem align %zx hlen %x",base,hlen)
  hb->regmem = (region *)base;
  base += rlen;
  ycheck(nil,loc,(base & 15),"xregmem align %zx hlen %x",base,hlen)
  hb->xregmem = (mpregion *)base;
  base += rxlen;

  hb->dirmem = (xregion ***)base;
  hb->dirmem_top = dlen;
  base += dlen * sizeof(void *);
  hb->leafdirmem = (xregion **)base;
  hb->ldirmem_top = llen;
  base += llen * sizeof(void *);
  ycheck(nil,loc,base - (size_t)vbase > len,"len %zu above %u",base - (size_t)vbase,len)

  hb->id = hid;

  heap_init(hb);
  hb->stat.mmaps = 1;

  vg_drd_rwlock_init(hb)
  zero = 0; didcas = Cas(hb->lock,zero,1); // Atomset(hb->lock,1,Morel); // give it locked
  ycheck(nil,loc,didcas == 0,"new heap %u from %u",hid,zero)
  vg_drd_wlock_acq(hb)

  iter = 20;

  // create list of all heaps for reassign
  do {
    orghb = Atomget(global_heaps,Moacq);
    hb->nxt = orghb;
    didcas = Cas(global_heaps,orghb,hb);
  } while (didcas == 0 && --iter);

  if (didcas == 0) {
    hd->stat.nolink++;
    do_ylog(Diagcode,loc,fln,Info,0,"base %u new heap %u not linked",id,hid);
  }
  return hb;
}

// create new heap or reassign an existing one
static heap *heap_new(heapdesc *hd,enum Loc loc,ub4 fln)
{
  heap *hb,*ohb = nil;
  bool didcas;
  ub4 zero;

  hb = Atomget(global_heaps,Moacq);

  while (hb) {
    zero = 0;
    didcas = Cas(hb->lock,zero,1);
    if (unlikely(didcas == 0)) {
      Pause
      zero = 0;
      didcas = Cas(hb->lock,zero,1);
    }
    if (didcas) {
      vg_drd_wlock_acq(hb)
      Atomset(hb->locfln,Fln,Morel);
      heap_reset(hb);
      hd->stat.useheaps++;
      ydbg2(fln,Lnone,"use next heap %u for %u",hb->id,hd->id);
      return hb;
    }
#if Yal_dbg_level > 1
    do_ylog(Diagcode,loc,fln,Debug,0,"no next heap %u for %u",hb->id,hd->id);
    do_ylog(Diagcode,loc,Atomget(hb->locfln,Moacq),Debug,0,"no next heap %u for %u",hb->id,hd->id);
#endif
    hd->stat.nogetheap0s++;
    hb = hb->nxt;
  }

  ohb = newheap(hd,loc,fln);
  hd->stat.newheaps++;

  return ohb; // locked
}

// get mem from pool for remote bin. len and pos in ub4
static void *getrbinmem(heap *hb,ub4 len)
{
  ub4 *mem = hb->rbinmem;
  ub4 pos = hb->rbmempos;
  ub4 end = hb->rbmemlen;
  ub4 inc,meminc = hb->rmeminc;

  len = doalign4(len,8);

  if (pos + len > end) { // won't fit
    pos = 0;
    inc = max(meminc,len);
    inc = doalign4(inc,meminc);
    mem = hb->rbinmem = osmmap(inc * 4);
    hb->stat.rbinallocs++;
    hb->rbmemlen = inc;
    if (meminc < Hi24) hb->rmeminc = meminc << 1;
    hb->stat.xbufbytes += inc * 4;
  }
  hb->rbmempos = pos + len;
  return mem + pos;
}

static struct rembuf *newrem(heap *hb)
{
  struct remote *rem;
  struct rembuf *rb;
  ub4 len = Clascnt * Clasregs * sizeof(struct remote);
  ub4 blen = sizeof(struct rembuf);

  rem = getrbinmem(hb,(len + 4) / 4);
  rb = getrbinmem(hb,(blen + 4) / 4);

  if (rem == nil || rb == nil) return nil;
  rb->rem = rem;
  return rb;
}

#if Yal_prep_TLS // assumes attributes are supported, not checked
static void __attribute__((constructor)) __attribute__((used))  yal_before_TLS(void)
{
  yal_tls_inited = 0;
  thread_heap = nil; // on some platforms, e.g. arm64-gcc-darwin, TLS is inited with malloc(). Trigger it before main
  yal_tls_inited = 1;
}
#endif

static ub4 newregorder(void)
{
  ub4 mapcnt;
  ub4 mapord=0,ord;
  ub4 shift = 0;

  mapcnt = Atomget(global_mapadd,Monone) - Atomget(global_mapdel,Monone);

  if (mapcnt == 0 || mapcnt >= Hi24) ord = Minregion;
  else {
    mapord = 32u - clz(mapcnt);
    if (mapcnt & (mapcnt - 1)) mapord++;
    shift = mapshifts[min(mapord,23)];
    ord = Minregion + shift;
  }
  return ord;
}
#undef Logfile
