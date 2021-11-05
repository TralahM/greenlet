/* -*- indent-tabs-mode: nil; tab-width: 4; -*- */
/* Format with:
 *  clang-format -i --style=file src/greenlet/greenlet.c
 *
 *
 * Fix missing braces with:
 *   clang-tidy src/greenlet/greenlet.c -fix -checks="readability-braces-around-statements"
*/
#include <string>
#include <algorithm>
#include <exception>


#include "greenlet_internal.hpp"
#include "greenlet_refs.hpp"
#include "greenlet_slp_switch.hpp"
#include "greenlet_thread_state.hpp"
#include "greenlet_thread_support.hpp"
#include "greenlet_greenlet.hpp"

using greenlet::ThreadState;
using greenlet::Mutex;
using greenlet::LockGuard;
using greenlet::LockInitError;
using greenlet::PyErrOccurred;
using greenlet::Require;
using greenlet::PyFatalError;
using greenlet::ExceptionState;
using greenlet::StackState;


// Helpers for reference counting.
// XXX: running the test cases for greenlet 1.1.2 under Python 3.10+pydebug
// with zope.testrunner's "report refcounts" option shows a growth of
// over 500 references when running 90 tests at a steady state (10 repeats)
// Running in verbose mode and adding objgraph to report gives us this
// info in a steady state:
//   Ran 90 tests with 0 failures, 0 errors and 1 skipped in 2.120 seconds.
// Showing growth
// tuple                 2811       +16
// list                  1733       +14
// function              6304       +11
// dict                  3604        +9
// cell                   707        +9
// greenlet                81        +8
// method                 103        +5
// Genlet                  40        +4
// list_iterator           30        +3
// getset_descriptor      916        +2
//   sum detail refcount=341678   sys refcount=379357   change=523
//     Leak details, changes in instances and refcounts by type/class:
//     type/class                                               insts   refs
//     -------------------------------------------------------  -----   ----
//     builtins.NoneType                                            0      2
//     builtins.cell                                                9     20
//     builtins.code                                                0     31
//     builtins.dict                                               18     91
//     builtins.frame                                              20     32
//     builtins.function                                           11     28
//     builtins.getset_descriptor                                   2      2
//     builtins.int                                                 2     42
//     builtins.list                                               14     37
//     builtins.list_iterator                                       3      3
//     builtins.method                                              5      5
//     builtins.method_descriptor                                   0      9
//     builtins.str                                                11     76
//     builtins.traceback                                           1      2
//     builtins.tuple                                              20     42
//     builtins.type                                                2     28
//     builtins.weakref                                             2      2
//     greenlet.GreenletExit                                        1      1
//     greenlet.greenlet                                            8     26
//     greenlet.tests.test_contextvars.NoContextVarsTests           0      1
//     greenlet.tests.test_gc.object_with_finalizer                 1      1
//     greenlet.tests.test_generator_nested.Genlet                  4     26
//     greenlet.tests.test_greenlet.convoluted                      1      2
//     -------------------------------------------------------  -----   ----
//     total                                                      135    509
//
// As of the commit that adds this comment, we're doing better than
// 1.1.2, but still not perfect:
//   Ran 115 tests with 0 failures, 0 errors, 1 skipped in 8.623 seconds.
// tuple            21310       +23
// dict              5428       +18
// frame              183       +17
// list              1760       +14
// function          6359       +11
// cell               698        +8
// method             105        +5
// int               2709        +4
// TheGenlet           40        +4
// list_iterator       30        +3
//   sum detail refcount=345051   sys refcount=383043   change=494
//     Leak details, changes in instances and refcounts by type/class:
//     type/class                                               insts   refs
//     -------------------------------------------------------  -----   ----
//     builtins.NoneType                                            0     12
//     builtins.bool                                                0      2
//     builtins.cell                                                8     16
//     builtins.code                                                0     28
//     builtins.dict                                               18     74
//     builtins.frame                                              17     28
//     builtins.function                                           11     28
//     builtins.getset_descriptor                                   2      2
//     builtins.int                                                 4     44
//     builtins.list                                               14     39
//     builtins.list_iterator                                       3      3
//     builtins.method                                              5      5
//     builtins.method_descriptor                                   0      8
//     builtins.str                                                -2     69
//     builtins.tuple                                              23     42
//     builtins.type                                                2     28
//     builtins.weakref                                             2      2
//     greenlet.greenlet                                            1      1
//     greenlet.main_greenlet                                       1     16
//     greenlet.tests.test_contextvars.NoContextVarsTests           0      1
//     greenlet.tests.test_gc.object_with_finalizer                 1      1
//     greenlet.tests.test_generator_nested.TheGenlet               4     29
//     greenlet.tests.test_greenlet.convoluted                      1      2
//     greenlet.tests.test_leaks.HasFinalizerTracksInstances        2      2
//     -------------------------------------------------------  -----   ----
//     total                                                      117    482

using greenlet::refs::BorrowedObject;
using greenlet::refs::BorrowedGreenlet;
using greenlet::refs::BorrowedMainGreenlet;
using greenlet::refs::OwnedObject;
using greenlet::refs::PyErrFetchParam;
using greenlet::refs::PyArgParseParam;
using greenlet::refs::ImmortalString;
using greenlet::refs::ImmortalObject;
using greenlet::refs::CreatedModule;
using greenlet::refs::PyErrPieces;
using greenlet::refs::PyObjectPointer;

// ******* Implementation of things from included files
template<typename T>
greenlet::refs::_BorrowedGreenlet<T>& greenlet::refs::_BorrowedGreenlet<T>::operator=(const greenlet::refs::BorrowedObject& other)
{
    if (!PyGreenlet_Check(other)) {
        throw TypeError("Expected a greenlet");
    }
    this->_set_raw_pointer(static_cast<PyObject*>(other));
    return *this;
}

// TODO: These two methods only make sense for greenlet
// objects, but there's not a good spot in the inheritance
// tree to put them without introducing VTable pointers
template<typename T>
bool PyObjectPointer<T>::active() const
{
    return PyGreenlet_ACTIVE(this->p);
}
template<typename T>
bool PyObjectPointer<T>::started() const
{
    return PyGreenlet_STARTED(this->p);
}


#include "structmember.h"

#ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wunused-parameter"
#    pragma clang diagnostic ignored "-Wmissing-field-initializers"
#    pragma clang diagnostic ignored "-Wwritable-strings"
#elif defined(__GNUC__)
#    pragma GCC diagnostic push
//  warning: ISO C++ forbids converting a string constant to ‘char*’
// (The python APIs aren't const correct and accept writable char*)
#    pragma GCC diagnostic ignored "-Wwrite-strings"
#endif


/***********************************************************

A PyGreenlet is a range of C stack addresses that must be
saved and restored in such a way that the full range of the
stack contains valid data when we switch to it.

Stack layout for a greenlet:

               |     ^^^       |
               |  older data   |
               |               |
  stack_stop . |_______________|
        .      |               |
        .      | greenlet data |
        .      |   in stack    |
        .    * |_______________| . .  _____________  stack_copy + stack_saved
        .      |               |     |             |
        .      |     data      |     |greenlet data|
        .      |   unrelated   |     |    saved    |
        .      |      to       |     |   in heap   |
 stack_start . |     this      | . . |_____________| stack_copy
               |   greenlet    |
               |               |
               |  newer data   |
               |     vvv       |


Note that a greenlet's stack data is typically partly at its correct
place in the stack, and partly saved away in the heap, but always in
the above configuration: two blocks, the more recent one in the heap
and the older one still in the stack (either block may be empty).

Greenlets are chained: each points to the previous greenlet, which is
the one that owns the data currently in the C stack above my
stack_stop.  The currently running greenlet is the first element of
this chain.  The main (initial) greenlet is the last one.  Greenlets
whose stack is entirely in the heap can be skipped from the chain.

The chain is not related to execution order, but only to the order
in which bits of C stack happen to belong to greenlets at a particular
point in time.

The main greenlet doesn't have a stack_stop: it is responsible for the
complete rest of the C stack, and we don't know where it begins.  We
use (char*) -1, the largest possible address.

States:
  stack_stop == NULL && stack_start == NULL:  did not start yet
  stack_stop != NULL && stack_start == NULL:  already finished
  stack_stop != NULL && stack_start != NULL:  active

The running greenlet's stack_start is undefined but not NULL.

 ***********************************************************/

/*** global state ***/

/* In the presence of multithreading, this is a bit tricky; see
   greenlet_thread_state.hpp for details.
*/


static inline OwnedObject
single_result(const OwnedObject& results)
{
    if (results
        && PyTuple_Check(results.borrow())
        && PyTuple_GET_SIZE(results.borrow()) == 1) {
        PyObject* result = PyTuple_GET_ITEM(results.borrow(), 0);
        return OwnedObject::owning(result);
    }
    return results;
}



class ImmortalEventName : public ImmortalString
{
private:
    G_NO_COPIES_OF_CLS(ImmortalEventName);
public:
    ImmortalEventName(const char* const str) : ImmortalString(str)
    {}
};

class ImmortalException : public ImmortalObject
{
private:
    G_NO_COPIES_OF_CLS(ImmortalException);
public:
    ImmortalException(const char* const name, PyObject* base=nullptr) :
        ImmortalObject(name
                       // Python 2.7 isn't const correct
                       ? Require(PyErr_NewException((char*)name, base, nullptr))
                       : nullptr)
    {}

    inline bool PyExceptionMatches() const
    {
        return PyErr_ExceptionMatches(this->p) > 0;
    }

};

// This encapsulates what were previously module global "constants"
// established at init time.
// This is a step towards Python3 style module state that allows
// reloading.
// We play some tricks with placement new to be able to allocate this
// object statically still, so that references to its members don't
// incur an extra pointer indirection.
class GreenletGlobals
{
public:
    const ImmortalEventName event_switch;
    const ImmortalEventName event_throw;
    const ImmortalException PyExc_GreenletError;
    const ImmortalException PyExc_GreenletExit;
    const ImmortalObject empty_tuple;
    const ImmortalObject empty_dict;
    const ImmortalString str_run;
    Mutex* const thread_states_to_destroy_lock;
    greenlet::cleanup_queue_t thread_states_to_destroy;

    GreenletGlobals(const int dummy) :
        event_switch(0),
        event_throw(0),
        PyExc_GreenletError(0),
        PyExc_GreenletExit(0),
        empty_tuple(0),
        empty_dict(0),
        str_run(0),
        thread_states_to_destroy_lock(0)
    {}

    GreenletGlobals() :
        event_switch("switch"),
        event_throw("throw"),
        PyExc_GreenletError("greenlet.error"),
        PyExc_GreenletExit("greenlet.GreenletExit", PyExc_BaseException),
        empty_tuple(Require(PyTuple_New(0))),
        empty_dict(Require(PyDict_New())),
        str_run("run"),
        thread_states_to_destroy_lock(new Mutex())
    {}

    ~GreenletGlobals()
    {
        // This object is (currently) effectively immortal, and not
        // just because of those placement new tricks; if we try to
        // deallocate the static object we allocated, and overwrote,
        // we would be doing so at C++ teardown time, which is after
        // the final Python GIL is released, and we can't use the API
        // then.
        // (The members will still be destructed, but they also don't
        // do any deallocation.)
    }

