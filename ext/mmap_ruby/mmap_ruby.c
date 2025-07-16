#include "mmap_ruby.h"

#define EXP_INCR_SIZE 4096

#define MMAP_RUBY_MODIFY  1
#define MMAP_RUBY_ORIGIN  2
#define MMAP_RUBY_CHANGE  (MMAP_RUBY_MODIFY | 4)
#define MMAP_RUBY_PROTECT 8

#define MMAP_RUBY_FIXED (1<<1)
#define MMAP_RUBY_ANON  (1<<2)
#define MMAP_RUBY_LOCK  (1<<3)
#define MMAP_RUBY_IPC   (1<<4)
#define MMAP_RUBY_TMP   (1<<5)

#define GET_MMAP(self, mmap, t_modify) \
  TypedData_Get_Struct(self, mmap_t, &mmap_type, mmap); \
  if (!mmap->path) { \
    rb_raise(rb_eIOError, "unmapped file"); \
  } \
  if (t_modify & MMAP_RUBY_MODIFY) { \
    rb_check_frozen(self); \
  }

static char template[1024];

#if defined(__linux__) || defined(__GNU__) || defined(__GLIBC__)
union semun
{
  int val;
  struct semid_ds *buf;
  unsigned short int *array;
};
#endif

typedef struct {
  char *path;
  char *template;

  void *addr;
  size_t len;
  size_t real;
  off_t offset;

  int smode;
  int pmode;
  int vscope;

  int flag;

  size_t incr;
  int advice;

  VALUE key;
  int semid;
  VALUE shmid;

  int count;
} mmap_t;

typedef struct {
  mmap_t *mmap;
  size_t len;
} mmap_st;

typedef struct {
  int argc;
  VALUE *argv;
  VALUE id;
  VALUE obj;
  int flag;
} mmap_bang;

void *(*mmap_func)(void *, size_t, int, int, int, off_t) = mmap;

static void mmap_update(mmap_t *str, long beg, long len, VALUE val);
static void mmap_subpat_set(VALUE obj, VALUE re, int offset, VALUE val);
static VALUE rb_cMmap_index(int argc, VALUE *argv, VALUE self);
static void mmap_realloc(mmap_t *mmap, size_t len);
static void mmap_expandf(mmap_t *mmap, size_t len);

static void
mmap_mark(void *ptr)
{
  mmap_t *mmap = (mmap_t *)ptr;

  rb_gc_mark_movable(mmap->key);
}

static void
mmap_free(void *ptr)
{
  mmap_t *mmap = (mmap_t *)ptr;

  xfree(mmap);
}

static size_t
mmap_memsize(const void *ptr)
{
  (void)ptr;

  return sizeof(mmap_t);
}

static void
mmap_compact(void *ptr)
{
  mmap_t *mmap = (mmap_t *)ptr;

  mmap->key = rb_gc_location(mmap->key);
}

static const rb_data_type_t mmap_type = {
  .wrap_struct_name = "MmapRuby::Mmap",
  .function = {
    .dmark = mmap_mark,
    .dfree = mmap_free,
    .dsize = mmap_memsize,
    .dcompact = mmap_compact
  },
  .flags = RUBY_TYPED_FREE_IMMEDIATELY
};

/*
 * call-seq:
 *   lockall(flag) -> nil
 *   mlockall(flag) -> nil
 *
 * Disables paging of all pages mapped into the address space of the calling
 * process. The +flag+ parameter can be MCL_CURRENT (lock all currently mapped
 * pages) or MCL_FUTURE (lock all pages that become mapped in the future).
 */
static VALUE
rb_cMmap_mlockall(VALUE self, VALUE flag)
{
  if (mlockall(NUM2INT(flag)) == -1) {
    rb_raise(rb_eArgError, "mlockall(%d)", errno);
  }
  return Qnil;
}

/*
 * call-seq:
 *   unlockall -> nil
 *   munlockall -> nil
 *
 * Re-enables paging for all pages mapped into the address space of the
 * calling process.
 */
static VALUE
rb_cMmap_munlockall(VALUE self)
{
  (void)self;
  if (munlockall() == -1) {
    rb_raise(rb_eArgError, "munlockall(%d)", errno);
  }
  return Qnil;
}

static VALUE
rb_cMmap_allocate(VALUE klass)
{
  mmap_t *mmap;
  VALUE obj;

  obj = TypedData_Make_Struct(klass, mmap_t, &mmap_type, mmap);
  MEMZERO(mmap, mmap_t, 1);
  mmap->incr = EXP_INCR_SIZE;

  return obj;
}

static VALUE
mmap_ipc_initialize(VALUE pair, VALUE self, int argc, const VALUE *argv, VALUE blockarg)
{
  mmap_t *mmap;
  VALUE key, value;
  const char *options;

  (void)argc;
  (void)argv;
  (void)blockarg;

  TypedData_Get_Struct(self, mmap_t, &mmap_type, mmap);
  key = rb_obj_as_string(rb_ary_entry(pair, 0));
  value = rb_ary_entry(pair, 1);
  options = StringValuePtr(key);

  if (strcmp(options, "key") == 0) {
    mmap->key = rb_funcall2(value, rb_intern("to_int"), 0, 0);
  }
  else if (strcmp(options, "permanent") == 0) {
    if (RTEST(value)) {
      mmap->flag &= ~MMAP_RUBY_TMP;
    }
  }
  else if (strcmp(options, "mode") == 0) {
    mmap->semid = NUM2INT(value);
  }
  else {
    rb_warning("Unknown option `%s'", options);
  }

  return Qnil;
}

/*
 * call-seq:
 *   new(file, mode = "r", protection = Mmap::MAP_SHARED, options = {})
 *
 * Returns a new MmapRuby object.
 *
 * * +file+
 *
 *   Pathname of the file. If +nil+ is given, an anonymous map
 *   is created (+Mmap::MAP_ANON+).
 *
 * * +mode+
 *
 *   Mode to open the file. Can be "r", "w", "rw", or "a".
 *
 * * +protection+
 *
 *   Specifies the nature of the mapping:
 *
 *   * +Mmap::MAP_SHARED+
 *
 *     Creates a mapping that's shared with all other processes
 *     mapping the same areas of the file.
 *     This is the default value.
 *
 *   * +Mmap::MAP_PRIVATE+
 *
 *     Creates a private copy-on-write mapping, so changes to the
 *     contents of the mmap object will be private to this process.
 *
 * * +options+
 *
 *   Hash. If one of the options +length+ or +offset+
 *   is specified, it will not be possible to modify the size of
 *   the mapped file.
 *
 *   length:: Maps +length+ bytes from the file.
 *
 *   offset:: The mapping begins at +offset+.
 *
 *   advice:: The type of access (see #madvise).
 */
