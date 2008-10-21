/* Copyright (C) 2008 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/*
  "waiting threads" subsystem - a unified interface for threads to wait
  on each other, with built-in deadlock detection.

  Main concepts
  ^^^^^^^^^^^^^
    a thread - is represented by a WT_THD structure. One physical thread
      can have only one WT_THD descriptor.

    a resource - a thread does not wait for other threads directly,
      instead it waits for a "resource", which is "owned" by other threads.
      It waits, exactly, for all "owners" to "release" a resource.
      It does not have to correspond to a physical resource. For example, it
      may be convenient in certain cases to force resource == thread.
      A resource is represented by a WT_RESOURCE structure. 

    a resource identifier - a pair of {resource type, value}. A value is
      an ulonglong number. Represented by a WT_RESOURCE_ID structure.

    a resource type - a pointer to a statically defined instance of
    WT_RESOURCE_TYPE structure. This structure contains a pointer to
    a function that knows how to compare values of this resource type.
    In the simple case it could be wt_resource_id_memcmp().

   Usage
   ^^^^^
   to use the interface one needs to use this thread's WT_THD,
   call wt_thd_will_wait_for() for every thread it needs to wait on,
   then call wt_thd_cond_timedwait(). When thread releases a resource
   it should call wt_thd_release() (or wt_thd_release_all()) - it will
   notify (send a signal) threads waiting in wt_thd_cond_timedwait(),
   if appropriate.

   Just like with pthread's cond_wait, there could be spurious
   wake-ups from wt_thd_cond_timedwait(). A caller is expected to
   handle that.

   wt_thd_will_wait_for() and wt_thd_cond_timedwait() return either
   WT_OK or WT_DEADLOCK. Additionally wt_thd_cond_timedwait() can return
   WT_TIMEOUT. Out of memory and other fatal errors are reported as
   WT_DEADLOCK - and a transaction must be aborted just the same.

   Configuration
   ^^^^^^^^^^^^^
   There are four config variables. Two deadlock search depths - short and
   long - and two timeouts. Deadlock search is performed with the short
   depth on every wt_thd_will_wait_for() call. wt_thd_cond_timedwait()
   waits with a short timeout, performs a deadlock search with the long
   depth, and waits with a long timeout. As most deadlock cycles are supposed
   to be short, most deadlocks will be detected at once, and waits will
   rarely be necessary.

   These config variables are thread-local. Different threads may have
   different search depth and timeout values.

   Also, deadlock detector supports different killing strategies, the victim
   in a deadlock cycle is selected based on the "weight". See "weight"
   description in waiting_threads.h for details. It's up to the caller to
   set weights accordingly.

   Status
   ^^^^^^
   We calculate the number of successfull waits (WT_OK returned from
   wt_thd_cond_timedwait()), a number of timeouts, a deadlock cycle
   length distribution - number of deadlocks with every length from
   1 to WT_CYCLE_STATS, and a wait time distribution - number
   of waits with a time from 1 us to 1 min in WT_CYCLE_STATS
   intervals on a log scale.
*/

/*
  Note that if your lock system satisfy the following condition:

    there exist four lock levels A, B, C, D, such as
      A is compatible with B
      A is not compatible with C
      D is not compatible with B

      (example A=IX, B=IS, C=S, D=X)

   you need to include lock level in the resource identifier - thread 1
   waiting for lock A on resource R and thread 2 waiting for lock B
   on resource R should wait on different WT_RESOURCE structures, on different
   {lock, resource} pairs. Otherwise the following is possible:

      thread1> take S-lock on R
      thread2> take IS-lock on R
      thread3> wants X-lock on R, starts waiting for threads 1 and 2 on R.
      thread3 is killed (or timeout or whatever)
      WT_RESOURCE structure for R is still in the hash, as it has two owners
      thread4> wants an IX-lock on R
      WT_RESOURCE for R is found in the hash, thread4 starts waiting on it.
      !! now thread4 is waiting for both thread1 and thread2
      !! while, in fact, IX-lock and IS-lock are compatible and
      !! thread4 should not wait for thread2.
*/

