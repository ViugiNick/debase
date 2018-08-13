#ifndef METHOD_RESOLVER
#define METHOD_RESOLVER

#include <ruby.h>
#include <ruby/debug.h>
#include <vm_core.h>
#include <version.h>
#include <iseq.h>
#include <vm_insnhelper.h>
#include <method.h>
#include <symbol.h>
#include <id_table.h>
#include <gc.h>

#define         FALSE   0
#define         TRUE    1

#ifndef ID_TABLE_IMPL
#define ID_TABLE_IMPL 34
#endif

#if ID_TABLE_IMPL == 0
#define ID_TABLE_NAME st
#define ID_TABLE_IMPL_TYPE struct st_id_table

#define ID_TABLE_USE_ST 1
#define ID_TABLE_USE_ST_DEBUG 1

#elif ID_TABLE_IMPL == 1
#define ID_TABLE_NAME st
#define ID_TABLE_IMPL_TYPE struct st_id_table

#define ID_TABLE_USE_ST 1
#define ID_TABLE_USE_ST_DEBUG 0

#elif ID_TABLE_IMPL == 11
#define ID_TABLE_NAME list
#define ID_TABLE_IMPL_TYPE struct list_id_table

#define ID_TABLE_USE_LIST 1
#define ID_TABLE_USE_CALC_VALUES 1

#elif ID_TABLE_IMPL == 12
#define ID_TABLE_NAME list
#define ID_TABLE_IMPL_TYPE struct list_id_table

#define ID_TABLE_USE_LIST 1
#define ID_TABLE_USE_CALC_VALUES 1
#define ID_TABLE_USE_ID_SERIAL 1

#elif ID_TABLE_IMPL == 13
#define ID_TABLE_NAME list
#define ID_TABLE_IMPL_TYPE struct list_id_table

#define ID_TABLE_USE_LIST 1
#define ID_TABLE_USE_CALC_VALUES 1
#define ID_TABLE_USE_ID_SERIAL 1
#define ID_TABLE_SWAP_RECENT_ACCESS 1

#elif ID_TABLE_IMPL == 14
#define ID_TABLE_NAME list
#define ID_TABLE_IMPL_TYPE struct list_id_table

#define ID_TABLE_USE_LIST 1
#define ID_TABLE_USE_CALC_VALUES 1
#define ID_TABLE_USE_ID_SERIAL 1
#define ID_TABLE_USE_LIST_SORTED 1

#elif ID_TABLE_IMPL == 15
#define ID_TABLE_NAME list
#define ID_TABLE_IMPL_TYPE struct list_id_table

#define ID_TABLE_USE_LIST 1
#define ID_TABLE_USE_CALC_VALUES 1
#define ID_TABLE_USE_ID_SERIAL 1
#define ID_TABLE_USE_LIST_SORTED 1
#define ID_TABLE_USE_LIST_SORTED_LINEAR_SMALL_RANGE 1

#elif ID_TABLE_IMPL == 21
#define ID_TABLE_NAME hash
#define ID_TABLE_IMPL_TYPE sa_table

#define ID_TABLE_USE_COALESCED_HASHING 1
#define ID_TABLE_USE_ID_SERIAL 1

#elif ID_TABLE_IMPL == 22
#define ID_TABLE_NAME hash
#define ID_TABLE_IMPL_TYPE struct hash_id_table

#define ID_TABLE_USE_SMALL_HASH 1
#define ID_TABLE_USE_ID_SERIAL 1

#elif ID_TABLE_IMPL == 31
#define ID_TABLE_NAME mix
#define ID_TABLE_IMPL_TYPE struct mix_id_table

#define ID_TABLE_USE_MIX 1
#define ID_TABLE_USE_MIX_LIST_MAX_CAPA 32

#define ID_TABLE_USE_ID_SERIAL 1

#define ID_TABLE_USE_LIST 1
#define ID_TABLE_USE_CALC_VALUES 1
#define ID_TABLE_USE_SMALL_HASH 1

#elif ID_TABLE_IMPL == 32
#define ID_TABLE_NAME mix
#define ID_TABLE_IMPL_TYPE struct mix_id_table

#define ID_TABLE_USE_MIX 1
#define ID_TABLE_USE_MIX_LIST_MAX_CAPA 32

