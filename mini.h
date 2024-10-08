/* mini.h - miniature bump allocator

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   Hand out the first few, possibly unpopular, blocks from a small preallocated pool before creating a full heap.
   This benefits threads that only allocate a few small blocks.
*/

#define Logfile Fmini

// mini bump allocator
static void *mini_alloc(heapdesc *hd,ub4 len,ub4 ulen,enum Loc loc,ub4 tag)
{
  bregion *reg;
  ub4 ord,cnt;
  ub4 id = hd->id;

  ord = 32 - clz(len);
  ycheck(nil,loc,ord >= 16 + 4,"mini len %u above %u",len,1u<< 20);

  if (ord < Miniord && ord > 4) {
    cnt = hd->minicnts[ord - 4];
    if  (unlikely(cnt > 64)) return nil;
    hd->minicnts[ord - 4] = (ub1)(cnt + 1);
  }

  reg = hd->mhb;

  if (unlikely(reg == nil)) {
    reg = bootalloc(Fln,id,loc,sizeof(struct st_bregion));
    if (reg == nil) return nil;
    if (newbump(nil,id,reg,Minilen,0,Rmini,loc)) return nil;
    setgregion(nil,(xregion *)reg,reg->user,reg->len,1); // mini has no heap yet
    hd->mhb = reg;
  }

  return bumpalloc(nil,hd->id,reg,1,len,ulen,loc,tag);
}

#undef Logfile