#include <waiting_threads.h>
#include <m_string.h>

/*
  status variables:
    distribution of cycle lengths
    wait time log distribution

  Note:

    we call deadlock() twice per wait (with different search lengths).
    it means a deadlock will be counted twice. It's difficult to avoid,
    as on the second search we could find a *different* deadlock and we
    *want* to count it too. So we just count all deadlocks - two searches
    mean two increments on the wt_cycle_stats.
*/

ulonglong wt_wait_table[WT_WAIT_STATS];
uint32    wt_wait_stats[WT_WAIT_STATS+1];
uint32    wt_cycle_stats[2][WT_CYCLE_STATS+1], wt_success_stats;

static my_atomic_rwlock_t cycle_stats_lock, wait_stats_lock, success_stats_lock;

#define increment_success_stats()                                       \
  do {                                                                  \
    my_atomic_rwlock_wrlock(&success_stats_lock);                       \
    my_atomic_add32(&wt_success_stats, 1);                              \
    my_atomic_rwlock_wrunlock(&success_stats_lock);                     \
  } while (0)

#define increment_cycle_stats(X,SLOT)                                   \
  do {                                                                  \
    uint i= (X);                                                        \
    if (i >= WT_CYCLE_STATS)                                            \
      i= WT_CYCLE_STATS;                                                \
    my_atomic_rwlock_wrlock(&cycle_stats_lock);                         \
    my_atomic_add32(&wt_cycle_stats[SLOT][i], 1);                       \
    my_atomic_rwlock_wrunlock(&cycle_stats_lock);                       \
  } while (0)

#define increment_wait_stats(X,RET)                                     \
  do {                                                                  \
    uint i;                                                             \
    if ((RET) == ETIMEDOUT)                                             \
      i= WT_WAIT_STATS;                                                 \
    else                                                                \
    {                                                                   \
      ulonglong w=(X)/10;                                               \
      for (i=0; i < WT_WAIT_STATS && w > wt_wait_table[i]; i++) ;       \
    }                                                                   \
    my_atomic_rwlock_wrlock(&wait_stats_lock);                          \
    my_atomic_add32(wt_wait_stats+i, 1);                                \
    my_atomic_rwlock_wrunlock(&wait_stats_lock);                        \
  } while (0)

#define rc_rdlock(X)                                                    \
  do {                                                                  \
    WT_RESOURCE *R=(X);                                                 \
    DBUG_PRINT("wt", ("LOCK resid=%lld for READ", R->id.value));        \
    rw_rdlock(&R->lock);                                                \
  } while (0)
#define rc_wrlock(X)                                                    \
  do {                                                                  \
    WT_RESOURCE *R=(X);                                                 \
    DBUG_PRINT("wt", ("LOCK resid=%lld for WRITE", R->id.value));       \
    rw_wrlock(&R->lock);                                                \
  } while (0)
#define rc_unlock(X)                                                    \
  do {                                                                  \
    WT_RESOURCE *R=(X);                                                 \
    DBUG_PRINT("wt", ("UNLOCK resid=%lld", R->id.value));               \
    rw_unlock(&R->lock);                                                \
  } while (0)

/*
  All resources are stored in a lock-free hash. Different threads
  may add new resources and perform deadlock detection concurrently.
*/
static LF_HASH      reshash;

/**
  WT_RESOURCE constructor

  It's called from lf_hash and takes an offset to LF_SLIST instance.
  WT_RESOURCE is located at arg+sizeof(LF_SLIST)
*/
static void wt_resource_init(uchar *arg)
{
  WT_RESOURCE *rc=(WT_RESOURCE*)(arg+LF_HASH_OVERHEAD);
  DBUG_ENTER("wt_resource_init");

  bzero(rc, sizeof(*rc));
  my_rwlock_init(&rc->lock, 0);
  pthread_cond_init(&rc->cond, 0);
  my_init_dynamic_array(&rc->owners, sizeof(WT_THD *), 0, 5);
  DBUG_VOID_RETURN;
}

