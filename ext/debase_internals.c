#include <debase_internals.h>
#include <hacks.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <execinfo.h>

static VALUE mDebase;                 /* Ruby Debase Module object */
static VALUE cContext;

static int breakpoint_max = 0;

static breakpoint_list_node_t* breakpoint_list = NULL;
static breakpoint_list_node_t* checkpoint_list = NULL;


static VALUE idAlive;
static VALUE tpLine;
static VALUE tpLine2;
static VALUE tpCall;
static VALUE tpRaise;
static VALUE idAtLine;
static VALUE idAtBreakpoint;
static VALUE idMpLoadIseq;
static VALUE cDebugThread;
static VALUE contexts;
static VALUE breakpoints;

#define MY_RCLASS_M_TBL(c) (RCLASS(c)->m_tbl)

#if RUBY_API_VERSION_CODE >= 20500
  #if (RUBY_RELEASE_YEAR == 2017 && RUBY_RELEASE_MONTH == 10 && RUBY_RELEASE_DAY == 10) //workaround for 2.5.0-preview1
    #define TH_CFP(thread) ((rb_control_frame_t *)(thread)->ec.cfp)
  #else
    #define TH_CFP(thread) ((rb_control_frame_t *)(thread)->ec->cfp)
  #endif
#else
  #define TH_CFP(thread) ((rb_control_frame_t *)(thread)->cfp)
#endif

static int started = 0;

static const rb_iseq_t *
iseqw_check(VALUE iseqw)
{
    rb_iseq_t *iseq = DATA_PTR(iseqw);

    if (!iseq->body) {
	    ibf_load_iseq_complete(iseq);
    }

    if (!iseq->body->location.label) {
	    rb_raise(rb_eTypeError, "uninitialized InstructionSequence");
    }
    return iseq;
}

static void add_traces(rb_iseq_t* iseq);

struct set_specifc_data {
    int pos;
    int set;
    int order;
    int prev; /* 1: set, 2: unset, 0: not found */
};

static int
my_line_trace_specify(int line, rb_event_flag_t *events_ptr, void *ptr)
{
    struct set_specifc_data *data = (struct set_specifc_data *)ptr;

    if (data->pos == line) {
        if(data->order == 0) {
            data->prev = *events_ptr & RUBY_EVENT_SPECIFIED_LINE ? 1 : 2;
            if (data->set) {
                *events_ptr = *events_ptr | RUBY_EVENT_SPECIFIED_LINE;
            }
            else {
                *events_ptr = *events_ptr & ~RUBY_EVENT_SPECIFIED_LINE;
            }
            return 0; /* found */
        }
        else {
            data->order--;
        }
    }
    return 1;
}

static int
my_first_line_trace_specify(int line, rb_event_flag_t *events_ptr, void *ptr)
{
    struct set_specifc_data *data = (struct set_specifc_data *)ptr;

    data->prev = *events_ptr & RUBY_EVENT_SPECIFIED_LINE ? 1 : 2;
    if (data->set) {
        *events_ptr = *events_ptr | RUBY_EVENT_SPECIFIED_LINE;
    }
    else {
        *events_ptr = *events_ptr & ~RUBY_EVENT_SPECIFIED_LINE;
    }
    return 0;
}

VALUE
my_rb_iseqw_line_trace_specify_order(VALUE iseqval, int needed_line, int order, VALUE set)
{
    struct set_specifc_data data;

    data.prev = 0;
    data.pos = needed_line;
    data.order = order;
    if (data.pos < 0) rb_raise(rb_eTypeError, "`pos' is negative");

    switch (set) {
      case Qtrue:  data.set = 1; break;
      case Qfalse: data.set = 0; break;
      default:
	    rb_raise(rb_eTypeError, "`set' should be true/false");
    }

    rb_iseqw_line_trace_each(iseqval, my_line_trace_specify, (void *)&data);

    return data.prev == 1 ? Qtrue : Qfalse;
}

VALUE
my_rb_iseqw_line_trace_specify(VALUE iseqval, int needed_line, VALUE set)
{
    return my_rb_iseqw_line_trace_specify_order(iseqval, needed_line, 0, set);
}