#define ID_TABLE_USE_ID_SERIAL 1

#define ID_TABLE_USE_LIST 1
#define ID_TABLE_USE_CALC_VALUES 1
#define ID_TABLE_USE_LIST_SORTED 1

#define ID_TABLE_USE_SMALL_HASH 1

#elif ID_TABLE_IMPL == 33
#define ID_TABLE_NAME mix
#define ID_TABLE_IMPL_TYPE struct mix_id_table

#define ID_TABLE_USE_MIX 1
#define ID_TABLE_USE_MIX_LIST_MAX_CAPA 64

#define ID_TABLE_USE_ID_SERIAL 1

#define ID_TABLE_USE_LIST 1
#define ID_TABLE_USE_CALC_VALUES 1
#define ID_TABLE_USE_SMALL_HASH 1

#elif ID_TABLE_IMPL == 34
#define ID_TABLE_NAME mix
#define ID_TABLE_IMPL_TYPE struct mix_id_table

#define ID_TABLE_USE_MIX 1
#define ID_TABLE_USE_MIX_LIST_MAX_CAPA 64

#define ID_TABLE_USE_ID_SERIAL 1

#define ID_TABLE_USE_LIST 1
#define ID_TABLE_USE_CALC_VALUES 1
#define ID_TABLE_USE_LIST_SORTED 1

#define ID_TABLE_USE_SMALL_HASH 1

#elif ID_TABLE_IMPL == 35
#define ID_TABLE_NAME mix
#define ID_TABLE_IMPL_TYPE struct mix_id_table

#define ID_TABLE_USE_MIX 1
#define ID_TABLE_USE_MIX_LIST_MAX_CAPA 64

#define ID_TABLE_USE_ID_SERIAL 1

#define ID_TABLE_USE_LIST 1
#define ID_TABLE_USE_CALC_VALUES 1
#define ID_TABLE_USE_LIST_SORTED 1
#define ID_TABLE_USE_LIST_SORTED_LINEAR_SMALL_RANGE 1

#define ID_TABLE_USE_SMALL_HASH 1

#else
#error
#endif

#if ID_TABLE_SWAP_RECENT_ACCESS && ID_TABLE_USE_LIST_SORTED
#error
#endif

/* IMPL(create) will be "hash_id_table_create" and so on */
#define IMPL1(name, op) TOKEN_PASTE(name, _id##op) /* expand `name' */
#define IMPL(op)        IMPL1(ID_TABLE_NAME, _table##op) /* but prevent `op' */

#ifdef __GNUC__
# define UNUSED(func) static func __attribute__((unused))
#else
# define UNUSED(func) static func
#endif

typedef rb_id_serial_t id_key_t;

typedef struct rb_id_item {
    id_key_t key;
#if SIZEOF_VALUE == 8
    int      collision;
#endif
    VALUE    val;
} item_t;

#if SIZEOF_VALUE == 8
#define ITEM_GET_KEY(tbl, i) ((tbl)->items[i].key)
#define ITEM_KEY_ISSET(tbl, i) ((tbl)->items[i].key)
#define ITEM_COLLIDED(tbl, i) ((tbl)->items[i].collision)
#define ITEM_SET_COLLIDED(tbl, i) ((tbl)->items[i].collision = 1)
#else
#define ITEM_GET_KEY(tbl, i) ((tbl)->items[i].key >> 1)
#define ITEM_KEY_ISSET(tbl, i) ((tbl)->items[i].key > 1)
#define ITEM_COLLIDED(tbl, i) ((tbl)->items[i].key & 1)
#define ITEM_SET_COLLIDED(tbl, i) ((tbl)->items[i].key |= 1)
#endif

#if ID_TABLE_USE_CALC_VALUES
    #define TABLE_VALUES(tbl) ((VALUE *)((tbl)->keys + (tbl)->capa))
#else
    #define TABLE_VALUES(tbl) (tbl)->values_
#endif

struct list_id_table {
    int capa;
    int num;
    id_key_t *keys;
#if ID_TABLE_USE_CALC_VALUES == 0
    VALUE *values_;
#endif
};

struct rb_id_table {
    int capa;
    int num;
    int used;
    item_t *items;
};

rb_method_entry_t* search_method(VALUE klass, ID id, VALUE *defined_class_ptr);
#endif