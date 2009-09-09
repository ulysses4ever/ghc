/* ---------------------------------------------------------------------------
 *
 * (c) The GHC Team, 2001-2006
 *
 * Capabilities
 *
 * A Capability holds all the state an OS thread/task needs to run
 * Haskell code: its STG registers, a pointer to its TSO, a nursery
 * etc. During STG execution, a pointer to the Capabilitity is kept in
 * a register (BaseReg).
 *
 * Only in a THREADED_RTS build will there be multiple capabilities,
 * in the non-threaded RTS there is one global capability, called
 * MainCapability.
 *
 * --------------------------------------------------------------------------*/

#ifndef CAPABILITY_H
#define CAPABILITY_H

#include "sm/GC.h" // for evac_fn
#include "Task.h"
#include "Sparks.h"

BEGIN_RTS_PRIVATE

struct Capability_ {
    // State required by the STG virtual machine when running Haskell
    // code.  During STG execution, the BaseReg register always points
    // to the StgRegTable of the current Capability (&cap->r).
    StgFunTable f;
    StgRegTable r;

    nat no;  // capability number.

    // The Task currently holding this Capability.  This task has
    // exclusive access to the contents of this Capability (apart from
    // returning_tasks_hd/returning_tasks_tl).
    // Locks required: cap->lock.
    Task *running_task;

    // true if this Capability is running Haskell code, used for
    // catching unsafe call-ins.
    rtsBool in_haskell;

    // true if this Capability is currently in the GC
    rtsBool in_gc;

    // The run queue.  The Task owning this Capability has exclusive
    // access to its run queue, so can wake up threads without
    // taking a lock, and the common path through the scheduler is
    // also lock-free.
    StgTSO *run_queue_hd;
    StgTSO *run_queue_tl;

    // Tasks currently making safe foreign calls.  Doubly-linked.
    // When returning, a task first acquires the Capability before
    // removing itself from this list, so that the GC can find all
    // the suspended TSOs easily.  Hence, when migrating a Task from
    // the returning_tasks list, we must also migrate its entry from
    // this list.
    Task *suspended_ccalling_tasks;

    // One mutable list per generation, so we don't need to take any
    // locks when updating an old-generation thunk.  This also lets us
    // keep track of which closures this CPU has been mutating, so we
    // can traverse them using the right thread during GC and avoid
    // unnecessarily moving the data from one cache to another.
    bdescr **mut_lists;
    bdescr **saved_mut_lists; // tmp use during GC

    // Context switch flag. We used to have one global flag, now one 
    // per capability. Locks required  : none (conflicts are harmless)
    int context_switch;

#if defined(THREADED_RTS)
    // Worker Tasks waiting in the wings.  Singly-linked.
    Task *spare_workers;

    // This lock protects running_task, returning_tasks_{hd,tl}, wakeup_queue.
    Mutex lock;

    // Tasks waiting to return from a foreign call, or waiting to make
    // a new call-in using this Capability (NULL if empty).
    // NB. this field needs to be modified by tasks other than the
    // running_task, so it requires cap->lock to modify.  A task can
    // check whether it is NULL without taking the lock, however.
    Task *returning_tasks_hd; // Singly-linked, with head/tail
    Task *returning_tasks_tl;

    // A list of threads to append to this Capability's run queue at
    // the earliest opportunity.  These are threads that have been
    // woken up by another Capability.
    StgTSO *wakeup_queue_hd;
    StgTSO *wakeup_queue_tl;

    SparkPool *sparks;

    // Stats on spark creation/conversion
    nat sparks_created;
    nat sparks_converted;
    nat sparks_pruned;
#endif

    // Per-capability STM-related data
    StgTVarWatchQueue *free_tvar_watch_queues;
    StgInvariantCheckQueue *free_invariant_check_queues;
    StgTRecChunk *free_trec_chunks;
    StgTRecHeader *free_trec_headers;
    nat transaction_tokens;
} // typedef Capability is defined in RtsAPI.h
  // Capabilities are stored in an array, so make sure that adjacent
  // Capabilities don't share any cache-lines:
#ifndef mingw32_HOST_OS
  ATTRIBUTE_ALIGNED(64)
#endif
  ;


#if defined(THREADED_RTS)
#define ASSERT_TASK_ID(task) ASSERT(task->id == osThreadId())
#else
#define ASSERT_TASK_ID(task) /*empty*/
#endif

// These properties should be true when a Task is holding a Capability
#define ASSERT_FULL_CAPABILITY_INVARIANTS(cap,task)			\
  ASSERT(cap->running_task != NULL && cap->running_task == task);	\
  ASSERT(task->cap == cap);						\
  ASSERT_PARTIAL_CAPABILITY_INVARIANTS(cap,task)

// Sometimes a Task holds a Capability, but the Task is not associated
// with that Capability (ie. task->cap != cap).  This happens when
// (a) a Task holds multiple Capabilities, and (b) when the current
// Task is bound, its thread has just blocked, and it may have been
// moved to another Capability.
#define ASSERT_PARTIAL_CAPABILITY_INVARIANTS(cap,task)	\
  ASSERT(cap->run_queue_hd == END_TSO_QUEUE ?		\
	    cap->run_queue_tl == END_TSO_QUEUE : 1);	\
  ASSERT(myTask() == task);				\
  ASSERT_TASK_ID(task);

// Converts a *StgRegTable into a *Capability.
//
INLINE_HEADER Capability *
regTableToCapability (StgRegTable *reg)
{
    return (Capability *)((void *)((unsigned char*)reg - STG_FIELD_OFFSET(Capability,r)));
}

// Initialise the available capabilities.
//
void initCapabilities (void);

// Release a capability.  This is called by a Task that is exiting
// Haskell to make a foreign call, or in various other cases when we
// want to relinquish a Capability that we currently hold.
//
// ASSUMES: cap->running_task is the current Task.
//
#if defined(THREADED_RTS)
void releaseCapability           (Capability* cap);
void releaseAndWakeupCapability  (Capability* cap);
void releaseCapability_ (Capability* cap, rtsBool always_wakeup); 
// assumes cap->lock is held
#else
// releaseCapability() is empty in non-threaded RTS
INLINE_HEADER void releaseCapability  (Capability* cap STG_UNUSED) {};
INLINE_HEADER void releaseAndWakeupCapability  (Capability* cap STG_UNUSED) {};
INLINE_HEADER void releaseCapability_ (Capability* cap STG_UNUSED, 
                                       rtsBool always_wakeup STG_UNUSED) {};
#endif

// declared in includes/rts/Threads.h:
// extern Capability MainCapability; 

// declared in includes/rts/Threads.h:
// extern nat n_capabilities;

// Array of all the capabilities
//
extern Capability *capabilities;

// The Capability that was last free.  Used as a good guess for where
// to assign new threads.
//
extern Capability *last_free_capability;

// GC indicator, in scope for the scheduler
#define PENDING_GC_SEQ 1
#define PENDING_GC_PAR 2
extern volatile StgWord waiting_for_gc;

// Acquires a capability at a return point.  If *cap is non-NULL, then
// this is taken as a preference for the Capability we wish to
// acquire.
//
// OS threads waiting in this function get priority over those waiting
// in waitForCapability().
//
// On return, *cap is non-NULL, and points to the Capability acquired.
//
void waitForReturnCapability (Capability **cap/*in/out*/, Task *task);

INLINE_HEADER void recordMutableCap (StgClosure *p, Capability *cap, nat gen);

#if defined(THREADED_RTS)

