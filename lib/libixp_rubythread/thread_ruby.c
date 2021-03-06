/* Copyright ©2007-2010 Kris Maglione <maglione.k at Gmail>
 * See LICENSE file for license details.
 */
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <ruby.h>
#include "ixp_local.h"

static IxpThread ixp_rthread;
static char RWLock[];

/**
 * Function: ixp_rubyinit
 *
 * This function initializes libixp for use in multithreaded
 * programs embedded in the ruby interpreter. When using a pthread
 * edition of ruby, ixp_pthread_init should be used instead. When
 * using libixp in such programs, this function must be called
 * before any other libixp functions.
 *
 * This function is part of libixp_rubythread, which is part of the
 * libixp distribution, but is not built by default unless enabled
 * in config.mk.
 */
int
ixp_rubyinit(void) {
	rb_require("thread.rb");
	rb_eval_string(RWLock);
	ixp_thread = &ixp_rthread;
	return 0;
}

static char*
errbuf(void) {
	static ID key;
	volatile VALUE val;

	if(key == 0L)
		key = rb_intern("_ixp_errbuf");

	val = rb_thread_local_aref(rb_thread_current(), key);
	if(NIL_P(val)) {
		val = rb_str_new(nil, IXP_ERRMAX);
		rb_thread_local_aset(rb_thread_current(), key, val);
	}

	Check_Type(val, T_STRING);
	return RSTRING(val)->ptr;
}

static void
save(char *eval, void **place) {
	*place = (void*)rb_eval_string(eval);
	rb_gc_register_address((VALUE*)place);
}

static void
unsave(void **place) {
	rb_gc_unregister_address((VALUE*)place);
}

#define call(obj, meth, ...) rb_funcall((VALUE)obj, rb_intern(meth), __VA_ARGS__)

/* Mutex */
static int
initmutex(IxpMutex *m) {
	save("Mutex.new", &m->aux);
	return 0;
}

static void
mdestroy(IxpMutex *m) {
	unsave(&m->aux);
}

static void
mlock(IxpMutex *m) {
	call(m->aux, "lock", 0);
}

static int
mcanlock(IxpMutex *m) {
	return call(m->aux, "trylock", 0);
}

static void
munlock(IxpMutex *m) {
	call(m->aux, "unlock", 0);
}

/* RWLock */
static int
initrwlock(IxpRWLock *rw) {
	save("RWLock.new", &rw->aux);
	return 0;
}

static void
rwdestroy(IxpRWLock *rw) {
	unsave(&rw->aux);
}

static void
rlock(IxpRWLock *rw) {
	call(rw->aux, "rdlock", 0);
}

static int
canrlock(IxpRWLock *rw) {
	return call(rw->aux, "tryrdlock", 0) == Qtrue;
}

static void
wlock(IxpRWLock *rw) {
	call(rw->aux, "wrlock", 0);
}

static int
canwlock(IxpRWLock *rw) {
	return call(rw->aux, "trywrlock", 0) == Qtrue;
}

static void
rwunlock(IxpRWLock *rw) {
	call(rw->aux, "unlock", 0);
}

/* Rendez */
static int
initrendez(IxpRendez *r) {
	save("ConditionVariable.new", &r->aux);
	return 0;
}

static void
rdestroy(IxpRendez *r) {
	unsave(&r->aux);
}

static void
rsleep(IxpRendez *r) {
	call(r->aux, "wait", 1, (VALUE)r->mutex->aux);
}

static int
rwake(IxpRendez *r) {
	call(r->aux, "signal", 0);
	return 0;
}

static int
rwakeall(IxpRendez *r) {
	call(r->aux, "broadcast", 0);
	return 0;
}

/* Yielding IO */
static ssize_t
_read(int fd, void *buf, size_t size) {
	int n;

	rb_thread_wait_fd(fd);
	n = read(fd, buf, size);

	if(n < 0 && errno == EINTR)
		rb_thread_schedule();
	return n;
}

static ssize_t
_write(int fd, const void *buf, size_t size) {
	int n;

	rb_thread_fd_writable(fd);
	n = write(fd, buf, size);

	if(n < 0 && errno == EINTR)
		rb_thread_schedule();
	return n;
}