static VALUE
rb_cMmap_initialize(int argc, VALUE *argv, VALUE self)
{
  mmap_t *mmap;
  struct stat st;

  VALUE fname, vmode, scope, options;
  VALUE fdv = Qnil;

  const char *path = 0, *mode = 0;
  int fd = -1, perm = 0666;

  caddr_t addr;
  size_t size = 0;
  off_t offset = 0;
  int smode = 0, pmode = 0, vscope = 0;

  int anonymous = 0, init = 0;

  options = Qnil;
  argc = rb_scan_args(argc, argv, "12:", &fname, &vmode, &scope, &options);

  if (NIL_P(fname)) {
    vscope = MAP_ANON | MAP_SHARED;
    anonymous = 1;
  }
  else
  {
    if (rb_respond_to(fname, rb_intern("fileno"))) {
      fdv = rb_funcall2(fname, rb_intern("fileno"), 0, 0);
    }

    if (NIL_P(fdv)) {
      fname = rb_str_to_str(fname);
      StringValue(fname);
      path = StringValuePtr(fname);
    }
    else {
      fd = NUM2INT(fdv);
      if (fd < 0) {
        rb_raise(rb_eArgError, "invalid file descriptor %d", fd);
      }
    }

    if (!NIL_P(scope)) {
      vscope = NUM2INT(scope);
      if (vscope & MAP_ANON) {
        rb_raise(rb_eArgError, "filename specified for an anonymous map");
      }
    }
  }

  vscope |= NIL_P(scope) ? MAP_SHARED : NUM2INT(scope);

  if (!anonymous) {
    if (NIL_P(vmode)) {
      mode = "r";
    }
    else if (rb_respond_to(vmode, rb_intern("to_ary"))) {
      VALUE tmp;

      vmode = rb_convert_type(vmode, T_ARRAY, "Array", "to_ary");
      if (RARRAY_LEN(vmode) != 2) {
        rb_raise(rb_eArgError, "invalid length %ld (expected 2)",
                 RARRAY_LEN(vmode));
      }
      tmp = rb_ary_entry(vmode, 0);
      mode = StringValuePtr(tmp);
      perm = NUM2INT(rb_ary_entry(vmode, 1));
    }
    else {
      mode = StringValuePtr(vmode);
    }

    if (strcmp(mode, "r") == 0) {
      smode = O_RDONLY;
      pmode = PROT_READ;
    }
    else if (strcmp(mode, "w") == 0) {
      smode = O_RDWR | O_TRUNC;
      pmode = PROT_READ | PROT_WRITE;
    }
    else if (strcmp(mode, "rw") == 0 || strcmp(mode, "wr") == 0) {
      smode = O_RDWR;
      pmode = PROT_READ | PROT_WRITE;
    }
    else if (strcmp(mode, "a") == 0) {
      smode = O_RDWR | O_CREAT;
      pmode = PROT_READ | PROT_WRITE;
    }
    else {
      rb_raise(rb_eArgError, "invalid mode %s", mode);
    }

    if (NIL_P(fdv)) {
      if ((fd = open(path, smode, perm)) == -1) {
        rb_raise(rb_eArgError, "can't open %s", path);
      }
    }
    if (fstat(fd, &st) == -1) {
      rb_raise(rb_eArgError, "can't stat %s", path);
    }
    size = st.st_size;
  }
  else {
    fd = -1;
    if (!NIL_P(vmode) && TYPE(vmode) != T_STRING) {
      size = NUM2INT(vmode);
    }
  }

  TypedData_Get_Struct(self, mmap_t, &mmap_type, mmap);
  rb_check_frozen(self);
  mmap->shmid = 0;
  mmap->semid = 0;

  if (options != Qnil) {
    rb_funcall(self, rb_intern("process_options"), 1, options);
    if (path && (mmap->len + mmap->offset) > (size_t)st.st_size) {
      rb_raise(rb_eArgError, "invalid value for length (%ld) or offset (%ld)",
               (long)mmap->len, (long)mmap->offset);
    }
    if (mmap->len) size = mmap->len;
    offset = mmap->offset;

    if (mmap->flag & MMAP_RUBY_IPC) {
      key_t key;
      int shmid, semid, mode;
      union semun sem_val;
      struct shmid_ds buf;

      if (!(vscope & MAP_SHARED)) {
        rb_warning("Probably it will not do what you expect ...");
      }
      mmap->key = -1;
      mmap->semid = 0;
      if (TYPE(mmap->shmid) == T_HASH) {
        rb_block_call(mmap->shmid, rb_intern("each"), 0, NULL, mmap_ipc_initialize, self);
      }
      mmap->shmid = 0;

      if (mmap->semid) {
        mode = mmap->semid;
        mmap->semid = 0;
      }
      else {
        mode = 0644;
      }

      if ((int)mmap->key <= 0) {
        mode |= IPC_CREAT;
        strcpy(template, "/tmp/ruby_mmap.XXXXXX");
        if (mkstemp(template) == -1) {
          rb_sys_fail("mkstemp()");
        }
        if ((key = ftok(template, 'R')) == -1) {
          rb_sys_fail("ftok()");
        }
      }
      else {
        key = (key_t)mmap->key;
      }

      if ((shmid = shmget(key, sizeof(mmap_t), mode)) == -1) {
        rb_sys_fail("shmget()");
      }
      mmap = shmat(shmid, (void *)0, 0);
      if (mmap == (mmap_t *)-1) {
        rb_sys_fail("shmat()");
      }
      if (mmap->flag & MMAP_RUBY_TMP) {
        if (shmctl(shmid, IPC_RMID, &buf) == -1) {
          rb_sys_fail("shmctl()");
        }
      }

      if ((semid = semget(key, 1, mode)) == -1) {
        rb_sys_fail("semget()");
      }
      if (mode & IPC_CREAT) {
        sem_val.val = 1;
        if (semctl(semid, 0, SETVAL, sem_val) == -1) {
          rb_sys_fail("semctl()");
        }
      }

      mmap->key = key;
      mmap->semid = semid;
      mmap->shmid = shmid;
      if (mmap->flag & MMAP_RUBY_TMP) {
        mmap->template = ALLOC_N(char, strlen(template) + 1);
        strcpy(mmap->template, template);
      }
    }
  }

  if (anonymous) {
    if (size <= 0) {
      rb_raise(rb_eArgError, "length not specified for an anonymous map");
    }
    if (offset) {
      rb_warning("Ignoring offset for an anonymous map");
      offset = 0;
    }
    smode = O_RDWR;
    pmode = PROT_READ | PROT_WRITE;
    mmap->flag |= MMAP_RUBY_FIXED | MMAP_RUBY_ANON;
  }
  else {
    if (size == 0 && (smode & O_RDWR)) {
      if (lseek(fd, mmap->incr - 1, SEEK_END) == -1) {
        rb_raise(rb_eIOError, "can't lseek %lu", mmap->incr - 1);
      }
      if (write(fd, "\000", 1) != 1) {
        rb_raise(rb_eIOError, "can't extend %s", path);
      }
      init = 1;
      size = mmap->incr;
    }
    if (!NIL_P(fdv)) {
      mmap->flag |= MMAP_RUBY_FIXED;
    }
  }

  addr = mmap_func(0, size, pmode, vscope, fd, offset);
  if (NIL_P(fdv) && !anonymous) {
    close(fd);
  }
  if (addr == MAP_FAILED || !addr) {
    rb_raise(rb_eArgError, "mmap failed (%d)", errno);
  }

#ifdef MADV_NORMAL
  if (mmap->advice && madvise(addr, size, mmap->advice) == -1) {
    rb_raise(rb_eArgError, "madvise(%d)", errno);
  }
#endif

  if (anonymous && TYPE(options) == T_HASH) {
    VALUE val;
    char *ptr;

    val = rb_hash_aref(options, rb_str_new2("initialize"));
    if (NIL_P(val)) val = rb_hash_aref(options, ID2SYM(rb_intern("initialize")));
    if (!NIL_P(val)) {
      ptr = StringValuePtr(val);
      memset(addr, ptr[0], size);
    }
  }

  mmap->addr = addr;
  mmap->len = size;
  if (!init) mmap->real = size;
  mmap->pmode = pmode;
  mmap->vscope = vscope;
  mmap->smode = smode & ~O_TRUNC;
  mmap->path = (path) ? strdup(path) : (char *)(intptr_t)-1;

  if (smode == O_RDONLY) {
    self = rb_obj_freeze(self);
  }
  else {
    if (smode == O_WRONLY) {
      mmap->flag |= MMAP_RUBY_FIXED;
    }
  }

  return self;
}

static VALUE
mmap_str(VALUE self, int modify)
{
  mmap_t *mmap;

  GET_MMAP(self, mmap, modify & ~MMAP_RUBY_ORIGIN);
  if (modify & MMAP_RUBY_MODIFY) {
    rb_check_frozen(self);
  }

  VALUE string = rb_obj_alloc(rb_cString);
  RSTRING(string)->len = mmap->real;
  RSTRING(string)->as.heap.ptr = mmap->addr;
  RSTRING(string)->as.heap.aux.capa = mmap->len;
  if (modify & MMAP_RUBY_ORIGIN) {
    RSTRING(string)->as.heap.aux.shared = self;
    FL_SET(string, RSTRING_NOEMBED);
    FL_SET(string, FL_USER18);
  }

  if (RB_OBJ_FROZEN(self)) {
    string = rb_obj_freeze(string);
  }

  return string;
}

/*
 * call-seq:
 *   to_str
 *
 * Convert object to a string.
 */
static VALUE
rb_cMmap_to_str(VALUE self)
{
  return mmap_str(self, MMAP_RUBY_ORIGIN);
}

/*
 * call-seq:
 *   hash -> integer
 *
 * Returns the hash value for the mapped memory content. Objects with the
 * same content will have the same hash value.
 */
static VALUE
rb_cMmap_hash(VALUE self)
{
  VALUE str;
  VALUE result;

  str = mmap_str(self, MMAP_RUBY_ORIGIN);
  result = rb_funcall(str, rb_intern("hash"), 0);
  RB_GC_GUARD(str);
  return result;
}

/*
 * call-seq:
 *   eql?(other) -> true or false
 *
 * Returns +true+ if the content and type of the mapped memory is equal to +other+.
 * Unlike +==+, this method only returns +true+ for other Mmap objects, not strings.
 */
static VALUE
rb_cMmap_eql(VALUE self, VALUE other)
{
  mmap_t *mmap, *other_mmap;

  if (self == other) return Qtrue;
  if (!rb_typeddata_is_kind_of(other, &mmap_type)) return Qfalse;

  GET_MMAP(self, mmap, 0);
  GET_MMAP(other, other_mmap, 0);
  if (mmap->real != other_mmap->real) {
    return Qfalse;
  }

  if (memcmp(mmap->addr, other_mmap->addr, mmap->real) == 0) return Qtrue;
  return Qfalse;
}

/*
 * call-seq:
 *   ==(other) -> true or false
 *   ===(other) -> true or false
 *
 * Returns +true+ if the content of the mapped memory is equal to +other+.
 * Compares with other Mmap objects or strings.
 */
static VALUE
rb_cMmap_equal(VALUE self, VALUE other)
{
  VALUE result;
  mmap_t *mmap, *other_mmap;

  if (self == other) return Qtrue;
  if (!rb_typeddata_is_kind_of(other, &mmap_type)) return Qfalse;

  GET_MMAP(self, mmap, 0);
  GET_MMAP(other, other_mmap, 0);
  if (mmap->real != other_mmap->real) {
    return Qfalse;
  }

  self = mmap_str(self, MMAP_RUBY_ORIGIN);
  other = mmap_str(other, MMAP_RUBY_ORIGIN);
  result = rb_funcall2(self, rb_intern("=="), 1, &other);
  RB_GC_GUARD(self);
  RB_GC_GUARD(other);
  return result;
}

/*
 * call-seq:
 *   <=>(other) -> -1, 0, 1, or nil
 *
 * Compares the mapped memory with +other+. Returns -1 if the mapped memory
 * is less than +other+, 0 if they are equal, 1 if the mapped memory is
 * greater than +other+, or +nil+ if the values are incomparable.
 */
static VALUE
rb_cMmap_cmp(VALUE self, VALUE other)
{
  VALUE self_str, other_str;
  VALUE result;

  self_str = mmap_str(self, MMAP_RUBY_ORIGIN);
  other_str = rb_str_to_str(other);
  result = rb_funcall2(self_str, rb_intern("<=>"), 1, &other_str);
  RB_GC_GUARD(self_str);
  RB_GC_GUARD(other_str);
  return result;
}

/*
 * call-seq:
 *   casecmp(other) -> -1, 0, 1, or nil
 *
 * Performs a case-insensitive comparison of the mapped memory with +other+.
 * Returns -1, 0, or 1 depending on whether the mapped memory is less than,
 * equal to, or greater than +other+. Returns +nil+ if the two values are
 * incomparable.
 */