// Gives up the current capability IFF there is a higher-priority
// thread waiting for it.  This happens in one of two ways:
//
//   (a) we are passing the capability to another OS thread, so
//       that it can run a bound Haskell thread, or
//
//   (b) there is an OS thread waiting to return from a foreign call
//
// On return: *pCap is NULL if the capability was released.  The
// current task should then re-acquire it using waitForCapability().
//
void yieldCapability (Capability** pCap, Task *task);

// Acquires a capability for doing some work.
//
// On return: pCap points to the capability.
//
void waitForCapability (Task *task, Mutex *mutex, Capability **pCap);

// Wakes up a thread on a Capability (probably a different Capability
// from the one held by the current Task).
//
void wakeupThreadOnCapability (Capability *my_cap, Capability *other_cap,
                               StgTSO *tso);

// Wakes up a worker thread on just one Capability, used when we
// need to service some global event.
//
void prodOneCapability (void);
void prodCapability (Capability *cap, Task *task);

// Similar to prodOneCapability(), but prods all of them.
//
void prodAllCapabilities (void);

// Waits for a capability to drain of runnable threads and workers,
// and then acquires it.  Used at shutdown time.
//
void shutdownCapability (Capability *cap, Task *task, rtsBool wait_foreign);

// Attempt to gain control of a Capability if it is free.
//
rtsBool tryGrabCapability (Capability *cap, Task *task);

// Try to find a spark to run
//
StgClosure *findSpark (Capability *cap);

// True if any capabilities have sparks
//
rtsBool anySparks (void);

INLINE_HEADER rtsBool emptySparkPoolCap (Capability *cap);
INLINE_HEADER nat     sparkPoolSizeCap  (Capability *cap);
INLINE_HEADER void    discardSparksCap  (Capability *cap);

#else // !THREADED_RTS

// Grab a capability.  (Only in the non-threaded RTS; in the threaded
// RTS one of the waitFor*Capability() functions must be used).
//
extern void grabCapability (Capability **pCap);

#endif /* !THREADED_RTS */

// cause all capabilities to context switch as soon as possible.
void setContextSwitches(void);
INLINE_HEADER void contextSwitchCapability(Capability *cap);

// Free all capabilities
void freeCapabilities (void);

// For the GC:
void markSomeCapabilities (evac_fn evac, void *user, nat i0, nat delta, 
                           rtsBool prune_sparks);
void markCapabilities (evac_fn evac, void *user);
void traverseSparkQueues (evac_fn evac, void *user);

/* -----------------------------------------------------------------------------
 * INLINE functions... private below here
 * -------------------------------------------------------------------------- */

INLINE_HEADER void
recordMutableCap (StgClosure *p, Capability *cap, nat gen)
{
    bdescr *bd;

    // We must own this Capability in order to modify its mutable list.
    ASSERT(cap->running_task == myTask());
    bd = cap->mut_lists[gen];
    if (bd->free >= bd->start + BLOCK_SIZE_W) {
	bdescr *new_bd;
	new_bd = allocBlock_lock();
	new_bd->link = bd;
	bd = new_bd;
	cap->mut_lists[gen] = bd;
    }
    *bd->free++ = (StgWord)p;
}

#if defined(THREADED_RTS)
INLINE_HEADER rtsBool
emptySparkPoolCap (Capability *cap) 
{ return looksEmpty(cap->sparks); }

INLINE_HEADER nat
sparkPoolSizeCap (Capability *cap) 
{ return sparkPoolSize(cap->sparks); }

INLINE_HEADER void
discardSparksCap (Capability *cap) 
{ return discardSparks(cap->sparks); }
#endif

INLINE_HEADER void
contextSwitchCapability (Capability *cap)
{
    // setting HpLim to NULL ensures that the next heap check will
    // fail, and the thread will return to the scheduler.
    cap->r.rHpLim = NULL;
    // But just in case it didn't work (the target thread might be
    // modifying HpLim at the same time), we set the end-of-block
    // context-switch flag too:
    cap->context_switch = 1;
}

END_RTS_PRIVATE

#endif /* CAPABILITY_H */
