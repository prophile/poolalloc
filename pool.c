#include "pool.h"
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

// some maths utility functions
// Get next power of two.
// This is useful because we require objects to be
// powers of two in size, for alignment.
static unsigned npot(unsigned x)
{
	--x;
	x = (x >>  1) | x;
	x = (x >>  2) | x;
	x = (x >>  4) | x;
	x = (x >>  8) | x;
	x = (x >> 16) | x;
	++x;
	return x;
}

// Slot Set.
// This structure keeps track of "slots", being free or empty, using bitwise arithmetic.
// It starts out equal to SS_EMPTY (all ones) and will fill up to SS_FULL.
typedef unsigned long ss_t;

#ifdef __ARM__
// on ARM, count leading zeros is cheaper than count trailing zeros
#define REVERSE_SS
#endif

// find an unmarked location
#ifdef REVERSE_SS

#define SIGNBIT 0x80000000

// Find a free slot in this slot set.
static inline unsigned ss_find(ss_t x)
{
	return __builtin_clz(x);
}

// Test whether a given slot is free.
static inline bool ss_test(ss_t x, unsigned idx)
{
	ss_t mask = SIGNBIT;
	mask >>= idx;
	return (x & mask) == 0;
}

// Mark a given slot as occupied.
static inline ss_t ss_mark(ss_t x, unsigned idx)
{
	ss_t mask = SIGNBIT;
	mask >>= idx;
	return x & ~mask;
}

// Mark a given slot as free.
static inline ss_t ss_unmark(ss_t x, unsigned idx)
{
	ss_t mask = SIGNBIT;
	mask >>= idx;
	return x | mask;
}
#else
// see comments above
static inline unsigned ss_find(ss_t x)
{
	return __builtin_ctz(x);
}

static inline bool ss_test(ss_t x, unsigned idx)
{
	ss_t mask = 1;
	mask <<= idx;
	return (x & mask) == 0;
}

static inline ss_t ss_mark(ss_t x, unsigned idx)
{
	ss_t mask = 1;
	mask <<= idx;
	return x & ~mask;
}

static inline ss_t ss_unmark(ss_t x, unsigned idx)
{
	ss_t mask = 1;
	mask <<= idx;
	return x | mask;
}
#endif

const static ss_t SS_EMPTY = -1UL;
const static ss_t SS_FULL = 0UL;

// The number of slots in one ss_t
#define SS_SIZE (sizeof(ss_t)*8)

// Slot Set Group.
// This is an extension of the slot-set mechanism.
// Adds a few more slots by combining slot groups.
#define SSG_COUNT (8/sizeof(ss_t))
#define SSG_SIZE (SSG_COUNT*SS_SIZE)

typedef struct _ssg {
	ss_t s[SSG_COUNT];
} ssg_t;

static inline bool ssg_is_full(ssg_t x)
{
	int i;
	for (i = 0; i < SSG_COUNT; ++i) {
		if (x.s[i] != SS_FULL) return false;
	}
	return true;
}

static inline int ssg_find(ssg_t x)
{
	int i, pos = 0;
	for (i = 0; i < SSG_COUNT; ++i) {
		if (x.s[i] != SS_FULL) {
			return pos + ss_find(x.s[i]);
		}
		pos += SS_SIZE;
	}
	return 0;
}

static inline unsigned _ssg_bucket(unsigned idx)
{
	return idx / SSG_COUNT;
}

static inline unsigned _ssg_subindex(unsigned idx)
{
	return idx % SSG_COUNT;
}

static inline bool ssg_test(ssg_t x, int idx)
{
	return ss_test(x.s[_ssg_bucket(idx)], _ssg_subindex(idx));
}

static inline ssg_t ssg_mark(ssg_t x, int idx)
{
	x.s[_ssg_bucket(idx)] = ss_mark(x.s[_ssg_bucket(idx)], _ssg_subindex(idx));
	return x;
}

static inline ssg_t ssg_unmark(ssg_t x, int idx)
{
	x.s[_ssg_bucket(idx)] = ss_unmark(x.s[_ssg_bucket(idx)], _ssg_subindex(idx));
	return x;
}

static inline ssg_t ssg_empty()
{
	ssg_t x;
	memset(&x, 0xFF, sizeof(x));
	return x;
}

// actual pool structure
struct _pool {
	// which slots are free
	ssg_t slots;
	// log2(object size). This is kept in this form for fast multiplies/divides,
	// since it's known to be a power of two.
	unsigned l2s;
	// the next chained pool, if this one runs out of space.
	pool* successor;
};

// Gets the pointer to the actual data buffer, from a given pool.
static unsigned char* pool_source(pool* p)
{
	return ((unsigned char*)p) + sizeof(pool);
}

// creates a new pool, optionally with extra data attached
// the extra data is just a convenient way of getting other data in
// the same malloc() call, to keep it nearby in memory and reduce heap eatage.
pool* pool_create_extra(unsigned objsize, unsigned extradata, void** edata)
{
	pool* pl;
	unsigned char* buf;
	assert(objsize);
	objsize = npot(objsize);
	// allocate the space using malloc()
	buf = malloc(sizeof(pool) + SSG_SIZE*objsize + extradata);
	assert(buf);
	pl = (pool*)(buf);
	// set everything up
	pl->l2s = __builtin_ctz(objsize);
	pl->slots = ssg_empty();
	pl->successor = 0;
	// set the extra data pointer, if it was requested
	if (edata)
		*edata = buf + sizeof(pool) + SSG_SIZE*objsize;
	return pl;
}

// free a pool
void pool_destroy(pool* pl)
{
	pool* next;
	if (!pl) {
		return;
	}
	// loop through all the chained pools, freeing them as we go
	// note the first free will free any extra data associated
	next = pl;
	while (next) {
		pl = next;
		next = next->successor;
		free(pl);
	}
}

// allocate one object out of the pool
void* pool_alloc(pool* pl)
{
	assert(pl);
	// check if this pool is full (and thus overflow)
	if (ssg_is_full(pl->slots)) {
		// if there's no successor pool, create it now
		if (!pl->successor) {
			pl->successor = pool_create(1 << pl->l2s);
		}
		// tail-recurse
		return pool_alloc(pl->successor);
	} else {
		// find the first free slot
		int pos = ssg_find(pl->slots);
		// find the location within the actual buffer
		unsigned char* loc = pool_source(pl) + (pos << pl->l2s);
		pl->slots = ssg_mark(pl->slots, pos);
		return loc;
	}
}

// free one object from the pool
void pool_free(pool* pl, void* ptr)
{
	unsigned char* p = (unsigned char*)ptr;
	assert(pl);
	if (!ptr) return;
	// check if it's within this pool
	if (p >= pool_source(pl) && p < (pool_source(pl) + (SSG_SIZE << pl->l2s))) {
		// in range - calculate slot
		// fast divide
		int slot = (unsigned)(p - pool_source(pl)) >> pl->l2s;
		assert(ssg_test(pl->slots, slot)); // check we had it marked
		// mark it as unused
		pl->slots = ssg_unmark(pl->slots, slot);
	} else {
		// it's not within this pool- check whether it's in an overflow pool
		if (pl->successor) {
			pool_free(pl->successor, ptr);
		} else {
			assert(0 && "trying to free a non-allocated pointer");
		}
	}
}
