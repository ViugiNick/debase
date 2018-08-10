#include <debase_internals.h>

static VALUE cContext;
static int thnum_current = 0;

static VALUE idAlive;

#define MAX_POSBUF 128
#define RUBY_VM_IFUNC_P(ptr)        (RB_TYPE_P((VALUE)(ptr), T_IMEMO) && imemo_type((VALUE)ptr) == imemo_ifunc)
#define ruby_current_thread ((rb_thread_t *)RTYPEDDATA_DATA(rb_thread_current()))

#if RUBY_API_VERSION_CODE >= 20500
  #if (RUBY_RELEASE_YEAR == 2017 && RUBY_RELEASE_MONTH == 10 && RUBY_RELEASE_DAY == 10) //workaround for 2.5.0-preview1
    #define TH_CFP(thread) ((rb_control_frame_t *)(thread)->ec.cfp)
  #else
    #define TH_CFP(thread) ((rb_control_frame_t *)(thread)->ec->cfp)
  #endif
#else
  #define TH_CFP(thread) ((rb_control_frame_t *)(thread)->cfp)
#endif

/* "Step", "Next" and "Finish" do their work by saving information
   about where to stop next. reset_stopping_points removes/resets this
   information. */
extern void
reset_stepping_stop_points(debug_context_t *context)
{
  context->dest_frame = -1;
  context->stop_line  = -1;
  context->stop_next  = -1;
}

static void debug_print(VALUE v) {
    ID sym_puts = rb_intern("puts");
    rb_funcall(rb_mKernel, sym_puts, 1, v);
}

static inline VALUE
Context_thnum(VALUE self) {
  debug_context_t *context;
  Data_Get_Struct(self, debug_context_t, context);
  return INT2FIX(context->thnum);
}

static inline void
fill_frame(debug_frame_t *frame, const char* file, int line, VALUE binding, VALUE self)
{ 
  frame->file = file;
  frame->line = line;
  frame->binding = binding;
  frame->self = self;
}

extern void 
fill_stack(debug_context_t *context, const rb_debug_inspector_t *inspector) {
  debug_frame_t *frame;
  VALUE locations;
  VALUE location;
  VALUE path;
  VALUE lineno;
  const char *file;
  int line;
  int stack_size;
  int i;

  locations = rb_debug_inspector_backtrace_locations(inspector);
  stack_size = locations == Qnil ? 0 : RARRAY_LENINT(locations);
  context->stack_size = 0;
  
  for (i = 0; i < stack_size; i++) {
    
    location = rb_ary_entry(locations, i);
    VALUE iseq = rb_debug_inspector_frame_iseq_get(inspector, i);
      
    if(iseq != Qnil)
    {
      frame = ALLOC(debug_frame_t);
      path = rb_funcall(location, rb_intern("path"), 0);
      lineno = rb_funcall(location, rb_intern("lineno"), 0);
      file = path != Qnil ? RSTRING_PTR(path) : "";
      line = FIX2INT(lineno);
      
      fill_frame(frame, file, line, rb_debug_inspector_frame_binding_get(inspector, i), rb_debug_inspector_frame_self_get(inspector, i));
      frame->prev = context->stack;
      context->stack = frame;
      context->stack_size++;
    }
  }
}

extern void
clear_stack(debug_context_t *context)
{
  debug_frame_t *frame;
  debug_frame_t *prev;

  frame = context->stack;
  while(frame != NULL) {
    prev = frame->prev;
    xfree(frame);
    frame = prev;
  }
  context->stack = NULL;
}

static inline VALUE
Context_stack_size(VALUE self)
{
  debug_context_t *context;
  Data_Get_Struct(self, debug_context_t, context);
  return INT2FIX(context->stack_size);
}

static inline VALUE
Context_step_in_variants(VALUE self)
{
  VALUE ary;
  int i;

  debug_context_t *context;
  Data_Get_Struct(self, debug_context_t, context);

  step_in_info_t* step_in_info = context->step_in_info;

  ary = rb_ary_new();

  for(i = 0; i < step_in_info->size; i++) {
    step_in_variant_t* variant = step_in_info->variants[i];
    rb_ary_push(ary, variant->mid);
  }

  return ary;
}