VALUE
my_rb_iseqw_first_line_trace_specify(VALUE iseqval, VALUE set)
{
    struct set_specifc_data data;

    data.prev = 0;

    switch (set) {
      case Qtrue:  data.set = 1; break;
      case Qfalse: data.set = 0; break;
      default:
	    rb_raise(rb_eTypeError, "`set' should be true/false");
    }

    rb_iseqw_line_trace_each(iseqval, my_first_line_trace_specify, (void *)&data);

    return data.prev == 1 ? Qtrue : Qfalse;
}

static void debug_class_print(VALUE v) {
    ID sym_puts = rb_intern("puts");
    ID sym_inspect = rb_intern("class");
    rb_funcall(rb_mKernel, sym_puts, 1,
        rb_funcall(v, sym_inspect, 0));
}

static VALUE
Debase_thread_context(VALUE self, const rb_thread_t* thread)
{
  VALUE context;
  context = rb_hash_aref(contexts, (VALUE)thread);
  if (context == Qnil) {
    context = context_create(thread, cDebugThread);
    debug_class_print(context);
    rb_hash_aset(contexts, thread, context);
  }
  return context;
}

static VALUE
Debase_current_context(VALUE self)
{
  return Debase_thread_context(self, rb_thread_current());
}

static breakpoint_t* find_breakpoint_by_pos(char* path, int line) {
    breakpoint_list_node_t* breakpoint = breakpoint_list;

    while(breakpoint != NULL) {
        if(strcmp (breakpoint->breakpoint->source, path) == 0 && breakpoint->breakpoint->line == line) {
            return breakpoint->breakpoint;
        }
        breakpoint = breakpoint->next;
    }

    return NULL;
}

static breakpoint_t* find_checkpoint_by_pos(char* path, int line) {
    breakpoint_list_node_t* breakpoint = checkpoint_list;

    while(breakpoint != NULL) {
        if(strcmp (breakpoint->breakpoint->source, path) == 0 && breakpoint->breakpoint->line == line) {
            return breakpoint->breakpoint;
        }
        breakpoint = breakpoint->next;
    }

    return NULL;
}

static VALUE
fill_stack_and_invoke(const rb_debug_inspector_t *inspector, void *data)
{
  debug_context_t *context;
  VALUE context_object;

  context_object = *(VALUE *)data;
  Data_Get_Struct(context_object, debug_context_t, context);
  fill_stack(context, inspector);

  return Qnil;
}

static VALUE
start_inspector(VALUE data)
{
  return rb_debug_inspector_open(fill_stack_and_invoke, &data);
}

static VALUE
stop_inspector(VALUE data)
{
  return Qnil;
}

static int
remove_pause_flag(VALUE thread, VALUE context_object, VALUE ignored)
{
  debug_context_t *context;

  Data_Get_Struct(context_object, debug_context_t, context);
  context->thread_pause = 0;

  return ST_CONTINUE;
}

static void
call_at_line(debug_context_t *context, char *file, int line, VALUE context_object)
{
  rb_hash_foreach(contexts, remove_pause_flag, 0);
  CTX_FL_UNSET(context, CTX_FL_STEPPED);
  CTX_FL_UNSET(context, CTX_FL_FORCE_MOVE);
  context->last_file = file;
  context->last_line = line;
  rb_funcall(context_object, idAtLine, 2, rb_str_new2(file), INT2FIX(line));
}

static void debug_print(VALUE v) {
    ID sym_puts = rb_intern("puts");
    rb_funcall(rb_mKernel, sym_puts, 1, v);
}

static void debug_print_class(VALUE v) {
    ID sym_puts = rb_intern("puts");
    ID sym_inspect = rb_intern("class");
    rb_funcall(rb_mKernel, sym_puts, 1,
    rb_funcall(v, sym_inspect, 0));
}

