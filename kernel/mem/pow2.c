/*                    The Quest Operating System
 *  Copyright (C) 2005-2012  Richard West, Boston University
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "kernel.h"
#include "mem/mem.h"
#include "smp/spinlock.h"
#include <util/printf.h>

//#define DEBUG_POW2

#ifdef DEBUG_POW2
#define DLOG(fmt,...) DLOG_PREFIX("pow2",fmt,##__VA_ARGS__)
#else
#define DLOG(fmt,...) ;
#endif

/* power-of-2 memory allocator works with blocks in increasing sizes
 * in powers of 2, from 2^POW2_MIN_POW to 2^POW2_MAX_POW */

#define POW2_MIN_POW 5
#define POW2_MAX_POW 22
/* NB: the value of POW2_MAX_POW has to be smaller than 2^POW2_MIN_POW
 * because it needs to be able to fit within the POW2_MASK_POW.  Also
 * keep in mind it needs to be able to find a contiguous virtual
 * address range every time it allocates one of these large
 * blocks. */

#define POW2_MIN_SIZE (1<<POW2_MIN_POW)
#define POW2_MAX_SIZE (1<<POW2_MAX_POW)

/* Length of the central header table */
#define POW2_TABLE_LEN ((POW2_MAX_POW - POW2_MIN_POW)+1)

/* How many frames to allocate for a max-size block: */
#define POW2_MAX_POW_FRAMES (1 << (POW2_MAX_POW - 12))

/* Mask the index from a descriptor: */
#define POW2_MASK_POW ((1<<POW2_MIN_POW)-1)

struct _POW2_HEADER
{
  struct _POW2_HEADER *next;
  uint32 count;
  uint8 *ptrs[0];
} PACKED;
typedef struct _POW2_HEADER POW2_HEADER;
#define POW2_MAX_COUNT ((0x1000 - sizeof(POW2_HEADER)) >> 2)

static POW2_HEADER *pow2_table[POW2_TABLE_LEN];

static uint32 *pow2_used_table; /* array of descriptors:
                                 * pointer | index */
static uint32 pow2_used_count, pow2_used_table_pages;

static spinlock pow2_lock = SPINLOCK_INIT;

static void
pow2_add_free_block (uint8 * ptr, uint8 index)
{
  POW2_HEADER *hdr = pow2_table[index - POW2_MIN_POW];

  for (;;) {
    if (hdr->count < POW2_MAX_COUNT) {
      /* There is room in the current header's block */
      hdr->ptrs[hdr->count++] = ptr;
      return;
    } else {
      /* There is not enough room */
      if (hdr->next) {
        /* Chase the next pointer */
        hdr = hdr->next;
      } else {
        /* End of the list -- make a new header */
        uint32 frame = alloc_phys_frame ();
        if(frame == 0xFFFFFFFF) {
          DLOG("Failed to allocate physical frame in %s", __FUNCTION__);
          panic("Failed to allocate physical frame in pow2_add_free_block");
        }
        if((hdr->next = map_malloc_pool_virtual_page (frame | 3)) == NULL) {
          DLOG("Failed to map malloc pool frame");
          panic("Failed to map malloc pool frame");
        }

        memset (hdr->next, 0, 0x1000);
        hdr = hdr->next;
      }
    }
  }
}

/* Trying to avoid making the frame-size of pow2_get_free_block any
 * larger than it is -- because of recursion -- and this is used in a
 * re-entrantly safe fashion. */
static uint32 pow2_tmp_phys_frames[POW2_MAX_POW_FRAMES];

static uint8 *
pow2_get_free_block (uint8 index)
{
  POW2_HEADER *hdr = pow2_table[index - POW2_MIN_POW], *prev = NULL;
  uint8 *ptr;

  for (;;) {
    if (hdr->count == 0) {
      /* no free blocks -- get one */
      /* hdr->count can only be 0 for first in chain */
      if (index < POW2_MAX_POW) {
        uint8 *ptr1, *ptr2;

        /* Recursive call: 32 bytes per frame, 11 calls deep, 352
         * bytes of stack worst-case.  Should be acceptable, for
         * now. */
        ptr1 = pow2_get_free_block (index + 1);
        ptr2 = ptr1 + (1 << (index));

        pow2_add_free_block (ptr1, index);

        /* It is safe to return ptr2 here because if there were no
         * free blocks upon entering this section of code, then there
         * is no need to check 'prev' and possibly zero its next
         * pointer. */
        return ptr2;
      } else {
        /* grab new pages */
        int i;
        for (i = 0; i < POW2_MAX_POW_FRAMES; i++) {
          pow2_tmp_phys_frames[i] = alloc_phys_frame () | 3;
          if(pow2_tmp_phys_frames[i] == 0xFFFFFFFF) {
            DLOG("Failed to allocate physical frame in %s", __FUNCTION__);
            panic("Failed to allocate physical frame for malloc pool");
          }
        }
        return map_malloc_pool_virtual_pages (pow2_tmp_phys_frames, POW2_MAX_POW_FRAMES);
      }
    } else if (hdr->count < POW2_MAX_COUNT || hdr->next == NULL) {
      /* There are free blocks ready to go */
      ptr = hdr->ptrs[--hdr->count];
      if (prev && hdr->count == 0) {
        /* We followed a next pointer to get here. */
        uint32 frame = (uint32) get_phys_addr ((void *) hdr);
        prev->next = NULL;
        unmap_malloc_pool_virtual_page ((void *) hdr);
        free_phys_frame (frame);
      }
      return ptr;
    } else {
      /* hdr->count == POW2_MAX_COUNT && hdr->next != NULL */
      /* chase next pointer and try to use that header instead */
      prev = hdr;
      hdr = hdr->next;
    }
  }
}