static inline VALUE
Context_thread(VALUE self)
{
  debug_context_t *context;
  Data_Get_Struct(self, debug_context_t, context);
  return context->thread;
}

static inline VALUE
Context_dead(VALUE self)
{
  debug_context_t *context;
  Data_Get_Struct(self, debug_context_t, context);
  return IS_THREAD_ALIVE(context->thread) ? Qfalse : Qtrue;
}

extern VALUE
Context_ignored(VALUE self)
{
  debug_context_t *context;

  if (self == Qnil) return Qtrue;
  Data_Get_Struct(self, debug_context_t, context);
  return CTX_FL_TEST(context, CTX_FL_IGNORE) ? Qtrue : Qfalse;
}

static void
Context_mark(debug_context_t *context)
{
  debug_frame_t *frame;

  rb_gc_mark(context->thread);
  frame = context->stack;
  while(frame != NULL)
  {
    rb_gc_mark(frame->self);
    rb_gc_mark(frame->binding);
    frame = frame->prev;
  }
}

static void
Context_free(debug_context_t *context) {
  xfree(context);
}

static void debug_class_print(VALUE v) {
    ID sym_puts = rb_intern("puts");
    ID sym_inspect = rb_intern("class");
    rb_funcall(rb_mKernel, sym_puts, 1,
        rb_funcall(v, sym_inspect, 0));
}

extern VALUE
context_create(VALUE thread, VALUE cDebugThread) {
  debug_context_t *context;
  VALUE locations;

  context = ALLOC(debug_context_t);
  context->stack_size = 0;
  locations = rb_funcall(thread, rb_intern("backtrace_locations"), 1, INT2FIX(1));
  context->calced_stack_size = locations != Qnil ? RARRAY_LENINT(locations) : 0;

  context->stack = NULL;
  context->thnum = ++thnum_current;
  context->thread = thread;
  context->flags = 0;
  context->last_file = NULL;
  context->last_line = -1;
  context->stop_frame = -1;
  context->thread_pause = 0;
  context->stop_reason = CTX_STOP_NONE;

  context->should_step_in = -1;
  context->iseq = NULL;
  context->pc = -1;

  reset_stepping_stop_points(context);
  if(rb_obj_class(thread) == cDebugThread) CTX_FL_SET(context, CTX_FL_IGNORE);
  return Data_Wrap_Struct(cContext, Context_mark, Context_free, context);
}

static inline void
check_frame_number_valid(debug_context_t *context, int frame_no)
{
  if (frame_no < 0 || frame_no >= context->stack_size) {
    rb_raise(rb_eArgError, "Invalid frame number %d, stack (0...%d)",
        frame_no, context->stack_size);
  }
}

static debug_frame_t*
get_frame_no(debug_context_t *context, int frame_no)
{
  debug_frame_t *frame;
  int i;

  check_frame_number_valid(context, frame_no);
  frame = context->stack;
  for (i = 0; i < context->stack_size - frame_no - 1; i++) {
    frame = frame->prev;
  }
  return frame;
}

static VALUE
Context_frame_file(int argc, VALUE *argv, VALUE self)
{
  debug_context_t *context;
  debug_frame_t *frame;
  VALUE frame_no;
  int frame_n;

  Data_Get_Struct(self, debug_context_t, context);
  frame_n = rb_scan_args(argc, argv, "01", &frame_no) == 0 ? 0 : FIX2INT(frame_no);
  frame = get_frame_no(context, frame_n);
  return rb_str_new2(frame->file);
}

static VALUE
Context_frame_line(int argc, VALUE *argv, VALUE self)
{
  debug_context_t *context;
  debug_frame_t *frame;
  VALUE frame_no;
  int frame_n;

  Data_Get_Struct(self, debug_context_t, context);
  frame_n = rb_scan_args(argc, argv, "01", &frame_no) == 0 ? 0 : FIX2INT(frame_no);
  frame = get_frame_no(context, frame_n);
  return INT2FIX(frame->line);
}