    void queue_to_destroy(ThreadState* ts) const
    {
        // we're currently accessed through a static const object,
        // implicitly marking our members as const, so code can't just
        // call push_back (or pop_back) without casting away the
        // const.
        //
        // Do that for callers.
        greenlet::cleanup_queue_t& q = const_cast<greenlet::cleanup_queue_t&>(this->thread_states_to_destroy);
        q.push_back(ts);
    }

    ThreadState* take_next_to_destroy() const
    {
        greenlet::cleanup_queue_t& q = const_cast<greenlet::cleanup_queue_t&>(this->thread_states_to_destroy);
        ThreadState* result = q.back();
        q.pop_back();
        return result;
    }
};

static const GreenletGlobals mod_globs(0);

// Protected by the GIL. Incremented when we create a main greenlet,
// in a new thread, decremented when it is destroyed.
static Py_ssize_t total_main_greenlets;

struct ThreadState_DestroyWithGIL
{
    ThreadState_DestroyWithGIL(ThreadState* state)
    {
        if (state && state->has_main_greenlet()) {
            DestroyWithGIL(state);
        }
    }

    static int
    DestroyWithGIL(ThreadState* state)
    {
        // Holding the GIL.
        // Passed a non-shared pointer to the actual thread state.
        // state -> main greenlet
        // main greenlet -> main greenlet
        assert(state->has_main_greenlet());
        PyMainGreenlet* main(state->borrow_main_greenlet());
        // When we need to do cross-thread operations, we check this.
        // A NULL value means the thread died some time ago.
        // We do this here, rather than in a Python dealloc function
        // for the greenlet, in case there's still a reference out there.
        main->thread_state = nullptr;
        delete state; // Deleting this runs the destructor, DECREFs the main greenlet.
        return 0;
    }
};

struct ThreadState_DestroyNoGIL
{
    ThreadState_DestroyNoGIL(ThreadState* state)
    {
        // We are *NOT* holding the GIL. Our thread is in the middle
        // of its death throes and the Python thread state is already
        // gone so we can't use most Python APIs. One that is safe is
        // ``Py_AddPendingCall``, unless the interpreter itself has
        // been torn down. There is a limited number of calls that can
        // be queued: 32 (NPENDINGCALLS) in CPython 3.10, so we
        // coalesce these calls using our own queue.
        if (state && state->has_main_greenlet()) {
            // mark the thread as dead ASAP.
            // this is racy! If we try to throw or switch to a
            // greenlet from this thread from some other thread before
            // we clear the state pointer, it won't realize the state
            // is dead which can crash the process.
            assert(state->borrow_main_greenlet()->thread_state == state
                   || state->borrow_main_greenlet()->thread_state == nullptr);
            state->borrow_main_greenlet()->thread_state = nullptr;
        }

        // NOTE: Because we're not holding the GIL here, some other
        // Python thread could run and call ``os.fork()``, which would
        // be bad if that happenend while we are holding the cleanup
        // lock (it wouldn't function in the child process).
        // Make a best effort to try to keep the duration we hold the
        // lock short.
        // TODO: On platforms that support it, use ``pthread_atfork`` to
        // drop this lock.
        LockGuard cleanup_lock(*mod_globs.thread_states_to_destroy_lock);

        if (state && state->has_main_greenlet()) {
            // Because we don't have the GIL, this is a race condition.
            if (!PyInterpreterState_Head()) {
                // We have to leak the thread state, if the
                // interpreter has shut down when we're getting
                // deallocated, we can't run the cleanup code that
                // deleting it would imply.
                return;
            }

            mod_globs.queue_to_destroy(state);
            if (mod_globs.thread_states_to_destroy.size() == 1) {
                // We added the first item to the queue. We need to schedule
                // the cleanup.
                int result = Py_AddPendingCall(ThreadState_DestroyNoGIL::DestroyQueueWithGIL,
                                               NULL);
                if (result < 0) {
                    // Hmm, what can we do here?
                    fprintf(stderr,
                            "greenlet: WARNING: failed in call to Py_AddPendingCall; "
                            "expect a memory leak.\n");
                }
            }
        }
    }

    static int
    DestroyQueueWithGIL(void* arg)
    {
        // We're holding the GIL here, so no Python code should be able to
        // run to call ``os.fork()``.
        while (1) {
            ThreadState* to_destroy;
            {
                LockGuard cleanup_lock(*mod_globs.thread_states_to_destroy_lock);
                if (mod_globs.thread_states_to_destroy.empty()) {
                    break;
                }
                to_destroy = mod_globs.take_next_to_destroy();
            }
            // Drop the lock while we do the actual deletion.
            ThreadState_DestroyWithGIL::DestroyWithGIL(to_destroy);
        }
        return 0;
    }

};

// The intent when GET_THREAD_STATE() is used multiple times in a function is to
// take a reference to it in a local variable, to avoid the
// thread-local indirection. On some platforms (macOS),
// accessing a thread-local involves a function call (plus an initial
// function call in each function that uses a thread local); in
// contrast, static volatile variables are at some pre-computed offset.

#if G_USE_STANDARD_THREADING == 1
typedef greenlet::ThreadStateCreator<ThreadState_DestroyNoGIL> ThreadStateCreator;
static G_THREAD_LOCAL_VAR ThreadStateCreator g_thread_state_global;
#define GET_THREAD_STATE() g_thread_state_global
#else
// if we're not using standard threading, we're using
// the Python thread-local dictionary to perform our cleanup,
// which means we're deallocated when holding the GIL. The
// thread state is valid enough still for us to destroy
// stuff.
typedef greenlet::ThreadStateCreator<ThreadState_DestroyWithGIL> ThreadStateCreator;
#define G_THREAD_STATE_DICT_CLEANUP_TYPE
#include "greenlet_thread_state_dict_cleanup.hpp"
typedef greenlet::refs::OwnedReference<PyGreenletCleanup> OwnedGreenletCleanup;
// RECALL: legacy thread-local objects (__thread on GCC, __declspec(thread) on
// MSVC) can't have constructors or destructors, they have to be
// constant. So we indirect through a pointer and a function.
static G_THREAD_LOCAL_VAR ThreadStateCreator* _g_thread_state_global_ptr = nullptr;
static ThreadStateCreator& GET_THREAD_STATE()
{
    if (!_g_thread_state_global_ptr) {
        // NOTE: If any of this fails, we'll probably go on to hard
        // crash the process, because we're returning a reference to a
        // null pointer. we've called Py_FatalError(), but have no way
        // to commuticate that to the caller. Since these should
        // essentially never fail unless the entire process is borked,
        // a hard crash with a decent C++ backtrace from the exception
        // is much more useful.
        _g_thread_state_global_ptr = new ThreadStateCreator();
        if (!_g_thread_state_global_ptr) {
            throw PyFatalError("greenlet: Failed to create greenlet thread state.");
        }

        OwnedGreenletCleanup cleanup(OwnedGreenletCleanup::consuming(PyType_GenericAlloc(&PyGreenletCleanup_Type, 0)));
        if (!cleanup) {
            throw PyFatalError("greenlet: Failed to create greenlet thread state cleanup.");
        }

        cleanup->thread_state_creator = _g_thread_state_global_ptr;
        assert(PyObject_GC_IsTracked(cleanup.borrow_o()));

        PyObject* ts_dict_w = PyThreadState_GetDict();
        if (!ts_dict_w) {
            throw PyFatalError("greenlet: Failed to get Python thread state.");
        }
        if (PyDict_SetItemString(ts_dict_w, "__greenlet_cleanup", cleanup.borrow_o()) < 0) {
            throw PyFatalError("greenlet: Failed to save cleanup key in Python thread state.");
        }
    }
    return *_g_thread_state_global_ptr;
}
#endif


static PyMainGreenlet*
green_create_main(void)
{
    PyMainGreenlet* gmain;

    /* create the main greenlet for this thread */
    gmain = (PyMainGreenlet*)PyType_GenericAlloc(&PyMainGreenlet_Type, 0);
    if (gmain == NULL) {
        Py_FatalError("green_create_main failed to alloc");
        return NULL;
    }
    // XXX: Temporarily manually calling C++ constructors.
    new((void*)&gmain->super.python_state) PythonState;
    gmain->super.stack_state = StackState::make_main();

    // circular reference; the pending call will clean this up.
    gmain->super.main_greenlet_s = gmain;
    Py_INCREF(gmain);
    assert(Py_REFCNT(gmain) == 2);
    total_main_greenlets++;
    return gmain;
}


static BorrowedMainGreenlet
find_and_borrow_main_greenlet_in_lineage(const PyObjectPointer<PyGreenlet>& start)
{
    PyGreenlet* g(start.borrow());
    while (!PyGreenlet_STARTED(g)) {
        g = g->parent;
        if (g == NULL) {
            /* garbage collected greenlet in chain */
            // XXX: WHAT?
            return BorrowedMainGreenlet(nullptr);
        }
    }
    // XXX: What about the actual main greenlet?
    // This is never actually called with a main greenlet, so it
    // doesn't matter.
    return BorrowedMainGreenlet(g->main_greenlet_s);
}

/***********************************************************/

/* Some functions must not be inlined:
   * slp_restore_state, when inlined into slp_switch might cause
     it to restore stack over its own local variables
   * slp_save_state, when inlined would add its own local
     variables to the saved stack, wasting space
   * slp_switch, cannot be inlined for obvious reasons
   * g_initialstub, when inlined would receive a pointer into its
     own stack frame, leading to incomplete stack save/restore

g_initialstub is a member function and declared virtual so that the
compiler always calls it through a vtable.

slp_save_state and slp_restore_state are also member functions. They
are called from trampoline functions that themselves are declared as
not eligible for inlining.
*/



/* add forward declarations */


static void
g_calltrace(const OwnedObject& tracefunc,
            const ImmortalEventName& event,
            const BorrowedGreenlet& origin,
            const BorrowedGreenlet& target);

static OwnedObject
g_handle_exit(const OwnedObject& greenlet_result, PyGreenlet* dead);



template<typename T>
std::ostream& operator<<(std::ostream& os, const PyObjectPointer<T>& s)
{
    os << s.as_str();
    return os;
}

class SwitchingArgs
{
private:
    G_NO_ASSIGNMENT_OF_CLS(SwitchingArgs);
    // If args and kwargs are both false (NULL), this is a *throw*, not a
    // switch. PyErr_... must have been called already.
    OwnedObject _args;
    OwnedObject _kwargs;
public:

    SwitchingArgs()
    {}

    SwitchingArgs(const OwnedObject& args, const OwnedObject& kwargs)
        : _args(args),
          _kwargs(kwargs)
    {}

    SwitchingArgs(const SwitchingArgs& other)
        : _args(other._args),
          _kwargs(other._kwargs)
    {}

    OwnedObject& args()
    {
        return this->_args;
    }

    OwnedObject& kwargs()
    {
        return this->_kwargs;
    }

    /**
     * Moves ownership from the argument to this object.
     */
    SwitchingArgs& operator<<=(SwitchingArgs& other)
    {
        if (this != &other) {
            this->_args = other._args;
            this->_kwargs = other._kwargs;
            other.CLEAR();
        }
        return *this;
    }

    /**
     * Acquires ownership of the argument (consumes the reference).
     */
    SwitchingArgs& operator<<=(PyObject* args)
    {
        this->_args = OwnedObject::consuming(args);
        this->_kwargs.CLEAR();
        return *this;
    }