static VALUE
rb_cMmap_casecmp(VALUE self, VALUE other)
{
  VALUE result;
  VALUE self_str, other_str;

  self_str = mmap_str(self, MMAP_RUBY_ORIGIN);
  other_str = rb_str_to_str(other);
  result = rb_funcall2(self_str, rb_intern("casecmp"), 1, &other_str);
  RB_GC_GUARD(self_str);
  RB_GC_GUARD(other_str);
  return result;
}

/*
 * call-seq:
 *   =~(other) -> integer or nil
 *
 * Returns the index of the first match of +other+ in the mapped memory,
 * or +nil+ if no match is found. The +other+ parameter can be a Regexp,
 * String, or any object that responds to +=~+.
 */
static VALUE
rb_cMmap_match(VALUE self, VALUE other)
{
  VALUE reg, res, self_str;
  long start;

  self_str = mmap_str(self, MMAP_RUBY_ORIGIN);
  if (rb_typeddata_is_kind_of(other, &mmap_type)) {
    other = rb_cMmap_to_str(other);
  }

  switch (TYPE(other)) {
    case T_REGEXP:
      res = rb_reg_match(other, self_str);
      break;

    case T_STRING:
      reg = rb_reg_regcomp(other);
      start = rb_reg_search(reg, self_str, 0, 0);
      if (start == -1) {
        res = Qnil;
      }
      else {
        res = LONG2NUM(start);
      }
      break;

    default:
      res = rb_funcall(other, rb_intern("=~"), 1, self_str);
      break;
  }

  RB_GC_GUARD(self_str);
  return res;
}

static VALUE
mmap_bang_protect(VALUE tmp)
{
  VALUE *t = (VALUE *)tmp;

  return rb_funcall2(t[0], (ID)t[1], (int)t[2], (VALUE *)t[3]);
}

static VALUE
mmap_bang_exec(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  VALUE str, res;
  mmap_t *mmap;

  str = mmap_str(bang_st->obj, (int)bang_st->flag);
  if (bang_st->flag & MMAP_RUBY_PROTECT) {
    VALUE tmp[4];
    tmp[0] = str;
    tmp[1] = (VALUE)bang_st->id;
    tmp[2] = (VALUE)bang_st->argc;
    tmp[3] = (VALUE)bang_st->argv;
    res = rb_ensure(mmap_bang_protect, (VALUE)tmp, 0, str);
  }
  else {
    res = rb_funcall2(str, bang_st->id, bang_st->argc, bang_st->argv);
    RB_GC_GUARD(res);
  }

  if (res != Qnil) {
    GET_MMAP(bang_st->obj, mmap, 0);
    mmap->real = RSTRING_LEN(str);
  }

  return res;
}

static void
mmap_lock(mmap_t *mmap, int wait_lock)
{
  struct sembuf sem_op;

  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap->count++;
    if (mmap->count == 1) {
retry:
      sem_op.sem_num = 0;
      sem_op.sem_op = -1;
      sem_op.sem_flg = IPC_NOWAIT;
      if (semop(mmap->semid, &sem_op, 1) == -1) {
        if (errno == EAGAIN) {
          if (!wait_lock) {
            rb_raise(rb_const_get(rb_mErrno, rb_intern("EAGAIN")), "EAGAIN");
          }
          rb_thread_sleep(1);
          goto retry;
        }
        rb_sys_fail("semop()");
      }
    }
  }
}

static void
mmap_unlock(mmap_t *mmap)
{
  struct sembuf sem_op;

  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap->count--;
    if (!mmap->count) {
retry:
      sem_op.sem_num = 0;
      sem_op.sem_op = 1;
      sem_op.sem_flg = IPC_NOWAIT;
      if (semop(mmap->semid, &sem_op, 1) == -1) {
        if (errno == EAGAIN) {
          rb_thread_sleep(1);
          goto retry;
        }
        rb_sys_fail("semop()");
      }
    }
  }
}

static VALUE
mmap_vunlock(VALUE obj)
{
  mmap_t *mmap;

  GET_MMAP(obj, mmap, 0);
  mmap_unlock(mmap);
  return Qnil;
}

static VALUE
mmap_bang_initialize(VALUE obj, int flag, ID id, int argc, VALUE *argv)
{
  VALUE res;
  mmap_t *mmap;
  mmap_bang bang_st;

  GET_MMAP(obj, mmap, 0);
  if ((flag & MMAP_RUBY_CHANGE) && (mmap->flag & MMAP_RUBY_FIXED)) {
    rb_raise(rb_eTypeError, "can't change the size of a fixed map");
  }

  bang_st.obj = obj;
  bang_st.flag = flag;
  bang_st.id = id;
  bang_st.argc = argc;
  bang_st.argv = argv;

  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_bang_exec, (VALUE)&bang_st, mmap_vunlock, obj);
  }
  else {
    res = mmap_bang_exec((VALUE)&bang_st);
  }

  if (res == Qnil) return res;
  return (flag & MMAP_RUBY_ORIGIN) ? res : obj;
}

/*
 * call-seq:
 *   match(pattern) -> MatchData or nil
 *
 * Converts +pattern+ to a Regexp (if it isn't already one) and returns a
 * MatchData object describing the match, or +nil+ if there was no match.
 * This is equivalent to calling +pattern.match+ on the mapped memory content.
 */
static VALUE
rb_cMmap_match_m(VALUE self, VALUE pattern)
{
  return mmap_bang_initialize(self, MMAP_RUBY_ORIGIN, rb_intern("match"), 1, &pattern);
}

/*
 * call-seq:
 *   length -> integer
 *   size -> integer
 *
 * Returns the size of the file.
 */
static VALUE
rb_cMmap_size(VALUE self)
{
  mmap_t *mmap;

  GET_MMAP(self, mmap, 0);
  return SIZET2NUM(mmap->real);
}

/*
 * call-seq:
 *   empty? -> true or false
 *
 * Returns +true+ if the file is empty, +false+ otherwise.
 */
static VALUE
rb_cMmap_empty(VALUE self)
{
  mmap_t *mmap;

  GET_MMAP(self, mmap, 0);
  if (mmap->real == 0) return Qtrue;
  return Qfalse;
}

/*
 * call-seq:
 *   [](nth) -> string or nil
 *   [](start, length) -> string or nil
 *   [](start..last) -> string or nil
 *   [](pattern) -> string or nil
 *   slice(nth) -> string or nil
 *   slice(start, length) -> string or nil
 *   slice(start..last) -> string or nil
 *   slice(pattern) -> string or nil
 *
 * Element reference with the following syntax:
 *
 *   self[nth]
 *
 * Retrieves the +nth+ character.
 *
 *   self[start..last]
 *
 * Returns a substring from +start+ to +last+.
 *
 *   self[start, length]
 *
 * Returns a substring of +length+ characters from +start+.
 *
 *   self[pattern]
 *
 * Returns the first match of +pattern+ (String or Regexp).
 */
static VALUE
rb_cMmap_aref(int argc, VALUE *argv, VALUE self)
{
  return mmap_bang_initialize(self, MMAP_RUBY_ORIGIN, rb_intern("[]"), argc, argv);
}

static VALUE
mmap_aset_m(VALUE self, VALUE indx, VALUE val)
{
  long idx;
  mmap_t *mmap;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  switch (TYPE(indx)) {
    case T_FIXNUM:
num_index:
      idx = NUM2INT(indx);
      if (idx < 0) {
        idx += mmap->real;
      }
      if (idx < 0 || mmap->real <= (size_t)idx) {
        rb_raise(rb_eIndexError, "index %ld out of string", idx);
      }
      if (FIXNUM_P(val)) {
        if (mmap->real == (size_t)idx) {
          mmap->real += 1;
          if (mmap->flag & MMAP_RUBY_FIXED) {
            rb_raise(rb_eTypeError, "can't change the size of a fixed map");
          }
        }
        ((char *)mmap->addr)[idx] = NUM2INT(val) & 0xff;
      }
      else {
        mmap_update(mmap, idx, 1, val);
      }
      return val;

    case T_REGEXP:
      mmap_subpat_set(self, indx, 0, val);
      return val;

    case T_STRING:
      {
        VALUE res;

        res = rb_cMmap_index(1, &indx, self);
        if (!NIL_P(res)) {
          mmap_update(mmap, NUM2LONG(res), RSTRING_LEN(indx), val);
        }
        return val;
      }

    default:
      {
        long beg, len;
        if (rb_range_beg_len(indx, &beg, &len, mmap->real, 2)) {
          mmap_update(mmap, beg, len, val);
          return val;
        }
      }
      idx = NUM2LONG(indx);
      goto num_index;
  }
}

static void
mmap_update(mmap_t *str, long beg, long len, VALUE val)
{
  char *valp;
  long vall;

  if (len < 0) rb_raise(rb_eIndexError, "negative length %ld", len);
  mmap_lock(str, Qtrue);
  if (beg < 0) {
    beg += str->real;
  }
  if (beg < 0 || str->real < (size_t)beg) {
    if (beg < 0) {
      beg -= str->real;
    }
    mmap_unlock(str);
    rb_raise(rb_eIndexError, "index %ld out of string", beg);
  }
  if (str->real < (size_t)(beg + len)) {
    len = str->real - beg;
  }

  mmap_unlock(str);
  valp = StringValuePtr(val);
  vall = RSTRING_LEN(val);
  mmap_lock(str, Qtrue);

  if ((str->flag & MMAP_RUBY_FIXED) && vall != len) {
    mmap_unlock(str);
    rb_raise(rb_eTypeError, "try to change the size of a fixed map");
  }
  if (len < vall) {
    mmap_realloc(str, str->real + vall - len);
  }

  if (vall != len) {
    memmove((char *)str->addr + beg + vall,
            (char *)str->addr + beg + len,
            str->real - (beg + len));
  }
  if (str->real < (size_t)beg && len < 0) {
    MEMZERO((char *)str->addr + str->real, char, -len);
  }
  if (vall > 0) {
    memmove((char *)str->addr + beg, valp, vall);
  }
  str->real += vall - len;
  mmap_unlock(str);
}