static void
pow2_insert_used_table (uint8 * ptr, uint8 index)
{
  if (pow2_used_count >= (pow2_used_table_pages * 0x400)) {
    uint32 count = pow2_used_table_pages + 1;
    uint32 frames = alloc_phys_frames (count), old_frames;
    void *virt;

    if(frames == 0xFFFFFFFF) {
      DLOG("Failed to allocate physical frames in %s", __FUNCTION__);
      panic("Failed to allocate physical frames for malloc pool");
    }

    virt = map_contiguous_malloc_pool_virtual_pages (frames | 3, count);
    if(virt == NULL) {
      DLOG("Failed to map frames for malloc pool in %s", __FUNCTION__);
      panic("Failed to map frames for malloc pool");
    }
    memcpy (virt, pow2_used_table, sizeof (uint32) * pow2_used_count);
    old_frames = (uint32) get_phys_addr (pow2_used_table);
    unmap_malloc_pool_virtual_pages ((void *) pow2_used_table, pow2_used_table_pages);
    free_phys_frames (old_frames, pow2_used_table_pages);
    pow2_used_table = (uint32 *) virt;
    pow2_used_table_pages = count;
  }
  pow2_used_table[pow2_used_count++] = (uint32) ptr | (uint32) index;
}

static int
pow2_remove_used_table (uint8 * ptr, uint8 * index)
{
  int i;
  for (i = 0; i < pow2_used_count; i++) {
    if ((uint32) ptr == (pow2_used_table[i] & (~POW2_MASK_POW))) {
      /* found it */
      *index = pow2_used_table[i] & POW2_MASK_POW;
      pow2_used_table[i] = pow2_used_table[--pow2_used_count];
      pow2_used_table[pow2_used_count] = 0;
      return 0;
    }
  }
  return -1;
}

static uint8
pow2_compute_index (uint32 size)
{
  int i;
  if (size <= POW2_MIN_SIZE)
    return POW2_MIN_POW;
  else if (size >= POW2_MAX_SIZE)
    return POW2_MAX_POW;
  else {
    size--;
    /* bit scan reverse -- find most significant set bit */
    asm volatile ("bsrl %1,%0":"=r" (i):"r" (size));
    return (i + 1);
  }
}

int
pow2_alloc (uint32 size, uint8 ** ptr)
{
  uint8 index;

  if(size > POW2_MAX_SIZE) {
    DLOG("tried to allocate something larger "
                "than the pow2 allocator allows");
    *ptr = NULL;
    return -1;
  }
  index = pow2_compute_index (size);
  spinlock_lock (&pow2_lock);
  *ptr = pow2_get_free_block (index);
  if(*ptr == NULL) {
    return -1;
  }
  pow2_insert_used_table (*ptr, index);
  spinlock_unlock (&pow2_lock);
  if(*ptr != NULL) {
    memset(*ptr, 0, size);
  }
  return index;
}

void* kmalloc(uint32 size) {
  uint8* temp;
  pow2_alloc(size, &temp);
  return temp;
}

void
pow2_free (uint8 * ptr)
{
  uint8 index;
  spinlock_lock (&pow2_lock);
  if (pow2_remove_used_table (ptr, &index) == 0) {
    pow2_add_free_block (ptr, index);
  }
  spinlock_unlock (&pow2_lock);
}

void kfree(void* ptr)
{
  pow2_free(ptr);
}

bool malloc_uses_page_tables(void) { return TRUE; }

void
init_malloc(void)
{
  int i;
  uint32 frame;
  for (i = 0; i < POW2_TABLE_LEN; i++) {
    frame = alloc_phys_frame ();
    if(frame == 0xFFFFFFFF) {
      panic("Failed to allocate physical frame for pow2 allocator");
    }
    pow2_table[i] = map_malloc_pool_virtual_page (frame | 3);
    memset (pow2_table[i], 0, 0x1000);
  }
  frame = alloc_phys_frame ();
  if(frame == 0xFFFFFFFF) {
    panic("Failed to allocate physical frame for pow2 allocator");
  }
  pow2_used_table = map_malloc_pool_virtual_page (frame | 3);
  memset (pow2_used_table, 0, 0x1000);
  pow2_used_count = 0;
  pow2_used_table_pages = 1;
}

/* 
 * Local Variables:
 * indent-tabs-mode: nil
 * mode: C
 * c-file-style: "gnu"
 * c-basic-offset: 2
 * End: 
 */

/* vi: set et sw=2 sts=2: */
