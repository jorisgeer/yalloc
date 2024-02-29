/* heap.h - generic heap admin

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

static ub4 newregorder(heap *hb)
{
  uint32_t mapcnt = atomic_load_explicit(&global_mapcnt,memory_order_relaxed);
  ub4 ord = 0;
  ub4 shift;

  if (mapcnt == 0 || mapcnt > (unsigned int)INT_MAX) ord = Minregion;
  else {
    ord = 32u - ctz(mapcnt);
    shift = mapshifts[min(ord,31)];
    ord = Minregion + shift;
  }
  ylog(Fheap,"heap %u ord %u",hb->id,ord);
  return ord;
}

// create heap base for new thread
static heap *newheap(ub4 delcnt)
{
  static _Atomic ub4 heapmem_pos;

  static char Align(16) heapmem[Iniheap];

  char *cbase;
  heap *base;
  ub4 pos;
  ub4 id;
  ub4 hlen = sizeof(struct st_heap);
  ub4 rlen = inireg * sizeof(region);
  ub4 dlen = inidir * Dir * sizeof(region);
  ub4 blen = Inimem;
  ub4 len = hlen + rlen + dlen + blen;

  sassert(Basealign >= 4,"Basealign >= 4");

  id = atomic_fetch_add_explicit(&heap_gid,1,memory_order_relaxed);

  ylog(Fheap,"new heap id %u base %u + regs %u + dir %u = %u",id,hlen,rlen,dlen,len)
  len = doalign(len,16u);
  pos = atomic_fetch_add(&heapmem_pos,len);
  atomic_fetch_and_explicit(&heapmem_pos,hi16,memory_order_relaxed); // avoid overflow
  if (pos + len <= Iniheap) {
    cbase = heapmem + pos;
    base = (heap *)(void *)cbase;
    base->iniheap = 1;
  } else {
    cbase = osmmap(len);
    ylog(Fheap,"mmap for heap base = %p",(void *)cbase);
    base = (heap *)(void *)cbase;
    if (cbase == nil) return nil;
  }
  base->regmem = (region *)(void *)(cbase + hlen);
  base->regmem_top = inireg;

  base->dirmem = (struct direntry *)(void *)(cbase + hlen + rlen);
  base->dirmem_top = inidir;

  base->inimem = cbase + hlen + rlen + dlen;

  base->delcnt = delcnt;
  base->baselen = len;
  base->id = id;
  memset(base->len2tclas,0xff,sizeof(base->len2tclas));
  memset(base->tclas2clas,0xff,sizeof(base->tclas2clas));
  return base;
}

// speculatively called when a heap becomes empty
static void delheap(heap *hb,bool trim)
{
  size_t x;
  ub4 delcnt = hb->delcnt ;
  region *reg,*xreg;

  if (hb->iniheap || (trim == 0 && delcnt > Heap_del_threshhold)) return; // prevent continuous delete-create cycles

  reg = hb->nxtregs;
  while (reg) {
    xreg = reg->nxt;
    osunmem(__LINE__,Fheap,hb,reg,Regmem_inc * sizeof(struct st_region),"region pool");
    reg = xreg;
  }

  delcnt = (delcnt + 1) & hi24;
  x = (delcnt << 1) | 1;
  thread_heap = (heap *)x;
  osunmem(__LINE__,Fheap,hb,hb,hb->baselen,"heap base");
}

static heap *getheap(void)
{
  ub4  delcnt ;
  size_t hx;
  heap *hb;

  hb= thread_heap;

  // ylog(Fheap,"heap %p",(void *)hb);

  hx = (size_t)hb;
  if ( (hx & 1) ) {
    delcnt = (ub4)(hx >> 1); // preserve #deletes
    hb = newheap(delcnt);
  } else {
    delcnt = 0;
    if (hb == nil) hb = newheap(delcnt);
  }
  if (hb == nil) return nil;
  thread_heap = hb;
  hb->delcnt = (ub4)delcnt;
  return hb;
}
