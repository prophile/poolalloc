#ifndef _INCLUDED_POOL
#define _INCLUDED_POOL

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct _pool pool;

pool* pool_create_extra(unsigned objsize, unsigned extradata, void** edata);

inline static pool* pool_create(unsigned objsize)
	{ return pool_create_extra(objsize, 0, 0); }
void pool_destroy(pool* pl);

void* pool_alloc(pool* pl);
void pool_free(pool* pl, void* ptr);

#ifdef __cplusplus
}
#endif

#endif