    /**
     * Acquires ownership of the argument.
     *
     * Sets the args to be the given value; clears the kwargs.
     */
    SwitchingArgs& operator<<=(OwnedObject& args)
    {
        assert(&args != &this->_args);
        this->_args = args;
        this->_kwargs.CLEAR();
        args.CLEAR();

        return *this;
    }

    G_EXPLICIT_OP operator bool() const G_NOEXCEPT
    {
        return this->_args || this->_kwargs;
    }

    inline void CLEAR()
    {
        this->_args.CLEAR();
        this->_kwargs.CLEAR();
    }
};

/**
 * CAUTION: May invoke arbitrary Python code.
 *
 * Figure out what the result of ``greenlet.switch(arg, kwargs)``
 * should be and transfers ownerhsip of it to the left-hand-side.
 *
 * If switch() was just passed an arg tuple, then we'll just return that.
 * If only keyword arguments were passed, then we'll pass the keyword
 * argument dict. Otherwise, we'll create a tuple of (args, kwargs) and
 * return both.
 */
OwnedObject& operator<<=(OwnedObject& lhs, SwitchingArgs& rhs) G_NOEXCEPT
{
    // Because this may invoke arbitrary Python code, which could
    // result in switching back to us, we need to get the
    // arguments locally on the stack.
    assert(rhs);
    OwnedObject args = rhs.args();
    OwnedObject kwargs = rhs.kwargs();
    rhs.CLEAR();
    // We shouldn't be called twice for the same switch.
    assert(args || kwargs);
    assert(!rhs);

    if (!kwargs) {
        lhs = args;
    }
    else if (!PyDict_Size(kwargs.borrow())) {
        lhs = args;
    }
    else if (!PySequence_Length(args.borrow())) {
        lhs = kwargs;
    }
    else {
        lhs = OwnedObject::consuming(PyTuple_Pack(2, args.borrow(), kwargs.borrow()));
    }
    return lhs;
}

class SwitchingState {
private:
    static greenlet::PythonAllocator<SwitchingState> allocator;
    // We are owned by a greenlet that serves as the target;
    // we live as long as it does and so don't need to own it.
    // TODO: When we make the greenlet object opaque, we should find a
    // way to get rid of this.
    // We'll make the greenlet opaque by making it just point to this class.
    const BorrowedGreenlet target;
    SwitchingArgs switch_args;
    ThreadState& thread_state;

    // Also TODO: Switch away from integer error codes and to enums,
    // or throw exceptions when possible.
    struct switchstack_result_t
    {
        int status;
        SwitchingState* the_state_that_switched;
        OwnedGreenlet origin_greenlet;

        switchstack_result_t()
            : status(0),
              the_state_that_switched(nullptr)
        {}

        switchstack_result_t(int err)
            : status(err),
              the_state_that_switched(nullptr)
        {}

        switchstack_result_t(int err, SwitchingState* state, OwnedGreenlet& origin)
            : status(err),
              the_state_that_switched(state),
              origin_greenlet(origin)
        {
        }

        switchstack_result_t(int err, SwitchingState* state, const BorrowedGreenlet& origin)
            : status(err),
              the_state_that_switched(state),
              origin_greenlet(origin)
        {
        }

        switchstack_result_t& operator=(const switchstack_result_t& other)
        {
            this->status = other.status;
            this->the_state_that_switched = other.the_state_that_switched;
            this->origin_greenlet = other.origin_greenlet;
            return *this;
        }
    };

    class GreenletStartedWhileInPython : public std::runtime_error
    {
    public:
        GreenletStartedWhileInPython() : std::runtime_error("")
        {}
    };

    void release_args()
    {
        this->switch_args.CLEAR();
    }
    G_NO_COPIES_OF_CLS(SwitchingState);

public:

    static void* operator new(size_t count)
    {
        UNUSED(count);
        assert(count == sizeof(SwitchingState));
        return allocator.allocate(1);
    }

    static void operator delete(void* ptr)
    {
        return allocator.deallocate(static_cast<SwitchingState*>(ptr),
                                    1);
    }

    SwitchingState(const BorrowedGreenlet& target) :
        target(target),
        thread_state(GET_THREAD_STATE().state())
    {

    }

    SwitchingArgs& args()
    {
        return this->switch_args;
    }

    OwnedObject kill()
    {
        // If we're killed because we lost all references in the
        // middle of a switch, that's ok. Don't reset the args/kwargs,
        // we still want to pass them to the parent.
        PyErr_SetString(mod_globs.PyExc_GreenletExit,
                        "Killing the greenlet because all references have vanished.");
        // To get here it had to have run before
        return this->g_switch();
    }

    inline const BorrowedGreenlet& get_target() const
    {
        return this->target;
    }

    inline void slp_restore_state() G_NOEXCEPT
    {
#ifdef SLP_BEFORE_RESTORE_STATE
        SLP_BEFORE_RESTORE_STATE();
#endif
        this->target->stack_state.copy_heap_to_stack(
              this->thread_state.borrow_current()->stack_state);
    }

    inline int slp_save_state(char *const stackref) G_NOEXCEPT
    {
        // XXX: This used to happen in the middle, before saving, but
        // after finding the next owner. Does that matter? This is
        // only defined for Sparc/GCC where it flushes register
        // windows to the stack (I think)
#ifdef SLP_BEFORE_SAVE_STATE
        SLP_BEFORE_SAVE_STATE();
#endif
        return this->target->stack_state.copy_stack_to_heap(stackref, this->thread_state.borrow_current()->stack_state);
    }

    OwnedObject g_switch()
    {
        try {
            this->check_switch_allowed();
        }
        catch(const PyErrOccurred&) {
            this->release_args();
            throw;
        }

        // Switching greenlets used to attempt to clean out ones that need
        // deleted *if* we detected a thread switch. Should it still do
        // that?
        // An issue is that if we delete a greenlet from another thread,
        // it gets queued to this thread, and ``kill_greenlet()`` switches
        // back into the greenlet

#ifndef NDEBUG
        const BorrowedGreenlet origin(this->thread_state.borrow_current());
#endif

        /* find the real target by ignoring dead greenlets,
           and if necessary starting a greenlet. */
        switchstack_result_t err;
        PyGreenlet* target(this->target);
        // TODO: probably cleaner to handle the case where we do
        // switch to ourself separately from the other cases.
        // This can probably even further be simplified if we keep
        // track of the switching_state we're going for and just call
        // into g_switch() if it's not ourself.
        bool target_was_me = true;
        while (target) {

            if (PyGreenlet_ACTIVE(target)) {
                if (!target_was_me) {
                    target->switching_state->switch_args <<= this->switch_args;
                    assert(!this->switch_args);
                }
                err = target->switching_state->g_switchstack();
                break;
            }
            if (!PyGreenlet_STARTED(target)) {
                void* dummymarker;
                if (!target_was_me) {
                    // XXX: See green_switch. This allocation will go away.
                    if (!target->switching_state) {
                        target->switching_state = new SwitchingState(target);
                    }
                    target->switching_state->switch_args <<= this->switch_args;
                    assert(!this->switch_args);
                }

                try {
                    // This can only throw back to us while we're
                    // still in this greenlet. Once the new greenlet
                    // is bootstrapped, it has its own exception state.
                    err = target->switching_state->g_initialstub(&dummymarker);
                }
                catch (const PyErrOccurred&) {
                    this->release_args();
                    throw;
                }
                catch (const GreenletStartedWhileInPython&) {
                    // The greenlet was started sometime before this
                    // greenlet actually switched to it, i.e.,
                    // "concurrent" calls to switch() or throw().
                    // We need to retry the switch.
                    // Note that the current greenlet has been reset
                    // to this one (or we wouldn't be running!)
                    assert(this->thread_state.borrow_current() == origin);
                    continue;
                }
                break;
            }

            target = target->parent;
            target_was_me = false;
        }
        // The this pointer and all other stack or register based
        // variables are invalid now, at least where things succeed
        // above.
        // But this one, probably not so much? It's not clear if it's
        // safe to throw an exception at this point.

        if (err.status < 0) {
            // XXX: This code path is untested.
            assert(PyErr_Occurred());
            assert(!err.the_state_that_switched);
            assert(!err.origin_greenlet);
            return OwnedObject();
        }

        return err.the_state_that_switched->g_switch_finish(err);
    }

    virtual ~SwitchingState()
    {
    }

protected:
    // The functions that must not be inlined are declared virtual.
    // We also mark them as protected, not private, so that the
    // compiler is forced to call them through a function pointer.
    // (A sufficiently smart compiler could directly call a private
    // virtual function since it can never be overridden in a
    // subclass).

    // Returns the previous greenlet we just switched away from.
    virtual OwnedGreenlet g_switchstack_success() G_NOEXCEPT
    {
        PyThreadState* tstate = PyThreadState_GET();

        this->target->python_state >> tstate;
        this->target->exception_state >> tstate;

        // The thread state hasn't been changed yet.
        OwnedGreenlet result(thread_state.get_current());
        thread_state.set_current(this->target);
        assert(thread_state.borrow_current() == this->target);
        return result;
    }

    virtual switchstack_result_t g_initialstub(void* mark)
    {
        OwnedObject run;
        ThreadState& state = thread_state;
        const BorrowedGreenlet& self(this->target);

        // We need to grab a reference to the current switch arguments
        // in case we're entered concurrently during the call to
        // GetAttr() and have to try again.
        // We'll restore them when we return in that case.
        // Scope them tightly to avoid ref leaks.
        {
            SwitchingArgs args(this->switch_args);

            /* save exception in case getattr clears it */
            PyErrPieces saved;

            /*
              self.run is the object to call in the new greenlet.
              This could run arbitrary python code and switch greenlets!
            */
            run = self.PyRequireAttr(mod_globs.str_run);


            /* restore saved exception */
            saved.PyErrRestore();


            /* recheck that it's safe to switch in case greenlet reparented anywhere above */
            this->check_switch_allowed();

            /* by the time we got here another start could happen elsewhere,
             * that means it should now be a regular switch.
             * This can happen if the Python code is a subclass that implements
             * __getattribute__ or __getattr__, or makes ``run`` a descriptor;
             * all of those can run arbitrary code that switches back into
             * this greenlet.
             */
            if (self.started()) {
                // the successful switch cleared these out, we need to
                // restore our version.
                assert(!this->switch_args);
                this->switch_args <<= args;

                throw GreenletStartedWhileInPython();
            }
        }

        // Sweet, if we got here, we have the go-ahead and will switch
        // greenlets.
        // Nothing we do from here on out should allow for a thread or
        // greenlet switch: No arbitrary calls to Python, including
        // decref'ing

#if GREENLET_USE_CFRAME
        /* OK, we need it, we're about to switch greenlets, save the state. */
        /*
          See green_new(). This is a stack-allocated variable used
          while *self* is in PyObject_Call().
          We want to defer copying the state info until we're sure
          we need it and are in a stable place to do so.
        */
        CFrame trace_info;

        self->python_state.set_new_cframe(trace_info);
#endif
        /* start the greenlet */
        self->stack_state = StackState(mark, state.borrow_current()->stack_state);
        self->python_state.set_initial_state(PyThreadState_GET());
        self->exception_state.clear();

        /* perform the initial switch */
        switchstack_result_t err = this->g_switchstack();
        /* returns twice!
           The 1st time with ``err == 1``: we are in the new greenlet.
           This one owns a greenlet that used to be current.
           The 2nd time with ``err <= 0``: back in the caller's
           greenlet; this happens if the child finishes or switches
           explicitly to us. Either way, the ``err`` variable is
           created twice at the same memory location, but possibly
           having different ``origin`` values. Note that it's not
           constructed for the second time until the switch actually happens.`
        */
        if (err.status == 1) {
            // This never returns!
            this->inner_bootstrap(err.origin_greenlet, run);
        }
        // The child will take care of decrefing this.
        run.relinquish_ownership();
        // In contrast, notice that we're keeping the origin greenlet
        // around as an owned reference; we need it to call the trace
        // function for the switch back into the parent. It was only
        // captured at the time the switch actually happened, though,
        // so we haven't been keeping an extra reference around this
        // whole time.

        /* back in the parent */
        if (err.status < 0) {
            /* start failed badly, restore greenlet state */
            // XXX: This code path is not tested.
            self->stack_state = StackState();
        }
        return err;
    }

private:

    void inner_bootstrap(OwnedGreenlet& origin_greenlet, OwnedObject& run) G_NOEXCEPT
    {
        // The arguments here would be another great place for move.
        // As it is, we take them as a reference so that when we clear
        // them we clear what's on the stack above us.

        /* in the new greenlet */
        ThreadState& state = thread_state;
        const BorrowedGreenlet& self(this->target);


        assert(this->thread_state.borrow_current() == this->target);
        // C++ exceptions cannot propagate to the parent greenlet from
        // here. (TODO: Do we need a catch(...) clause, perhaps on the
        // function itself? ALl we could do is terminate the program.)
        // NOTE: On 32-bit Windows, the call chain is extremely
        // important here in ways that are subtle, having to do with
        // the depth of the SEH list. The call to restore it MUST NOT
        // add a new SEH handler to the list, or we'll restore it to
        // the wrong thing.
        this->thread_state.restore_exception_state();
        /* stack variables from above are no good and also will not unwind! */
        // EXCEPT: That can't be true, we access run, among others, here.

        self->stack_state.set_active(); /* running */

        // XXX: We could clear this much earlier, right?
        // Or would that introduce the possibility of running Python
        // code when we don't want to?
        Py_CLEAR(self->run_callable);
        assert(!self->main_greenlet_s);
        self->main_greenlet_s = state.get_main_greenlet().acquire();
        assert(self->main_greenlet_s);


        // We're about to possibly run Python code again, which
        // could switch back to us, so we need to grab the
        // arguments locally.
        SwitchingArgs args;
        args <<= this->switch_args;
        assert(!this->switch_args);

        // The first switch we need to manually call the trace
        // function here instead of in g_switch_finish, because we
        // never return there.

        if (OwnedObject tracefunc = state.get_tracefunc()) {
            try {
                g_calltrace(tracefunc,
                            args ? mod_globs.event_switch : mod_globs.event_throw,
                            origin_greenlet,
                            self);
            }
            catch (const PyErrOccurred&) {
                /* Turn trace errors into switch throws */
                args.CLEAR();
            }
        }

        // We no longer need the origin, it was only here for
        // tracing.
        // We may never actually exit this stack frame so we need
        // to explicitly clear it.
        // This could run Python code and switch.
        origin_greenlet.CLEAR();

        OwnedObject result;
        if (!args) {
            /* pending exception */
            result = NULL;
        }
        else {
            /* call g.run(*args, **kwargs) */
            // This could result in further switches
            result = run.PyCall(args.args(), args.kwargs());
        }
        args.CLEAR();
        run.CLEAR();

        if (!result
            && mod_globs.PyExc_GreenletExit.PyExceptionMatches()
            && (this->switch_args)) {
            // This can happen, for example, if our only reference
            // goes away after we switch back to the parent.
            // See test_dealloc_switch_args_not_lost
            PyErrPieces clear_error;
            result <<= this->switch_args;
            result = single_result(result);
        }
        this->release_args();

        result = g_handle_exit(result, this->target.borrow());
        assert(this->thread_state.borrow_current() == this->target);
        /* jump back to parent */
        self->stack_state.set_inactive(); /* dead */
        for (PyGreenlet* parent = self->parent; parent != NULL; parent = parent->parent) {
            // We need to somewhere consume a reference to
            // the result; in most cases we'll never have control
            // back in this stack frame again. Calling
            // green_switch actually adds another reference!
            // This would probably be clearer with a specific API
            // to hand results to the parent.
            if (!parent->switching_state) {
                // XXX: See green_switch
                parent->switching_state = new SwitchingState(parent);
            }
            parent->switching_state->args() <<= result;
            assert(!result);
            // The parent greenlet now owns the result; in the
            // typical case we'll never get back here to assign to
            // result and thus release the reference.
            try {
                result = parent->switching_state->g_switch();
            }
            catch (const PyErrOccurred&) {
                // Ignore.
            }

            /* Return here means switch to parent failed,
             * in which case we throw *current* exception
             * to the next parent in chain.
             */
            assert(!result);
        }
        /* We ran out of parents, cannot continue */
        PyErr_WriteUnraisable(self.borrow_o());
        Py_FatalError("greenlet: ran out of parent greenlets while propagating exception; "
                      "cannot continue");
    }


    /**
       Perform a stack switch into this greenlet.

       This temporarily sets the global variable
       ``switching_thread_state`` to this greenlet; as soon as the
       call to ``slp_switch`` completes, this is reset to NULL.
       Consequently, this depends on the GIL.

       TODO: Adopt the stackman model and pass ``slp_switch`` a
       callback function and context pointer; this eliminates the need
       for global variables altogether.

       Because the stack switch happens in this function, this function
       can't use its own stack (local) variables, set before the switch,
       and then accessed after the switch.

       Further, you con't even access ``g_thread_state_global`` before
       and after the switch from the global variable. Because it is
       thread local some compilers cache it in a register/on the
       stack, notably new versions of MSVC; this breaks with strange
       crashes sometime later, because writing to anything in
       ``g_thread_state_global`` after the switch is actually writing
       to random memory. For this reason, we call a non-inlined
       function to finish the operation. (XXX: The ``/GT`` MSVC
       compiler argument probably fixes that.)

       It is very important that stack switch is 'atomic', i.e. no
       calls into other Python code allowed (except very few that
       are safe), because global variables are very fragile. (This should
       no longer be the case with thread-local variables.)

    */
    switchstack_result_t g_switchstack(void)
    {
        { /* save state */
            PyGreenlet* current(thread_state.borrow_current());
            if (current == target) {
                // Hmm, nothing to do.
                // TODO: Does this bypass trace events that are
                // important?
                return switchstack_result_t(0,
                                            this, thread_state.borrow_current());
            }
            PyThreadState* tstate = PyThreadState_GET();
            current->python_state << tstate;
            current->exception_state << tstate;
            this->target->python_state.will_switch_from(tstate);
            switching_thread_state = this->target;
        }
        // If this is the first switch into a greenlet, this will
        // return twice, once with 1 in the new greenlet, once with 0
        // in the origin.
        int err = slp_switch();

        if (err < 0) { /* error */
            // XXX: This code path is not tested.
            BorrowedGreenlet current(GET_THREAD_STATE().borrow_current());
            //current->top_frame = NULL; // This probably leaks?
            current->exception_state.clear();

            switching_thread_state = NULL;
            //GET_THREAD_STATE().state().wref_target(NULL);
            this->release_args();
            // It's important to make sure not to actually return an
            // owned greenlet here, no telling how long before it
            // could be cleaned up.
            // TODO: Can this be a throw? How stable is the stack in
            // an error case like this?
            return switchstack_result_t(err);
        }

        // No stack-based variables are valid anymore.

        // But the global is volatile so we can reload it without the
        // compiler caching it from earlier.
        SwitchingState* after_switch = switching_thread_state->switching_state;
        OwnedGreenlet origin = after_switch->g_switchstack_success();
        switching_thread_state = NULL;
        return switchstack_result_t(err, after_switch, origin);
    }

    // Check the preconditions for switching to this greenlet; if they
    // aren't met, throws PyErrOccurred. Most callers will want to
    // catch this and clear the arguments
    inline void check_switch_allowed() const
    {
        // TODO: Make this take a parameter of the current greenlet,
        // or current main greenlet, to make the check for
        // cross-thread switching cheaper. Surely somewhere up the
        // call stack we've already accessed the thread local variable.

        // We expect to always have a main greenlet now; accessing the thread state
        // created it. However, if we get here and cleanup has already
        // begun because we're a greenlet that was running in a
        // (now dead) thread, these invariants will not hold true. In
        // fact, accessing `this->thread_state` may not even be possible.

        // If the thread this greenlet was running in is dead,
        // we'll still have a reference to a main greenlet, but the
        // thread state pointer we have is bogus.
        // TODO: Give the objects an API to determine if they belong
        // to a dead thread.

        const BorrowedMainGreenlet main_greenlet = find_and_borrow_main_greenlet_in_lineage(target);

        if (!main_greenlet) {
            throw PyErrOccurred(mod_globs.PyExc_GreenletError,
                                "cannot switch to a garbage collected greenlet");
        }
        else if (!main_greenlet->thread_state) {
            throw PyErrOccurred(mod_globs.PyExc_GreenletError,
                                "cannot switch to a different thread (which happens to have exited)");
        }
        // The main greenlet we found was from the .parent lineage.
        // That may or may not have any relationship to the main
        // greenlet of the running thread. We can't actually access
        // our this->thread_state members to try to check that,
        // because it could be in the process of getting destroyed,
        // but setting the main_greenlet->thread_state member to NULL
        // may not be visible yet. So we need to check against the
        // current thread state (once the cheaper checks are out of
        // the way)
        else if (main_greenlet->thread_state != &this->thread_state
                 || GET_THREAD_STATE().state().borrow_main_greenlet()->thread_state != &this->thread_state
                 ) {
            throw PyErrOccurred(mod_globs.PyExc_GreenletError,
                                "cannot switch to a different thread");
        }
    }

    OwnedObject g_switch_finish(const switchstack_result_t& err)
    {

        const ThreadState& state = thread_state;
        try {
            // Our only caller handles the bad error case
            assert(err.status >= 0);
            assert(state.borrow_current() == target);

            if (OwnedObject tracefunc = state.get_tracefunc()) {
                g_calltrace(tracefunc,
                            this->args() ? mod_globs.event_switch : mod_globs.event_throw,
                            err.origin_greenlet,
                            this->target);
            }
            // The above could have invoked arbitrary Python code, but
            // it couldn't switch back to this object and *also*
            // throw an exception, so the args won't have changed.

            if (PyErr_Occurred()) {
                // We get here if we fell of the end of the run() function
                // raising an exception. The switch itself was
                // successful, but the function raised.
                throw PyErrOccurred();
            }

            OwnedObject result;
            result <<= this->switch_args;
            assert(!this->switch_args);
            return result;
        }
        catch (const PyErrOccurred&) {
            /* Turn switch errors into switch throws */
            /* Turn trace errors into switch throws */
            this->release_args();
            throw;
        }
    }

};

greenlet::PythonAllocator<SwitchingState> SwitchingState::allocator;