static void
mmap_subpat_set(VALUE obj, VALUE re, int offset, VALUE val)
{
  VALUE str, match;
  OnigPosition start, end;
  int len;
  mmap_t *mmap;
  struct re_registers *regs;

  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);
  if (rb_reg_search(re, str, 0, 0) < 0) {
    rb_raise(rb_eIndexError, "regexp not matched");
  }

  match = rb_backref_get();
  regs = RMATCH_REGS(match);
  if (offset >= regs->num_regs) {
    rb_raise(rb_eIndexError, "index %d out of regexp", offset);
  }

  start = regs->beg[offset];
  if (start == -1) {
    rb_raise(rb_eIndexError, "regexp group %d not matched", offset);
  }
  end = regs->end[offset];
  len = (int)(end - start);

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  mmap_update(mmap, start, len, val);
}

/*
 * call-seq:
 *   []=(nth, val) -> val
 *   []=(start, length, val) -> val
 *   []=(start..last, val) -> val
 *   []=(pattern, val) -> val
 *   []=(pattern, nth, val) -> val
 *
 * Element assignment with the following syntax:
 *
 *   self[nth] = val
 *
 * Changes the +nth+ character with +val+.
 *
 *   self[start..last] = val
 *
 * Changes substring from +start+ to +last+ with +val+.
 *
 *   self[start, length] = val
 *
 * Replaces +length+ characters from +start+ with +val+.
 *
 *   self[pattern] = val
 *
 * Replaces the first match of +pattern+ with +val+.
 *
 *   self[pattern, nth] = val
 *
 * Replaces the +nth+ match of +pattern+ with +val+.
 */
static VALUE
rb_cMmap_aset(int argc, VALUE *argv, VALUE self)
{
  mmap_t *mmap;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (argc == 3) {
    long beg, len;

    if (TYPE(argv[0]) == T_REGEXP) {
      mmap_subpat_set(self, argv[0], NUM2INT(argv[1]), argv[2]);
    }
    else {
      beg = NUM2INT(argv[0]);
      len = NUM2INT(argv[1]);
      mmap_update(mmap, beg, len, argv[2]);
    }
    return argv[2];
  }
  if (argc != 2) {
    rb_raise(rb_eArgError, "wrong # of arguments(%d for 2)", argc);
  }
  return mmap_aset_m(self, argv[0], argv[1]);
}

/*
 * call-seq:
 *   slice!(nth) -> string or nil
 *   slice!(start, length) -> string or nil
 *   slice!(start..last) -> string or nil
 *   slice!(pattern) -> string or nil
 *
 * Deletes the specified portion of the mapped memory and returns it.
 * Returns +nil+ if the portion is not found.
 */
static VALUE
rb_cMmap_slice_bang(int argc, VALUE *argv, VALUE self)
{
  VALUE result;
  VALUE buf[3];
  int i;

  if (argc < 1 || 2 < argc) {
    rb_raise(rb_eArgError, "wrong # of arguments(%d for 1)", argc);
  }

  for (i = 0; i < argc; i++) {
    buf[i] = argv[i];
  }
  buf[i] = rb_str_new(0, 0);
  result = rb_cMmap_aref(argc, buf, self);
  if (!NIL_P(result)) {
    rb_cMmap_aset(argc + 1, buf, self);
  }

  return result;
}

/*
 * call-seq:
 *   include?(other) -> true or false
 *
 * Returns +true+ if +other+ is found in the mapped memory, +false+ otherwise.
 * The +other+ parameter can be a string or regular expression.
 */
static VALUE
rb_cMmap_include(VALUE self, VALUE other)
{
  return mmap_bang_initialize(self, MMAP_RUBY_ORIGIN, rb_intern("include?"), 1, &other);
}

/*
 * call-seq:
 *   index(substr) -> integer or nil
 *   index(pattern) -> integer or nil
 *
 * Returns the index of +substr+ or +pattern+, or +nil+ if not found.
 */
static VALUE
rb_cMmap_index(int argc, VALUE *argv, VALUE self)
{
  return mmap_bang_initialize(self, MMAP_RUBY_ORIGIN, rb_intern("index"), argc, argv);
}

/*
 * call-seq:
 *   rindex(substr, pos = nil) -> integer or nil
 *   rindex(pattern, pos = nil) -> integer or nil
 *
 * Returns the index of the last occurrence of +substr+ or +pattern+, or +nil+ if not found.
 */
static VALUE
rb_cMmap_rindex(int argc, VALUE *argv, VALUE self)
{
  return mmap_bang_initialize(self, MMAP_RUBY_ORIGIN, rb_intern("rindex"), argc, argv);
}

/*
 * call-seq:
 *   count(o1, *args) -> integer
 *
 * Each parameter defines a set of characters to count in the mapped memory.
 * Returns the total count.
 */
static VALUE
rb_cMmap_count(int argc, VALUE *argv, VALUE self)
{
  return mmap_bang_initialize(self, MMAP_RUBY_ORIGIN, rb_intern("count"), argc, argv);
}

/*
 * call-seq:
 *   sum(bits = 16) -> integer
 *
 * Returns a checksum for the mapped memory content.
 */
static VALUE
rb_cMmap_sum(int argc, VALUE *argv, VALUE self)
{
  return mmap_bang_initialize(self, MMAP_RUBY_ORIGIN, rb_intern("sum"), argc, argv);
}

/*
 * call-seq:
 *   insert(index, str) -> self
 *
 * Inserts +str+ at +index+. Returns +self+.
 */
static VALUE
rb_cMmap_insert(VALUE self, VALUE idx, VALUE str)
{
  mmap_t *mmap;
  long pos = NUM2LONG(idx);

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (pos == -1) {
    pos = mmap->real;
  }
  else if (pos < 0) {
    pos++;
  }
  mmap_update(mmap, pos, 0, str);
  return self;
}

static VALUE
mmap_cat(VALUE self, const char *ptr, long len)
{
  mmap_t *mmap;
  char *sptr;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (len > 0) {
    ptrdiff_t poffset = -1;
    sptr = (char *)mmap->addr;

    if (sptr <= ptr && ptr < sptr + mmap->real) {
      poffset = ptr - sptr;
    }
    mmap_lock(mmap, Qtrue);
    if (mmap->flag & MMAP_RUBY_FIXED) {
      mmap_unlock(mmap);
      rb_raise(rb_eTypeError, "can't change the size of a fixed map");
    }

    mmap_realloc(mmap, mmap->real + len);

    sptr = (char *)mmap->addr;
    if (ptr) {
      if (poffset >= 0) ptr = sptr + poffset;
      memcpy(sptr + mmap->real, ptr, len);
    }
    mmap->real += len;
    mmap_unlock(mmap);
  }
  return self;
}

static VALUE
mmap_append(VALUE self, VALUE str)
{
  str = rb_str_to_str(str);
  self = mmap_cat(self, StringValuePtr(str), RSTRING_LEN(str));
  return self;
}

/*
 * call-seq:
 *   concat(other) -> self
 *   <<(other) -> self
 *
 * Appends the contents of +other+ to the mapped memory. If +other+ is an
 * integer between 0 and 255, it is treated as a byte value. Returns +self+.
 */
static VALUE
rb_cMmap_concat(VALUE self, VALUE other)
{
  if (FIXNUM_P(other)) {
    int i = FIX2INT(other);
    if (0 <= i && i <= 0xff) {
      char c = i;
      return mmap_cat(self, &c, 1);
    }
  }
  self = mmap_append(self, other);
  return self;
}

static VALUE
get_pat(VALUE pat)
{
  switch (TYPE(pat)) {
    case T_REGEXP:
      break;

    case T_STRING:
      pat = rb_reg_regcomp(pat);
      break;

    default:
      Check_Type(pat, T_REGEXP);
  }

  return pat;
}

static int
mmap_correct_backref(void)
{
  VALUE match;
  struct re_registers *regs;
  int i;
  OnigPosition start;

  match = rb_backref_get();
  if (NIL_P(match)) return 0;

  regs = RMATCH_REGS(match);
  if (regs->beg[0] == -1) return 0;

  start = regs->beg[0];

  RMATCH(match)->str = rb_str_new(RSTRING_PTR(RMATCH(match)->str) + start,
                                  regs->end[0] - start);

  for (i = 0; i < regs->num_regs && regs->beg[i] != -1; i++) {
    regs->beg[i] -= start;
    regs->end[i] -= start;
  }

  rb_backref_set(match);
  return (int)start;
}

static VALUE
mmap_sub_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  int argc = bang_st->argc;
  VALUE *argv = bang_st->argv;
  VALUE obj = bang_st->obj;
  VALUE pat, repl = Qnil, match, str, res;
  struct re_registers *regs;
  int start, iter = 0;
  long plen;
  mmap_t *mmap;

  if (argc == 1 && rb_block_given_p()) {
    iter = 1;
  }
  else if (argc == 2) {
    repl = rb_str_to_str(argv[1]);
  }
  else {
    rb_raise(rb_eArgError, "wrong # of arguments(%d for 2)", argc);
  }

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);

  pat = get_pat(argv[0]);
  res = Qnil;
  if (rb_reg_search(pat, str, 0, 0) >= 0) {
    start = mmap_correct_backref();
    match = rb_backref_get();
    regs = RMATCH_REGS(match);

    if (iter) {
      rb_match_busy(match);
      repl = rb_obj_as_string(rb_yield(rb_reg_nth_match(0, match)));
      rb_backref_set(match);
    }
    else {
      VALUE substr = rb_str_subseq(str, start + regs->beg[0], regs->end[0] - regs->beg[0]);
      repl = rb_reg_regsub(repl, substr, regs, match);
    }

    plen = regs->end[0] - regs->beg[0];

    if (RSTRING_LEN(repl) != plen) {
      if (mmap->flag & MMAP_RUBY_FIXED) {
        rb_raise(rb_eTypeError, "can't change the size of a fixed map");
      }

      memmove(RSTRING_PTR(str) + start + regs->beg[0] + RSTRING_LEN(repl),
              RSTRING_PTR(str) + start + regs->beg[0] + plen,
              RSTRING_LEN(str) - start - regs->beg[0] - plen);
    }

    memcpy(RSTRING_PTR(str) + start + regs->beg[0],
           RSTRING_PTR(repl), RSTRING_LEN(repl));
    mmap->real += RSTRING_LEN(repl) - plen;

    res = obj;
  }

  RB_GC_GUARD(str);
  return res;
}

