/*realloc.h - realloc() toplevel

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

static void *realloc_copy(heap *hb,void *op,size_t olen,size_t nlen,bool dofree)
{
  void *np = yalloc_heap(hb,nlen,0);

  if (np) memcpy(np,op,olen);
  if (dofree && (np || FREE_FAIL_REALLOC) ) yfree_heap(hb,op,0);
  return np;
}

static void *yrealloc(void *p,size_t newlen)
{
  heap *hb = thread_heap;
  region *reg;
  char *cp = p;
  ub4 oldlen,*up;
  ub2 e;
  ub2 binmask;
  ub2 clas;
  struct binentry *binp;
  // void *newp;
  size_t ip = (size_t)p;
  size_t orglen;

  if (hb == nil) {
    error(__LINE__,Frealloc,"realloc(%p) in nonexistent heap",p);
    return nil;
  }

  if (cp >= hb->inimem + 4 && cp < hb->inimem + Inimem) {  // initial bump alloc
    up = (ub4 *)cp;
    oldlen = up[-1];
    if (oldlen == 0) free2(__LINE__,Frealloc,p,0,"in bootmem");
    if (newlen <= oldlen) return p;
    return  realloc_copy(hb,p,oldlen,newlen,0);
  }

  reg = findregion(hb,ip);
  if (reg == nil) {
    error(__LINE__,Frealloc,"realloc(%p,`%zu) was not malloc()ed",p,newlen);
    return nil;
  }

  if (reg->typ == Rslab) {
    orglen = reg->cellen;
    clas = reg->clas;
    binmask = hb->binmasks[clas];
    binp = hb->bins + clas * Bin;
    for (e = 0; e < Bin; e++) {
      if ( (binmask & (1u << e)) && binp[e].p == p) { free2(__LINE__,Ffree,p,orglen,"recycled"); return nil; }
    }
    if (newlen <= orglen) return p;
    return realloc_copy(hb,p,orglen,newlen,1);
  } else if (reg->typ == Rbuddy) {
    return buddy_realloc(hb,reg,p,newlen);
  } else if (reg->typ == Rmmap) {
    if ( (size_t)p & (Page - 1)) {
      error(__LINE__,Frealloc,"realloc: invalid ptr %p",p);
      return nil;
    }
    orglen = reg->len;
    if (newlen <= orglen) return p;
    return mmap_realloc(hb,reg,p,orglen,newlen);
  } else return nil;
}
