/*
* $Id:  $
* $Version: $
*
* Copyright (c) Tanel Tammet 2004,2005,2006,2007,2008,2009
*
* Contact: tanel.tammet@gmail.com                 
*
* This file is part of wgandalf
*
* Wgandalf is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
* 
* Wgandalf is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
* 
* You should have received a copy of the GNU General Public License
* along with Wgandalf.  If not, see <http://www.gnu.org/licenses/>.
*
*/

 /** @file dballoc.h
 * Public headers for database heap allocation procedures.
 */

#ifndef __defined_dballoc_h
#define __defined_dballoc_h

#include "../config.h"

/*


Levels of allocation used:

- Memory segment allocation: gives a large contiguous area of memory (typically shared memory).
  Could be extended later (has to be contiguous).

- Inside the contiguous memory segment: Allocate usage areas for different heaps
  (data records, strings, doubles, lists, etc).
  Each area is typically not contiguous: can consist of several subareas of different length.
  
  Areas have different object allocation principles:
  - fixed-length object area (e.g. list cells) allocation uses pre-calced freelists
  - various-length object area (e.g. data records) allocation uses ordinary allocation techniques:
    - objects initialised from next free  / designated victim object, split as needed
    - short freed objects are put into freelists in size-corresponding buckets
    - large freed object lists contain objects of different sizes
  
- Data object allocation: data records, strings, list cells etc. 
  Allocated in corresponding subareas.

Fixlen allocation:

- Fixlen objects are allocated using a pre-calced singly-linked freelist. When one subarea 
  is exhausted(freelist empty), a new subarea is taken, it is organised into a long 
  freelist and the beginning of the freelist is stored in db_area_header.freelist.

- Each freelist element is one fixlen object. The first gint of the object is an offset of
  the next freelist element. The list is terminated with 0.

Varlen allocation follows the main ideas of the Doug Lea allocator:

- the minimum size to allocate is 4 gints (MIN_VARLENOBJ_SIZE) and all objects
  should be aligned at least to a gint.

- each varlen area contains a number of gint-size buckets for storing different
  doubly-linked freelists. The buckets are:
  - EXACTBUCKETS_NR of buckets for exact object size. Contains an offset of the first
      free object of this size.
  - VARBUCKETS_NR of buckets for variable (interval between prev and next) object size,
      growing exponentially. Contains an offset of the first free object in this size interval.
  - EXACTBUCKETS_NR+VARBUCKETS_NR+1 is a designated victim (marked as in use): 
      offset of the preferred place to split off new objects. 
      Initially the whole free area is made one big designated victim.
  - EXACTBUCKETS_NR+VARBUCKETS_NR+2 is a size of the designated victim.
    
- a free object contains gints:
  - size (in bytes) with last two bits marked (i.e. not part of size!):
    - last bit: 1 (iff set, a free object)
    - one-before-last: iff set, then previous neighbour is also free 
  - offset of the next element in the freelist (terminated with 0).  
  - offset of the previous element in the freelist (can be offset of the bucket!)
  ... arbitrary nr of bytes ...
  - size (in bytes) with last two bits marked as the initial size gint.
    This repeats the initial size gint and is located at the very end of the
    memory block.

- an in-use object contains gints:
  - size (in bytes) with mark bits and assumptions:
     - last 2 bits markers, not part of size:
        - for normal in-use objects 00 
        - for specials (dv area and start/end markers) 10
     - note that the last bit set 0 marks in-use (last bit 1 iff free): both normal and specials are in use
     - real size taken is always 8-aligned (minimal granularity 8 bytes)
     - size gint may be not 8-aligned if 32-bit gint used (but still has to be 4-aligned). In this case:
        - if size gint is not 8-aligned, real size taken either:
           - if size less than MIN_VARLENOBJ_SIZE, then MIN_VARLENOBJ_SIZE
           - else size+4 bytes (but used size is just size, no bytes added)
  - usable gints following
  
- a designated victim is marked to be in use: 
  - the first gint has last bits 10 to differentiate from normal in-use objects (00 bits) 
  - the second gint contains 0 to indicate that it is a dv object, and not start marker (1) or end marker (2)
  - all the following gints are arbitrary and contain no markup.
    
- the first 4 gints and the last 4 gints of each subarea are marked as in-use objects, although
  they should be never used! The reason is to give a markup for subarea beginning and end.
  - last bits 10 to differentiate from normal in-use objects (00 bits) 
  - the next gint is 1 for start marker an 2 for end marker
  - the following 2 gints are arbitrary and contain no markup 
  

*/