extern "C" {
static int GREENLET_NOINLINE(slp_save_state_trampoline)(char* stackref)
{
    return switching_thread_state->switching_state->slp_save_state(stackref);
}
static void GREENLET_NOINLINE(slp_restore_state_trampoline)()
{
    switching_thread_state->switching_state->slp_restore_state();
}
}



/***********************************************************/

class TracingGuard
{
private:
    PyThreadState* tstate;
public:
    TracingGuard()
        : tstate(PyThreadState_GET())
    {
        PyThreadState_EnterTracing(this->tstate);
    }

    ~TracingGuard()
    {
        PyThreadState_LeaveTracing(this->tstate);
        this->tstate = nullptr;
    }

    inline void CallTraceFunction(const OwnedObject& tracefunc,
                                  const ImmortalEventName& event,
                                  const BorrowedGreenlet& origin,
                                  const BorrowedGreenlet& target)
    {
        // TODO: This calls tracefunc(event, (origin, target)). Add a shortcut
        // function for that that's specialized to avoid the Py_BuildValue
        // string parsing, or start with just using "ON" format with PyTuple_Pack(2,
        // origin, target). That seems like what the N format is meant
        // for.
        // XXX: Why does event not automatically cast back to a PyObject?
        // It tries to call the "deleted constructor ImmortalEventName
        // const" instead.
        assert(tracefunc);
        assert(event);
        assert(origin);
        assert(target);
        NewReference retval(PyObject_CallFunction(tracefunc.borrow(),
                                             "O(OO)",
                                             event.borrow(),
                                             origin.borrow(),
                                             target.borrow()));
        if (!retval) {
            throw PyErrOccurred();
        }
    }
};

static void
g_calltrace(const OwnedObject& tracefunc,
            const ImmortalEventName& event,
            const BorrowedGreenlet& origin,
            const BorrowedGreenlet& target)
{
    PyErrPieces saved_exc;
    try {
        TracingGuard tracing_guard;
        tracing_guard.CallTraceFunction(tracefunc, event, origin, target);
    }
    catch (const PyErrOccurred&) {
        // In case of exceptions trace function is removed,
        // and any existing exception is replaced with the tracing
        // exception.
        GET_THREAD_STATE().state().set_tracefunc(Py_None);
        throw;
    }

    saved_exc.PyErrRestore();
}



static OwnedObject
g_handle_exit(const OwnedObject& greenlet_result, PyGreenlet* dead)
{
    if (!greenlet_result && mod_globs.PyExc_GreenletExit.PyExceptionMatches()) {
        /* catch and ignore GreenletExit */
        PyErrFetchParam val;
        PyErr_Fetch(PyErrFetchParam(), val, PyErrFetchParam());
        if (!val) {
            return OwnedObject::None();
        }
        return OwnedObject(val);
    }

    if (greenlet_result) {
        // package the result into a 1-tuple
        // PyTuple_Pack increments the reference of its arguments,
        // so we always need to decref the greenlet result;
        // the owner will do that.
        return OwnedObject::consuming(PyTuple_Pack(1, greenlet_result.borrow()));
    }

    return OwnedObject();
}



/***********************************************************/

static PyGreenlet*
green_new(PyTypeObject* type, PyObject* args, PyObject* kwds)
{
    UNUSED(args);
    UNUSED(kwds);
    PyGreenlet* o =
        (PyGreenlet*)PyBaseObject_Type.tp_new(type, mod_globs.empty_tuple, mod_globs.empty_dict);
    if (o != NULL) {
        o->parent = GET_THREAD_STATE().state().get_current().relinquish_ownership();

        // XXX: Temporary. Manually call the C++ constructors of
        // contained objects.
        new((void*)&o->python_state) PythonState;
        new((void*)&o->stack_state) StackState;
    }
    return o;
}

static int
green_setrun(BorrowedGreenlet self, BorrowedObject nrun, void* c);
static int
green_setparent(BorrowedGreenlet self, BorrowedObject nparent, void* c);

static int
green_init(BorrowedGreenlet self, BorrowedObject args, BorrowedObject kwargs)
{
    PyArgParseParam run;
    PyArgParseParam nparent;
    static const char* const kwlist[] = {
        "run",
        "parent",
        NULL
    };

    // recall: The O specifier does NOT increase the reference count.
    if (!PyArg_ParseTupleAndKeywords(
             args, kwargs, "|OO:green", (char**)kwlist, &run, &nparent)) {
        return -1;
    }

    if (run) {
        if (green_setrun(self, run, NULL)) {
            return -1;
        }
    }
    if (nparent && !nparent.is_None()) {
        return green_setparent(self, nparent, NULL);
    }
    return 0;
}

static void
kill_greenlet(BorrowedGreenlet& self)
{
    /* Cannot raise an exception to kill the greenlet if
       it is not running in the same thread! */
    if (self->main_greenlet_s == GET_THREAD_STATE().borrow_main_greenlet()) {
        /* The dying greenlet cannot be a parent of ts_current
           because the 'parent' field chain would hold a
           reference */

        // XXX: should not be needed here, right? Plus, this causes recursion.
        // if (!STATE_OK) {
        //     return -1;
        // }
        BorrowedGreenlet oldparent(self->parent);
        // XXX: Temporary monkey business.
        OwnedGreenlet current = GET_THREAD_STATE().state().get_current();
        self->parent = current.borrow();
        // To get here it had to have run before
        /* Send the greenlet a GreenletExit exception. */

        // We don't care about the return value, only whether an
        // exception happened. Whether or not an exception happens,
        // we need to restore the parent in case the greenlet gets
        // resurrected.
        try {
            self->switching_state->kill();
        }
        catch(const PyErrOccurred&) {
            self->parent = oldparent;
            throw;
        }
        self->parent = oldparent;
        return;
    }

    // Not the same thread! Temporarily save the greenlet
    // into its thread's deleteme list, *if* it exists.
    // If that thread has already exited, and processed its pending
    // cleanup, we'll never be able to clean everything up: we won't
    // be able to raise an exception.
    // That's mostly OK! Since we can't add it to a list, our refcount
    // won't increase, and we'll go ahead with the DECREFs later.
    if (self->main_greenlet_s->thread_state) {
        self->main_greenlet_s->thread_state->delete_when_thread_running(self);
    }
    else {
        // The thread is dead, we can't raise an exception.
        // We need to make it look non-active, though, so that dealloc
        // finishes killing it.
        self->stack_state = StackState(); // XXX: This was only
                                          // defining stack_start,
                                          // yes? Do we need any of
                                          // the other variables?
        assert(!self.active());
        self->python_state.tp_clear(true);
    }
    return;
}

static int
green_traverse(PyGreenlet* self, visitproc visit, void* arg)
{
    // We must only visit referenced objects, i.e. only objects
    // Py_INCREF'ed by this greenlet (directly or indirectly):
    //
    // - stack_prev is not visited: holds previous stack pointer, but it's not
    //    referenced
    // - frames are not visited as we don't strongly reference them;
    //    alive greenlets are not garbage collected
    //    anyway. This can be a problem, however, if this greenlet is
    //    never allowed to finish, and is referenced from the frame: we
    //    have an uncollectable cycle in that case. Note that the
    //    frame object itself is also frequently not even tracked by the GC
    //    starting with Python 3.7 (frames are allocated by the
    //    interpreter untracked, and only become tracked when their
    //    evaluation is finished if they have a refcount > 1). All of
    //    this is to say that we should probably strongly reference
    //    the frame object. Doing so, while always allowing GC on a
    //    greenlet, solves several leaks for us.
    Py_VISIT((PyObject*)self->parent);
    Py_VISIT(self->main_greenlet_s);
    Py_VISIT(self->run_callable);

    int result;
    if ((result = self->exception_state.tp_traverse(visit, arg)) != 0) {
        return result;
    }
    //XXX: This is ugly. But so is handling everything having to do
    //with the top frame.
    bool visit_top_frame = (!self->main_greenlet_s || !self->main_greenlet_s->thread_state);
    // When true, the thread is dead. Our implicit weak reference to the
    // frame is now all that's left; we consider ourselves to
    // strongly own it now.
    if ((result = self->python_state.tp_traverse(visit, arg, visit_top_frame)) != 0) {
        return result;
    }

    Py_VISIT(self->dict);

    return 0;
}

static int
green_is_gc(PyGreenlet* self)
{
    int result = 0;
    /* Main greenlet can be garbage collected since it can only
       become unreachable if the underlying thread exited.
       Active greenlets --- including those that are suspended ---
       cannot be garbage collected, however.
    */
    if (PyGreenlet_MAIN(self) || !PyGreenlet_ACTIVE(self)) {
        result = 1;
    }
    // The main greenlet pointer will eventually go away after the thread dies.
    if (self->main_greenlet_s && !self->main_greenlet_s->thread_state) {
        // Our thread is dead! We can never run again. Might as well
        // GC us. Note that if a tuple containing only us and other
        // immutable objects had been scanned before this, when we
        // would have returned 0, the tuple will take itself out of GC
        // tracking and never be investigated again. So that could
        // result in both us and the tuple leaking due to an
        // unreachable/uncollectable reference. The same goes for
        // dictionaries.
        //
        // It's not a great idea to be changing our GC state on the
        // fly.
        result = 1;
    }
    return result;
}

static int
green_clear(PyGreenlet* self)
{
    /* Greenlet is only cleared if it is about to be collected.
       Since active greenlets are not garbage collectable, we can
       be sure that, even if they are deallocated during clear,
       nothing they reference is in unreachable or finalizers,
       so even if it switches we are relatively safe. */
    Py_CLEAR(self->parent);
    Py_CLEAR(self->main_greenlet_s); // XXX breaks when this is a state
    Py_CLEAR(self->run_callable);

    bool own_top_frame = (!self->main_greenlet_s || !self->main_greenlet_s->thread_state);
    self->python_state.tp_clear(own_top_frame);
    self->exception_state.tp_clear();
    Py_CLEAR(self->dict);
    return 0;
}

/**
 * Returns 0 on failure (the object was resurrected) or 1 on success.
 **/
