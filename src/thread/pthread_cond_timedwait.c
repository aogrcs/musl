#include "pthread_impl.h"

/*
 * struct waiter
 *
 * Waiter objects have automatic storage on the waiting thread, and
 * are used in building a linked list representing waiters currently
 * waiting on the condition variable or a group of waiters woken
 * together by a broadcast or signal; in the case of signal, this is a
 * degenerate list of one member.
 *
 * Waiter lists attached to the condition variable itself are
 * protected by the lock on the cv. Detached waiter lists are
 * protected by the associated mutex. The hand-off between protections
 * is handled by a "barrier" lock in each node, which disallows
 * signaled waiters from making forward progress to the code that will
 * access the list using the mutex until the list is in a consistent
 * state and the cv lock as been released.
 *
 * Since process-shared cond var semantics do not necessarily allow
 * one thread to see another's automatic storage (they may be in
 * different processes), the waiter list is not used for the
 * process-shared case, but the structure is still used to store data
 * needed by the cancellation cleanup handler.
 */

struct waiter {
	struct waiter *prev, *next;
	int state, barrier, requeued, mutex_ret;
	int *notify;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	int shared;
};

/* Self-synchronized-destruction-safe lock functions */

static inline void lock(volatile int *l)
{
	if (a_cas(l, 0, 1)) {
		a_cas(l, 1, 2);
		do __wait(l, 0, 2, 1);
		while (a_cas(l, 0, 2));
	}
}

static inline void unlock(volatile int *l)
{
	if (a_swap(l, 0)==2)
		__wake(l, 1, 1);
}

enum {
	WAITING,
	SIGNALED,
	LEAVING,
};

static void unwait(void *arg)
{
	struct waiter *node = arg, *p;

	if (node->shared) {
		pthread_cond_t *c = node->cond;
		pthread_mutex_t *m = node->mutex;
		if (a_fetch_add(&c->_c_waiters, -1) == -0x7fffffff)
			__wake(&c->_c_waiters, 1, 0);
		node->mutex_ret = pthread_mutex_lock(m);
		return;
	}

	int oldstate = a_cas(&node->state, WAITING, LEAVING);

	if (oldstate == WAITING) {
		/* Access to cv object is valid because this waiter was not
		 * yet signaled and a new signal/broadcast cannot return
		 * after seeing a LEAVING waiter without getting notified
		 * via the futex notify below. */

		pthread_cond_t *c = node->cond;
		lock(&c->_c_lock);
		
		if (c->_c_head == node) c->_c_head = node->next;
		else if (node->prev) node->prev->next = node->next;
		if (c->_c_tail == node) c->_c_tail = node->prev;
		else if (node->next) node->next->prev = node->prev;
		
		unlock(&c->_c_lock);

		if (node->notify) {
			if (a_fetch_add(node->notify, -1)==1)
				__wake(node->notify, 1, 1);
		}
	}

	node->mutex_ret = pthread_mutex_lock(node->mutex);

	if (oldstate == WAITING) return;

	/* If the mutex can't be locked, we're in big trouble because
	 * it's all that protects access to the shared list state.
	 * In order to prevent catastrophic stack corruption from
	 * unsynchronized access, simply deadlock. */
	if (node->mutex_ret && node->mutex_ret != EOWNERDEAD)
		for (;;) lock(&(int){0});

	/* Wait until control of the list has been handed over from
	 * the cv lock (signaling thread) to the mutex (waiters). */
	lock(&node->barrier);

	/* If this thread was requeued to the mutex, undo the extra
	 * waiter count that was added to the mutex. */
	if (node->requeued) a_dec(&node->mutex->_m_waiters);

	/* Find a thread to requeue to the mutex, starting from the
	 * end of the list (oldest waiters). */
	for (p=node; p->next; p=p->next);
	if (p==node) p=node->prev;
	for (; p && p->requeued; p=p->prev);
	if (p==node) p=node->prev;
	if (p) {
		p->requeued = 1;
		a_inc(&node->mutex->_m_waiters);
		/* The futex requeue command cannot requeue from
		 * private to shared, so for process-shared mutexes,
		 * simply wake the target. */
		int wake = node->mutex->_m_type & 128;
		__syscall(SYS_futex, &p->state, FUTEX_REQUEUE|128,
			wake, 1, &node->mutex->_m_lock) != -EINVAL
		|| __syscall(SYS_futex, &p->state, FUTEX_REQUEUE,
			0, 1, &node->mutex->_m_lock);
	}

	/* Remove this thread from the list. */
	if (node->next) node->next->prev = node->prev;
	if (node->prev) node->prev->next = node->next;
}

int pthread_cond_timedwait(pthread_cond_t *restrict c, pthread_mutex_t *restrict m, const struct timespec *restrict ts)
{
	struct waiter node = { .cond = c, .mutex = m };
	int e, seq, *fut, clock = c->_c_clock;

	if ((m->_m_type&15) && (m->_m_lock&INT_MAX) != __pthread_self()->tid)
		return EPERM;

	if (ts && ts->tv_nsec >= 1000000000UL)
		return EINVAL;

	pthread_testcancel();

	if (c->_c_shared) {
		node.shared = 1;
		fut = &c->_c_seq;
		seq = c->_c_seq;
		a_inc(&c->_c_waiters);
	} else {
		lock(&c->_c_lock);

		node.barrier = 1;
		fut = &node.state;
		seq = node.state = WAITING;
		node.next = c->_c_head;
		c->_c_head = &node;
		if (!c->_c_tail) c->_c_tail = &node;
		else node.next->prev = &node;

		unlock(&c->_c_lock);
	}

	pthread_mutex_unlock(m);

	do e = __timedwait(fut, seq, clock, ts, unwait, &node, !node.shared);
	while (*fut==seq && (!e || e==EINTR));
	if (e == EINTR) e = 0;

	unwait(&node);

	return node.mutex_ret ? node.mutex_ret : e;
}

int __private_cond_signal(pthread_cond_t *c, int n)
{
	struct waiter *p, *q=0;
	int ref = 0, cur;

	lock(&c->_c_lock);
	for (p=c->_c_tail; n && p; p=p->prev) {
		/* The per-waiter-node barrier lock is held at this
		 * point, so while the following CAS may allow forward
		 * progress in the target thread, it doesn't allow
		 * access to the waiter list yet. Ideally the target
		 * does not run until the futex wake anyway. */
		if (a_cas(&p->state, WAITING, SIGNALED) != WAITING) {
			ref++;
			p->notify = &ref;
		} else {
			n--;
			if (!q) q=p;
		}
	}
	/* Split the list, leaving any remainder on the cv. */
	if (p) {
		if (p->next) p->next->prev = 0;
		p->next = 0;
	} else {
		c->_c_head = 0;
	}
	c->_c_tail = p;
	unlock(&c->_c_lock);

	/* Wait for any waiters in the LEAVING state to remove
	 * themselves from the list before returning or allowing
	 * signaled threads to proceed. */
	while ((cur = ref)) __wait(&ref, 0, cur, 1);

	/* Wake the first signaled thread and unlock the per-waiter
	 * barriers preventing their forward progress. */
	for (p=q; p; p=q) {
		q = p->prev;
		if (!p->next) __wake(&p->state, 1, 1);
		unlock(&p->barrier);
	}
	return 0;
}