/**
  WT_RESOURCE destructor

  It's called from lf_hash and takes an offset to LF_SLIST instance.
  WT_RESOURCE is located at arg+sizeof(LF_SLIST)
*/
static void wt_resource_destroy(uchar *arg)
{
  WT_RESOURCE *rc=(WT_RESOURCE*)(arg+LF_HASH_OVERHEAD);
  DBUG_ENTER("wt_resource_destroy");

  DBUG_ASSERT(rc->owners.elements == 0);
  rwlock_destroy(&rc->lock);
  pthread_cond_destroy(&rc->cond);
  delete_dynamic(&rc->owners);
  DBUG_VOID_RETURN;
}

void wt_init()
{
  DBUG_ENTER("wt_init");

  lf_hash_init(&reshash, sizeof(WT_RESOURCE), LF_HASH_UNIQUE, 0,
               sizeof(struct st_wt_resource_id), 0, 0);
  reshash.alloc.constructor= wt_resource_init;
  reshash.alloc.destructor= wt_resource_destroy;
  /*
    Note a trick: we initialize the hash with the real element size,
    but fix it later to a shortened element size. This way
    the allocator will allocate elements correctly, but
    lf_hash_insert() will only overwrite part of the element with memcpy().
    lock, condition, and dynamic array will be intact.
  */
  reshash.element_size= offsetof(WT_RESOURCE, lock);
  bzero(wt_wait_stats, sizeof(wt_wait_stats));
  bzero(wt_cycle_stats, sizeof(wt_cycle_stats));
  wt_success_stats=0;
  { /* initialize wt_wait_table[]. from 1 us to 1 min, log scale */
    int i;
    double from=log(1);   /* 1 us */
    double to=log(60e6);  /* 1 min */
    for (i=0; i < WT_WAIT_STATS; i++)
    {
      wt_wait_table[i]=(ulonglong)exp((to-from)/(WT_WAIT_STATS-1)*i+from);
      DBUG_ASSERT(i==0 || wt_wait_table[i-1] != wt_wait_table[i]);
    }
  }
  my_atomic_rwlock_init(&cycle_stats_lock);
  my_atomic_rwlock_init(&success_stats_lock);
  my_atomic_rwlock_init(&wait_stats_lock);
  DBUG_VOID_RETURN;
}

void wt_end()
{
  DBUG_ENTER("wt_end");

  DBUG_ASSERT(reshash.count == 0);
  lf_hash_destroy(&reshash);
  my_atomic_rwlock_destroy(&cycle_stats_lock);
  my_atomic_rwlock_destroy(&success_stats_lock);
  my_atomic_rwlock_destroy(&wait_stats_lock);
  DBUG_VOID_RETURN;
}

/**
  Lazy WT_THD initialization

  Cheap initialization of WT_THD. Only initialized fields that don't require
  memory allocations - basically, it only does assignments. The rest of the
  WT_THD structure will be initialized on demand, on the first use.
  This allows one to initialize lazily all WT_THD structures, even if some
  (or even most) of them will never be used for deadlock detection.

  @param ds     a pointer to deadlock search depth short value
  @param ts     a pointer to deadlock timeout short value
  @param dl     a pointer to deadlock search depth long value
  @param tl     a pointer to deadlock timeout long value
*/
void wt_thd_lazy_init(WT_THD *thd, ulong *ds, ulong *ts, ulong *dl, ulong *tl)
{
  DBUG_ENTER("wt_thd_lazy_init");
  thd->waiting_for=0;
  thd->my_resources.buffer= 0;
  thd->my_resources.elements= 0;
  thd->weight=0;
  thd->deadlock_search_depth_short= ds;
  thd->timeout_short= ts;
  thd->deadlock_search_depth_long= dl;
  thd->timeout_long= tl;
  /* dynamic array is also initialized lazily - without memory allocations */
  my_init_dynamic_array(&thd->my_resources, sizeof(WT_RESOURCE *), 0, 5);
#ifndef DBUG_OFF
  thd->name=my_thread_name();
#endif
  DBUG_VOID_RETURN;
}