static int
_green_dealloc_kill_started_non_main_greenlet(BorrowedGreenlet self)
{
    /* Hacks hacks hacks copied from instance_dealloc() */
    /* Temporarily resurrect the greenlet. */
    assert(self.REFCNT() == 0);
    Py_SET_REFCNT(self.borrow(), 1);
    /* Save the current exception, if any. */
    PyErrPieces saved_err;
    try {
        kill_greenlet(self);
    }
    catch (const PyErrOccurred&) {
        PyErr_WriteUnraisable(self.borrow_o());
        /* XXX what else should we do? */
    }
    /* Check for no resurrection must be done while we keep
     * our internal reference, otherwise PyFile_WriteObject
     * causes recursion if using Py_INCREF/Py_DECREF
     */
    if (self.REFCNT() == 1 && self.active()) {
        /* Not resurrected, but still not dead!
           XXX what else should we do? we complain. */
        PyObject* f = PySys_GetObject("stderr");
        Py_INCREF(self.borrow_o()); /* leak! */
        if (f != NULL) {
            PyFile_WriteString("GreenletExit did not kill ", f);
            PyFile_WriteObject(self.borrow_o(), f, 0);
            PyFile_WriteString("\n", f);
        }
    }
    /* Restore the saved exception. */
    saved_err.PyErrRestore();
    /* Undo the temporary resurrection; can't use DECREF here,
     * it would cause a recursive call.
     */
    assert(self.REFCNT() > 0);

    Py_ssize_t refcnt = self.REFCNT() - 1;
    Py_SET_REFCNT(self.borrow_o(), refcnt);
    if (refcnt != 0) {
        /* Resurrected! */
        _Py_NewReference(self.borrow_o());
        Py_SET_REFCNT(self.borrow_o(), refcnt);
        /* Better to use tp_finalizer slot (PEP 442)
         * and call ``PyObject_CallFinalizerFromDealloc``,
         * but that's only supported in Python 3.4+; see
         * Modules/_io/iobase.c for an example.
         *
         * The following approach is copied from iobase.c in CPython 2.7.
         * (along with much of this function in general). Here's their
         * comment:
         *
         * When called from a heap type's dealloc, the type will be
         * decref'ed on return (see e.g. subtype_dealloc in typeobject.c). */
        if (PyType_HasFeature(self.TYPE(), Py_TPFLAGS_HEAPTYPE)) {
            Py_INCREF(self.TYPE());
        }

        PyObject_GC_Track((PyObject*)self);

        _Py_DEC_REFTOTAL;
#ifdef COUNT_ALLOCS
        --Py_TYPE(self)->tp_frees;
        --Py_TYPE(self)->tp_allocs;
#endif /* COUNT_ALLOCS */
        return 0;
    }
    return 1;
}

static void
green_dealloc(PyGreenlet* self)
{
    PyObject_GC_UnTrack(self);
#ifndef NDEBUG
    PyObject* already_in_err = PyErr_Occurred();
#endif
    if (PyGreenlet_ACTIVE(self)
        && self->main_greenlet_s != NULL // means started
        && !PyGreenlet_MAIN(self)) {
        if (!_green_dealloc_kill_started_non_main_greenlet(self)) {
            return;
        }
    }

    if (self->weakreflist != NULL) {
        PyObject_ClearWeakRefs((PyObject*)self);
    }
    assert(already_in_err || !PyErr_Occurred());
    Py_CLEAR(self->run_callable);
    assert(already_in_err || !PyErr_Occurred());
    Py_CLEAR(self->parent);
    assert(already_in_err || !PyErr_Occurred());
    bool own_top_frame = (!self->main_greenlet_s || !self->main_greenlet_s->thread_state);
    Py_CLEAR(self->main_greenlet_s);
    assert(already_in_err || !PyErr_Occurred());

    // TODO: Express this in the reference object framework better.
    self->python_state.tp_clear(own_top_frame);
    self->exception_state.tp_clear();
    // Run the destructor; this is temporary until we move the whole
    // object to pointer-to-impl that we delete.
    self->stack_state = StackState();


    assert(already_in_err || !PyErr_Occurred());
    Py_CLEAR(self->dict);
    assert(already_in_err || !PyErr_Occurred());
    if (self->switching_state) {
        delete self->switching_state;
        self->switching_state = nullptr;
    }
    // and finally we're done. self is now invalid.
    Py_TYPE(self)->tp_free((PyObject*)self);
    assert(already_in_err || !PyErr_Occurred());
}

static void
maingreen_dealloc(PyMainGreenlet* self)
{
    // The ThreadState cleanup should have taken care of this.
    assert(!self->thread_state);
    total_main_greenlets--;
    green_dealloc(reinterpret_cast<PyGreenlet*>(self));
}


static OwnedObject
throw_greenlet(PyGreenlet* self, PyErrPieces& err_pieces)
{
    PyObject* result = nullptr;
    err_pieces.PyErrRestore();
    assert(PyErr_Occurred());
    if (PyGreenlet_STARTED(self) && !PyGreenlet_ACTIVE(self)) {
        /* dead greenlet: turn GreenletExit into a regular return */
        result = g_handle_exit(OwnedObject(), self).relinquish_ownership();
    }
    if (!self->switching_state) {
        self->switching_state = new SwitchingState(self);
    }
    self->switching_state->args() <<= result;

    return single_result(self->switching_state->g_switch());
}



PyDoc_STRVAR(
    green_switch_doc,
    "switch(*args, **kwargs)\n"
    "\n"
    "Switch execution to this greenlet.\n"
    "\n"
    "If this greenlet has never been run, then this greenlet\n"
    "will be switched to using the body of ``self.run(*args, **kwargs)``.\n"
    "\n"
    "If the greenlet is active (has been run, but was switch()'ed\n"
    "out before leaving its run function), then this greenlet will\n"
    "be resumed and the return value to its switch call will be\n"
    "None if no arguments are given, the given argument if one\n"
    "argument is given, or the args tuple and keyword args dict if\n"
    "multiple arguments are given.\n"
    "\n"
    "If the greenlet is dead, or is the current greenlet then this\n"
    "function will simply return the arguments using the same rules as\n"
    "above.\n");

static PyObject*
green_switch(PyGreenlet* self, PyObject* args, PyObject* kwargs)
{
    // XXX This extra allocation will go away when we make the
    // greenlet internal opaque.
    //
    // This is safe because of the GIL.
    // cerr << "Switching to greenlet with refcount: " << Py_REFCNT(self) << endl;
    // cerr<< "From greenlet with refcount " << GET_THREAD_STATE().state().borrow_current().REFCNT() << endl;
    if (!self->switching_state) {
        self->switching_state = new SwitchingState(self);
    }
    SwitchingArgs switch_args(OwnedObject::owning(args), OwnedObject::owning(kwargs));
    self->switching_state->args() <<= switch_args;


    // If we're switching out of a greenlet, and that switch is the
    // last thing the greenlet does, the greenlet ought to be able to
    // go ahead and die at that point. Currently, someone else must
    // manually switch back to the greenlet so that we "fall off the
    // end" and can perform cleanup. You'd think we'd be able to
    // figure out that this is happening using the frame's ``f_lasti``
    // member, which is supposed to be an index into
    // ``frame->f_code->co_code``, the bytecode string. However, in
    // recent interpreters, ``f_lasti`` tends not to be updated thanks
    // to things like the PREDICT() macros in ceval.c. So it doesn't
    // really work to do that in many cases. For example, the Python
    // code:
    //     def run():
    //         greenlet.getcurrent().parent.switch()
    // produces bytecode of len 16, with the actual call to switch()
    // being at index 10 (in Python 3.10). However, the reported
    // ``f_lasti`` we actually see is...5! (Which happens to be the
    // second byte of the CALL_METHOD op for ``getcurrent()``).

    try {
        OwnedObject result = single_result(self->switching_state->g_switch());
#ifndef NDEBUG
        // Note that the current greenlet isn't necessarily self. If self
        // finished, we went to one of its parents.
        assert(!self->switching_state->args());

        const BorrowedGreenlet& current = GET_THREAD_STATE().state().borrow_current();
        SwitchingState* const current_state = current->switching_state;
        if (current_state) {
            // It's possible it's never been switched to.
            assert(!current_state->args());
        }
#endif
        return result.relinquish_ownership();
    }
    catch(const PyErrOccurred&) {
        return nullptr;
    }
}

PyDoc_STRVAR(
    green_throw_doc,
    "Switches execution to this greenlet, but immediately raises the\n"
    "given exception in this greenlet.  If no argument is provided, the "
    "exception\n"
    "defaults to `greenlet.GreenletExit`.  The normal exception\n"
    "propagation rules apply, as described for `switch`.  Note that calling "
    "this\n"
    "method is almost equivalent to the following::\n"
    "\n"
    "    def raiser():\n"
    "        raise typ, val, tb\n"
    "    g_raiser = greenlet(raiser, parent=g)\n"
    "    g_raiser.switch()\n"
    "\n"
    "except that this trick does not work for the\n"
    "`greenlet.GreenletExit` exception, which would not propagate\n"
    "from ``g_raiser`` to ``g``.\n");

static PyObject*
green_throw(PyGreenlet* self, PyObject* args)
{
    PyArgParseParam typ(mod_globs.PyExc_GreenletExit);
    PyArgParseParam val;
    PyArgParseParam tb;

    if (!PyArg_ParseTuple(args, "|OOO:throw", &typ, &val, &tb)) {
        return NULL;
    }

    try {
        // Both normalizing the error and the actual throw_greenlet
        // could throw PyErrOccurred.
        PyErrPieces err_pieces(typ.borrow(), val.borrow(), tb.borrow());

        return throw_greenlet(self, err_pieces).relinquish_ownership();
    }
    catch (const PyErrOccurred&) {
        return nullptr;
    }
}

static int
green_bool(PyGreenlet* self)
{
    return PyGreenlet_ACTIVE(self);
}

static PyObject*
green_getdict(PyGreenlet* self, void* c)
{
    if (self->dict == NULL) {
        self->dict = PyDict_New();
        if (self->dict == NULL) {
            return NULL;
        }
    }
    Py_INCREF(self->dict);
    return self->dict;
}

static int
green_setdict(PyGreenlet* self, PyObject* val, void* c)
{
    PyObject* tmp;

    if (val == NULL) {
        PyErr_SetString(PyExc_TypeError, "__dict__ may not be deleted");
        return -1;
    }
    if (!PyDict_Check(val)) {
        PyErr_SetString(PyExc_TypeError, "__dict__ must be a dictionary");
        return -1;
    }
    tmp = self->dict;
    Py_INCREF(val);
    self->dict = val;
    Py_XDECREF(tmp);
    return 0;
}

static int
_green_not_dead(PyGreenlet* self)
{
    return PyGreenlet_ACTIVE(self) || !PyGreenlet_STARTED(self);
}


static PyObject*
green_getdead(PyGreenlet* self, void* c)
{
    if (_green_not_dead(self)) {
        Py_RETURN_FALSE;
    }
    else {
        Py_RETURN_TRUE;
    }
}

static PyObject*
green_get_stack_saved(PyGreenlet* self, void* c)
{
    return PyLong_FromSsize_t(self->stack_state.stack_saved());
}

static PyObject*
green_getrun(PyGreenlet* self, void* c)
{
    if (PyGreenlet_STARTED(self) || self->run_callable == NULL) {
        PyErr_SetString(PyExc_AttributeError, "run");
        return NULL;
    }
    Py_INCREF(self->run_callable);
    return self->run_callable;
}

static int
green_setrun(BorrowedGreenlet self, BorrowedObject nrun, void* c)
{
    if (PyGreenlet_STARTED(self)) {
        PyErr_SetString(PyExc_AttributeError,
                        "run cannot be set "
                        "after the start of the greenlet");
        return -1;
    }
    PyObject* old = self->run_callable;
    // XXX: Temporary convert to a PyObject* manually.
    // Only needed for Py2, which doesn't do a cast to PyObject*
    // before a null check, leading to an ambiguous override for
    // BorrowedObject == null;
    PyObject* new_run(nrun);
    self->run_callable = new_run;
    Py_XINCREF(new_run);
    Py_XDECREF(old);
    return 0;
}

static PyObject*
green_getparent(PyGreenlet* self, void* c)
{
    PyObject* result = self->parent ? (PyObject*)self->parent : Py_None;
    Py_INCREF(result);
    return result;
}