/*
 * call-seq:
 *   sub!(pattern, replacement) -> self or nil
 *   sub!(pattern) {|match| block } -> self or nil
 *
 * Performs substitution on the mapped memory. Returns +self+ if a substitution
 * was made, or +nil+ if no substitution occurred.
 */
static VALUE
rb_cMmap_sub_bang(int argc, VALUE *argv, VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = argc;
  bang_st.argv = argv;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_sub_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_sub_bang_int((VALUE)&bang_st);
  }

  return res;
}

static VALUE
mmap_gsub_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  int argc = bang_st->argc;
  VALUE *argv = bang_st->argv;
  VALUE obj = bang_st->obj;
  VALUE pat, val, repl = Qnil, match, str;
  struct re_registers *regs;
  long beg, offset;
  int start, iter = 0;
  long plen;
  mmap_t *mmap;

  if (argc == 1 && rb_block_given_p()) {
    iter = 1;
  }
  else if (argc == 2) {
    repl = rb_str_to_str(argv[1]);
  }
  else {
    rb_raise(rb_eArgError, "wrong # of arguments(%d for 2)", argc);
  }

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);

  pat = get_pat(argv[0]);
  offset = 0;
  beg = rb_reg_search(pat, str, 0, 0);
  if (beg < 0) {
    RB_GC_GUARD(str);
    return Qnil;
  }

  while (beg >= 0) {
    start = mmap_correct_backref();
    match = rb_backref_get();
    regs = RMATCH_REGS(match);

    if (iter) {
      rb_match_busy(match);
      val = rb_obj_as_string(rb_yield(rb_reg_nth_match(0, match)));
      rb_backref_set(match);
    }
    else {
      VALUE substr = rb_str_subseq(str, start + regs->beg[0], regs->end[0] - regs->beg[0]);
      val = rb_reg_regsub(repl, substr, regs, match);
    }

    plen = regs->end[0] - regs->beg[0];

    if (RSTRING_LEN(val) != plen) {
      if (mmap->flag & MMAP_RUBY_FIXED) {
        rb_raise(rb_eTypeError, "can't change the size of a fixed map");
      }

      if ((mmap->real + RSTRING_LEN(val) - plen) > mmap->len) {
        rb_raise(rb_eTypeError, "replacement would exceed mmap size");
      }

      memmove(RSTRING_PTR(str) + start + regs->beg[0] + RSTRING_LEN(val),
              RSTRING_PTR(str) + start + regs->beg[0] + plen,
              RSTRING_LEN(str) - start - regs->beg[0] - plen);
    }

    memcpy(RSTRING_PTR(str) + start + regs->beg[0],
           RSTRING_PTR(val), RSTRING_LEN(val));
    mmap->real += RSTRING_LEN(val) - plen;

    if (regs->beg[0] == regs->end[0]) {
      offset = start + regs->end[0] + 1;
      offset += RSTRING_LEN(val) - plen;
    }
    else {
      offset = start + regs->end[0] + RSTRING_LEN(val) - plen;
    }

    if (offset > RSTRING_LEN(str)) break;
    beg = rb_reg_search(pat, str, offset, 0);
  }

  rb_backref_set(match);
  RB_GC_GUARD(str);
  return obj;
}

/*
 * call-seq:
 *   gsub!(pattern, replacement) -> self or nil
 *   gsub!(pattern) {|match| block } -> self or nil
 *
 * Performs global substitution on the mapped memory. Returns +self+ if any
 * substitutions were made, or +nil+ if no substitutions occurred.
 */
static VALUE
rb_cMmap_gsub_bang(int argc, VALUE *argv, VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = argc;
  bang_st.argv = argv;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_gsub_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_gsub_bang_int((VALUE)&bang_st);
  }

  return res;
}

static VALUE
mmap_upcase_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  VALUE obj = bang_st->obj;
  VALUE str;
  mmap_t *mmap;
  char *ptr;
  long len;
  long i;
  int changed = 0;

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);

  ptr = RSTRING_PTR(str);
  len = RSTRING_LEN(str);

  for (i = 0; i < len; i++) {
    if (ptr[i] >= 'a' && ptr[i] <= 'z') {
      ptr[i] = ptr[i] - 'a' + 'A';
      changed = 1;
    }
  }

  RB_GC_GUARD(str);
  return changed ? obj : Qnil;
}

/*
 * call-seq:
 *   upcase! -> self
 *
 * Replaces all lowercase characters to uppercase characters in the mapped memory.
 * Returns +self+.
 */
static VALUE
rb_cMmap_upcase_bang(VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = 0;
  bang_st.argv = NULL;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_upcase_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_upcase_bang_int((VALUE)&bang_st);
  }

  return res;
}

static VALUE
mmap_downcase_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  VALUE obj = bang_st->obj;
  VALUE str;
  mmap_t *mmap;
  char *ptr;
  long len;
  long i;
  int changed = 0;

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);

  ptr = RSTRING_PTR(str);
  len = RSTRING_LEN(str);

  for (i = 0; i < len; i++) {
    if (ptr[i] >= 'A' && ptr[i] <= 'Z') {
      ptr[i] = ptr[i] - 'A' + 'a';
      changed = 1;
    }
  }

  RB_GC_GUARD(str);
  return changed ? obj : Qnil;
}

/*
 * call-seq:
 *   downcase! -> self
 *
 * Changes all uppercase characters to lowercase characters in the mapped memory.
 * Returns +self+.
 */
static VALUE
rb_cMmap_downcase_bang(VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = 0;
  bang_st.argv = NULL;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_downcase_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_downcase_bang_int((VALUE)&bang_st);
  }

  return res;
}

static VALUE
mmap_capitalize_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  VALUE obj = bang_st->obj;
  VALUE str;
  mmap_t *mmap;
  char *ptr;
  long len;
  long i;
  int changed = 0;

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);

  ptr = RSTRING_PTR(str);
  len = RSTRING_LEN(str);

  for (i = 0; i < len; i++) {
    if (i == 0) {
      if (ptr[i] >= 'a' && ptr[i] <= 'z') {
        ptr[i] = ptr[i] - 'a' + 'A';
        changed = 1;
      }
    } else {
      if (ptr[i] >= 'A' && ptr[i] <= 'Z') {
        ptr[i] = ptr[i] - 'A' + 'a';
        changed = 1;
      }
    }
  }

  RB_GC_GUARD(str);
  return changed ? obj : Qnil;
}

/*
 * call-seq:
 *   capitalize! -> self
 *
 * Changes the first character to uppercase letter in the mapped memory.
 * Returns +self+.
 */
static VALUE
rb_cMmap_capitalize_bang(VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = 0;
  bang_st.argv = NULL;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_capitalize_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_capitalize_bang_int((VALUE)&bang_st);
  }

  return res;
}

static VALUE
mmap_swapcase_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  VALUE obj = bang_st->obj;
  VALUE str;
  mmap_t *mmap;
  char *ptr;
  long len;
  long i;
  int changed = 0;

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);

  ptr = RSTRING_PTR(str);
  len = RSTRING_LEN(str);

  for (i = 0; i < len; i++) {
    if (ptr[i] >= 'a' && ptr[i] <= 'z') {
      ptr[i] = ptr[i] - 'a' + 'A';
      changed = 1;
    } else if (ptr[i] >= 'A' && ptr[i] <= 'Z') {
      ptr[i] = ptr[i] - 'A' + 'a';
      changed = 1;
    }
  }

  RB_GC_GUARD(str);
  return changed ? obj : Qnil;
}

/*
 * call-seq:
 *   swapcase! -> self
 *
 * Replaces lowercase to uppercase and vice-versa in the mapped memory.
 * Returns +self+.
 */
static VALUE
rb_cMmap_swapcase_bang(VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = 0;
  bang_st.argv = NULL;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_swapcase_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_swapcase_bang_int((VALUE)&bang_st);
  }

  return res;
}

static VALUE
mmap_reverse_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  VALUE obj = bang_st->obj;
  VALUE str;
  mmap_t *mmap;
  char *ptr;
  long len;
  long i;
  char temp;

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);

  ptr = RSTRING_PTR(str);
  len = RSTRING_LEN(str);

  for (i = 0; i < len / 2; i++) {
    temp = ptr[i];
    ptr[i] = ptr[len - 1 - i];
    ptr[len - 1 - i] = temp;
  }

  RB_GC_GUARD(str);
  return obj;
}

/*
 * call-seq:
 *   reverse! -> self
 *
 * Reverses the characters in the mapped memory in place.
 * Returns +self+.
 */
static VALUE
rb_cMmap_reverse_bang(VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = 0;
  bang_st.argv = NULL;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_reverse_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_reverse_bang_int((VALUE)&bang_st);
  }

  return res;
}

/*
 * call-seq:
 *   strip! -> self
 *
 * Removes leading and trailing whitespace from the mapped memory.
 * Returns +self+.
 */
static VALUE
rb_cMmap_strip_bang(VALUE self)
{
  char *s, *t, *e;
  mmap_t *mmap;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  mmap_lock(mmap, Qtrue);
  s = (char *)mmap->addr;
  e = t = s + mmap->real;
  while (s < t && ISSPACE(*s)) s++;
  t--;
  while (s <= t && ISSPACE(*t)) t--;
  t++;

  if (mmap->real != (size_t)(t - s) && (mmap->flag & MMAP_RUBY_FIXED)) {
    mmap_unlock(mmap);
    rb_raise(rb_eTypeError, "can't change the size of a fixed map");
  }
  mmap->real = t - s;
  if (s > (char *)mmap->addr) {
    memmove(mmap->addr, s, mmap->real);
    ((char *)mmap->addr)[mmap->real] = '\0';
  }
  else if (t < e) {
    ((char *)mmap->addr)[mmap->real] = '\0';
  }
  else {
    self = Qnil;
  }
  mmap_unlock(mmap);
  return self;
}