rb_control_frame_t *
my_rb_vm_get_binding_creatable_next_cfp(const rb_thread_t *th, const rb_control_frame_t *cfp)
{
    while (!RUBY_VM_CONTROL_FRAME_STACK_OVERFLOW_P(th, cfp)) {
        if (cfp->iseq) {
            return (rb_control_frame_t *)cfp;
        }
	    cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
    }
    return 0;
}

static step_in_info_t*
get_step_in_info(rb_control_frame_t* cfp) {
    char type;
    int n;

    step_in_info_t* ans = ALLOC(step_in_info_t);
    ans->size = 0;

    if(cfp->iseq != NULL)
    {
        if(cfp->pc == NULL || cfp->iseq->body == NULL)
        {
            return Qnil;
        }
        else
        {
            const rb_iseq_t *iseq = cfp->iseq;
            VALUE* code = rb_iseq_original_iseq(iseq);

            char* file = RSTRING_PTR(StringValue(iseq->body->location.path));

            ptrdiff_t pc = cfp->pc - cfp->iseq->body->iseq_encoded;
            unsigned int size = cfp->iseq->body->iseq_size;

            for (n = pc; n < size;) {
                VALUE insn = code[n];
                char * instruction_name = insn_name(insn);

                const char *types = insn_op_types(insn);

                for (int j = 0; type = types[j]; j++) {
                    VALUE op = code[n + j + 1];

                    if(type == 0) {
                        break;
                    }

                    if(type == TS_CALLINFO) {
                        struct rb_call_info *ci = (struct rb_call_info *)op;

                        if (ci->mid) {
                            if(strcmp(instruction_name, "send") == 0) {
                                ans->size += 2;
                            }
                            if(strcmp(instruction_name, "opt_send_without_block") == 0) {
                                ans->size++;
                            }
                        }
                    }

                }

                n += insn_len(insn);
            }

            step_in_variant_t** variants = (step_in_variant_t*)malloc(ans->size + 1);
            int i = 0;
            VALUE mid_str = NULL;

            for (int insn_iter = 0, n = pc; n < size; insn_iter++, mid_str = NULL) {
                VALUE insn = code[n];
                char * instruction_name = insn_name(insn);

                const char *types = insn_op_types(insn);

                for (int j = 0; type = types[j]; j++) {
                    VALUE op = code[n + j + 1];

                    if(type == 0) {
                        return ans;
                    }

                    if(type == TS_CALLINFO) {
                        struct rb_call_info *ci = (struct rb_call_info *)op;

                        if (ci->mid) {
                            if(strcmp(instruction_name, "send") == 0) {
                                step_in_variant_t* variant = ALLOC(step_in_variant_t);
                                variant->mid = rb_id2str(ci->mid);
                                variant->pc_offset = insn_iter;
                                variant->block_iseq = NULL;
                                variant->pc = n + insn_len(insn);
                                variants[i++] = variant;
                                mid_str = rb_id2str(ci->mid);
                            }
                            if(strcmp(instruction_name, "opt_send_without_block") == 0) {
                                step_in_variant_t* variant = ALLOC(step_in_variant_t);
                                variant->mid = rb_id2str(ci->mid);
                                variant->pc_offset = insn_iter;
                                variant->block_iseq = NULL;
                                variant->pc = n + insn_len(insn);
                                variants[i++] = variant;
                            }
                        }
                    }

                    if(type == TS_ISEQ) {
                        if(mid_str != NULL) {
                            step_in_variant_t* variant = ALLOC(step_in_variant_t);
                            variant->mid = rb_sprintf("block for %"PRIsVALUE, mid_str);
                            variant->pc_offset = insn_iter;
                            variant->block_iseq = op;
                            variant->pc = n + insn_len(insn);
                            variants[i++] = variant;

                            mid_str = NULL;
                        }
                    }
                }

                n += insn_len(insn);
            }
            ans->variants = variants;
        }
    }
    return ans;
}

void c_add_breakpoint_first_line(rb_iseq_t *iseq)
{
    my_rb_iseqw_first_line_trace_specify(rb_iseqw_new(iseq), Qtrue);
}