static int
green_setparent(BorrowedGreenlet self, BorrowedObject nparent, void* c)
{
    PyGreenlet* run_info = NULL;
    if (!nparent) {
        PyErr_SetString(PyExc_AttributeError, "can't delete attribute");
        return -1;
    }

    BorrowedGreenlet new_parent;
    try {
        new_parent = nparent;
        for (BorrowedGreenlet p = new_parent; p; p = p->parent) {
            if (p == self) {
                PyErr_SetString(PyExc_ValueError, "cyclic parent chain");
                return -1;
            }
            run_info = PyGreenlet_ACTIVE(p) ? (PyGreenlet*)p->main_greenlet_s : NULL;
        }
    }
    catch (const greenlet::TypeError&) {
        return -1;
    }
    if (run_info == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "parent must not be garbage collected");
        return -1;
    }
    if (PyGreenlet_STARTED(self) && self->main_greenlet_s != (void*)run_info) {
        PyErr_SetString(PyExc_ValueError,
                        "parent cannot be on a different thread");
        return -1;
    }
    PyGreenlet* old_parent = self->parent;
    self->parent = new_parent.borrow();
    Py_INCREF(nparent);
    Py_XDECREF(old_parent);
    return 0;
}

#ifdef Py_CONTEXT_H
#    define GREENLET_NO_CONTEXTVARS_REASON "This build of greenlet"
#else
#    define GREENLET_NO_CONTEXTVARS_REASON "This Python interpreter"
#endif

static PyObject*
green_getcontext(PyGreenlet* self, void* c)
{
#if GREENLET_PY37

    PyThreadState* tstate = PyThreadState_GET();
    OwnedObject result;

    if (PyGreenlet_ACTIVE(self) && !self->python_state.has_top_frame()) {
        /* Currently running greenlet: context is stored in the thread state,
           not the greenlet object. */
        if (GET_THREAD_STATE().state().is_current(self)) {
            result = tstate->context;
        }
        else {
            PyErr_SetString(PyExc_ValueError,
                            "cannot get context of a "
                            "greenlet that is running in a different thread");
            return NULL;
        }
    }
    else {
        /* Greenlet is not running: just return context. */
        result = self->python_state.context();
    }
    if (!result) {
        result = OwnedObject::None();
    }

    return result.relinquish_ownership();
#else
    PyErr_SetString(PyExc_AttributeError,
                    GREENLET_NO_CONTEXTVARS_REASON
                    " does not support context variables");
    return NULL;
#endif
}

static int
green_setcontext(PyGreenlet* self, PyObject* nctx, void* c)
{
#if GREENLET_PY37
    if (nctx == NULL) {
        PyErr_SetString(PyExc_AttributeError, "can't delete attribute");
        return -1;
    }
    if (nctx == Py_None) {
        /* "Empty context" is stored as NULL, not None. */
        nctx = NULL;
    }
    else if (!PyContext_CheckExact(nctx)) {
        PyErr_SetString(PyExc_TypeError,
                        "greenlet context must be a "
                        "contextvars.Context or None");
        return -1;
    }

    PyThreadState* tstate = PyThreadState_GET();

    if (PyGreenlet_ACTIVE(self) && !self->python_state.has_top_frame()) {
        /* Currently running greenlet: context is stored in the thread state,
           not the greenlet object. */
        if (GET_THREAD_STATE().state().is_current(self)) {
            OwnedObject octx = OwnedObject::consuming(tstate->context);
            tstate->context = nctx;
            tstate->context_ver++;
            Py_XINCREF(nctx);
        }
        else {
            PyErr_SetString(PyExc_ValueError,
                            "cannot set context of a "
                            "greenlet that is running in a different thread");
            return -1;
        }
    }
    else {
        /* Greenlet is not running: just set context. Note that the
           greenlet may be dead.*/
        self->python_state.context() = nctx;
    }
    return 0;
#else
    PyErr_SetString(PyExc_AttributeError,
                    GREENLET_NO_CONTEXTVARS_REASON
                    " does not support context variables");
    return -1;
#endif
}

#undef GREENLET_NO_CONTEXTVARS_REASON

static PyObject*
green_getframe(PyGreenlet* self, void* c)
{
    PyObject* result = self->python_state.has_top_frame()
        ? self->python_state.top_frame()
        : Py_None;
    Py_INCREF(result);
    return result;
}

static PyObject*
green_getstate(PyGreenlet* self)
{
    PyErr_Format(PyExc_TypeError,
                 "cannot serialize '%s' object",
                 Py_TYPE(self)->tp_name);
    return NULL;
}

static PyObject*
green_repr(PyGreenlet* self)
{
    /*
      Return a string like
      <greenlet.greenlet at 0xdeadbeef [current][active started]|dead main>

      The handling of greenlets across threads is not super good.
      We mostly use the internal definitions of these terms, but they
      generally should make sense to users as well.
     */
    PyObject* result;
    int never_started = !PyGreenlet_STARTED(self) && !PyGreenlet_ACTIVE(self);

    // XXX: Should not need this, and it has side effects.
    /*
    if (!STATE_OK) {
        return NULL;
    }
    */

    // Disguise the main greenlet type; changing the name in the repr breaks
    // doctests, but having a different actual tp_name is important
    // for debugging.
    const char* const tp_name = Py_TYPE(self) == &PyMainGreenlet_Type
        ? PyGreenlet_Type.tp_name
        : Py_TYPE(self)->tp_name;

    if (_green_not_dead(self)) {
        /* XXX: The otid= is almost useless becasue you can't correlate it to
         any thread identifier exposed to Python. We could use
         PyThreadState_GET()->thread_id, but we'd need to save that in the
         greenlet, or save the whole PyThreadState object itself.

         As it stands, its only useful for identifying greenlets from the same thread.
        */
        result = GNative_FromFormat(
            "<%s object at %p (otid=%p)%s%s%s%s>",
            tp_name,
            self,
            self->main_greenlet_s,
            GET_THREAD_STATE().state().is_current(self)
                ? " current"
                : (PyGreenlet_STARTED(self) ? " suspended" : ""),
            PyGreenlet_ACTIVE(self) ? " active" : "",
            never_started ? " pending" : " started",
            PyGreenlet_MAIN(self) ? " main" : ""
        );
    }
    else {
        /* main greenlets never really appear dead. */
        result = GNative_FromFormat(
            "<%s object at %p (otid=%p) dead>",
            tp_name,
            self,
            self->main_greenlet_s
            );
    }

    return result;
}

/*****************************************************************************
 * C interface
 *
 * These are exported using the CObject API
 */

static PyGreenlet*
PyGreenlet_GetCurrent(void)
{
    return GET_THREAD_STATE().state().get_current().relinquish_ownership();
}

static int
PyGreenlet_SetParent(PyGreenlet* g, PyGreenlet* nparent)
{
    if (!PyGreenlet_Check(g)) {
        PyErr_SetString(PyExc_TypeError, "parent must be a greenlet");
        return -1;
    }

    return green_setparent((PyGreenlet*)g, (PyObject*)nparent, NULL);
}

static PyGreenlet*
PyGreenlet_New(PyObject* run, PyGreenlet* parent)
{
    using greenlet::refs::NewDictReference;
    // In the past, we didn't use green_new and green_init, but that
    // was a maintenance issue because we duplicated code. This way is
    // much safer, but slightly slower. If that's a problem, we could
    // refactor green_init to separate argument parsing from initialization.
    OwnedGreenlet g = OwnedGreenlet::consuming(green_new(&PyGreenlet_Type, nullptr, nullptr));
    if (!g) {
        return NULL;
    }

    try {
        NewDictReference kwargs;
        if (run) {
            kwargs.SetItem(mod_globs.str_run, run);
        }
        if (parent) {
            kwargs.SetItem("parent", (PyObject*)parent);
        }

        Require(green_init(g, mod_globs.empty_tuple, kwargs));
    }
    catch (const PyErrOccurred&) {
        return nullptr;
    }

    return g.relinquish_ownership();
}

static PyObject*
PyGreenlet_Switch(PyGreenlet* g, PyObject* args, PyObject* kwargs)
{
    PyGreenlet* self = (PyGreenlet*)g;

    if (!PyGreenlet_Check(self)) {
        PyErr_BadArgument();
        return NULL;
    }

    if (args == NULL) {
        args = mod_globs.empty_tuple;
    }

    if (kwargs == NULL || !PyDict_Check(kwargs)) {
        kwargs = NULL;
    }

    return green_switch(g, args, kwargs);
}

static PyObject*
PyGreenlet_Throw(PyGreenlet* self, PyObject* typ, PyObject* val, PyObject* tb)
{
    if (!PyGreenlet_Check(self)) {
        PyErr_BadArgument();
        return nullptr;
    }
    try {
        PyErrPieces err_pieces(typ, val, tb);
        return throw_greenlet(self, err_pieces).relinquish_ownership();
    }
    catch (const PyErrOccurred&) {
        return nullptr;
    }
}

static int
Extern_PyGreenlet_MAIN(PyGreenlet* self)
{
    if (!PyGreenlet_Check(self)) {
        PyErr_BadArgument();
        return -1;
    }
    return self->stack_state.main();
}

static int
Extern_PyGreenlet_ACTIVE(PyGreenlet* self)
{
    if (!PyGreenlet_Check(self)) {
        PyErr_BadArgument();
        return -1;
    }
    return self->stack_state.active();
}

static int
Extern_PyGreenlet_STARTED(PyGreenlet* self)
{
    if (!PyGreenlet_Check(self)) {
        PyErr_BadArgument();
        return -1;
    }
    return self->stack_state.started();
}

static PyGreenlet*
Extern_PyGreenlet_GET_PARENT(PyGreenlet* self)
{
    if (!PyGreenlet_Check(self)) {
        PyErr_BadArgument();
        return NULL;;
    }
    Py_XINCREF(self->parent);
    return self->parent;
}

/** End C API ****************************************************************/

static PyMethodDef green_methods[] = {
    {"switch",
     reinterpret_cast<PyCFunction>(green_switch),
     METH_VARARGS | METH_KEYWORDS,
     green_switch_doc},
    {"throw", (PyCFunction)green_throw, METH_VARARGS, green_throw_doc},
    {"__getstate__", (PyCFunction)green_getstate, METH_NOARGS, NULL},
    {NULL, NULL} /* sentinel */
};

static PyGetSetDef green_getsets[] = {
    {"__dict__", (getter)green_getdict, (setter)green_setdict, /*XXX*/ NULL},
    {"run", (getter)green_getrun, (setter)green_setrun, /*XXX*/ NULL},
    {"parent", (getter)green_getparent, (setter)green_setparent, /*XXX*/ NULL},
    {"gr_frame", (getter)green_getframe, NULL, /*XXX*/ NULL},
    {"gr_context",
     (getter)green_getcontext,
     (setter)green_setcontext,
     /*XXX*/ NULL},
    {"dead", (getter)green_getdead, NULL, /*XXX*/ NULL},
    {"_stack_saved", (getter)green_get_stack_saved, NULL, /*XXX*/ NULL},
    {NULL}};

static PyMemberDef green_members[] = {
    {NULL}
};

