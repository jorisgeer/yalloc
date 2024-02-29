/*alloc.h - alloc() toplevel

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Largest blocks are served directly by mmap(2) or equivalent, yet wrapped in a region for free(3) to find.
  Midsize blocks are served by a buddy heap
  Small blocks are either served by fixed-sized slab or buddy, dependent on usage stats
  A recycling bin for the latter two categories serves as a cache, forming a fast path
*/

// Alloc large blocks directly with mmap
static void *yal_mmap(heap *hb,size_t len)
{
  size_t n = doalign(len,Page);
  void *p = osmem(__LINE__,Falloc,hb,n,"block > mmap threshold");
  region *reg;

  if (p == nil) return nil;

  reg = newregion(hb,p,len,0,Rmmap);
  if (reg == nil) return nil;
  reg->clas = Noclass;
  reg->len = n;
  hb->lastreg = reg;
  return p;
}

static void *mmap_realloc(heap *hb,region *reg,void *p,size_t orglen,size_t newlen)
{
  void *np = osmremap(p,orglen,newlen);

  if (np) {
    if (np != p) regdir(hb,nil,(size_t)p,orglen);
    reg->len = newlen;
    reg->user = np;
    regdir(hb,reg,(size_t)np,newlen);
  } else {
    delregion(hb,reg);
  }
  return np;
}

static ub1 miniclas[8] = { 0,2,2,4, 4,8,8,8 };

// main entry
static void *yalloc_heap(heap *hb,size_t len,bool clear)
{
  ub4 ord;
  ub4 e;
  ub4 alen,calen;
  void *p;
  char *cp;
  region *reg,*newreg,**clasregs;
  ub4 pos,posa,align;
  ub2 clas;
  ub2 tclas;
  ub2 clascnt,tclascnt;
  struct binentry *binp;
  ub2 binmask;
  ub2 cnt;

  len <<= guardbit;
  reg = nil;

  if (unlikely(len >= mmap_threshold)) return yal_mmap(hb,len);

  if (len < Maxclasslen) {

#if 1
  // 'canned' initial bump allocator
    pos = hb->inipos;
    if (pos + len + 2 * Basealign <= Inimem) {
      cp = hb->inimem + pos;
      *(ub4 *)cp = (ub4)len;
      pos += Basealign;
      alen = doalign(len,Basealign);
      hb->inipos += alen + Basealign;
      p = hb->inimem + pos;
      ylog(Falloc,"heap %u bump %u`b to %u`b = %p",hb->id,(ub4)len,hb->inipos,p);
      return p;
    }
#endif

    if (len <= 8) {
      alen = calen = miniclas[len];
    } else if (len <= 16) {
      alen = calen = 16;
    } else {
      alen = doalign(len,16u);
      calen = (alen >> 4) + 16;
    }

    // check size classes aka slabs, tentative at first for all sizes
    tclas = hb->len2tclas[calen];
    if (tclas == hi16 && (tclascnt = hb->tclascnt) < Maxtclass) {
      tclas = hb->len2tclas[calen] = tclascnt;
      ylog(Falloc,"new tclas %u for len %u,%u",tclas,alen,calen);
      hb->tclas2len[tclas] = (ub2)calen;
      hb->tclascnt = tclascnt + 1;
    }

    if (tclas != hi16) {
      clas = hb->tclas2clas[tclas];
      if (clas != hi16) {
        if ( (binmask = hb->binmasks[clas]) ) { // check recycling bin
          binp = hb->bins + clas * Bin;
          e = ctz(binmask);
          reg = binp[e].reg;
          p = binp[e].p;
          hb->binmasks[clas] = binmask & ~(1u << e);
          if (clear) memset(p,0,len);
          return p;
        } // bin

        clasregs = hb->clasreg + clas;
        reg = *clasregs;
        if (reg) {
          if (reg->frecnt == 0) {
            newreg = newslab(hb,alen,len);
            if (newreg == nil) return nil;
            newreg->clas = clas;
            newreg->nxt = reg;
            newreg->prv = reg->prv;
            reg = newreg;
            *clasregs = reg;
          }
        } else { // regions deleted earlier
          reg = *clasregs = newslab(hb,alen,len);
        }
      } else if ( (clascnt = hb->clascnt) < Maxclass) { // no class yet, count
        cnt = (hb->sizecount[tclas] + 1) & 0x7f;
        hb->sizecount[tclas] = cnt;
        ylog(Falloc,"tclas %u cnt %u",tclas,cnt);
        if (cnt > Clas_threshold) { // new class
          ylog(Falloc,"new clas %u for len %u,%u",clascnt,alen,calen);
          hb->tclas2clas[tclas] = clas = clascnt;
          hb->clascnt = clascnt + 1;
          reg = newslab(hb,alen,len);
          if (reg == nil) return nil;
          reg->clas = clas;
          hb->clasreg[clas] = reg;
        } // new class
      } // no class
      if (reg) return slab_alloc(hb,reg,clear);
    }
  } // len < Maclass

  // default to buddy
  if (len < 1ul << Minorder) len = 1ul << Minorder;

  p = buddy_alloc(hb,len,clear);
  return p;
}

// main entry
static void *yalloc(size_t len,bool clear)
{
  void *p;
  heap *hb;
  ub4 pos,posa,align;
  static _Atomic ub4 nested;
  static char tls[512];

  ylog(Falloc,"yalloc %zu`b%s",len,clear ? " zeroed" : "");

  if  (atomic_fetch_add_explicit(&nested,1,memory_order_relaxed) > 5) {
    // ylog(Falloc,"yalloc > %p",tls);
    return tls;
  }

#if 0
  pos = atomic_fetch_add_explicit(&bootem_pos,len + Basealign,memory_order_relaxed);
  atomic_fetch_add_explicit(&bootem_pos,hi16,memory_order_relaxed); // avoid overflow

  if (pos + len + Basealign <= Inimem) {
    align = get_align((ub4)len);
    posa = doalign(pos,align);
    p = bootmem + posa;
    ylog(Falloc,"bump %u",(ub4)len);
    return p;
  }
#endif

  // oswrite(2,"yalloc 1\n",9);
  hb = getheap();
  atomic_fetch_sub_explicit(&nested,1,memory_order_relaxed);
  // oswrite(2,"yalloc 2\n",9);
  if (unlikely(hb == nil)) return nil;

  return yalloc_heap(hb,len,clear);
}

static void *yalloc_align(size_t align, size_t len)
{
  void *p,*ap;
  ub4 alen;
  heap *hb;
  region *reg;
  size_t ip;

  if (len <= 8) alen = miniclas[len];
  else alen = 16;

  if (align <= alen) return yalloc(len,0);

  hb = getheap();
  if (hb == nil) return nil;

  len = max(len,align);
  if (align > Page) len += align;
  if (len > Mmap_threshold) {
    if (align <= Page) return yalloc_heap(hb,len,0);
    p = yalloc_heap(hb,len,0);
    ip = (size_t)p;
    ip = doalign(ip,align);
    ap = (void *)ip;
    reg = hb->lastreg;
    reg->meta = ap;
  }
  p = buddy_alloc(hb,len,0);
  if (p == nil) return p;
  ip = (size_t)p;
  ip = doalign(ip,align);
  ap = (void *)ip;
  reg = hb->lastreg;
  buddy_addref(hb,reg,p,ap);
  return ap;
}