/**
  Finalize WT_THD initialization

  After lazy WT_THD initialization, parts of the structure are still
  uninitialized. This function completes the initialization, allocating
  memory, if necessary. It's called automatically on demand, when WT_THD
  is about to be used.
*/
static int fix_thd_pins(WT_THD *thd)
{
  if (unlikely(thd->pins == 0))
  {
    thd->pins=lf_hash_get_pins(&reshash);
#ifndef DBUG_OFF
    thd->name=my_thread_name();
#endif
  }
  return thd->pins == 0;
}

void wt_thd_destroy(WT_THD *thd)
{
  DBUG_ENTER("wt_thd_destroy");

  DBUG_ASSERT(thd->my_resources.elements == 0);

  if (thd->pins != 0)
    lf_hash_put_pins(thd->pins);

  delete_dynamic(&thd->my_resources);
  thd->waiting_for=0;
  DBUG_VOID_RETURN;
}
/**
  Trivial resource id comparison function - bytewise memcmp.

  It can be used in WT_RESOURCE_TYPE structures where bytewise
  comparison of values is sufficient.
*/
int wt_resource_id_memcmp(void *a, void *b)
{
  /* assert that the structure is not padded with random bytes */
  compile_time_assert(sizeof(WT_RESOURCE_ID)==sizeof(ulonglong)+sizeof(void*));
  return memcmp(a, b, sizeof(WT_RESOURCE_ID));
}

/**
  arguments for the recursive deadlock_search function
*/
struct deadlock_arg {
  WT_THD *thd;          /**< starting point of a search */
  uint    max_depth;    /**< search depth limit */
  WT_THD *victim;       /**< a thread to be killed to resolve a deadlock */
  WT_RESOURCE *rc;      /**< see comment at the end of deadlock_search() */
};

/**
  helper function to change the victim, according to the weight
*/
static void change_victim(WT_THD* found, struct deadlock_arg *arg)
{
  if (found->weight < arg->victim->weight)
  {
    if (arg->victim != arg->thd)
    {
      rc_unlock(arg->victim->waiting_for); /* release the previous victim */
      DBUG_ASSERT(arg->rc == found->waiting_for);
    }
    arg->victim= found;
    arg->rc= 0;
  }
}

/**
  recursive loop detection in a wait-for graph with a limited search depth
*/
static int deadlock_search(struct deadlock_arg *arg, WT_THD *blocker,
                           uint depth)
{
  WT_RESOURCE *rc, *volatile *shared_ptr= &blocker->waiting_for;
  WT_THD *cursor;
  uint i;
  int ret= WT_OK;
  DBUG_ENTER("deadlock_search");
  DBUG_PRINT("wt", ("enter: thd=%s, blocker=%s, depth=%u",
                    arg->thd->name, blocker->name, depth));

  LF_REQUIRE_PINS(1);

  arg->rc= 0;

  if (depth > arg->max_depth)
  {
    DBUG_PRINT("wt", ("exit: WT_DEPTH_EXCEEDED (early)"));
    DBUG_RETURN(WT_DEPTH_EXCEEDED);
  }

retry:
  /* safe dereference as explained in lf_alloc-pin.c */
  do
  {
    rc= *shared_ptr;
    lf_pin(arg->thd->pins, 0, rc);
  } while (rc != *shared_ptr && LF_BACKOFF);

  if (rc == 0)
  {
    DBUG_PRINT("wt", ("exit: OK (early)"));
    DBUG_RETURN(0);
  }

  rc_rdlock(rc);
  if (rc->state != ACTIVE || *shared_ptr != rc)
  {
    rc_unlock(rc);
    lf_unpin(arg->thd->pins, 0);
    goto retry;
  }
  /* as the state is locked, we can unpin now */
  lf_unpin(arg->thd->pins, 0);

  /*
    Below is not a pure depth-first search. It's a depth-first with a
    slightest hint of breadth-first. Depth-first is:

      check(element):
        foreach current in element->nodes[] do:
          if current == element return error;
          check(current);

    while we do

      check(element):
        foreach current in element->nodes[] do:
          if current == element return error;
        foreach current in element->nodes[] do:
          check(current);
  */
  for (i=0; i < rc->owners.elements; i++)
  {
    cursor= *dynamic_element(&rc->owners, i, WT_THD**);
    /*
      We're only looking for (and detecting) cycles that include 'arg->thd'.
      That is, only deadlocks that *we* have created. For example,
        thd->A->B->thd
      (thd waits for A, A waits for B, while B is waiting for thd).
      While walking the graph we can encounter other cicles, e.g.
        thd->A->B->C->A
      This will not be detected. Instead we will walk it in circles until
      the search depth limit is reached (the latter guarantees that an
      infinite loop is impossible). We expect the thread that has created
      the cycle (one of A, B, and C) to detect its deadlock.
    */
    if (cursor == arg->thd)
    {
      ret= WT_DEADLOCK;
      increment_cycle_stats(depth, arg->max_depth ==
                                   *arg->thd->deadlock_search_depth_long);
      arg->victim= cursor;
      goto end;
    }
  }
  for (i=0; i < rc->owners.elements; i++)
  {
    cursor= *dynamic_element(&rc->owners, i, WT_THD**);
    switch (deadlock_search(arg, cursor, depth+1)) {
    case WT_OK:
      break;
    case WT_DEPTH_EXCEEDED:
      ret= WT_DEPTH_EXCEEDED;
      break;
    case WT_DEADLOCK:
      ret= WT_DEADLOCK;
      change_victim(cursor, arg);       /* also sets arg->rc to 0 */
      i= rc->owners.elements;           /* jump out of the loop */
      break;
    default:
      DBUG_ASSERT(0);
    }
    if (arg->rc)
      rc_unlock(arg->rc);
  }
end:
  /*
    Note that 'rc' is locked in this function, but it's never unlocked there.
    Instead it's saved in arg->rc and the *caller* is expected to unlock it.
    It's done to support different killing strategies. This is how it works:
    Assuming a graph

      thd->A->B->C->thd

    deadlock_search() function starts from thd, locks it (in fact it locks not
    a thd, but a resource it is waiting on, but below, for simplicity, I'll
    talk about "locking a thd"). Then it goes down recursively, locks A, and so
    on. Goes down recursively, locks B. Goes down recursively, locks C.
    Notices that C is waiting on thd. Deadlock detected. Sets arg->victim=thd.
    Returns from the last deadlock_search() call. C stays locked!
    Now it checks whether C is a more appropriate victim then 'thd'.
    If yes - arg->victim=C, otherwise C is unlocked. Returns. B stays locked.
    Now it checks whether B is a more appropriate victim then arg->victim.
    If yes - old arg->victim is unlocked and arg->victim=B,
    otherwise B is unlocked. Return.
    And so on.

    In short, a resource is locked in a frame. But it's not unlocked in the
    same frame, it's unlocked by the caller, and only after the caller checks
    that it doesn't need to use current WT_THD as a victim. If it does - the
    lock is kept and the old victim's resource is unlocked. When the recursion
    is unrolled and we are back to deadlock() function, there are only two
    locks left - on thd and on the victim.
  */
  arg->rc= rc;
  DBUG_PRINT("wt", ("exit: %s",
                    ret == WT_DEPTH_EXCEEDED ? "WT_DEPTH_EXCEEDED" :
                    ret ? "WT_DEADLOCK" : "OK"));
  DBUG_RETURN(ret);
}

/**
  Deadlock detection in a wait-for graph

  A wrapper for recursive deadlock_search() - prepares deadlock_arg structure,
  invokes deadlock_search(), increments statistics, notifies the victim.

  @param thd            thread that is going to wait. Deadlock is detected
                        if, while walking the graph, we reach a thread that
                        is waiting on thd
  @param blocker        starting point of a search. In wt_thd_cond_timedwait()
                        it's thd, in wt_thd_will_wait_for() it's a thread that
                        thd is going to wait for
  @param depth          starting search depth. In general it's the number of
                        edges in the wait-for graph between thd and the
                        blocker. Practically only two values are used (and
                        supported) - when thd == blocker it's 0, when thd
                        waits directly for blocker, it's 1
  @param max_depth      search depth limit
*/
static int deadlock(WT_THD *thd, WT_THD *blocker, uint depth,
                            uint max_depth)
{
  struct deadlock_arg arg= {thd, max_depth, 0, 0};
  int ret;
  DBUG_ENTER("deadlock");
  ret= deadlock_search(&arg, blocker, depth);
  if (ret == WT_DEPTH_EXCEEDED)
  {
    increment_cycle_stats(WT_CYCLE_STATS, max_depth ==
                                          *thd->deadlock_search_depth_long);
    ret= WT_OK;
  }
  /*
    if we started with depth==1, blocker was never considered for a victim
    in deadlock_search(). Do it here.
  */
  if (ret == WT_DEADLOCK && depth)
    change_victim(blocker, &arg);
  if (arg.rc)
    rc_unlock(arg.rc);
  /* notify the victim, if appropriate */
  if (ret == WT_DEADLOCK && arg.victim != thd)
  {
    DBUG_PRINT("wt", ("killing %s", arg.victim->name));
    arg.victim->killed=1;
    pthread_cond_broadcast(&arg.victim->waiting_for->cond);
    rc_unlock(arg.victim->waiting_for);
    ret= WT_OK;
  }
  DBUG_RETURN(ret);
}


/**
  Delete an element from reshash if it has no waiters or owners

  rc->lock must be locked by the caller and it's unlocked on return.
*/
static int unlock_lock_and_free_resource(WT_THD *thd, WT_RESOURCE *rc)
{
  uint keylen;
  const void *key;
  DBUG_ENTER("unlock_lock_and_free_resource");

  DBUG_ASSERT(rc->state == ACTIVE);

  if (rc->owners.elements || rc->waiter_count)
  {
    DBUG_PRINT("wt", ("nothing to do, %d owners, %d waiters",
                      rc->owners.elements, rc->waiter_count));
    rc_unlock(rc);
    DBUG_RETURN(0);
  }

  if (fix_thd_pins(thd))
  {
    rc_unlock(rc);
    DBUG_RETURN(1);
  }

  /* XXX if (rc->id.type->make_key) key= rc->id.type->make_key(&rc->id, &keylen); else */
  {
    key= &rc->id;
    keylen= sizeof(rc->id);
  }

  /*
    To free the element correctly we need to:
     1. take its lock (already done).
     2. set the state to FREE
     3. release the lock
     4. remove from the hash

     I *think* it's safe to release the lock while the element is still
     in the hash. If not, the corrected procedure should be
     3. pin; 4; remove; 5; release; 6; unpin and it'll need pin[3].
  */
  rc->state=FREE;
  rc_unlock(rc);
  DBUG_RETURN(lf_hash_delete(&reshash, thd->pins, key, keylen) == -1);
}


/**
  register the fact that thd is not waiting anymore

  decrease waiter_count, clear waiting_for, free the resource if appropriate.
  thd->waiting_for must be locked!
*/
static int stop_waiting_locked(WT_THD *thd)
{
  int ret;
  WT_RESOURCE *rc= thd->waiting_for;
  DBUG_ENTER("stop_waiting_locked");

  DBUG_ASSERT(rc->waiter_count);
  DBUG_ASSERT(rc->state == ACTIVE);
  rc->waiter_count--;
  thd->waiting_for= 0;
  ret= unlock_lock_and_free_resource(thd, rc);
  DBUG_RETURN((thd->killed || ret) ? WT_DEADLOCK : WT_OK);
}

/**
  register the fact that thd is not waiting anymore

  locks thd->waiting_for and calls stop_waiting_locked().
*/
static int stop_waiting(WT_THD *thd)
{
  int ret;
  WT_RESOURCE *rc= thd->waiting_for;
  DBUG_ENTER("stop_waiting");

  if (!rc)
    DBUG_RETURN(WT_OK);
  /*
    nobody's trying to free the resource now,
    as its waiter_count is guaranteed to be non-zero
  */
  rc_wrlock(rc);
  ret= stop_waiting_locked(thd);
  DBUG_RETURN(ret);
}

/**
  notify the system that a thread needs to wait for another thread

  called by a *waiter* to declare what resource it will wait for.
  can be called many times, if many blockers own a blocking resource.
  but must always be called with the same resource id - a thread cannot
  wait for more than one resource at a time.

  As a new edge is added to the wait-for graph, a deadlock detection is
  performed for this new edge.
*/
int wt_thd_will_wait_for(WT_THD *thd, WT_THD *blocker, WT_RESOURCE_ID *resid)
{
  uint i;
  WT_RESOURCE *rc;
  DBUG_ENTER("wt_thd_will_wait_for");

  LF_REQUIRE_PINS(3);

  DBUG_PRINT("wt", ("enter: thd=%s, blocker=%s, resid=%llu",
                    thd->name, blocker->name, resid->value));

  if (fix_thd_pins(thd))
    DBUG_RETURN(WT_DEADLOCK);

  if (thd->waiting_for == 0)
  {
    uint keylen;
    const void *key;
    /* XXX if (restype->make_key) key= restype->make_key(resid, &keylen); else */
    {
      key= resid;
      keylen= sizeof(*resid);
    }

    DBUG_PRINT("wt", ("first blocker"));

retry:
    while ((rc= lf_hash_search(&reshash, thd->pins, key, keylen)) == 0)
    {
      WT_RESOURCE tmp;

      DBUG_PRINT("wt", ("failed to find rc in hash, inserting"));
      bzero(&tmp, sizeof(tmp));
      tmp.id= *resid;
      tmp.state= ACTIVE;

      if (lf_hash_insert(&reshash, thd->pins, &tmp) == -1) /* if OOM */
        DBUG_RETURN(WT_DEADLOCK);
      /*
        Two cases: either lf_hash_insert() failed - because another thread
        has just inserted a resource with the same id - and we need to retry.
        Or lf_hash_insert() succeeded, and then we need to repeat
        lf_hash_search() to find a real address of the newly inserted element.
        That is, we don't care what lf_hash_insert() has returned.
        And we need to repeat the loop anyway.
      */
    }
    if (rc == MY_ERRPTR)
      DBUG_RETURN(WT_DEADLOCK);

    DBUG_PRINT("wt", ("found in hash rc=%p", rc));

    rc_wrlock(rc);
    if (rc->state != ACTIVE)
    {
      DBUG_PRINT("wt", ("but it's not active, retrying"));
      /* Somebody has freed the element while we weren't looking */
      rc_unlock(rc);
      lf_hash_search_unpin(thd->pins);
      goto retry;
    }

    lf_hash_search_unpin(thd->pins); /* the element cannot go away anymore */
    thd->waiting_for= rc;
    rc->waiter_count++;
    thd->killed= 0;
  }
  else
  {
    DBUG_ASSERT(thd->waiting_for->id.type == resid->type);
    DBUG_ASSERT(resid->type->compare(&thd->waiting_for->id, resid) == 0);
    DBUG_PRINT("wt", ("adding another blocker"));

    /*
      we can safely access the resource here, it's in the hash as it has
      at least one owner, and non-zero waiter_count
    */
    rc= thd->waiting_for;
    rc_wrlock(rc);
    DBUG_ASSERT(rc->waiter_count);
    DBUG_ASSERT(rc->state == ACTIVE);

    if (thd->killed)
    {
      stop_waiting_locked(thd);
      DBUG_RETURN(WT_DEADLOCK);
    }
  }
  for (i=0; i < rc->owners.elements; i++)
    if (*dynamic_element(&rc->owners, i, WT_THD**) == blocker)
      break;
  if (i >= rc->owners.elements)
  {
    if (push_dynamic(&blocker->my_resources, (void*)&rc))
    {
      stop_waiting_locked(thd);
      DBUG_RETURN(WT_DEADLOCK); /* deadlock and OOM use the same error code */
    }
    if (push_dynamic(&rc->owners, (void*)&blocker))
    {
      pop_dynamic(&blocker->my_resources);
      stop_waiting_locked(thd);
      DBUG_RETURN(WT_DEADLOCK);
    }
  }
  rc_unlock(rc);

  if (deadlock(thd, blocker, 1, *thd->deadlock_search_depth_short))
  {
    stop_waiting(thd);
    DBUG_RETURN(WT_DEADLOCK);
  }
  DBUG_RETURN(0);
}