static PyNumberMethods green_as_number = {
    NULL, /* nb_add */
    NULL, /* nb_subtract */
    NULL, /* nb_multiply */
#if PY_MAJOR_VERSION < 3
    NULL, /* nb_divide */
#endif
    NULL,                /* nb_remainder */
    NULL,                /* nb_divmod */
    NULL,                /* nb_power */
    NULL,                /* nb_negative */
    NULL,                /* nb_positive */
    NULL,                /* nb_absolute */
    (inquiry)green_bool, /* nb_bool */
};


PyTypeObject PyGreenlet_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "greenlet.greenlet", /* tp_name */
    sizeof(PyGreenlet),  /* tp_basicsize */
    0,                   /* tp_itemsize */
    /* methods */
    (destructor)green_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_compare */
    (reprfunc)green_repr,      /* tp_repr */
    &green_as_number,          /* tp_as _number*/
    0,                         /* tp_as _sequence*/
    0,                         /* tp_as _mapping*/
    0,                         /* tp_hash */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer*/
    G_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "greenlet(run=None, parent=None) -> greenlet\n\n"
    "Creates a new greenlet object (without running it).\n\n"
    " - *run* -- The callable to invoke.\n"
    " - *parent* -- The parent greenlet. The default is the current "
    "greenlet.",                        /* tp_doc */
    (traverseproc)green_traverse, /* tp_traverse */
    (inquiry)green_clear,         /* tp_clear */
    0,                                  /* tp_richcompare */
    offsetof(PyGreenlet, weakreflist),  /* tp_weaklistoffset */
    0,                                  /* tp_iter */
    0,                                  /* tp_iternext */
    green_methods,                      /* tp_methods */
    green_members,                      /* tp_members */
    green_getsets,                      /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    offsetof(PyGreenlet, dict),         /* tp_dictoffset */
    (initproc)green_init,               /* tp_init */
    PyType_GenericAlloc,                  /* tp_alloc */
    (newfunc)green_new,                          /* tp_new */
    PyObject_GC_Del,                   /* tp_free */
    (inquiry)green_is_gc,         /* tp_is_gc */
};



PyDoc_STRVAR(mod_getcurrent_doc,
             "getcurrent() -> greenlet\n"
             "\n"
             "Returns the current greenlet (i.e. the one which called this "
             "function).\n");

static PyObject*
mod_getcurrent(PyObject* self)
{
    return GET_THREAD_STATE().state().get_current().relinquish_ownership_o();
}

PyDoc_STRVAR(mod_settrace_doc,
             "settrace(callback) -> object\n"
             "\n"
             "Sets a new tracing function and returns the previous one.\n");
static PyObject*
mod_settrace(PyObject* self, PyObject* args)
{
    PyArgParseParam tracefunc;
    if (!PyArg_ParseTuple(args, "O", &tracefunc)) {
        return NULL;
    }
    ThreadState& state = GET_THREAD_STATE();
    OwnedObject previous = state.get_tracefunc();
    if (!previous) {
        previous = Py_None;
    }

    state.set_tracefunc(tracefunc);

    return previous.relinquish_ownership();
}

PyDoc_STRVAR(mod_gettrace_doc,
             "gettrace() -> object\n"
             "\n"
             "Returns the currently set tracing function, or None.\n");

static PyObject*
mod_gettrace(PyObject* self)
{
    OwnedObject tracefunc = GET_THREAD_STATE().state().get_tracefunc();
    if (!tracefunc) {
        tracefunc = Py_None;
    }
    return tracefunc.relinquish_ownership();
}

PyDoc_STRVAR(mod_set_thread_local_doc,
             "set_thread_local(key, value) -> None\n"
             "\n"
             "Set a value in the current thread-local dictionary. Debbuging only.\n");

static PyObject*
mod_set_thread_local(PyObject* mod, PyObject* args)
{
    PyArgParseParam key;
    PyArgParseParam value;
    PyObject* result = NULL;

    if (PyArg_UnpackTuple(args, "set_thread_local", 2, 2, &key, &value)) {
        if(PyDict_SetItem(
                          PyThreadState_GetDict(), // borrow
                          key,
                          value) == 0 ) {
            // success
            Py_INCREF(Py_None);
            result = Py_None;
        }
    }
    return result;
}

PyDoc_STRVAR(mod_get_pending_cleanup_count_doc,
             "get_pending_cleanup_count() -> Integer\n"
             "\n"
             "Get the number of greenlet cleanup operations pending. Testing only.\n");


static PyObject*
mod_get_pending_cleanup_count(PyObject* mod)
{
    LockGuard cleanup_lock(*mod_globs.thread_states_to_destroy_lock);
    return PyLong_FromSize_t(mod_globs.thread_states_to_destroy.size());
}

PyDoc_STRVAR(mod_get_total_main_greenlets_doc,
             "get_total_main_greenlets() -> Integer\n"
             "\n"
             "Quickly return the number of main greenlets that exist. Testing only.\n");

static PyObject*
mod_get_total_main_greenlets(PyObject* mod)
{
    return PyLong_FromSize_t(total_main_greenlets);
}

static PyMethodDef GreenMethods[] = {
    {"getcurrent",
     (PyCFunction)mod_getcurrent,
     METH_NOARGS,
     mod_getcurrent_doc},
    {"settrace", (PyCFunction)mod_settrace, METH_VARARGS, mod_settrace_doc},
    {"gettrace", (PyCFunction)mod_gettrace, METH_NOARGS, mod_gettrace_doc},
    {"set_thread_local", (PyCFunction)mod_set_thread_local, METH_VARARGS, mod_set_thread_local_doc},
    {"get_pending_cleanup_count", (PyCFunction)mod_get_pending_cleanup_count, METH_NOARGS, mod_get_pending_cleanup_count_doc},
    {"get_total_main_greenlets", (PyCFunction)mod_get_total_main_greenlets, METH_NOARGS, mod_get_total_main_greenlets_doc},
    {NULL, NULL} /* Sentinel */
};

static const char* const copy_on_greentype[] = {
    "getcurrent",
    "error",
    "GreenletExit",
    "settrace",
    "gettrace",
    NULL
};

static struct PyModuleDef greenlet_module_def = {
    PyModuleDef_HEAD_INIT,
    "greenlet._greenlet",
    NULL,
    -1,
    GreenMethods,
};



static PyObject*
greenlet_internal_mod_init() G_NOEXCEPT
{
    static void* _PyGreenlet_API[PyGreenlet_API_pointers];
    GREENLET_NOINLINE_INIT();

    try {
        CreatedModule m(greenlet_module_def);

        Require(PyType_Ready(&PyGreenlet_Type));

        PyMainGreenlet_Type.tp_base = &PyGreenlet_Type;
        Py_INCREF(&PyGreenlet_Type);
        // On Py27, if we don't manually inherit the flags, we don't get
        // Py_TPFLAGS_HAVE_CLASS, which breaks lots of things, notably
        // type checking for the subclass. We also wind up inheriting
        // HAVE_GC, which means we must set those fields as well, since if
        // its explicitly set they don't get copied
        PyMainGreenlet_Type.tp_flags = G_TPFLAGS_DEFAULT;
        PyMainGreenlet_Type.tp_traverse = (traverseproc)green_traverse;
        PyMainGreenlet_Type.tp_clear = (inquiry)green_clear;
        PyMainGreenlet_Type.tp_is_gc = (inquiry)green_is_gc;
        PyMainGreenlet_Type.tp_dealloc = (destructor)maingreen_dealloc;

        Require(PyType_Ready(&PyMainGreenlet_Type));

#if G_USE_STANDARD_THREADING == 0
        Require(PyType_Ready(&PyGreenletCleanup_Type));
#endif

        new((void*)&mod_globs) GreenletGlobals;

        m.PyAddObject("greenlet", PyGreenlet_Type);
        m.PyAddObject("error", mod_globs.PyExc_GreenletError);
        m.PyAddObject("GreenletExit", mod_globs.PyExc_GreenletExit);

        m.PyAddObject("GREENLET_USE_GC", 1);
        m.PyAddObject("GREENLET_USE_TRACING", 1);
        // The macros are eithre 0 or 1; the 0 case can be interpreted
        // the same as NULL, which is ambiguous with a pointer.
        m.PyAddObject("GREENLET_USE_CONTEXT_VARS", (long)GREENLET_PY37);
        m.PyAddObject("GREENLET_USE_STANDARD_THREADING", (long)G_USE_STANDARD_THREADING);

        /* also publish module-level data as attributes of the greentype. */
        // XXX: This is weird, and enables a strange pattern of
        // confusing the class greenlet with the module greenlet; with
        // the exception of (possibly) ``getcurrent()``, this
        // shouldn't be encouraged so don't add new items here.
        for (const char* const* p = copy_on_greentype; *p; p++) {
            OwnedObject o = m.PyRequireAttr(*p);
            PyDict_SetItemString(PyGreenlet_Type.tp_dict, *p, o.borrow());
        }

        /*
         * Expose C API
         */

        /* types */
        _PyGreenlet_API[PyGreenlet_Type_NUM] = (void*)&PyGreenlet_Type;

        /* exceptions */
        _PyGreenlet_API[PyExc_GreenletError_NUM] = (void*)mod_globs.PyExc_GreenletError;
        _PyGreenlet_API[PyExc_GreenletExit_NUM] = (void*)mod_globs.PyExc_GreenletExit;

        /* methods */
        _PyGreenlet_API[PyGreenlet_New_NUM] = (void*)PyGreenlet_New;
        _PyGreenlet_API[PyGreenlet_GetCurrent_NUM] = (void*)PyGreenlet_GetCurrent;
        _PyGreenlet_API[PyGreenlet_Throw_NUM] = (void*)PyGreenlet_Throw;
        _PyGreenlet_API[PyGreenlet_Switch_NUM] = (void*)PyGreenlet_Switch;
        _PyGreenlet_API[PyGreenlet_SetParent_NUM] = (void*)PyGreenlet_SetParent;

        /* Previously macros, but now need to be functions externally. */
        _PyGreenlet_API[PyGreenlet_MAIN_NUM] = (void*)Extern_PyGreenlet_MAIN;
        _PyGreenlet_API[PyGreenlet_STARTED_NUM] = (void*)Extern_PyGreenlet_STARTED;
        _PyGreenlet_API[PyGreenlet_ACTIVE_NUM] = (void*)Extern_PyGreenlet_ACTIVE;
        _PyGreenlet_API[PyGreenlet_GET_PARENT_NUM] = (void*)Extern_PyGreenlet_GET_PARENT;

        /* XXX: Note that our module name is ``greenlet._greenlet``, but for
           backwards compatibility with existing C code, we need the _C_API to
           be directly in greenlet.
        */
        const NewReference c_api_object(Require(
                                           PyCapsule_New(
                                               (void*)_PyGreenlet_API,
                                               "greenlet._C_API",
                                               NULL)));
        m.PyAddObject("_C_API", c_api_object);
        assert(c_api_object.REFCNT() == 2);
        return m.borrow(); // But really it's the main reference.
    }
    catch (const LockInitError& e) {
        PyErr_SetString(PyExc_MemoryError, e.what());
        return NULL;
    }
    catch (const PyErrOccurred&) {
        return NULL;
    }

}

extern "C" {
#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC
PyInit__greenlet(void)
{
    return greenlet_internal_mod_init();
}
#else
PyMODINIT_FUNC
init_greenlet(void)
{
    greenlet_internal_mod_init();
}
#endif
};

#ifdef __clang__
#    pragma clang diagnostic pop
#elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif
