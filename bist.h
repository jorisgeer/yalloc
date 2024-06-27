/*bist.h - builtin self test

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#undef Logfile
#define Logfile Fbist

struct st_bregion {
  ub4 cellen,celcnt,cnt;
  ub1 *map;
};
typedef struct st_bregion bregion;

static void newbistslab(heap *hb,bregion *breg,region *reg,ub4 id,ub4 ucellen)
{
  ub4 cellen = reg->cellen;
  ub4 celcnt = reg->celcnt;
  ub1 *map;

  breg->cellen = cellen;
  breg->celcnt = celcnt;
  map = osmmap(celcnt);
  breg->map = map;
}

static ub4 bist_alloc(heap *hb,bregion *breg,region *reg,void *p,ub4 ulen,enum Loc loc)
{
  size_t ip = (size_t)p;
  size_t base = (size_t)reg->user;
  size_t ofs8;
  ub4 cellen = breg->cellen;
  ub4 cel,celcnt = breg->celcnt;
  ub1 *map = breg->map;

  if (ip < base) return Fln;
  ofs8 = ip - base;
  if (ofs8 >= celcnt * cellen) { error(hb->errmsg,loc,"bist_add reg %u ofs %zu/%u cnt %u/%u",reg->id,ofs8,celcnt * cellen,breg->cnt,celcnt);  return Fln; }
  cel = (ub4)(ofs8 / cellen);
  if (cel * cellen != ofs8) return Fln;
  if (cel >= celcnt)  return Fln;

  if (map[cel])  {
    error(hb->errmsg,loc,"reg %u cel %u/%u free %u/%u",reg->dirid,cel,celcnt,reg->frecnt,celcnt)
    return Fln;
  }
  map[cel] = 1;
  breg->cnt++;
  return 0;
}

static ub4 bist_free(heap *hb,bregion *breg,region *reg,size_t ip,enum Loc loc)
{
  size_t base = (size_t)reg->user;
  size_t ofs8;
  ub4 cellen = breg->cellen;
  ub4 cel,celcnt = breg->celcnt;
  ub1 *map = breg->map;

  if (ip < base) return Fln;
  ofs8 = ip - base;
  if (ofs8 >= reg->len) return Fln;
  cel = (ub4)(ofs8 / cellen);
  if (cel * cellen != ofs8)  return Fln;
  if (cel >= celcnt)  return Fln;

  if (map[cel] == 0) return Fln;
  breg->cnt--;
  map[cel] = 0;
  return 0;
}

static void bist_init(heap *hb)
{
  hb->bistregs = osmmap(Bistregs * sizeof(struct st_bregion));
}

static ub4 bist_add(heap *hb,region *reg,void *p,size_t len,enum Loc loc)
{
  ub4 bi;
  ub4 *p4 = (ub4 *)p;
  ub4 id = reg->uid;
  bregion *breg = hb->bistregs + id;
  ub4 rv;

  if (p == nil || id >= Bistregs || len > hi32) return Fln;

  if (breg->map == nil) newbistslab(hb,breg,reg,id,(ub4)len);
  if (breg->map == nil) return Fln;
  rv = bist_alloc(hb,breg,reg,p,(ub4)len,loc);
  if (rv) {
    error(hb->errmsg,loc,"bist_add rerror at line %u",rv & hi16)
  }
  return rv;
}

// caller, owner
static ub4 bist_del(heap *hb,region *reg,size_t ip,enum Loc loc)
{
  ub4 bi,pos = hb->bistpos;
  ub4 id = reg->uid;
  bregion *breg = hb->bistregs + id;
  size_t len;
  ub4 rv;

  if (ip == 0 || id >= Bistregs || breg->celcnt == 0) return Fln;

  rv = bist_free(hb,breg,reg,ip,loc);
  if (rv) {
    error(hb->errmsg,loc,"bist_del rerror at line %u",rv & hi16)
  }
  return rv;
}