static void
process_raise_event(VALUE trace_point, void *data)
{
  VALUE path;
  VALUE lineno;
  VALUE context_object;
  VALUE hit_count;
  VALUE exception_name;
  debug_context_t *context;
  rb_trace_point_t *tp;
  char *file;
  int line;
  int c_hit_count;

  context_object = Debase_current_context(mDebase);
  Data_Get_Struct(context_object, debug_context_t, context);
  if (!check_start_processing(context, rb_thread_current())) return;

  update_stack_size(context);
  tp = TRACE_POINT;

  path = rb_tracearg_path(tp);
  lineno = rb_tracearg_lineno(tp);

  file = RSTRING_PTR(path);
  line = FIX2INT(lineno);

  fprintf(stdout, "process raise %s:%d\n", file, line);
}

static void
process_call_event(VALUE trace_point, void *data)
{
    VALUE path;
    VALUE lineno;
    VALUE context_object;
    breakpoint_t* breakpoint;
    debug_context_t *context;
    rb_trace_point_t *tp;
    char *file;
    int line;
    int n;
    char type;

    tp = TRACE_POINT;
    path = rb_tracearg_path(tp);
    lineno = rb_tracearg_lineno(tp);
    file = RSTRING_PTR(path);
    line = FIX2INT(lineno);

    context_object = Debase_current_context(mDebase);
    Data_Get_Struct(context_object, debug_context_t, context);

    rb_thread_t *thread;
    rb_control_frame_t *cfp;
    thread = ruby_current_thread;
    cfp = TH_CFP(thread);
    rb_control_frame_t *prev_cfp = cfp + 1;

    int pc = prev_cfp->pc - prev_cfp->iseq->body->iseq_encoded;

    if(pc == context->stop_pc) {
        c_add_breakpoint_first_line(cfp->iseq);
        Debase_call_tracepoint_disable();
    }
}

static VALUE my_top_n(const rb_control_frame_t *cfp, int n) {
    return (*(cfp->sp -(n) - 1));
}

typedef ID id_key_t;

static int
list_id_table_lookup(struct list_id_table *tbl, ID id, VALUE *valp)
{
    id_key_t key = id2key(id);
    int index = list_table_index(tbl, key);

    if (index >= 0) {
	*valp = TABLE_VALUES(tbl)[index];

#if ID_TABLE_SWAP_RECENT_ACCESS
	if (index > 0) {
	    VALUE *values = TABLE_VALUES(tbl);
	    id_key_t tk = tbl->keys[index-1];
	    VALUE tv = values[index-1];
	    tbl->keys[index-1] = tbl->keys[index];
	    tbl->keys[index] = tk;
	    values[index-1] = values[index];
	    values[index] = tv;
	}
#endif /* ID_TABLE_SWAP_RECENT_ACCESS */
	return TRUE;
    }
    else {
	return FALSE;
    }
}

static int
hash_id_table_lookup(register sa_table *table, ID id, VALUE *valuep)
{
    register sa_entry *entry;
    id_key_t key = id2key(id);

    if (table->num_entries == 0) return 0;

    entry = table->entries + calc_pos(table, key);
    if (entry->next == SA_EMPTY) return 0;

    if (entry->key == key) goto found;
    if (entry->next == SA_LAST) return 0;

    entry = table->entries + (entry->next - SA_OFFSET);
    if (entry->key == key) goto found;

    while(entry->next != SA_LAST) {
        entry = table->entries + (entry->next - SA_OFFSET);
        if (entry->key == key) goto found;
    }
    return 0;
  found:
    if (valuep) *valuep = entry->value;
    return 1;
}

static int
rb_id_table_lookup(struct mix_id_table *tbl, ID id, VALUE *valp)
{
    if (LIST_P(tbl)) return list_id_table_lookup(&tbl->aux.list, id, valp);
    else             return hash_id_table_lookup(&tbl->aux.hash, id, valp);
}

static inline rb_method_entry_t *
lookup_method_table(VALUE klass, ID id)
{
    fprintf(stderr, "#11\n");
    st_data_t body;
    fprintf(stderr, "#11'\n");
    struct rb_id_table *m_tbl = RMODULE_M_TBL(klass);

    fprintf(stderr, "#12\n");

    if (rb_id_table_lookup(m_tbl, id, &body)) {
	    return (rb_method_entry_t *) body;
    }
    else {
	    return 0;
    }
}

