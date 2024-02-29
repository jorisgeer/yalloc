/*free.h - free() toplevel

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Handle recycling bin and eventually pass on to slab, buddy or mmap region.free()
*/

static bool free_mmap(heap *hb,region *rp,size_t ip)
{
  size_t len = rp->len;

  // if (unlikely(mp == 0)) { error(__LINE__,Falloc,"free-mmap(): double free of ptr %z of len %z",ip,len); return; }
  // if (unlikely(ip != mp)) { error(__LINE__,Falloc,"free-mmap(): invalid ptr %z",ip); return; }
  if (unlikely(ip & (Page - 1))) { error(__LINE__,Falloc,"free-mmap(): invalid ptr %zx",ip); return 0; }
  if (unlikely(len < Mmap_threshold)) { error(__LINE__,Falloc,"free: ptr %zx len %zu` was not mmap()ed",ip,len); return 0; }
  // if (unlikely(len > (1UL << Vmsize)) { error(__LINE__,Falloc,"free: ptr %z len `%Ilu was not mmap()ed",ip,len); return; }
  return delregion(hb,rp);
}

// called by trimbin
static bool free_reg(heap *hb,region *rp,size_t ip)
{
  switch (rp->typ) {
  case Rnil: return 0;
  case Rbuddy: return buddy_free(hb,rp,ip);
  case Rxbuddy: return 0;
  case Rslab: return slab_free(hb,rp,ip);
  case Rmmap: return free_mmap(hb,rp,ip);
  }
  return 0;
}

static void trimbin(heap *hb,bool full)
{
  struct binentry *binp;
  region *reg;
  ub2 e;
  ub2 clas;
  ub2 binmask;
  size_t ip;

  for (clas = 0; clas < hb->clascnt; clas++) {
    binmask = hb->binmasks[clas];
    if (binmask == 0) continue;
    binp = hb->bins + clas * Bin;
    e = 0;
    do {
      if ( (binmask & 1) ) {
        reg = binp[e].reg;
        ip = (size_t)binp[e].p;
        if (free_reg(hb,reg,ip)) {
          if (delregion(hb,reg)) {
            if (full) delheap(hb,1);
            else memset(hb->bins,0,sizeof(hb->bins));
            return;
          }
        }
      }
      e++;
      binmask >>= 1;
    } while (binmask );
  }
}

static void yfree_heap(heap *hb,void *p,size_t len)
{
  size_t xp,ip = (size_t)p;
  char *cp = p;
  void *ap;
  ub4 *up;
  region *reg,*nxreg,*pxreg,*clreg,*oldreg;
  struct binentry *binp;
  ub2 clas;
  ub4 binmask,old,new;

  if (cp >= hb->inimem + 4 && cp < hb->inimem + Inimem) {
    up = ((ub4 *)cp) - 1;
    if (*up == 0) free2(__LINE__,Ffree,p,0,"in bootmem");
    *up = 0;
    return; // initial bump alloc
  }

  if (ip >= (1ul << Maxvm)) {
    error(__LINE__,Ffree,"free(): ptr %p is outside %u bit VM space",p,Maxvm);
    return;
  }

  reg = findregion(hb,ip);

  if (reg == nil) {
    error(__LINE__,Ffree,"free(%p) of unallocated pointer",p);
    return;
  }

  // slab
  clas = reg->clas;
  if (clas != Noclass) {
    len = reg->len;
    if (slab_chk4free(hb,reg,ip)) return;

    // put in recycling bin
    binmask = hb->binmasks[clas];
    binp = hb->bins + clas * Bin;
    if (binmask == Binmask) { // common: bin full
      for (new = 0; new < Bin; new++) if (binp[new].p == p) { free2(__LINE__,Ffree,p,len,"recycled"); return; }
      old = Bin-1;
      new = 0;
      oldreg = binp[old].reg;
      xp =(size_t) binp[old].p;
      if (slab_free(hb,oldreg,xp)) { // free oldest item
        nxreg = oldreg->nxt;
        pxreg = oldreg->prv;
        clreg = hb->clasreg[clas];
        nxreg->prv = pxreg;
        pxreg->nxt = nxreg;
        if (oldreg == clreg) {
          hb->clasreg[clas] = pxreg ? pxreg : nxreg;
        }
        delregion(hb,oldreg); // todo check
      }
      binp[old].reg = binp[new].reg;
      binp[old].p = binp[new].p;
      binp[new].reg = reg;
      binp[new].p = p;
    } else if (binmask == 0) { // bin empty
      binp[0].reg = reg;
      binp[0].p = p;
      hb->binmasks[clas] = 1;
    } else {
      for (new = 0; new < Bin; new++) {
        if ( (binmask & (1u << new)) && binp[new].p == p) { free2(__LINE__,Ffree,p,len,"recycled"); return; }
      }
      new = ctz(binmask);
      binp[new].reg = reg;
      binp[new].p = p;
      hb->binmasks[clas] |= (1u << new);
    }
  }

  if (reg->typ == Rbuddy) {
    if (buddy_free(hb,reg,ip)) {
      delregion(hb,reg);
    }
  } else if (reg->typ == Rmmap) {
    if (len && len != reg->len) error(__LINE__,Ffree,"free_sized(%p,%zu) mmap block had size %zu",p,len,reg->len);
    cp = reg->user;
    ap = reg->meta;
    if (ap) {
      if (p != ap) { error(__LINE__,Ffree,"free(%p) is %zu`b in aligned mmap block allocated at %p from %p",p,(size_t)p - (size_t)ap,ap,cp); return; }
    } else {
      if (p != cp) { error(__LINE__,Ffree,"free(%p) is %zu`b in mmap block allocated at %p",p,(size_t)p - (size_t)cp,cp); return; }
    }
    if (free_mmap(hb,reg,ip)) delheap(hb,0); // todo
    return;
  }
}

static void yfree(void *p,size_t len)
{
  heap *hb;

  // oswrite(2,"yfree 1\n",8);
  hb = thread_heap;
  // oswrite(2,"yfree 2\n",8);
  ylog(Falloc,"yfree heap %p",(void *)hb);

  if (hb == nil) {
    error(__LINE__,Ffree,"free(%p) in empty heap was not malloc()ed",p);
    return;
  }
  yfree_heap(hb,p,len);
}