#define MEMSEGMENT_MAGIC_MARK 1232319011  /** enables to check that we really have db pointer */
#define SUBAREA_ARRAY_SIZE 64      /** nr of possible subareas in each area  */
#define INITIAL_SUBAREA_SIZE 8192  /** size of the first created subarea (bytes)  */
#define MINIMAL_SUBAREA_SIZE 8192  /** checked before subarea creation to filter out stupid requests */
#define SUBAREA_ALIGNMENT_BYTES 8          /** subarea alignment     */

#define EXACTBUCKETS_NR 256                  /** amount of free ob buckets with exact length */
#define VARBUCKETS_NR 32                   /** amount of free ob buckets with varying length */
#define CACHEBUCKETS_NR 2                  /** buckets used as special caches */
#define DVBUCKET EXACTBUCKETS_NR+VARBUCKETS_NR     /** cachebucket: designated victim offset */
#define DVSIZEBUCKET EXACTBUCKETS_NR+VARBUCKETS_NR+1 /** cachebucket: byte size of designated victim */
#define MIN_VARLENOBJ_SIZE (4*(gint)(sizeof(gint)))  /** minimal size of variable length object */
#define OBJSIZE_GRANULARITY ((gint)(sizeof(gint)))   /** object size must be multiple of OBJSIZE_GRANULARITY */

#define SHORTSTR_SIZE 32 /** max len of short strings  */

/* ====== general typedefs and macros ======= */

// integer and address fetch and store

typedef int gint;  /** always used instead of int. Pointers are also handled as gint. */

#define dbfetch(db,offset) (*((gint*)(((char*)(db))+(offset)))) /** get gint from address */
#define dbstore(db,offset,data) (*((gint*)(((char*)(db))+(offset)))=data) /** store gint to address */
#define dbaddr(db,realptr) ((gint)(((char*)(realptr))-((char*)(db)))) /** give offset of real adress */
#define offsettoptr(db,offset) ((void*)(((char*)(db))+(offset))) /** give real address from offset */
#define ptrtooffset(db,realptr) (dbaddr((db),(realptr)))
#define dbcheck(db) (dbfetch((db),0)==MEMSEGMENT_MAGIC_MARK) /** check that correct db ptr */

/* ==== fixlen object allocation macros ==== */

#define alloc_listcell(db) alloc_fixlen_object((db),&(((db_memsegment_header*)(db))->listcell_area_header))
#define alloc_shortstr(db) alloc_fixlen_object((db),&(((db_memsegment_header*)(db))->shortstr_area_header))
#define alloc_word(db) alloc_fixlen_object((db),&(((db_memsegment_header*)(db))->word_area_header))
#define alloc_doubleword(db) alloc_fixlen_object((db),&(((db_memsegment_header*)(db))->doubleword_area_header))

/* ==== varlen object allocation special macros ==== */

#define isfreeobject(i)  ((i) & 1) /** object is free if lowest bit is 1, otherwise it is in use */ 
#define isnormalusedobject(i)  (!((i) & 3)) /** object is normally in use if lowest bits are 00 */
#define isspecialusedobject(i)  (((i) & 3) == 2) /** object is a special in use dv object if lowest bits are 10 */
#define getfreeobjectsize(i) ((i) & ~3) /** mask off two lowest bits: just keep all higher */
/** small size marks always use MIN_VARLENOBJ_SIZE, 
* non-8-aligned size marks mean obj really takes 4 more bytes (all real used sizes are 8-aligned)
*/
#define getusedobjectsize(i) (((i) & ~3)<=MIN_VARLENOBJ_SIZE ?  MIN_VARLENOBJ_SIZE : ((((i) & ~3)%8) ? (((i) & ~3)+4) : ((i) & ~3)) )
#define makefreeobjectsize(i)  ((i) | 1) /** set lowest bit to 1: current object is free */
#define makeusedobjectsize(i) ((i) & ~3) /** zero two end bits */
#define makespecialusedobjectsize(i) (((i) & ~3) | 2) /** set two end bits to 10 */

#define SPECIALGINT1DV 1    /** second gint of a special in use dv area */
#define SPECIALGINT1START 0 /** second gint of a special in use start marker area, should be 0 */
#define SPECIALGINT1END 0 /** second gint of a special in use end marker area, should be 0 */

