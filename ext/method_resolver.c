#include <method_resolver.h>

static inline id_key_t
id2key(ID id)
{
    return rb_id_to_serial(id);
}

static int
list_table_index(struct list_id_table *tbl, id_key_t key)
{
    const int num = tbl->num;
    const id_key_t *keys = tbl->keys;

    int i;

    for (i=0; i<num; i++) {
    	assert(keys[i] != 0);

        if (keys[i] == key) {
            return (int)i;
        }
    }
    return -1;
}

int
my_id_table_lookup(struct list_id_table *tbl, ID id, VALUE *valp)
{
    fprintf(stderr, "#14 %d %d\n", tbl->capa, tbl->num);
    id_key_t key = id2key(id);
    int index = list_table_index(tbl, key);
    fprintf(stderr, "#18\n");
    if (index >= 0) {
        fprintf(stderr, "#19 %d\n", ID_TABLE_USE_CALC_VALUES);
        *valp = TABLE_VALUES(tbl)[index];
        fprintf(stderr, "#20\n");

        return TRUE;
    }
    else {
	    return FALSE;
    }
}

static inline rb_method_entry_t *
lookup_method_table(VALUE klass, ID id)
{
    fprintf(stderr, "#11\n");
    st_data_t body;
    fprintf(stderr, "#11'\n");
    struct rb_id_table *m_tbl = RMODULE_M_TBL(klass);

    fprintf(stderr, "#12\n");

    if (my_id_table_lookup(m_tbl, id, &body)) {
	    fprintf(stderr, "#13\n");
	    return (rb_method_entry_t *) body;
    }
    else {
	    return 0;
    }
}

rb_method_entry_t*
search_method(VALUE klass, ID id, VALUE *defined_class_ptr)
{
    rb_method_entry_t *me;

    for (me = 0; klass; klass = RCLASS_SUPER(klass)) {
	    fprintf(stderr, "#1\n");
	    if ((me = lookup_method_table(klass, id)) != 0)
	        break;
	    fprintf(stderr, "#2\n");
    }

    if (defined_class_ptr)
	    *defined_class_ptr = klass;
    return me;
}