static IxpThread ixp_rthread = {
	/* Mutex */
	.initmutex = initmutex,
	.lock = mlock,
	.canlock = mcanlock,
	.unlock = munlock,
	.mdestroy = mdestroy,
	/* RWLock */
	.initrwlock = initrwlock,
	.rlock = rlock,
	.canrlock = canrlock,
	.wlock = wlock,
	.canwlock = canwlock,
	.runlock = rwunlock,
	.wunlock = rwunlock,
	.rwdestroy = rwdestroy,
	/* Rendez */
	.initrendez = initrendez,
	.sleep = rsleep,
	.wake = rwake,
	.wakeall = rwakeall,
	.rdestroy = rdestroy,
	/* Other */
	.errbuf = errbuf,
	.read = _read,
	.write = _write,
	.select = rb_thread_select,
};

static char RWLock[] =
        "class RWLock                                                                              \n"
        "       def initialize                                                                     \n"
        "               @rdqueue = []                                                              \n"
        "               @wrqueue = []                                                              \n"
        "               @wrheld = nil                                                              \n"
        "               @rdheld = []                                                               \n"
        "       end                                                                                \n"
        "                                                                                          \n"
        "       def rdlock                                                                         \n"
        "               cr = Thread.critical                                                       \n"
        "               while (Thread.critical = true; @wrheld != nil && @rwheld != Thread.current)\n"
        "                       @rdqueue.push Thread.current                                       \n"
        "                       Thread.stop                                                        \n"
        "               end                                                                        \n"
        "               @wrheld = nil                                                              \n"
        "               @rdheld.push Thread.current                                                \n"
        "                                                                                          \n"
        "               @rdqueue.each {|t| t.wakeup}                                               \n"
        "               Thread.critical = cr                                                       \n"
        "               self                                                                       \n"
        "       end                                                                                \n"
        "                                                                                          \n"
        "       def wrlock                                                                         \n"
        "               cr = Thread.critical                                                       \n"
        "               while (Thread.critical = true;                                             \n"
        "                      !@rdheld.empty? || (@wrheld != Thread.current && @wrheld != nil))   \n"
        "                       @wrqueue.push Thread.current                                       \n"
        "                       Thread.stop                                                        \n"
        "               end                                                                        \n"
        "               @wrheld = Thread.current                                                   \n"
        "               Thread.critical = cr                                                       \n"
        "               self                                                                       \n"
        "       end                                                                                \n"
        "                                                                                          \n"
        "                                                                                          \n"
        "       def tryrdlock                                                                      \n"
        "               cr = Thread.critical                                                       \n"
        "               if @wrheld == nil                                                          \n"
        "                       rdlock                                                             \n"
        "                       true                                                               \n"
        "               else                                                                       \n"
        "                       false                                                              \n"
        "               end                                                                        \n"
        "       ensure                                                                             \n"
        "               Thread.critical = cr                                                       \n"
        "       end                                                                                \n"
        "                                                                                          \n"
        "       def trywrlock                                                                      \n"
        "               cr = Thread.critical                                                       \n"
        "               if @wrheld == nil && @rdheld.empty?                                        \n"
        "                       wrlock                                                             \n"
        "                       true                                                               \n"
        "               else                                                                       \n"
        "                       false                                                              \n"
        "               end                                                                        \n"
        "       ensure                                                                             \n"
        "               Thread.critical = cr                                                       \n"
        "       end                                                                                \n"
        "                                                                                          \n"
        "       def unlock                                                                         \n"
        "               cr = Thread.critical                                                       \n"
        "               Thread.critical = true                                                     \n"
        "                                                                                          \n"
        "               if @rdheld.include?(Thread.current)                                        \n"
        "                       @rdheld.remove!(Thread.current)                                    \n"
        "                       raise if @wrheld                                                   \n"
        "               elsif @rwheld != Thread.current                                            \n"
        "                       raise                                                              \n"
        "               end                                                                        \n"
        "                                                                                          \n"
        "               @wrheld = nil                                                              \n"
        "               if !@rwqueue.empty? && @rdheld.empty?                                      \n"
        "                       @wrheld = @wrqueue.shift                                           \n"
        "               elsif !@rdqueue.empty                                                      \n"
        "                       @wrheld = @rdqueue.shift                                           \n"
        "               end                                                                        \n"
        "               @wrheld.wakeup if @wrheld                                                  \n"
        "       ensure                                                                             \n"
        "               Thread.critical = cr                                                       \n"
        "       end                                                                                \n"
        "end                                                                                       \n";

