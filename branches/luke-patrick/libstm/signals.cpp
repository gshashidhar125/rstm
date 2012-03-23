/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

#include <sys/time.h>                   // setitimer
#include "common/utils.hpp"             // stm::typed_memcpy
#include "common/platform.hpp"          // faiptr
#include "stm/txthread.hpp"
#include "stm/metadata.hpp"
#include "shadow-signals.h"
#include "sandboxing.hpp"
#include "interposition.hpp"            // stm::lazy_load_symbol
#include "policies/policies.hpp"        // curr_policy
#include "algs/algs.hpp"                // stms[]
#include "stm/lib_globals.hpp"

// Hack for this branch.
#include "libitm2stm/Checkpoint.h"

using stm::Self;
using stm::TxThread;
using stm::typed_memcpy;
using stm::lazy_load_symbol;
using stm::stms;
using stm::curr_policy;
using stm::threadcount;
using stm::pad_word_t;
using stm::MAX_THREADS;
using stm::trans_nums;
using stm::threads;

using stm::sandbox::start_timer;

/*** A thread local pointer to the stm-allocated alt stack. */
static __thread uint8_t* my_stack = NULL;

static __thread int in_lib = 0;

void
stm::sandbox::clear_in_lib() {
    in_lib = 0;
}

void
stm::sandbox::set_in_lib() {
    in_lib = 1;
}

void
stm_enter_waiver() {
    stm::sandbox::set_in_lib();
}

void
stm_leave_waiver() {
    stm::sandbox::clear_in_lib();
}

stm::sandbox::InLib::InLib() {
    ++in_lib;
}

stm::sandbox::InLib::~InLib() {
    --in_lib;
}

// The validation timer (initialized in init_system)
static struct itimerval timer;
static volatile sig_atomic_t timer_lock = 0;

static const int TIMER_MILLISECOND = 1000000 / 100;
/**
 *  Slow down the sandboxing timer.
 */
static void
inc_timer_period()
{
    // this can be somewhat gray, if someone else is mucking with the lock,
    // just skip this increment
    if (faiptr(&timer_lock))
        return;

    // printf("incrementing sandbox timer period\n");

    // we saturate at 1 second
    if (timer.it_interval.tv_sec == 0) {
        if ((timer.it_interval.tv_usec += 10 * TIMER_MILLISECOND) > 999999) {
            timer.it_interval.tv_sec = 1;
            timer.it_interval.tv_usec = 0;
        }
    }

    timer.it_value = timer.it_interval;
    start_timer();
    timer_lock = 0;
}

static void
dec_timer_period()
{
    // We really want to decrement the timer because this only happens when we
    // handle a SIGUSR2 that found us invalid. tatas-style acquire.
    while (faiptr(&timer_lock))
        while (timer_lock)
            spin64();

    // printf("decrementing sandbox timer period\n");

    // if we were saturated on the high-side, shift the second into usecs
    if (timer.it_interval.tv_sec == 1) {
        timer.it_interval.tv_sec = 0;
        timer.it_interval.tv_usec = 1000000;
    }

    // we saturate at 1 millisecond
    if ((timer.it_interval.tv_usec /= 2u) < 10 * TIMER_MILLISECOND)
        timer.it_interval.tv_usec = 10 * TIMER_MILLISECOND;

    timer.it_value = timer.it_interval;
    start_timer();
    timer_lock = 0;
}

/**
 *  Our signal handler (signal_shadowing_t).
 */
static void
prevalidate(int sig, siginfo_t* info, void* ctx, libc_sigaction_t cont)
{
    itm2stm::Checkpoint* scope = NULL;

    if (stms[curr_policy.ALG_ID].sandbox_signals &&
        !in_lib &&
        Self->scope &&
        !Self->tmvalidate(Self)) {
        // we're not valid.... we'll need to abort, but only for the signals
        // that we expect to be dealing with.
        switch (sig) {
          case SIGUSR2:
            dec_timer_period();         // was probably infinite looping
            // fall through to abort
          case SIGSEGV:
          case SIGBUS:
          case SIGFPE:
          case SIGILL:
          case SIGABRT:
            // We're going to longjmp from her. By setting the mask, and
            // clearing the sig from the mask, we'll be ok.
            scope = (itm2stm::Checkpoint*)Self->scope;
            scope->restoreMask_ = true;
            sigemptyset(&scope->mask_);
            pthread_sigmask(SIG_SETMASK, NULL, &scope->mask_);
            sigdelset(&scope->mask_, sig);
            sigdelset(&scope->mask_, SIGUSR2);
            Self->tmabort(Self); // just abort... noreturn
          default:
            fprintf(stderr, "libstm: saw a signal we didn't expect %i", sig);
        }
    }
    // SIGUSR2 is always for me!
    if (sig == SIGUSR2)
        return;

    // should be a tail call
    cont(sig, info, ctx);
}

static bool is_sig_set(int) __attribute__((used));
static sigset_t* get_sigs() __attribute__((used));

sigset_t*
get_sigs() {
    sigset_t* sigs = new sigset_t;
    sigemptyset(sigs);
    pthread_sigmask(SIG_SETMASK, NULL, sigs);
    return sigs;
}

bool
is_sig_set(int sig) {
    sigset_t sigs;
    pthread_sigmask(SIG_SETMASK, NULL, &sigs);
    return sigismember(&sigs, sig);
}



static void
ping_the_world(int)
{
    static volatile uintptr_t prev_trans[MAX_THREADS] = {0};
    static volatile sig_atomic_t pinging = 0;

    // single-threaded fastpath, we could turn off the timer here if we wanted
    // to force people to declare all their threads early.
    if (threadcount.val == 1) {
        inc_timer_period();
        return;
    }

    // if someone else is pinging, just continue
    // NB: this implementation is correctly synchronized, fai should be
    //     non-blocking, only one thread should get a timer so there should be
    //     no contention on it, though it will be a cache miss.
    if (faiptr(&pinging))
        return;

    // alert all of the threads that might need to be notified to validate
    int notified = 0;
    for (unsigned i = 0, e = threadcount.val; i < e; ++i) {
        // if the thread is not in a transaction, don't notify
        if (!threads[i]->scope)
            continue;

        // if the thread is progressing, update its most recently seen
        // transaction and sskip notification
        if (prev_trans[i] != trans_nums[i].val) {
            prev_trans[i] = trans_nums[i].val;
            continue;
        }

        // this thread hasn't committed since the last signal, send it a USR2
        pthread_kill(threads[i]->pthreadid, SIGUSR2);
        ++notified;
    }

    // if no notifications were necessary, slow down the timer
    if (!notified)
        inc_timer_period();

    // reset pinging
    pinging = 0;
}

/**
 *  This is installed as the timer handler. It shouldn't be run for an opaque
 *  TM, although it might be due to timer multiplexing. For the moment we
 *  discount this possibility, but we might want to check before calling
 *  ping_the_world.
 */
static void
checktimer(int sig, siginfo_t* info, void* ctx, libc_sigaction_t cont)
{
    ping_the_world(sig);
    // this is a hack right now---we'd normally be worried about demultiplexing
    // timers.
    // cont(sig, info, ctx);
}

/**
 *  Installs the signal handlers that sandboxing requires. Also prepare our
 *  thread-local alt stack for sigsegv processing.
 */
void
stm::sandbox::init_system()
{
    stm_shadow_t shadow;
    shadow.action = prevalidate;
    shadow.flags = SA_SIGINFO;
    sigemptyset(&shadow.mask);
    sigaddset(&shadow.mask, SIGUSR2);

    // these are the simple prevalidation signals (SIGSEGV is also
    // prevalidated, but its done seperately because it has to be done on an
    // altstack for stack overflow).
    const int handled[] = {
        SIGBUS,
        SIGFPE,
        SIGILL,
        SIGUSR2                         // ping_the_world
    };

    for (int i = 0, e = length_of(handled); i < e; ++i)
        stm_shadow_sigaction(handled[i], shadow);

    // SIGSEGV has to run on an alternate stack
    shadow.flags |= SA_ONSTACK;
    stm_shadow_sigaction(SIGSEGV, shadow);

    // timer handler for infinite loop suppression
    shadow.action = checktimer;
    shadow.flags = SA_SIGINFO;
    stm_shadow_sigaction(SIGVTALRM, shadow);

    // initialize the timer frequency
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 10 * TIMER_MILLISECOND;
    timer.it_value = timer.it_interval;
}

/**
 *  Lazy binding for library call interposition.
 */
static int
call_sigaltstack(const stack_t *ss, stack_t *oss)
{
    static int (*psigaltstack)(const stack_t*, stack_t*) = NULL;
    lazy_load_symbol(psigaltstack, "sigaltstack");
    return psigaltstack(ss, oss);
}

/**
 *  If the user tries to register an alt stack, we'll want to use it. Just
 *  check to see if the old alt stack was our stm-allocated one, and if it was
 *  then free it.
 */
extern "C" int
sigaltstack(const stack_t *ss, stack_t *oss)
{
    stack_t stack;
    int r = call_sigaltstack(ss, &stack);

    if (stack.ss_sp == my_stack) {
        delete[] my_stack;
        stack.ss_sp = NULL;
        stack.ss_flags = 0;
        stack.ss_size = 0;
    }

    if (oss)
        typed_memcpy(oss, &stack);

    return r;
}

/*** */
void
stm::sandbox::start_timer()
{
    // Start my validation timer.
    setitimer(ITIMER_VIRTUAL, &timer, NULL);
}

/*** */
void
stm::sandbox::stop_timer()
{
    // Start my validation timer.
    struct itimerval stop;

    stop.it_interval.tv_sec = 0;
    stop.it_interval.tv_usec = 0;
    stop.it_value = stop.it_interval;
    setitimer(ITIMER_VIRTUAL, &stop, NULL);
}

/**
 *  Sandboxing requires that we be prepared to run the SIGSEGV handler in
 *  low-stack conditions, which means that we need an altstack set up. This is
 *  called during stm::thread_init() and initializes that alt stack.
 */
void
stm::sandbox::init_thread()
{
    my_stack = new uint8_t[SIGSTKSZ];
    stack_t stack;
    stack.ss_sp = my_stack;
    stack.ss_flags = 0;
    stack.ss_size = SIGSTKSZ;
    call_sigaltstack(&stack, NULL);
}

static __thread struct sigaction gdb_helper;
static USED void gdb_get_sigaction(int i) {
    sigaction(i, NULL, &gdb_helper);
}

static USED void gdb_print_blocked(sigset_t* set) {
    printf("blocked: ");
    for (int i = 0; i < 32; ++i) {
        if (sigismember(set, i))
            printf("%i, ", i);
    }
    printf("\n");
}

static USED void gdb_print_current_blocked() {
    sigset_t set;
    pthread_sigmask(0, NULL, &set);
    gdb_print_blocked(&set);
}