// #define setpfree(i)  ((i) | 2) /** set next lowest bit to 1: previous object is free ???? */

/* ===  data structures used in allocated areas  ===== */


/** general list cell: a pair of two integers (both can be also used as pointers) */

typedef struct {
  gint car;  /** first element */
  gint cdr;} /** second element, often a pointer to the rest of the list */
gcell;

#define car(cell)  (((gint)((gcell*)(cell)))->car)  /** get list cell first elem gint */
#define cdr(cell)  (((gint)((gcell*)(cell)))->car)  /** get list cell second elem gint */
  

/* ====== segment/area header data structures ======== */

/*
memory segment structure:
  
-------------  
db_memsegment_header
- - - - - - - 
db_area_header
-   -   -  -  
db_subarea_header
...
db_subarea_header
- - - - - - -
...  
- - - - - - - 
db_area_header
-   -   -  -  
db_subarea_header
...
db_subarea_header  
----------------
various actual subareas 
----------------
*/
  

/** located inside db_area_header: one single memory subarea header
*
*  alignedoffset should be always used: it may come some bytes after offset
*/
  
typedef struct _db_subarea_header {    
  gint size; /** size of subarea */
  gint offset;          /** subarea exact offset from segment start: do not use for objects! */
  gint alignedoffset;   /** subarea start as to be used for object allocation */
} db_subarea_header;


/** located inside db_memsegment_header: one single memory area header
*
*/

typedef struct _db_area_header {   
  gint fixedlength;        /** 1 if fixed length area, 0 if variable length */
  gint objlength;          /** only for fixedlength: length of allocatable obs in bytes */ 
  gint freelist;           /** freelist start: if 0, then no free objects available */ 
  gint last_subarea_index; /** last used subarea index (0,...,) */
  db_subarea_header subarea_array[SUBAREA_ARRAY_SIZE]; /** array of subarea headers */
  gint freebuckets[EXACTBUCKETS_NR+VARBUCKETS_NR+CACHEBUCKETS_NR]; /** array of subarea headers */
} db_area_header;

/** located at the very beginning of the memory segment
*
*/

typedef struct _db_memsegment_header {  
  gint mark;       /** fixed uncommon int to check if really a segment */ 
  gint size;       /** segment size in bytes  */
  gint free;       /** pointer to first free area in segment (aligned) */
  gint initialadr; /** initial segment address, only valid for creator */
  gint key;        /** global shared mem key */
  db_area_header datarec_area_header;     
  db_area_header longstr_area_header;
  db_area_header listcell_area_header;
  db_area_header shortstr_area_header;
  db_area_header word_area_header;
  db_area_header doubleword_area_header;  
} db_memsegment_header;




/* ==== Protos ==== */

gint init_db_memsegment(void* db, gint key, gint size); // creates initial memory structures for a new db
gint init_db_subarea(void* db, void* area_header, gint index, gint size);
gint alloc_db_segmentchunk(void* db, gint size); // allocates a next chunk from db memory segment

gint make_subarea_freelist(void* db, void* area_header, gint arrayindex);
gint init_area_buckets(void* db, void* area_header);
gint init_subarea_freespace(void* db, void* area_header, gint arrayindex);

gint alloc_fixlen_object(void* db, void* area_header);
gint extend_fixedlen_area(void* db, void* area_header);
void free_cell(void* db, gint cell);

gint alloc_gints(void* db, void* area_header, gint nr);
gint split_free(void* db, void* area_header, gint nr, gint* freebuckets, gint i);
gint extend_varlen_area(void* db, void* area_header, gint minbytes);
gint freebuckets_index(void* db, gint size);
gint free_object(void* db, void* area_header, gint object) ;

void show_db_memsegment_header(void* db);
void show_db_area_header(void* db, void* area_header);
void show_bucket_freeobjects(void* db, gint freelist);
gint count_freelist(void* db, gint freelist); 

gint check_db(void* db);
gint check_varlen_area_freelist(void* db, void* area_header);
gint check_bucket_freeobjects(void* db, void* area_header, gint bucketindex);
gint check_object_in_areabounds(void*db,void* area_header,gint offset,gint size);

gint show_dballoc_error_nr(void* db, char* errmsg, gint nr);
gint show_dballoc_error(void* db, char* errmsg);

/* ------- testing ------------ */

#endif