static inline rb_method_entry_t*
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

static void
process_line_event(VALUE trace_point, void *data)
{
    VALUE path;
    VALUE lineno;
    VALUE context_object;
    breakpoint_t* breakpoint;
    debug_context_t *context;
    rb_trace_point_t *tp;
    char *file;
    int line;
    int n;
    char type;

    tp = TRACE_POINT;
    path = rb_tracearg_path(tp);
    lineno = rb_tracearg_lineno(tp);
    file = RSTRING_PTR(path);
    line = FIX2INT(lineno);

    breakpoint = find_breakpoint_by_pos(file, line);

    context_object = Debase_current_context(mDebase);
    Data_Get_Struct(context_object, debug_context_t, context);

    rb_thread_t *thread;
    rb_control_frame_t *cfp;
    thread = ruby_current_thread;
    cfp = TH_CFP(thread);

    cfp = my_rb_vm_get_binding_creatable_next_cfp(thread, cfp);
    rb_iseq_t *iseq = cfp->iseq;

    if(context->should_step_in > 0) {
        fprintf(stderr, "context->should_step_in %s %d\n", file, line);

        int pc = cfp->pc - iseq->body->iseq_encoded;
        printf("pc = %d\n", pc);
        VALUE* code = rb_iseq_original_iseq(iseq);

        struct rb_call_info *ci = (struct rb_call_info *)code[pc + 1];
        struct rb_call_cache *cc = (struct rb_call_cache *)code[pc + 2];

        VALUE defined_class;
        fprintf(stderr, "ci->orig_argc: %d\n", ci->orig_argc);
        VALUE klass = CLASS_OF(my_top_n(cfp, ci->orig_argc));

        cc->me = search_method(klass, ci->mid, &defined_class);

        c_add_breakpoint_first_line(cc->me->def->body.iseq.iseqptr);
    }

    context->iseq = cfp->iseq;
    context->pc = cfp->pc - cfp->iseq->body->iseq_encoded;
    context->step_in_info = get_step_in_info(cfp);

    rb_ensure(start_inspector, context_object, stop_inspector, Qnil);
    if(breakpoint != NULL) {
        context->stop_reason = CTX_STOP_BREAKPOINT;
        rb_funcall(context_object, idAtBreakpoint, 1, INT2NUM(breakpoint->id));
    }
    context->stop_reason = CTX_STOP_STEP;

    reset_stepping_stop_points(context);
    call_at_line(context, file, line, context_object);
}

static VALUE
Debase_setup_tracepoints(VALUE self)
{
  if (started) return Qnil;
  started = 1;

  rb_funcall(mDebase, idMpLoadIseq, 0);

  breakpoints = rb_hash_new();
  contexts = rb_hash_new();

  tpLine = rb_tracepoint_new(Qnil, RUBY_EVENT_SPECIFIED_LINE, process_line_event, NULL);
  rb_global_variable(&tpLine);

  rb_tracepoint_enable(tpLine);

  tpRaise = rb_tracepoint_new(Qnil, RUBY_EVENT_RAISE, process_raise_event, NULL);
  rb_global_variable(&tpRaise);

  return Qnil;
}

VALUE
Debase_call_tracepoint_enable()
{
  tpCall = rb_tracepoint_new(Qnil, RUBY_EVENT_CALL, process_call_event, NULL);
  rb_global_variable(&tpCall);

  rb_tracepoint_enable(tpCall);
  rb_tracepoint_enable(tpRaise);

  return Qnil;
}

VALUE
Debase_call_tracepoint_disable()
{
  rb_tracepoint_disable(tpCall);

  return Qnil;
}

