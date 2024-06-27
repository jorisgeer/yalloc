/* buddy.h - buddy stystem allocator

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

  Admin space overhead is 12%.
  Admin consists of a 'line' of allocated bitmap cells as ulongs per order. Each lower order doubles the cell count. Each cell bit represents a possible user block.
  The lowest few orders, representing 1 and 2-byte blocks, are omitted.
  admin is not cumulative: If block 2 of order 5 is used, blocks 4 and 5 of order 4 are *not* marked, nor is block 1 of order 6.

  2 bitmap sets are used:
   1 - An 'allocated' set is used for malloc() to search for a free block
   2 - A 'freed' set to tell invalid free apart from double free

  Each cell has a 1-byte order map for free() to determine the size given the pointer

  Cell address plus bit determines block address relative to region base, and order determines size. See below example.
  A summary counte per order tracks the number of available blocks.

  Each line has a set of accelerators to help locate a free block by reducing the search by 6 bits
  accel A of ulongs with 1 bit per line cell having at least one free block
  accel B of ulongs with 1 bit set per accel A
  accel C of ulongs with 1 bit set per accel B

  region layout for an example order 26 region of 64MB, with min_order 16:

  64 MB user data in 1 mmap block
  12 MB = 18% =  admin in 1 mmap block = 256k lines * ulong * 2 * 3 sets

  1 ulong base

  20 uint summary counts - number of free blocks per order

    order 2 accel
      1 ulong D
      1 ulong C  - 64 ulongs for 4G region
      64 ulong B
      4K ulong A

     order 3 accel
       ...

     order 14 accel

     -- first set of 'avail' bitmaps, used by alloc --
     256k ulong order 2 avail bitmap aka 'line'. Bit set means available. 16M * 4-byte blocks
     ...
     16k ulong order 6 avail bitmap. 64-byte blocks
     ...
     4k ulong order 8
      ...
      256 ulong order 12
      ...
      16 ulong order 16
      ...
      1 ulong order 20 - 64 * 1MB blocks
      ....
      1 ulong order 26

      -- second set of 'alloc' bitmaps, used by free --
         as above, no accelerators

      -- third set of bitmaps (check double free) --
        as above

    - 4 k ubyte order

  free() checks the order byte for its corresponding offset in the region. The block address tells which bit in which freemap cell corresponds to it.
  In example above,  an 4-byte block 30 KB into the region has freemap cell (=ulong) 480, bit 0
  If not allocated it was an invalid free

  alloc() looks in the summariies for the lowest suitable order
  If an exact match is found, it needs to be located:
  The accelerators for this order are scanned, from top to bottom, to locate the freemap cell and bit
  If only a higher order exists, recursive split is done down to the desired order

  free() uses the region directory to determine the region and the order to find the size and thus cell.
*/

#undef Logfile
#define Logfile Fbuddy

static ub4 admlens[Maxorder]; // todo

static region *newbuddy(heap *hb,ub4 order)
{
  // ub2 ordlo= max(Minorder,order - min(order,Orderrange));
  size_t len = 1UL << order;
  ub4 admlen = admlens[order];
  region *reg = newregion(hb,order,len,admlen,Rbuddy);

  return reg;
}

// body of buddy alloc
static void *buddy_allocreg(heap *hb,region *reg,uint32_t len,ub4 ord,ub4 alord,bool clear)
{
  ub8 *meta = reg->meta;
  void *user = reg->user;
  ub4 *sums = (ub4 *)meta;
  // ub2 order = reg->order;

  sums[alord]--;

  ylog(Lalloc,"heap %u reg %u len %u ord %u",hb->id,reg->id,len,ord);
  return nil;

  if (clear) memset(user,0,len);
  return user;
}

// as above, exact fit. No split & merge
static void *buddy_allocfixreg(heap *hb,region *reg,uint32_t len,ub4 ord,bool clear)
{
  // ub8 *meta = reg->meta;
  void *user = reg->user;

  ylog(Lalloc,"heap %u buddy reg %u len %u ord %u",hb->id,reg->id,len,ord);
  return nil;

  if (clear) memset(user,0,len);
  return user;
}

static void *buddy_alloc(heap *hb,size_t slen,bool clear)
{
  region *reg;
  uint32_t mask = hb->buddymask;
  ub4 ord,alord,order;

  uint32_t len = (uint32_t)slen;

  ord = 32u - clz(len);
  ylog(Lalloc,"buddy alloc len %u` ord %u %u mask %x",len,ord,clz(len),mask);
  if (len & (len-1)) len = 1U << (++ord); // next pwr2

  if ( (mask & (len - 1)) == 0 ) { // no space
    order = max(newregorder(),ord + Addorder);
    reg = newbuddy(hb,order);
    if (reg == nil) return nil;
    alord = ord;
    hb->buddies[ord - Minorder] = reg;
    hb->buddymask |= (1u << ord);
  } else {
    if (mask & len) {
      reg = hb->buddies[ord];
      return buddy_allocfixreg(hb,reg,len,ord,clear);
    }
    alord = clz(mask & (len - 1) ) & 31; // smallest size >= len
    reg = hb->buddies[alord];
  }
  return buddy_allocreg(hb,reg,len,ord,alord,clear);
}

static bool buddy_free(heap *hb,region *reg,size_t ip)
{
  ub8 *meta = reg->meta;
  void *user = reg->user;
  ub4 *sums = (ub4 *)meta;
  ub4 ord;
  ub4 order = reg->order;
  ub4 minord = reg->minorder;
  ub4 ordrng = order - minord;
  ub8 smask = reg->smask;
  uint32_t ofs = (uint32_t)(ip - (size_t)user) >> minord;
  ub4 cntord;

  ub1 *ordline = (ub1 *)(meta + ordrng);

  ord = ordline[ofs];

  cntord = (1u << (order - ord));

  if (++sums[ord] == cntord) smask |= ord;

  hb->buddyreg_f++;
  return (smask == hi32);
}

static void *buddy_realloc(heap *hb,region *reg,void *p,size_t newlen)
{
  size_t ip = (size_t)p;
  ub8 *meta = reg->meta;
  void *user = reg->user;
  ub4 order = reg->order;
  ub4 minord = reg->minorder;
  ub4 ordrng = order - minord;
  uint32_t ofs = (uint32_t)(ip - (size_t)user) >> minord;

  ub1 *ordline = (ub1 *)(meta + ordrng);
  ub4 ord = ordline[ofs];
  size_t orglen = 1ul << ord;

  void *np = buddy_alloc(hb,newlen,0);

  if (np == nil) return nil;
  memcpy(np,p,orglen);
  return np;
}
