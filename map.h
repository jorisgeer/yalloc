/* map.h - open addressing concurrent hash table

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

 */

// David Stafford's murmur3 variant mixer - http://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html
Overflows
static uint64_t murmurmix(uint64_t x) { // mix 13 = 30	0xbf58476d1ce4e5b9	27	0x94d049bb133111eb	31
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9UL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebUL;
  x ^= x >> 31;

  return x;
}

static struct tidentry *map_getadd(struct tidentry *mp,size_t tid,ub4 ord,ub4 *pcnt)
{
  ub4 cnt,len = 1U << ord;
  uint64_t h = murmurmix((uint64_t)tid);
  ub4 msk = len - 1;
  ub4 k2,k = h & msk;
  struct tidentry *hp = mp + k; // first probe
  size_t zero;

  size_t trytid = atomic_load(&hp->tid);

  sassert(Hash_order > 6,"hash order > 6");

  if (likely(trytid == tid)) return hp;

  zero = 0;
  if (trytid == 0) {
    if (atomic_compare_exchange_strong(&hp->tid,&zero,tid)) return hp;
  }

  h >>= ord;
  k = (k + h) & msk; // second probe with rest of hash
  k2 = k; cnt = 0;

  do {
    hp = mp + k;
    trytid = atomic_load(&hp->tid);
    if (trytid == tid) { *pcnt = cnt; return hp; }
    if (trytid == 0) {
      if (atomic_compare_exchange_strong(&hp->tid,&zero,tid)) { *pcnt = cnt; return hp; }
    }
    k = (k + 1) & msk;
    cnt++;
  } while (k != k2);
  return nil;
}

static struct tidentry *map_add(struct tidentry *mp,size_t tid,ub4 ord,ub4 msk)
{
  uint64_t h = murmurmix((uint64_t)tid);
  ub4 k2,k = h & msk;
  struct tidentry *hp = mp + k;
  size_t zero;

  size_t trytid = atomic_load(&hp->tid);

  if (unlikely(trytid == tid)) return hp;

  zero = 0;
  if (trytid == 0) {
    if (atomic_compare_exchange_strong(&hp->tid,&zero,tid)) return hp;
  }

  h >>= ord;
  k = (k + h) & msk;
  k2 = k;

  do {
    hp = mp + k;
    trytid = atomic_load(&hp->tid);
    if (trytid == tid) return hp;
    if (trytid == 0) {
      if (atomic_compare_exchange_strong(&hp->tid,&zero,tid)) return hp;
    }
    k = (k + 1) & msk;
  } while (k != k2);
  return nil;
}

static void map_grow(struct tidentry *mp,struct tidentry *newmp,ub4 ord,ub4 neword)
{
  ub4 len = 1U << ord;
  ub4 newlen = 1U << neword;
  ub4 newmsk = newlen - 1;
  ub4 k;
  struct tidentry *hp,*newhp;
  size_t tid;

  for (k = 0; k < len; k++) {
    hp = mp + k;
    tid = atomic_load(&hp->tid);
    if (tid == 0) continue;
    newhp = map_add(newmp,tid,neword,newmsk);
    if (unlikely(newhp == nil)) continue;
    newhp->heap = hp->heap;
    newhp->delcnt = hp->delcnt;
  }
}