static void
c_add_breakpoint(unsigned int lineno, rb_iseq_t *iseq)
{
    int i;
    VALUE *code;
    VALUE child = rb_ary_tmp_new(3);
    unsigned int size;
    size_t n;
    VALUE str = rb_str_new(0, 0);

    size = iseq->body->line_info_size;
    const struct iseq_line_info_entry *table = iseq->body->line_info_table;

    int left = 0;
    int right = size;

    int cnt = 0;

    my_rb_iseqw_line_trace_specify(rb_iseqw_new(iseq), lineno, Qtrue);

    size = iseq->body->iseq_size;
    code = rb_iseq_original_iseq(iseq);
    for (n = 0; n < size;) {
        n += rb_iseq_disasm_insn(str, code, n, iseq, child);
    }

    for (i = 0; i < RARRAY_LEN(child); i++) {
        VALUE isv = rb_ary_entry(child, i);
        rb_iseq_t *tmp_iseq = rb_iseq_check((rb_iseq_t *)isv);
        c_add_breakpoint(lineno, tmp_iseq);
    }
}

static void walk_throw_frames(rb_thread_t *th, char* source, int line)
{
        rb_control_frame_t *last_cfp = th->cfp;
        rb_control_frame_t *start_cfp = RUBY_VM_END_CONTROL_FRAME(th);
        rb_control_frame_t *cfp;
        ptrdiff_t size, i;

        start_cfp =
          RUBY_VM_NEXT_CONTROL_FRAME(
    	  RUBY_VM_NEXT_CONTROL_FRAME(start_cfp));

        if (start_cfp < last_cfp) {
    	    size = 0;
        }
        else {
    	    size = start_cfp - last_cfp + 1;
        }


        for (i = 0, cfp = start_cfp; i < size; i++, cfp = RUBY_VM_NEXT_CONTROL_FRAME(cfp)) {
            if (cfp->iseq) {
                if (cfp->pc) {
                    const rb_iseq_t *iseq = cfp->iseq;
                    char* file = RSTRING_PTR(StringValue(iseq->body->location.path));

                    if(strcmp (source, file) == 0) {
                        c_add_breakpoint(line, iseq);
                        break;
                    }
                }
    	    }
    	}
}

static void walk_throw_threads(char* source, int line)
{
    rb_vm_t *vm = GET_THREAD()->vm;
    rb_thread_t *th = 0;

    list_for_each(&vm->living_threads, th, vmlt_node) {
            switch (th->status) {
              case THREAD_RUNNABLE:
              case THREAD_STOPPED:
              case THREAD_STOPPED_FOREVER:
                walk_throw_frames(th, source, line);
              default:
                continue;
            }
        }
}

static VALUE
Debase_add_breakpoint(VALUE self, VALUE source, VALUE line, VALUE expr)
{
    breakpoint_t *breakpoint;
    breakpoint_list_node_t *breakpoint_node;

    breakpoint = ALLOC(breakpoint_t);

    breakpoint->id = ++breakpoint_max;
    breakpoint->source = RSTRING_PTR(StringValue(source));
    breakpoint->line = FIX2INT(line);
    breakpoint->expr = expr;

    breakpoint_node = ALLOC(breakpoint_list_node_t);
    breakpoint_node->breakpoint = breakpoint;
    breakpoint_node->next = breakpoint_list;
    breakpoint_list = breakpoint_node;

    walk_throw_threads(breakpoint->source, breakpoint->line);

    return INT2NUM(breakpoint->id);
}

static VALUE
Debase_prepare_context(VALUE self)
{
  return self;
}

static int
remove_dead_threads(VALUE thread, VALUE context, VALUE ignored)
{
  return (IS_THREAD_ALIVE(thread)) ? ST_CONTINUE : ST_DELETE;
}

static int
values_i(VALUE key, VALUE value, VALUE ary)
{
    rb_ary_push(ary, value);
    return ST_CONTINUE;
}

static VALUE
Debase_contexts(VALUE self)
{
  VALUE ary;

  ary = rb_ary_new();
  /* check that all contexts point to alive threads */
  rb_hash_foreach(contexts, remove_dead_threads, 0);

  rb_hash_foreach(contexts, values_i, ary);

  return ary;
}