/**
  called by a *waiter* to start waiting

  It's supposed to be a drop-in replacement for
  pthread_cond_timedwait(), and it takes mutex as an argument.
*/
int wt_thd_cond_timedwait(WT_THD *thd, pthread_mutex_t *mutex)
{
  int ret= WT_TIMEOUT;
  struct timespec timeout;
  ulonglong before, after, starttime;
  WT_RESOURCE *rc= thd->waiting_for;
  DBUG_ENTER("wt_thd_cond_timedwait");
  DBUG_PRINT("wt", ("enter: thd=%s, rc=%p", thd->name, rc));

#ifndef DBUG_OFF
  if (rc->mutex)
    DBUG_ASSERT(rc->mutex == mutex);
  else
    rc->mutex= mutex;
  safe_mutex_assert_owner(mutex);
#endif

  before= starttime= my_getsystime();

#ifdef __WIN__
  /*
    only for the sake of Windows we distinguish between
    'before' and 'starttime'
  */
  GetSystemTimeAsFileTime((PFILETIME)&starttime);
#endif

  rc_wrlock(rc);
  if (rc->owners.elements == 0 || thd->killed)
    ret= WT_OK;
  rc_unlock(rc);

  set_timespec_time_nsec(timeout, starttime, (*thd->timeout_short)*ULL(1000));
  if (ret == WT_TIMEOUT)
    ret= pthread_cond_timedwait(&rc->cond, mutex, &timeout);
  if (ret == WT_TIMEOUT)
  {
    if (deadlock(thd, thd, 0, *thd->deadlock_search_depth_long))
      ret= WT_DEADLOCK;
    else if (*thd->timeout_long > *thd->timeout_short)
    {
      set_timespec_time_nsec(timeout, starttime, (*thd->timeout_long)*ULL(1000));
      if (!thd->killed)
        ret= pthread_cond_timedwait(&rc->cond, mutex, &timeout);
    }
  }
  after= my_getsystime();
  if (stop_waiting(thd) == WT_DEADLOCK) /* if we're killed */
    ret= WT_DEADLOCK;
  increment_wait_stats(after-before, ret);
  if (ret == WT_OK)
    increment_success_stats();
  DBUG_RETURN(ret);
}

/**
  called by a *blocker* when it releases a resource

  it's conceptually similar to pthread_cond_broadcast, and must be done
  under the same mutex as wt_thd_cond_timedwait().

  @param resid   a resource to release. 0 to release all resources
*/

void wt_thd_release(WT_THD *thd, WT_RESOURCE_ID *resid)
{
  uint i;
  DBUG_ENTER("wt_thd_release");

  for (i=0; i < thd->my_resources.elements; i++)
  {
    uint j;
    WT_RESOURCE *rc= *dynamic_element(&thd->my_resources, i, WT_RESOURCE**);
    if (!resid || (resid->type->compare(&rc->id, resid) == 0))
    {
      rc_wrlock(rc);
      /*
        nobody's trying to free the resource now,
        as its owners[] array is not empty (at least thd must be there)
      */
      DBUG_ASSERT(rc->state == ACTIVE);
      for (j=0; j < rc->owners.elements; j++)
        if (*dynamic_element(&rc->owners, j, WT_THD**) == thd)
          break;
      DBUG_ASSERT(j < rc->owners.elements);
      delete_dynamic_element(&rc->owners, j);
      if (rc->owners.elements == 0)
      {
        pthread_cond_broadcast(&rc->cond);
#ifndef DBUG_OFF
        if (rc->mutex)
          safe_mutex_assert_owner(rc->mutex);
#endif
      }
      unlock_lock_and_free_resource(thd, rc);
      if (resid)
      {
        delete_dynamic_element(&thd->my_resources, i);
        DBUG_VOID_RETURN;
      }
    }
  }
  if (!resid)
    reset_dynamic(&thd->my_resources);
  DBUG_VOID_RETURN;
}