static VALUE
mmap_chop_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  VALUE obj = bang_st->obj;
  VALUE str;
  mmap_t *mmap;
  char *ptr;
  long len;

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);

  ptr = RSTRING_PTR(str);
  len = RSTRING_LEN(str);

  if (len == 0) {
    RB_GC_GUARD(str);
    return Qnil;
  }

  if (len >= 2 && ptr[len-2] == '\r' && ptr[len-1] == '\n') {
    mmap->real -= 2;
  } else {
    mmap->real -= 1;
  }

  RB_GC_GUARD(str);
  return obj;
}

/*
 * call-seq:
 *   chop! -> self
 *
 * Chops off the last character in the mapped memory.
 * Returns +self+.
 */
static VALUE
rb_cMmap_chop_bang(VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = 0;
  bang_st.argv = NULL;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_chop_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_chop_bang_int((VALUE)&bang_st);
  }

  return res;
}

static VALUE
mmap_chomp_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  VALUE obj = bang_st->obj;
  VALUE str;
  mmap_t *mmap;
  char *ptr;
  long len;
  VALUE rs;
  char *rs_ptr;
  long rs_len;
  int changed = 0;

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);

  ptr = RSTRING_PTR(str);
  len = RSTRING_LEN(str);

  if (len == 0) {
    RB_GC_GUARD(str);
    return Qnil;
  }

  if (bang_st->argc == 0) {
    rs = rb_rs;
  } else {
    rs = bang_st->argv[0];
  }

  if (NIL_P(rs)) {
    RB_GC_GUARD(str);
    return Qnil;
  }

  rs = rb_str_to_str(rs);
  rs_ptr = RSTRING_PTR(rs);
  rs_len = RSTRING_LEN(rs);

  if (rs_len == 0) {
    while (len > 0 && (ptr[len-1] == '\n' || ptr[len-1] == '\r')) {
      len--;
      changed = 1;
    }
  } else if (rs_len == 1 && rs_ptr[0] == '\n') {
    if (len >= 2 && ptr[len-2] == '\r' && ptr[len-1] == '\n') {
      len -= 2;
      changed = 1;
    } else if (len >= 1 && ptr[len-1] == '\n') {
      len -= 1;
      changed = 1;
    }
  } else {
    if (len >= rs_len && memcmp(ptr + len - rs_len, rs_ptr, rs_len) == 0) {
      len -= rs_len;
      changed = 1;
    }
  }

  if (changed) {
    mmap->real = len;
  }

  RB_GC_GUARD(str);
  return changed ? obj : Qnil;
}

/*
 * call-seq:
 *   chomp!(rs = $/) -> self
 *
 * Chops off line ending character specified by +rs+ in the mapped memory.
 * Returns +self+.
 */
static VALUE
rb_cMmap_chomp_bang(int argc, VALUE *argv, VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = argc;
  bang_st.argv = argv;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_chomp_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_chomp_bang_int((VALUE)&bang_st);
  }

  return res;
}

static VALUE
mmap_delete_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  VALUE obj = bang_st->obj;
  VALUE str, result;
  mmap_t *mmap;
  long new_len;

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);

  str = rb_str_new(mmap->addr, mmap->real);

  result = rb_funcall2(str, rb_intern("delete!"), bang_st->argc, bang_st->argv);

  if (result == Qnil) {
    return Qnil;
  }

  new_len = RSTRING_LEN(str);
  if (new_len > (long)mmap->len) {
    rb_raise(rb_eRuntimeError, "string too long for mmap");
  }

  memcpy(mmap->addr, RSTRING_PTR(str), new_len);
  mmap->real = new_len;

  return obj;
}

/*
 * call-seq:
 *   delete!(str) -> self
 *
 * Deletes every character included in +str+ from the mapped memory.
 * Returns +self+.
 */
static VALUE
rb_cMmap_delete_bang(int argc, VALUE *argv, VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = argc;
  bang_st.argv = argv;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_delete_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_delete_bang_int((VALUE)&bang_st);
  }

  return res;
}

static VALUE
mmap_squeeze_bang_int(VALUE data)
{
  mmap_bang *bang_st = (mmap_bang *)data;
  VALUE obj = bang_st->obj;
  VALUE str;
  mmap_t *mmap;
  char *ptr;
  long len;
  long i, j;
  VALUE squeeze_str;
  char *squeeze_ptr;
  long squeeze_len;
  int squeeze_table[256];
  int changed = 0;

  GET_MMAP(obj, mmap, MMAP_RUBY_MODIFY);
  str = mmap_str(obj, MMAP_RUBY_MODIFY | MMAP_RUBY_ORIGIN);

  ptr = RSTRING_PTR(str);
  len = RSTRING_LEN(str);

  if (len == 0) {
    RB_GC_GUARD(str);
    return Qnil;
  }

  if (bang_st->argc == 0) {
    memset(squeeze_table, 1, sizeof(squeeze_table));
  } else {
    squeeze_str = rb_str_to_str(bang_st->argv[0]);
    squeeze_ptr = RSTRING_PTR(squeeze_str);
    squeeze_len = RSTRING_LEN(squeeze_str);

    memset(squeeze_table, 0, sizeof(squeeze_table));
    for (i = 0; i < squeeze_len; i++) {
      squeeze_table[(unsigned char)squeeze_ptr[i]] = 1;
    }
  }

  j = 0;
  for (i = 0; i < len; i++) {
    if (i == 0 || ptr[i] != ptr[i-1] || !squeeze_table[(unsigned char)ptr[i]]) {
      ptr[j++] = ptr[i];
    } else {
      changed = 1;
    }
  }

  if (changed) {
    mmap->real = j;
  }

  RB_GC_GUARD(str);
  return changed ? obj : Qnil;
}

/*
 * call-seq:
 *   squeeze!(str) -> self
 *
 * Squeezes sequences of the same characters that are included in +str+.
 * Returns +self+.
 */
static VALUE
rb_cMmap_squeeze_bang(int argc, VALUE *argv, VALUE self)
{
  VALUE res;
  mmap_bang bang_st;
  mmap_t *mmap;

  bang_st.argc = argc;
  bang_st.argv = argv;
  bang_st.obj = self;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    res = rb_ensure(mmap_squeeze_bang_int, (VALUE)&bang_st, mmap_vunlock, self);
  }
  else {
    res = mmap_squeeze_bang_int((VALUE)&bang_st);
  }

  return res;
}

/*
 * call-seq:
 *   split(sep = $/, limit = 0) -> array
 *
 * Splits the mapped memory into an array of strings and returns this array.
 * The +sep+ parameter specifies the separator pattern (String or Regexp).
 * The +limit+ parameter controls the number of splits.
 */
static VALUE
rb_cMmap_split(int argc, VALUE *argv, VALUE self)
{
  return mmap_bang_initialize(self, MMAP_RUBY_ORIGIN, rb_intern("split"), argc, argv);
}

/*
 * call-seq:
 *   crypt(salt) -> string
 *
 * Encrypts the mapped memory using the standard Unix crypt function with
 * the given +salt+. Returns the encrypted string.
 */
static VALUE
rb_cMmap_crypt(VALUE self, VALUE salt)
{
  return mmap_bang_initialize(self, MMAP_RUBY_ORIGIN, rb_intern("crypt"), 1, &salt);
}

/*
 * call-seq:
 *   mprotect(mode) -> self
 *   protect(mode) -> self
 *
 * Changes the memory protection mode. The +mode+ value must be "r", "w", "rw",
 * or an integer representing protection flags. Returns +self+.
 */
static VALUE
rb_cMmap_mprotect(VALUE self, VALUE mode)
{
  mmap_t *mmap;
  int ret, pmode;
  const char *smode;

  GET_MMAP(self, mmap, 0);
  if (TYPE(mode) == T_STRING) {
    smode = StringValuePtr(mode);
    if (strcmp(smode, "r") == 0) {
      pmode = PROT_READ;
    }
    else if (strcmp(smode, "w") == 0) {
      pmode = PROT_WRITE;
    }
    else if (strcmp(smode, "rw") == 0 || strcmp(smode, "wr") == 0) {
      pmode = PROT_READ | PROT_WRITE;
    }
    else {
      rb_raise(rb_eArgError, "invalid mode %s", smode);
    }
  }
  else {
    pmode = NUM2INT(mode);
  }

  if ((pmode & PROT_WRITE) && RB_OBJ_FROZEN(self)) {
    rb_check_frozen(self);
  }

  if ((ret = mprotect(mmap->addr, mmap->len, pmode | PROT_READ)) != 0) {
    rb_raise(rb_eArgError, "mprotect(%d)", ret);
  }

  mmap->pmode = pmode;
  if (pmode & PROT_READ) {
    if (pmode & PROT_WRITE) {
      mmap->smode = O_RDWR;
    }
    else {
      mmap->smode = O_RDONLY;
      self = rb_obj_freeze(self);
    }
  }
  else if (pmode & PROT_WRITE) {
    mmap->flag |= MMAP_RUBY_FIXED;
    mmap->smode = O_WRONLY;
  }

  return self;
}

/*
 * call-seq:
 *   madvise(advice) -> nil
 *   advise(advice) -> nil
 *
 * Gives advice to the kernel about how the mapped memory will be accessed.
 * The +advice+ parameter can be one of the following constants:
 * MADV_NORMAL, MADV_RANDOM, MADV_SEQUENTIAL, MADV_WILLNEED, or MADV_DONTNEED.
 */