static VALUE
Debase_debug_load(int argc, VALUE *argv, VALUE self)
{
    VALUE file, stop, increment_start;
    int state;

    if(rb_scan_args(argc, argv, "12", &file, &stop, &increment_start) == 1)
    {
        stop = Qfalse;
        increment_start = Qtrue;
    }
    Debase_setup_tracepoints(self);
    //debase_prepare_context(self, file, stop);
    rb_load_protect(file, 0, &state);
    if (0 != state)
    {
        return rb_errinfo();
    }
    return Qnil;
}

static breakpoint_t* find_breakpoint(char* path) {
    breakpoint_list_node_t* breakpoint = breakpoint_list;

    while(breakpoint != NULL) {
        if(strcmp (breakpoint->breakpoint->source, path) == 0) {
            return breakpoint->breakpoint;
        }
        breakpoint = breakpoint->next;
    }

    return NULL;
}

static void add_traces_children(rb_iseq_t* iseq, VALUE *code) {
    unsigned int i;
    VALUE child;
    const struct rb_iseq_constant_body *const body = iseq->body;

    if(iseq->body == NULL) {
        return;
    }

    if (body->catch_table) {
        for (i = 0; i < body->catch_table->size; i++) {
            const struct iseq_catch_table_entry *entry = &body->catch_table->entries[i];
            if(entry->iseq) {
                add_traces(entry->iseq);
            }
        }
    }

    for (i=0; i<body->iseq_size;) {
        VALUE insn = code[i];
        int len = insn_len(insn);
        const char *types = insn_op_types(insn);
        int j;

        for (j=0; types[j]; j++) {
            switch (types[j]) {
                  case TS_ISEQ:
                    child = code[i + j + 1];
                    if (child) {
                        add_traces(rb_iseq_check((rb_iseq_t *)child));
                    }
                    break;
                  default:
                    break;
            }
        }
        i += len;
    }
}

static void add_traces(rb_iseq_t* iseq) {
    if(iseq == NULL || iseq->body == NULL || iseq->body->iseq_encoded == NULL) {
        return;
    }

    VALUE *generated_iseq;
    VALUE *code;
    int ii, jj, i, j, cnt = 0;
    code = rb_iseq_original_iseq(iseq);

    unsigned int size = iseq->body->iseq_size;

    VALUE child = rb_ary_tmp_new(3);
    VALUE str = rb_str_new(0, 0);
    code = rb_iseq_original_iseq(iseq);

    int *added_traces = malloc(sizeof(int) * size + 2 * cnt);
    added_traces[0] = 0;

    for(i = 0; i < size; ) {
        VALUE insn = code[i];
        int len = insn_len(insn);
        const char *types = insn_op_types(insn);

        if(i > 0) {
            added_traces[i] = added_traces[i - 1];
        }

        if(types[0] == TS_CALLINFO) {
            cnt++;
            added_traces[i] += 2;
        }

        for(int dx = 0; dx < len; dx++) {
            if(dx > 0) {
                added_traces[i + dx] = added_traces[i + dx - 1];
            }
        }

        i += len;
    }

    generated_iseq = ALLOC_N(VALUE, size + 2 * cnt);

    struct iseq_line_info_entry *line_info_table;
    line_info_table = ALLOC_N(struct iseq_line_info_entry, iseq->body->line_info_size + cnt);

    VALUE trace_adr = NULL;
    int dir;

    for(ii = 0, jj = 0, i = 0, j = 0; i < size; ) {
        VALUE insn = code[i];
        int len = insn_len(insn);
        const char *types = insn_op_types(insn);

        if(((int)insn) == YARVINSN_trace && trace_adr == NULL) {
            trace_adr = iseq->body->iseq_encoded[i];
        }

        if(types[0] == TS_CALLINFO) {

            while(iseq->body->line_info_table[jj].position < j && jj < iseq->body->line_info_size) {
                line_info_table[ii].position = iseq->body->line_info_table[jj].position;
                line_info_table[ii].line_no = iseq->body->line_info_table[jj].line_no;

                ii++;
                jj++;
            }

            line_info_table[ii].position = j;
            line_info_table[ii].line_no = line_info_table[ii - 1].line_no;
            ii++;

            generated_iseq[j++] = trace_adr;
            generated_iseq[j++] = RUBY_EVENT_NONE;
        }

        for(int dx = 0; dx < len; dx++) {
            generated_iseq[j + dx] = iseq->body->iseq_encoded[i + dx];
        }

        if(types[0] == TS_OFFSET) {
            dir = iseq->body->iseq_encoded[i + 1] + i + 2;
            generated_iseq[j + 1] += added_traces[dir] - added_traces[i - 1];
        }

        i += len;
        j += len;
    }

    for(; jj < iseq->body->line_info_size; jj++, ii++) {
        line_info_table[ii].position = iseq->body->line_info_table[jj].position;
        line_info_table[ii].line_no = iseq->body->line_info_table[jj].line_no;
    }

    const struct iseq_catch_table *table = iseq->body->catch_table;

    if (table) {
        for (i = 0; i < table->size; i++) {
            struct iseq_catch_table_entry *entry = &table->entries[i];
            entry->start += added_traces[entry->start];
            entry->end += added_traces[entry->end];
            entry->cont += added_traces[entry->cont];
            entry->sp += added_traces[entry->sp];
        }
    }

    ruby_xfree((void *)iseq->body->iseq_encoded);
    iseq->body->iseq_encoded = generated_iseq;
    iseq->body->iseq_size += 2 * cnt;
    xfree(iseq->body->line_info_table);
    iseq->body->line_info_table = line_info_table;
    iseq->body->line_info_size += cnt;

    RUBY_MARK_UNLESS_NULL(iseq->body->mark_ary);
    RB_OBJ_WRITE(iseq, &iseq->body->mark_ary, iseq_mark_ary_create((int)iseq->body->mark_ary));

    code = rb_iseq_original_iseq(iseq);
    free(added_traces);
    add_traces_children(iseq, code);
}