static VALUE
Context_frame_binding(int argc, VALUE *argv, VALUE self)
{
  debug_context_t *context;
  debug_frame_t *frame;
  VALUE frame_no;
  int frame_n;

  Data_Get_Struct(self, debug_context_t, context);
  frame_n = rb_scan_args(argc, argv, "01", &frame_no) == 0 ? 0 : FIX2INT(frame_no);
  frame = get_frame_no(context, frame_n);
  return frame->binding;
}

static VALUE
Context_frame_self(int argc, VALUE *argv, VALUE self)
{
  debug_context_t *context;
  debug_frame_t *frame;
  VALUE frame_no;
  int frame_n;

  Data_Get_Struct(self, debug_context_t, context);
  frame_n = rb_scan_args(argc, argv, "01", &frame_no) == 0 ? 0 : FIX2INT(frame_no);
  frame = get_frame_no(context, frame_n);
  return frame->self;
}

static VALUE
Context_stop_reason(VALUE self)
{
    debug_context_t *context;
    const char *symbol;

    Data_Get_Struct(self, debug_context_t, context);

    switch(context->stop_reason)
    {
        case CTX_STOP_STEP:
            symbol = "step";
            break;
        case CTX_STOP_BREAKPOINT:
            symbol = "breakpoint";
            break;
        case CTX_STOP_CATCHPOINT:
            symbol = "catchpoint";
            break;
        case CTX_STOP_NONE:
        default:
            symbol = "none";
    }
    if(CTX_FL_TEST(context, CTX_FL_DEAD))
        symbol = "post-mortem";

    return ID2SYM(rb_intern(symbol));
}

static VALUE
Context_pause(VALUE self)
{
  debug_context_t *context;

  Data_Get_Struct(self, debug_context_t, context);

  if (context->thread == rb_thread_current())
  {
    return Qfalse;
  }

  enable_trace_points();
  context->thread_pause = 1;
  return Qtrue;
}

static void trace_on_next(rb_iseq_t *iseq, int pc) {
    VALUE *iseq_original;
    iseq_original = rb_iseq_original_iseq(iseq);
    size_t insn;
    unsigned int pos;

    for (pos = pc; pos < iseq->body->iseq_size; pos += insn_len(insn)) {
        insn = iseq_original[pos];

        if (insn == BIN(trace)) {
            fprintf(stderr, "trace_on_next %d\n", pos);
            rb_event_flag_t current_events;

            current_events = (rb_event_flag_t)iseq_original[pos+1];

            VALUE *encoded = (VALUE *)iseq->body->iseq_encoded;
            iseq_original[pos+1] = encoded[pos+1] = (VALUE)(current_events | RUBY_EVENT_LINE | RUBY_EVENT_SPECIFIED_LINE);
            break;
        }
    }
}

static VALUE
Context_stop_next(int argc, VALUE *argv, VALUE self)
{
  VALUE steps;
  VALUE force;
  debug_context_t *context;

  rb_scan_args(argc, argv, "11", &steps, &force);
  if(FIX2INT(steps) < 0) rb_raise(rb_eRuntimeError, "Steps argument can't be negative.");

  Data_Get_Struct(self, debug_context_t, context);

  context->stop_next = FIX2INT(steps);
  context->should_step_in = 1;

  trace_on_next(context->iseq, context->pc);

  //VALUE str = rb_iseq_disasm(context->iseq);
  //fprintf(stderr, "%s\n", StringValueCStr(str));

  context->cfp = TH_CFP(ruby_current_thread);
  context->stop_pc = context->step_in_info->variants[0]->pc;
}

static VALUE
Context_smart_step(VALUE self, VALUE rb_variant_id, VALUE force)
{
  int variant_id;
  variant_id = FIX2INT(rb_variant_id);

  debug_context_t *context;

  Data_Get_Struct(self, debug_context_t, context);

  step_in_variant_t* variant = context->step_in_info->variants[variant_id];

  if(variant->block_iseq != NULL) {
    c_add_breakpoint_first_line(variant->block_iseq);
    return;
  }

  context->cfp = TH_CFP(ruby_current_thread);
  context->stop_pc = variant->pc;
}