static VALUE
rb_cMmap_madvise(VALUE self, VALUE advice)
{
  mmap_t *mmap;

  GET_MMAP(self, mmap, 0);
  if (madvise(mmap->addr, mmap->len, NUM2INT(advice)) == -1) {
    rb_raise(rb_eTypeError, "madvise(%d)", errno);
  }
  mmap->advice = NUM2INT(advice);
  return Qnil;
}

static void
mmap_realloc(mmap_t *mmap, size_t len)
{
  if (len > mmap->len) {
    if ((len - mmap->len) < mmap->incr) {
      len = mmap->len + mmap->incr;
    }
    mmap_expandf(mmap, len);
  }
}

static VALUE
mmap_expand_initialize(VALUE data)
{
  mmap_st *st_mm = (mmap_st *)data;
  int fd;
  mmap_t *mmap = st_mm->mmap;
  size_t len = st_mm->len;

  if (munmap(mmap->addr, mmap->len)) {
    rb_raise(rb_eArgError, "munmap failed");
  }

  if ((fd = open(mmap->path, mmap->smode)) == -1) {
    rb_raise(rb_eArgError, "can't open %s", mmap->path);
  }

  if (len > mmap->len) {
    if (lseek(fd, len - mmap->len - 1, SEEK_END) == -1) {
      rb_raise(rb_eIOError, "can't lseek %zu", len - mmap->len - 1);
    }
    if (write(fd, "\000", 1) != 1) {
      rb_raise(rb_eIOError, "can't extend %s", mmap->path);
    }
  }
  else if (len < mmap->len && truncate(mmap->path, len) == -1) {
    rb_raise(rb_eIOError, "can't truncate %s", mmap->path);
  }

  mmap->addr = mmap_func(0, len, mmap->pmode, mmap->vscope, fd, mmap->offset);
  close(fd);

  if (mmap->addr == MAP_FAILED) {
    rb_raise(rb_eArgError, "mmap failed");
  }

#ifdef MADV_NORMAL
  if (mmap->advice && madvise(mmap->addr, len, mmap->advice) == -1) {
    rb_raise(rb_eArgError, "madvise(%d)", errno);
  }
#endif

  if ((mmap->flag & MMAP_RUBY_LOCK) && mlock(mmap->addr, len) == -1) {
    rb_raise(rb_eArgError, "mlock(%d)", errno);
  }

  mmap->len = len;
  return Qnil;
}

static void
mmap_expandf(mmap_t *mmap, size_t len)
{
  int status;
  mmap_st st_mm;

  if (mmap->vscope == MAP_PRIVATE) {
    rb_raise(rb_eTypeError, "expand for a private map");
  }
  if (mmap->flag & MMAP_RUBY_FIXED) {
    rb_raise(rb_eTypeError, "expand for a fixed map");
  }
  if (!mmap->path || mmap->path == (char *)(intptr_t)-1) {
    rb_raise(rb_eTypeError, "expand for an anonymous map");
  }

  st_mm.mmap = mmap;
  st_mm.len = len;

  if (mmap->flag & MMAP_RUBY_IPC) {
    mmap_lock(mmap, Qtrue);
    rb_protect(mmap_expand_initialize, (VALUE)&st_mm, &status);
    mmap_unlock(mmap);
    if (status) {
      rb_jump_tag(status);
    }
  }
  else {
    mmap_expand_initialize((VALUE)&st_mm);
  }
}

/*
 * call-seq:
 *   msync(flag = MS_SYNC) -> self
 *   sync(flag = MS_SYNC) -> self
 *   flush(flag = MS_SYNC) -> self
 *
 * Flushes the mapped memory to the underlying file. The +flag+ parameter
 * controls the synchronization behavior (MS_SYNC, MS_ASYNC, or MS_INVALIDATE).
 * Returns +self+.
 */
static VALUE
rb_cMmap_msync(int argc, VALUE *argv, VALUE self)
{
  mmap_t *mmap;
  VALUE oflag;
  int ret;
  int flag = MS_SYNC;

  if (argc) {
    rb_scan_args(argc, argv, "01", &oflag);
    flag = NUM2INT(oflag);
  }

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  if ((ret = msync(mmap->addr, mmap->len, flag)) != 0) {
    rb_raise(rb_eArgError, "msync(%d)", ret);
  }

  if (mmap->real < mmap->len && mmap->vscope != MAP_PRIVATE) {
    mmap_expandf(mmap, mmap->real);
  }

  return self;
}

/*
 * call-seq:
 *   lock -> self
 *   mlock -> self
 *
 * Disables paging for the mapped memory, locking it in physical memory.
 * Returns +self+.
 */
static VALUE
rb_cMmap_mlock(VALUE self)
{
  mmap_t *mmap;

  TypedData_Get_Struct(self, mmap_t, &mmap_type, mmap);
  if (mmap->flag & MMAP_RUBY_LOCK) {
    return self;
  }
  if (mmap->flag & MMAP_RUBY_ANON) {
    rb_raise(rb_eArgError, "mlock(anonymous)");
  }
  if (mlock(mmap->addr, mmap->len) == -1) {
    rb_raise(rb_eArgError, "mlock(%d)", errno);
  }
  mmap->flag |= MMAP_RUBY_LOCK;
  return self;
}

/*
 * call-seq:
 *   unlock -> self
 *   munlock -> self
 *
 * Re-enables paging for the mapped memory.
 * Returns +self+.
 */
static VALUE
rb_cMmap_munlock(VALUE self)
{
  mmap_t *mmap;

  TypedData_Get_Struct(self, mmap_t, &mmap_type, mmap);
  if (!(mmap->flag & MMAP_RUBY_LOCK)) {
    return self;
  }
  if (munlock(mmap->addr, mmap->len) == -1) {
    rb_raise(rb_eArgError, "munlock(%d)", errno);
  }
  mmap->flag &= ~MMAP_RUBY_LOCK;
  return self;
}

/*
 * call-seq:
 *   extend(count) -> integer
 *
 * Adds +count+ bytes to the file (i.e. pre-extends the file).
 * Returns the new size of the mapped memory.
 */
static VALUE
rb_cMmap_extend(VALUE self, VALUE count)
{
  mmap_t *mmap;
  long len;

  GET_MMAP(self, mmap, MMAP_RUBY_MODIFY);
  len = NUM2LONG(count);
  if (len > 0) {
    mmap_expandf(mmap, mmap->len + len);
  }
  return SIZET2NUM(mmap->len);
}

/*
 * call-seq:
 *   munmap -> nil
 *   unmap -> nil
 *
 * Terminates the association between the mapped memory and the file.
 */
static VALUE
rb_cMmap_unmap(VALUE self)
{
  mmap_t *mmap;

  GET_MMAP(self, mmap, 0);
  if (mmap->path) {
    mmap_lock(mmap, Qtrue);
    munmap(mmap->addr, mmap->len);
    if (mmap->path != (char *)(intptr_t)-1) {
      if (mmap->real < mmap->len &&
          mmap->vscope != MAP_PRIVATE &&
          truncate(mmap->path, mmap->real) == -1) {
        rb_raise(rb_eTypeError, "truncate");
      }
      free(mmap->path);
    }
    mmap->path = NULL;
    mmap_unlock(mmap);
  }
  return Qnil;
}

/*
 * call-seq:
 *   semlock -> self
 *
 * Creates a lock.
 */
static VALUE
rb_cMmap_semlock(int argc, VALUE *argv, VALUE self)
{
  mmap_t *mmap;
  VALUE a;
  int wait_lock = Qtrue;

  GET_MMAP(self, mmap, 0);
  if (!(mmap->flag & MMAP_RUBY_IPC)) {
    rb_warning("useless use of #semlock");
    rb_yield(self);
  }
  else {
    if (rb_scan_args(argc, argv, "01", &a)) {
      wait_lock = RTEST(a);
    }
    mmap_lock(mmap, wait_lock);
    rb_ensure(rb_yield, self, mmap_vunlock, self);
  }
  return Qnil;
}

/*
 * call-seq:
 *   ipc_key -> integer
 *
 * Gets the IPC key.
 */
static VALUE
rb_cMmap_ipc_key(VALUE self)
{
  mmap_t *mmap;

  GET_MMAP(self, mmap, 0);
  if (mmap->flag & MMAP_RUBY_IPC) {
    return INT2NUM((int)mmap->key);
  }
  return INT2NUM(-1);
}

static VALUE
rb_cMmap_set_length(VALUE self, VALUE value)
{
  mmap_t *mmap;

  TypedData_Get_Struct(self, mmap_t, &mmap_type, mmap);

  mmap->len = NUM2UINT(value);
  if (mmap->len == 0) {
    rb_raise(rb_eArgError, "invalid value for length %zu", mmap->len);
  }
  mmap->flag |= MMAP_RUBY_FIXED;

  return self;
}

static VALUE
rb_cMmap_set_offset(VALUE self, VALUE value)
{
  mmap_t *mmap;

  TypedData_Get_Struct(self, mmap_t, &mmap_type, mmap);

  mmap->offset = NUM2INT(value);
  if (mmap->offset < 0) {
    rb_raise(rb_eArgError, "invalid value for offset %ld", (long)mmap->offset);
  }
  mmap->flag |= MMAP_RUBY_FIXED;

  return self;
}

static VALUE
rb_cMmap_set_increment(VALUE self, VALUE value)
{
  mmap_t *mmap;
  int incr;

  TypedData_Get_Struct(self, mmap_t, &mmap_type, mmap);

  incr = NUM2INT(value);
  if (incr < 0) {
    rb_raise(rb_eArgError, "invalid value for increment %d", incr);
  }
  mmap->incr = incr;

  return self;
}

static VALUE
rb_cMmap_set_ipc(VALUE self, VALUE value)
{
  mmap_t *mmap;

  TypedData_Get_Struct(self, mmap_t, &mmap_type, mmap);
  if (value != Qtrue && TYPE(value) != T_HASH) {
    rb_raise(rb_eArgError, "expected an Hash for :ipc");
  }
  mmap->shmid = value;
  mmap->flag |= (MMAP_RUBY_IPC | MMAP_RUBY_TMP);

  return self;
}