static VALUE
Debase_handle_iseq(VALUE self, VALUE path, VALUE file_iseq) {
    breakpoint_list_node_t* breakpoint = breakpoint_list;
    breakpoint_t* curr_breakpoint;

    rb_iseq_t* iseq =  iseqw_check(file_iseq);

    char* source = RSTRING_PTR(path);

    add_traces(iseq);

    while(breakpoint != NULL) {
        curr_breakpoint = breakpoint->breakpoint;

        if(strcmp (curr_breakpoint->source, source) == 0) {
            c_add_breakpoint(curr_breakpoint->line, iseq);
        }
        breakpoint = breakpoint->next;
    }
}

static VALUE
Debase_breakpoints(VALUE self)
{
  return breakpoints;
}

static VALUE
Debase_started(VALUE self)
{
  return started ? Qtrue : Qfalse;
}

void
Init_debase_internals()
{
    mDebase = rb_define_module("Debase");

    rb_define_module_function(mDebase, "setup_tracepoints", Debase_setup_tracepoints, 0);

    rb_define_module_function(mDebase, "current_context", Debase_current_context, 0);
    rb_define_module_function(mDebase, "do_add_breakpoint", Debase_add_breakpoint, 3);
    rb_define_module_function(mDebase, "debug_load", Debase_debug_load, -1);
    rb_define_module_function(mDebase, "contexts", Debase_contexts, 0);
    rb_define_module_function(mDebase, "prepare_context", Debase_prepare_context, 0);
    rb_define_module_function(mDebase, "handle_iseq", Debase_handle_iseq, 2);
    rb_define_module_function(mDebase, "breakpoints", Debase_breakpoints, 0);
    rb_define_module_function(mDebase, "started?", Debase_started, 0);

    idAtLine = rb_intern("at_line");
    idAtBreakpoint = rb_intern("at_breakpoint");
    idMpLoadIseq = rb_intern("mp_load_iseq");
    idAlive = rb_intern("alive?");

    cContext = Init_context(mDebase);

    cDebugThread  = rb_define_class_under(mDebase, "DebugThread", rb_cThread);

    rb_global_variable(&contexts);
    rb_global_variable(&breakpoints);
}