static VALUE
Context_step_over(int argc, VALUE *argv, VALUE self)
{
  VALUE lines, frame, force;
  debug_context_t *context;

  Data_Get_Struct(self, debug_context_t, context);

  if(context->stack_size == 0)
    rb_raise(rb_eRuntimeError, "No frames collected.");

  rb_scan_args(argc, argv, "12", &lines, &frame, &force);

  rb_control_frame_t *cfp = TH_CFP(ruby_current_thread);

  cfp += 10;

  debug_print(cfp->iseq->body->location.path);
  debug_print(cfp->iseq->body->location.first_lineno);
}

static VALUE
Context_stop_frame(VALUE self, VALUE frame)
{
  debug_context_t *debug_context;

  Data_Get_Struct(self, debug_context_t, debug_context);

  if(FIX2INT(frame) < 0 && FIX2INT(frame) >= debug_context->calced_stack_size)
    rb_raise(rb_eRuntimeError, "Stop frame is out of range.");
  /* we decrease stack size by frame and 1 because we use stop_frame after
     updating stack size.  If that code will be changed this should be changed accordingly.
   */
  debug_context->stop_frame = debug_context->calced_stack_size - FIX2INT(frame) - 1;

  return frame;
}

extern void
context_init_variables()
{
  thnum_current = 0;
}

/*
 *   Document-class: Context
 *
 *   == Summary
 *
 *   Debugger keeps a single instance of this class for each Ruby thread.
 */
VALUE
Init_context(VALUE mDebase)
{
  cContext = rb_define_class_under(mDebase, "Context", rb_cObject);
  rb_define_method(cContext, "stack_size", Context_stack_size, 0);
  rb_define_method(cContext, "step_variants", Context_step_in_variants, 0);
  rb_define_method(cContext, "thread", Context_thread, 0);
  rb_define_method(cContext, "dead?", Context_dead, 0);
  rb_define_method(cContext, "ignored?", Context_ignored, 0);
  rb_define_method(cContext, "thnum", Context_thnum, 0);
  rb_define_method(cContext, "stop_reason", Context_stop_reason, 0);
  rb_define_method(cContext, "frame_file", Context_frame_file, -1);
  rb_define_method(cContext, "frame_line", Context_frame_line, -1);
  rb_define_method(cContext, "frame_binding", Context_frame_binding, -1);
  rb_define_method(cContext, "frame_self", Context_frame_self, -1);
  rb_define_method(cContext, "stop_next=", Context_stop_next, -1);
  rb_define_method(cContext, "step", Context_stop_next, -1);
  rb_define_method(cContext, "smart_step", Context_smart_step, 2);
  rb_define_method(cContext, "step_over", Context_step_over, -1);
  rb_define_method(cContext, "stop_frame=", Context_stop_frame, 1);
  rb_define_method(cContext, "pause", Context_pause, 0);
  rb_define_method(cContext, "step_in_variants", Context_pause, 0);

  idAlive = rb_intern("alive?");
  context_init_variables();

  return cContext;
    // rb_define_method(cContext, "suspend", context_suspend, 0);
    // rb_define_method(cContext, "suspended?", context_is_suspended, 0);
    // rb_define_method(cContext, "resume", context_resume, 0);
    // rb_define_method(cContext, "tracing", context_tracing, 0);
    // rb_define_method(cContext, "tracing=", context_set_tracing, 1);
    // rb_define_method(cContext, "ignored?", context_ignored, 0);
    // rb_define_method(cContext, "frame_args", context_frame_args, -1);
    // rb_define_method(cContext, "frame_args_info", context_frame_args_info, -1);
    // rb_define_method(cContext, "frame_class", context_frame_class, -1);
    // rb_define_method(cContext, "frame_id", context_frame_id, -1);
    // rb_define_method(cContext, "frame_locals", context_frame_locals, -1);
    // rb_define_method(cContext, "frame_method", context_frame_id, -1);
    // rb_define_method(cContext, "breakpoint",
    //          context_breakpoint, 0);      /* in breakpoint.c */
    // rb_define_method(cContext, "set_breakpoint",
    //          context_set_breakpoint, -1); /* in breakpoint.c */
    // rb_define_method(cContext, "jump", context_jump, 2);
    // rb_define_method(cContext, "pause", context_pause, 0);
}