static VALUE
rb_cMmap_set_advice(VALUE self, VALUE value)
{
  mmap_t *mmap;

  TypedData_Get_Struct(self, mmap_t, &mmap_type, mmap);
  mmap->advice = NUM2INT(value);

  return self;
}

RUBY_FUNC_EXPORTED void
Init_mmap_ruby()
{
  VALUE rb_mMmapRuby = rb_define_module("MmapRuby");
  VALUE rb_cMmap = rb_define_class_under(rb_mMmapRuby, "Mmap", rb_cObject);

  rb_define_const(rb_cMmap, "MS_SYNC", INT2FIX(MS_SYNC));
  rb_define_const(rb_cMmap, "MS_ASYNC", INT2FIX(MS_ASYNC));
  rb_define_const(rb_cMmap, "MS_INVALIDATE", INT2FIX(MS_INVALIDATE));
  rb_define_const(rb_cMmap, "PROT_READ", INT2FIX(PROT_READ));
  rb_define_const(rb_cMmap, "PROT_WRITE", INT2FIX(PROT_WRITE));
  rb_define_const(rb_cMmap, "PROT_EXEC", INT2FIX(PROT_EXEC));
  rb_define_const(rb_cMmap, "PROT_NONE", INT2FIX(PROT_NONE));
  rb_define_const(rb_cMmap, "MAP_ANON", INT2FIX(MAP_ANON));
  rb_define_const(rb_cMmap, "MAP_ANONYMOUS", INT2FIX(MAP_ANONYMOUS));
  rb_define_const(rb_cMmap, "MAP_SHARED", INT2FIX(MAP_SHARED));
  rb_define_const(rb_cMmap, "MAP_PRIVATE", INT2FIX(MAP_PRIVATE));
  rb_define_const(rb_cMmap, "MADV_NORMAL", INT2FIX(MADV_NORMAL));
  rb_define_const(rb_cMmap, "MADV_RANDOM", INT2FIX(MADV_RANDOM));
  rb_define_const(rb_cMmap, "MADV_SEQUENTIAL", INT2FIX(MADV_SEQUENTIAL));
  rb_define_const(rb_cMmap, "MADV_WILLNEED", INT2FIX(MADV_WILLNEED));
  rb_define_const(rb_cMmap, "MADV_DONTNEED", INT2FIX(MADV_DONTNEED));
#ifdef MAP_DENYWRITE
  rb_define_const(rb_cMmap, "MAP_DENYWRITE", INT2FIX(MAP_DENYWRITE));
#endif
#ifdef MAP_EXECUTABLE
  rb_define_const(rb_cMmap, "MAP_EXECUTABLE", INT2FIX(MAP_EXECUTABLE));
#endif
#ifdef MAP_NORESERVE
  rb_define_const(rb_cMmap, "MAP_NORESERVE", INT2FIX(MAP_NORESERVE));
#endif
#ifdef MAP_LOCKED
  rb_define_const(rb_cMmap, "MAP_LOCKED", INT2FIX(MAP_LOCKED));
#endif
#ifdef MAP_GROWSDOWN
  rb_define_const(rb_cMmap, "MAP_GROWSDOWN", INT2FIX(MAP_GROWSDOWN));
#endif
#ifdef MAP_NOSYNC
  rb_define_const(rb_cMmap, "MAP_NOSYNC", INT2FIX(MAP_NOSYNC));
#endif
#ifdef MCL_CURRENT
  rb_define_const(rb_cMmap, "MCL_CURRENT", INT2FIX(MCL_CURRENT));
  rb_define_const(rb_cMmap, "MCL_FUTURE", INT2FIX(MCL_FUTURE));
#endif

  rb_define_singleton_method(rb_cMmap, "mlockall", rb_cMmap_mlockall, 1);
  rb_define_singleton_method(rb_cMmap, "lockall", rb_cMmap_mlockall, 1);
  rb_define_singleton_method(rb_cMmap, "munlockall", rb_cMmap_munlockall, 0);
  rb_define_singleton_method(rb_cMmap, "unlockall", rb_cMmap_munlockall, 0);

  rb_define_alloc_func(rb_cMmap, rb_cMmap_allocate);
  rb_define_method(rb_cMmap, "initialize", rb_cMmap_initialize, -1);

  rb_define_method(rb_cMmap, "to_str", rb_cMmap_to_str, 0);
  rb_define_method(rb_cMmap, "hash", rb_cMmap_hash, 0);
  rb_define_method(rb_cMmap, "eql?", rb_cMmap_eql, 1);
  rb_define_method(rb_cMmap, "==", rb_cMmap_equal, 1);
  rb_define_method(rb_cMmap, "===", rb_cMmap_equal, 1);
  rb_define_method(rb_cMmap, "<=>", rb_cMmap_cmp, 1);
  rb_define_method(rb_cMmap, "casecmp", rb_cMmap_casecmp, 1);
  rb_define_method(rb_cMmap, "=~", rb_cMmap_match, 1);
  rb_define_method(rb_cMmap, "match", rb_cMmap_match_m, 1);

  rb_define_method(rb_cMmap, "size", rb_cMmap_size, 0);
  rb_define_method(rb_cMmap, "length", rb_cMmap_size, 0);
  rb_define_method(rb_cMmap, "empty?", rb_cMmap_empty, 0);

  rb_define_method(rb_cMmap, "[]", rb_cMmap_aref, -1);
  rb_define_method(rb_cMmap, "slice", rb_cMmap_aref, -1);
  rb_define_method(rb_cMmap, "[]=", rb_cMmap_aset, -1);
  rb_define_method(rb_cMmap, "slice!", rb_cMmap_slice_bang, -1);

  rb_define_method(rb_cMmap, "include?", rb_cMmap_include, 1);
  rb_define_method(rb_cMmap, "index", rb_cMmap_index, -1);
  rb_define_method(rb_cMmap, "rindex", rb_cMmap_rindex, -1);
  rb_define_method(rb_cMmap, "count", rb_cMmap_count, -1);
  rb_define_method(rb_cMmap, "sum", rb_cMmap_sum, -1);

  rb_define_method(rb_cMmap, "insert", rb_cMmap_insert, 2);
  rb_define_method(rb_cMmap, "concat", rb_cMmap_concat, 1);
  rb_define_method(rb_cMmap, "<<", rb_cMmap_concat, 1);

  rb_define_method(rb_cMmap, "sub!", rb_cMmap_sub_bang, -1);
  rb_define_method(rb_cMmap, "gsub!", rb_cMmap_gsub_bang, -1);
  rb_define_method(rb_cMmap, "upcase!", rb_cMmap_upcase_bang, 0);
  rb_define_method(rb_cMmap, "downcase!", rb_cMmap_downcase_bang, 0);
  rb_define_method(rb_cMmap, "capitalize!", rb_cMmap_capitalize_bang, 0);
  rb_define_method(rb_cMmap, "swapcase!", rb_cMmap_swapcase_bang, 0);
  rb_define_method(rb_cMmap, "reverse!", rb_cMmap_reverse_bang, 0);
  rb_define_method(rb_cMmap, "strip!", rb_cMmap_strip_bang, 0);
  rb_define_method(rb_cMmap, "chop!", rb_cMmap_chop_bang, 0);
  rb_define_method(rb_cMmap, "chomp!", rb_cMmap_chomp_bang, -1);
  // rb_define_method(rb_cMmap, "tr!", rb_cMmap_tr_bang, 2);
  // rb_define_method(rb_cMmap, "tr_s!", rb_cMmap_tr_s_bang, 2);
  rb_define_method(rb_cMmap, "delete!", rb_cMmap_delete_bang, -1);
  rb_define_method(rb_cMmap, "squeeze!", rb_cMmap_squeeze_bang, -1);

  rb_define_method(rb_cMmap, "split", rb_cMmap_split, -1);
  rb_define_method(rb_cMmap, "crypt", rb_cMmap_crypt, 1);

  rb_define_method(rb_cMmap, "mprotect", rb_cMmap_mprotect, 1);
  rb_define_method(rb_cMmap, "protect", rb_cMmap_mprotect, 1);
  rb_define_method(rb_cMmap, "madvise", rb_cMmap_madvise, 1);
  rb_define_method(rb_cMmap, "advise", rb_cMmap_madvise, 1);
  rb_define_method(rb_cMmap, "msync", rb_cMmap_msync, -1);
  rb_define_method(rb_cMmap, "sync", rb_cMmap_msync, -1);
  rb_define_method(rb_cMmap, "flush", rb_cMmap_msync, -1);
  rb_define_method(rb_cMmap, "mlock", rb_cMmap_mlock, 0);
  rb_define_method(rb_cMmap, "lock", rb_cMmap_mlock, 0);
  rb_define_method(rb_cMmap, "munlock", rb_cMmap_munlock, 0);
  rb_define_method(rb_cMmap, "unlock", rb_cMmap_munlock, 0);

  rb_define_method(rb_cMmap, "extend", rb_cMmap_extend, 1);
  rb_define_method(rb_cMmap, "unmap", rb_cMmap_unmap, 0);
  rb_define_method(rb_cMmap, "munmap", rb_cMmap_unmap, 0);

  rb_define_method(rb_cMmap, "semlock", rb_cMmap_semlock, -1);
  rb_define_method(rb_cMmap, "ipc_key", rb_cMmap_ipc_key, 0);

  rb_define_private_method(rb_cMmap, "set_length", rb_cMmap_set_length, 1);
  rb_define_private_method(rb_cMmap, "set_offset", rb_cMmap_set_offset, 1);
  rb_define_private_method(rb_cMmap, "set_increment", rb_cMmap_set_increment, 1);
  rb_define_private_method(rb_cMmap, "set_advice", rb_cMmap_set_advice, 1);
  rb_define_private_method(rb_cMmap, "set_ipc", rb_cMmap_set_ipc, 1);
}
