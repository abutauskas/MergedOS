/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS ntdll, part 3: synchronisation. Locks, critical sections, SRW
 * locks, condition variables and wait-on-address use the kernel futex
 * directly; events, mutants and semaphores are user-space objects behind
 * handles, and every wait on them sleeps on one process-wide generation
 * counter that each state change bumps. Spec: docs/NT_USERSPACE.md, part 3.
 */
#include "src/ntrt/nt.h"

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define WAKE_ALL   0x7FFFFFFF

static volatile u32 generation;

static u32 my_tid(void)
{
	return (u32)(*(u64 *)(nt_teb() + 0x48));
}

static void futex_wait(volatile u32 *a, u32 v, s64 rel_100ns)
{
	struct {
		s64 sec, nsec;
	} ts;

	if (rel_100ns < 0) {
		mos_futex((void *)a, FUTEX_WAIT, v, NULL);
		return;
	}
	ts.sec = rel_100ns / 10000000;
	ts.nsec = rel_100ns % 10000000 * 100;
	mos_futex((void *)a, FUTEX_WAIT, v, &ts);
}

static void futex_wake(volatile u32 *a, u32 n)
{
	mos_futex((void *)a, FUTEX_WAKE, n, NULL);
}

/* 0 free, 1 locked, 2 locked with waiters. */
void nt_lock(volatile u32 *l)
{
	u32 c = 0;

	if (__atomic_compare_exchange_n(l, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	if (c != 2)
		c = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
	while (c) {
		futex_wait(l, 2, -1);
		c = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
	}
}

void nt_unlock(volatile u32 *l)
{
	if (__atomic_exchange_n(l, 0, __ATOMIC_RELEASE) == 2)
		futex_wake(l, 1);
}

static bool nt_trylock(volatile u32 *l)
{
	u32 c = 0;

	return __atomic_compare_exchange_n(l, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void nt_sync_wake_all(void)
{
	__atomic_add_fetch(&generation, 1, __ATOMIC_SEQ_CST);
	futex_wake(&generation, WAKE_ALL);
}

/* ---- critical sections ------------------------------------------------------------
 * CRITICAL_SECTION: DebugInfo, LockCount, RecursionCount, OwningThread,
 * LockSemaphore (low 32 bits: our futex lock word), SpinCount.
 */
struct cs {
	void *debug;
	s32 lock_count, recursion;
	u64 owner;
	u64 sem;
	u64 spin;
};

static API NTSTATUS w_RtlInitializeCriticalSection(struct cs *c)
{
	memset(c, 0, sizeof(*c));
	c->lock_count = -1;
	c->debug = nt_alloc(0x30);      /* RTL_CRITICAL_SECTION_DEBUG; Wine's CRT writes Spare[0] */
	return 0;
}
EXPORT(RtlInitializeCriticalSection);

static API NTSTATUS w_RtlInitializeCriticalSectionAndSpinCount(struct cs *c, u32 spin)
{
	w_RtlInitializeCriticalSection(c);
	c->spin = spin;
	return 0;
}
EXPORT(RtlInitializeCriticalSectionAndSpinCount);

static API NTSTATUS w_RtlInitializeCriticalSectionEx(struct cs *c, u32 spin, u32 flags)
{
	(void)flags;
	return w_RtlInitializeCriticalSectionAndSpinCount(c, spin);
}
EXPORT(RtlInitializeCriticalSectionEx);

static API NTSTATUS w_RtlDeleteCriticalSection(struct cs *c)
{
	if (c->debug && c->debug != (void *)-1)
		nt_free(c->debug);
	c->debug = NULL;
	c->lock_count = -1;
	c->owner = 0;
	c->recursion = 0;
	return 0;
}
EXPORT(RtlDeleteCriticalSection);

static API NTSTATUS w_RtlEnterCriticalSection(struct cs *c)
{
	u64 me = my_tid();

	if (c->owner == me) {
		c->recursion++;
		__atomic_add_fetch(&c->lock_count, 1, __ATOMIC_RELAXED);
		return 0;
	}
	nt_lock((volatile u32 *)&c->sem);
	__atomic_add_fetch(&c->lock_count, 1, __ATOMIC_RELAXED);
	c->owner = me;
	c->recursion = 1;
	return 0;
}
EXPORT(RtlEnterCriticalSection);

static API u8 w_RtlTryEnterCriticalSection(struct cs *c)
{
	u64 me = my_tid();

	if (c->owner == me) {
		c->recursion++;
		__atomic_add_fetch(&c->lock_count, 1, __ATOMIC_RELAXED);
		return 1;
	}
	if (!nt_trylock((volatile u32 *)&c->sem))
		return 0;
	__atomic_add_fetch(&c->lock_count, 1, __ATOMIC_RELAXED);
	c->owner = me;
	c->recursion = 1;
	return 1;
}
EXPORT(RtlTryEnterCriticalSection);

static API NTSTATUS w_RtlLeaveCriticalSection(struct cs *c)
{
	__atomic_sub_fetch(&c->lock_count, 1, __ATOMIC_RELAXED);
	if (--c->recursion > 0)
		return 0;
	c->recursion = 0;
	c->owner = 0;
	nt_unlock((volatile u32 *)&c->sem);
	return 0;
}
EXPORT(RtlLeaveCriticalSection);

static API u8 w_RtlIsCriticalSectionLockedByThread(struct cs *c)
{
	return c->owner == my_tid() && c->recursion;
}
EXPORT(RtlIsCriticalSectionLockedByThread);

static API u8 w_RtlIsCriticalSectionLocked(struct cs *c)
{
	return c->recursion != 0;
}
EXPORT(RtlIsCriticalSectionLocked);

/* ---- SRW locks: bit 31 writer, low bits reader count ------------------------------ */

static API void w_RtlInitializeSRWLock(u64 *l)
{
	*l = 0;
}
EXPORT(RtlInitializeSRWLock);
EXPORT_AS("RtlInitializeConditionVariable", w_RtlInitializeSRWLock);

static API u8 w_RtlTryAcquireSRWLockExclusive(u64 *l)
{
	u32 z = 0;

	return __atomic_compare_exchange_n((u32 *)l, &z, 0x80000000u, false, __ATOMIC_ACQUIRE,
					   __ATOMIC_RELAXED);
}
EXPORT(RtlTryAcquireSRWLockExclusive);

static API void w_RtlAcquireSRWLockExclusive(u64 *l)
{
	for (;;) {
		u32 v = __atomic_load_n((u32 *)l, __ATOMIC_RELAXED);

		if (!v && w_RtlTryAcquireSRWLockExclusive(l))
			return;
		if (v)
			futex_wait((u32 *)l, v, -1);
	}
}
EXPORT(RtlAcquireSRWLockExclusive);

static API u8 w_RtlTryAcquireSRWLockShared(u64 *l)
{
	u32 v = __atomic_load_n((u32 *)l, __ATOMIC_RELAXED);

	while (!(v & 0x80000000u))
		if (__atomic_compare_exchange_n((u32 *)l, &v, v + 1, false, __ATOMIC_ACQUIRE,
						__ATOMIC_RELAXED))
			return 1;
	return 0;
}
EXPORT(RtlTryAcquireSRWLockShared);

static API void w_RtlAcquireSRWLockShared(u64 *l)
{
	while (!w_RtlTryAcquireSRWLockShared(l)) {
		u32 v = __atomic_load_n((u32 *)l, __ATOMIC_RELAXED);

		if (v & 0x80000000u)
			futex_wait((u32 *)l, v, -1);
	}
}
EXPORT(RtlAcquireSRWLockShared);

static API void w_RtlReleaseSRWLockExclusive(u64 *l)
{
	__atomic_store_n((u32 *)l, 0, __ATOMIC_RELEASE);
	futex_wake((u32 *)l, WAKE_ALL);
}
EXPORT(RtlReleaseSRWLockExclusive);

static API void w_RtlReleaseSRWLockShared(u64 *l)
{
	if (__atomic_sub_fetch((u32 *)l, 1, __ATOMIC_RELEASE) == 0)
		futex_wake((u32 *)l, WAKE_ALL);
}
EXPORT(RtlReleaseSRWLockShared);

/* ---- condition variables: a sequence counter ---------------------------------------- */

static API void w_RtlWakeConditionVariable(u64 *cv)
{
	__atomic_add_fetch((u32 *)cv, 1, __ATOMIC_SEQ_CST);
	futex_wake((u32 *)cv, 1);
}
EXPORT(RtlWakeConditionVariable);

static API void w_RtlWakeAllConditionVariable(u64 *cv)
{
	__atomic_add_fetch((u32 *)cv, 1, __ATOMIC_SEQ_CST);
	futex_wake((u32 *)cv, WAKE_ALL);
}
EXPORT(RtlWakeAllConditionVariable);

static s64 rel_timeout(const s64 *t)
{
	s64 now;

	if (!t)
		return -1;
	if (*t <= 0)
		return -*t;
	NtQuerySystemTime(&now);
	return *t > now ? *t - now : 0;
}

static NTSTATUS cv_wait(u64 *cv, const s64 *timeout)
{
	u32 seq = __atomic_load_n((u32 *)cv, __ATOMIC_SEQ_CST);
	s64 rel = rel_timeout(timeout);

	if (!rel)
		return STATUS_TIMEOUT;
	futex_wait((u32 *)cv, seq, rel);
	return __atomic_load_n((u32 *)cv, __ATOMIC_SEQ_CST) == seq && timeout ? STATUS_TIMEOUT : 0;
}

static API NTSTATUS w_RtlSleepConditionVariableCS(u64 *cv, struct cs *c, const s64 *timeout)
{
	NTSTATUS s;
	s32 rec = c->recursion;

	c->recursion = 1;
	w_RtlLeaveCriticalSection(c);
	s = cv_wait(cv, timeout);
	w_RtlEnterCriticalSection(c);
	c->recursion = rec;
	return s;
}
EXPORT(RtlSleepConditionVariableCS);

static API NTSTATUS w_RtlSleepConditionVariableSRW(u64 *cv, u64 *l, const s64 *timeout, u32 flags)
{
	NTSTATUS s;

	if (flags & 1)
		w_RtlReleaseSRWLockShared(l);
	else
		w_RtlReleaseSRWLockExclusive(l);
	s = cv_wait(cv, timeout);
	if (flags & 1)
		w_RtlAcquireSRWLockShared(l);
	else
		w_RtlAcquireSRWLockExclusive(l);
	return s;
}
EXPORT(RtlSleepConditionVariableSRW);

/* ---- wait on address ----------------------------------------------------------------- */

static API NTSTATUS w_RtlWaitOnAddress(const void *addr, const void *cmp, size_t size,
				       const s64 *timeout)
{
	u32 gen = __atomic_load_n(&generation, __ATOMIC_SEQ_CST);
	s64 rel;

	if (memcmp(addr, cmp, size))
		return 0;
	rel = rel_timeout(timeout);
	if (!rel)
		return STATUS_TIMEOUT;
	futex_wait(&generation, gen, rel);
	return 0;
}
EXPORT(RtlWaitOnAddress);

static API void w_RtlWakeAddress(const void *addr)
{
	(void)addr;
	nt_sync_wake_all();
}
EXPORT_AS("RtlWakeAddressAll", w_RtlWakeAddress);
EXPORT_AS("RtlWakeAddressSingle", w_RtlWakeAddress);

/* ---- run once ------------------------------------------------------------------------ */

typedef API u32 (*once_fn)(void *once, void *param, void **ctx);

static API void w_RtlRunOnceInitialize(u64 *once)
{
	*once = 0;
}
EXPORT(RtlRunOnceInitialize);

/* state: 0 new, 1 running, 2 done (value in the upper bits). */
static API NTSTATUS w_RtlRunOnceBeginInitialize(u64 *once, u32 flags, void **ctx)
{
	for (;;) {
		u64 v = __atomic_load_n(once, __ATOMIC_ACQUIRE), z = 0;

		if ((v & 3) == 2) {
			if (ctx)
				*ctx = (void *)(v & ~3ull);
			return 0;
		}
		if (flags & 2)                          /* RTL_RUN_ONCE_CHECK_ONLY */
			return STATUS_UNSUCCESSFUL;
		if (!v && __atomic_compare_exchange_n(once, &z, 1, false, __ATOMIC_ACQUIRE,
						      __ATOMIC_RELAXED))
			return STATUS_PENDING;
		mos_sleep(100000);
	}
}
EXPORT(RtlRunOnceBeginInitialize);

static API NTSTATUS w_RtlRunOnceComplete(u64 *once, u32 flags, void *ctx)
{
	if (flags & 4)                                  /* RTL_RUN_ONCE_INIT_FAILED */
		__atomic_store_n(once, 0, __ATOMIC_RELEASE);
	else
		__atomic_store_n(once, ((u64)ctx & ~3ull) | 2, __ATOMIC_RELEASE);
	return 0;
}
EXPORT(RtlRunOnceComplete);

static API NTSTATUS w_RtlRunOnceExecuteOnce(u64 *once, once_fn fn, void *param, void **ctx)
{
	NTSTATUS s = w_RtlRunOnceBeginInitialize(once, 0, ctx);
	void *c = NULL;

	if (s != STATUS_PENDING)
		return s;
	if (!fn(once, param, &c)) {
		w_RtlRunOnceComplete(once, 4, NULL);
		return STATUS_UNSUCCESSFUL;
	}
	w_RtlRunOnceComplete(once, 0, c);
	if (ctx)
		*ctx = c;
	return 0;
}
EXPORT(RtlRunOnceExecuteOnce);

/* ---- FLS: one array per thread in TEB->FlsData-like storage ------------------------- */

#define FLS_MAX 128
static u32 fls_used[FLS_MAX / 32];
static void *fls_cb[FLS_MAX];

static void **fls_slots(void)
{
	void ***p = (void ***)(nt_teb() + 0x17C8);

	if (!*p)
		*p = nt_alloc(FLS_MAX * sizeof(void *));
	return *p;
}

static API NTSTATUS w_RtlFlsAlloc(void *cb, u32 *idx)
{
	for (u32 i = 1; i < FLS_MAX; i++)
		if (!(fls_used[i / 32] >> (i % 32) & 1)) {
			fls_used[i / 32] |= 1u << (i % 32);
			fls_cb[i] = cb;
			*idx = i;
			return 0;
		}
	return STATUS_NO_MEMORY;
}
EXPORT(RtlFlsAlloc);

static API NTSTATUS w_RtlFlsFree(u32 i)
{
	if (i >= FLS_MAX)
		return STATUS_INVALID_PARAMETER;
	fls_used[i / 32] &= ~(1u << (i % 32));
	return 0;
}
EXPORT(RtlFlsFree);

static API NTSTATUS w_RtlFlsGetValue(u32 i, void **v)
{
	if (!i || i >= FLS_MAX)
		return STATUS_INVALID_PARAMETER;
	*v = fls_slots()[i];
	return 0;
}
EXPORT(RtlFlsGetValue);

static API NTSTATUS w_RtlFlsSetValue(u32 i, void *v)
{
	if (!i || i >= FLS_MAX)
		return STATUS_INVALID_PARAMETER;
	fls_slots()[i] = v;
	return 0;
}
EXPORT(RtlFlsSetValue);

static API void w_RtlProcessFlsData(void *data, u32 flags)
{
	(void)data;
	(void)flags;
}
EXPORT(RtlProcessFlsData);

/* ---- events, mutants, semaphores ------------------------------------------------------ */

static NTSTATUS new_obj(u64 *h, enum nt_kind k, u32 state, u32 max, bool manual)
{
	struct nt_obj *o = nt_obj_new(k);

	if (!o)
		return STATUS_NO_MEMORY;
	o->state = state;
	o->max = max;
	o->manual = manual;
	*h = nt_handle_new(o);
	return *h ? 0 : STATUS_NO_MEMORY;
}

static API NTSTATUS w_NtCreateEvent(u64 *h, u32 access, void *oa, u32 type, u8 state)
{
	(void)access;
	(void)oa;
	return new_obj(h, NTH_EVENT, state, 1, type == 0 /* NotificationEvent */);
}
EXPORT(NtCreateEvent);

static struct nt_obj *get(u64 h, enum nt_kind k)
{
	struct nt_obj *o = nt_handle_get(h);

	return o && o->kind == k ? o : NULL;
}

static API NTSTATUS w_NtSetEvent(u64 h, s32 *prev)
{
	struct nt_obj *o = get(h, NTH_EVENT);

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (prev)
		*prev = (s32)o->state;
	o->state = 1;
	nt_sync_wake_all();
	return 0;
}
EXPORT(NtSetEvent);

static API NTSTATUS w_NtResetEvent(u64 h, s32 *prev)
{
	struct nt_obj *o = get(h, NTH_EVENT);

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (prev)
		*prev = (s32)o->state;
	o->state = 0;
	return 0;
}
EXPORT(NtResetEvent);
EXPORT_AS("NtClearEvent", w_NtResetEvent);

static API NTSTATUS w_NtPulseEvent(u64 h, s32 *prev)
{
	NTSTATUS s = w_NtSetEvent(h, prev);

	if (!s)
		w_NtResetEvent(h, NULL);
	return s;
}
EXPORT(NtPulseEvent);

static API NTSTATUS w_NtCreateMutant(u64 *h, u32 access, void *oa, u8 owned)
{
	NTSTATUS s;

	(void)access;
	(void)oa;
	s = new_obj(h, NTH_MUTANT, owned ? 1 : 0, 0, false);
	if (!s && owned)
		nt_handle_get(*h)->owner = my_tid();
	return s;
}
EXPORT(NtCreateMutant);

static API NTSTATUS w_NtReleaseMutant(u64 h, s32 *prev)
{
	struct nt_obj *o = get(h, NTH_MUTANT);

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (!o->state || o->owner != my_tid())
		return STATUS_MUTANT_NOT_OWNED;
	if (prev)
		*prev = (s32)o->state;
	if (!--o->state)
		o->owner = 0;
	nt_sync_wake_all();
	return 0;
}
EXPORT(NtReleaseMutant);

static API NTSTATUS w_NtCreateSemaphore(u64 *h, u32 access, void *oa, s32 init, s32 max)
{
	(void)access;
	(void)oa;
	if (max <= 0 || init < 0 || init > max)
		return STATUS_INVALID_PARAMETER;
	return new_obj(h, NTH_SEMAPHORE, (u32)init, (u32)max, false);
}
EXPORT(NtCreateSemaphore);

static API NTSTATUS w_NtReleaseSemaphore(u64 h, u32 n, u32 *prev)
{
	struct nt_obj *o = get(h, NTH_SEMAPHORE);

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (o->state + n > o->max)
		return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
	if (prev)
		*prev = o->state;
	o->state += n;
	nt_sync_wake_all();
	return 0;
}
EXPORT(NtReleaseSemaphore);

/* Named objects are process-local: opening by name finds nothing. */
static API NTSTATUS w_NtOpenNamed(u64 *h, u32 access, void *oa)
{
	(void)access;
	(void)oa;
	*h = 0;
	return STATUS_OBJECT_NAME_NOT_FOUND;
}
EXPORT_AS("NtOpenEvent", w_NtOpenNamed);
EXPORT_AS("NtOpenMutant", w_NtOpenNamed);
EXPORT_AS("NtOpenSemaphore", w_NtOpenNamed);
EXPORT_AS("NtOpenSection", w_NtOpenNamed);
EXPORT_AS("NtOpenDirectoryObject", w_NtOpenNamed);
EXPORT_AS("NtOpenSymbolicLinkObject", w_NtOpenNamed);
EXPORT_AS("NtOpenTimer", w_NtOpenNamed);
EXPORT_AS("NtOpenJobObject", w_NtOpenNamed);

/* Is object `o` signalled for this thread? Consume it if so and `take`. */
static int signalled(struct nt_obj *o, bool take)
{
	switch (o->kind) {
	case NTH_EVENT:
		if (!o->state)
			return 0;
		if (take && !o->manual)
			o->state = 0;
		return 1;
	case NTH_SEMAPHORE:
		if (!o->state)
			return 0;
		if (take)
			o->state--;
		return 1;
	case NTH_MUTANT:
		if (o->state && o->owner != my_tid())
			return 0;
		if (take) {
			o->state++;
			o->owner = my_tid();
		}
		return 1;
	case NTH_THREAD: {
		s64 zero = 0;

		return NtWaitForSingleObject(0x40000 + o->tid * 4, 0, &zero) == 0;
	}
	case NTH_FILE:
	case NTH_DIR:
	case NTH_OTHER:
		return 1;
	default:
		return -1;
	}
}

static volatile u32 wait_lock;

NTSTATUS nt_wait_objects(u32 n, const u64 *h, bool all, const s64 *timeout)
{
	struct nt_obj *o[64];
	s64 rel = rel_timeout(timeout), start = 0, now;
	bool has_thread = false;

	if (!n || n > 64)
		return STATUS_INVALID_PARAMETER;
	for (u32 i = 0; i < n; i++) {
		o[i] = nt_handle_get(h[i]);
		if (!o[i]) {
			if (h[i] == (u64)-1)            /* the process itself: never signalled */
				o[i] = NULL;
			else
				return STATUS_INVALID_HANDLE;
		}
		if (o[i] && o[i]->kind == NTH_THREAD)
			has_thread = true;
	}
	NtQueryPerformanceCounter(&start, NULL);
	for (;;) {
		u32 gen = __atomic_load_n(&generation, __ATOMIC_SEQ_CST);
		u32 ready = 0, first = n;

		nt_lock(&wait_lock);
		for (u32 i = 0; i < n; i++) {
			int s = o[i] ? signalled(o[i], false) : 0;

			if (s < 0) {
				nt_unlock(&wait_lock);
				return STATUS_OBJECT_TYPE_MISMATCH;
			}
			if (s) {
				ready++;
				if (first == n)
					first = i;
			}
		}
		if (all ? ready == n : ready > 0) {
			for (u32 i = 0; i < n; i++)
				if (all || i == first)
					signalled(o[i], true);
			nt_unlock(&wait_lock);
			return all ? 0 : first;
		}
		nt_unlock(&wait_lock);
		NtQueryPerformanceCounter(&now, NULL);
		if (rel >= 0 && now - start >= rel)
			return STATUS_TIMEOUT;
		/* Threads end without bumping the counter: poll them every 10 ms. */
		futex_wait(&generation, gen,
			   has_thread ? 100000 : rel < 0 ? -1 : rel - (now - start));
	}
}

static API NTSTATUS w_NtWaitForSingleObject(u64 h, u8 alertable, const s64 *timeout)
{
	struct nt_obj *o = nt_handle_get(h);

	(void)alertable;
	if (!o && h >= 0x40000 && h < 0x80000000ull)    /* raw kernel thread handle */
		return NtWaitForSingleObject(h, 0, (s64 *)timeout);
	return nt_wait_objects(1, &h, false, timeout);
}
EXPORT(NtWaitForSingleObject);

static API NTSTATUS w_NtWaitForMultipleObjects(u32 n, const u64 *h, u32 type, u8 alertable,
					       const s64 *timeout)
{
	(void)alertable;
	return nt_wait_objects(n, h, type == 0 /* WaitAll */, timeout);
}
EXPORT(NtWaitForMultipleObjects);

static API NTSTATUS w_NtSignalAndWaitForSingleObject(u64 sig, u64 wait, u8 alertable,
						     const s64 *timeout)
{
	struct nt_obj *o = nt_handle_get(sig);

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (o->kind == NTH_EVENT)
		w_NtSetEvent(sig, NULL);
	else if (o->kind == NTH_MUTANT)
		w_NtReleaseMutant(sig, NULL);
	else if (o->kind == NTH_SEMAPHORE)
		w_NtReleaseSemaphore(sig, 1, NULL);
	return w_NtWaitForSingleObject(wait, alertable, timeout);
}
EXPORT(NtSignalAndWaitForSingleObject);

static API NTSTATUS w_NtDelayExecution(u8 alertable, const s64 *timeout)
{
	s64 rel = rel_timeout(timeout);

	(void)alertable;
	if (rel < 0)
		for (;;)
			mos_sleep(1000000000ull);
	mos_sleep(rel ? (u64)rel * 100 : 0);
	return 0;
}
EXPORT(NtDelayExecution);

static API NTSTATUS w_NtYieldExecution(void)
{
	mos_sleep(0);
	return 0;
}
EXPORT(NtYieldExecution);

static API NTSTATUS w_NtQueryEvent(u64 h, u32 cls, u32 *buf, u32 len, u32 *ret)
{
	struct nt_obj *o = get(h, NTH_EVENT);

	(void)cls;
	if (!o)
		return STATUS_INVALID_HANDLE;
	if (len < 8)
		return STATUS_INFO_LENGTH_MISMATCH;
	buf[0] = o->manual ? 0 : 1;
	buf[1] = o->state;
	if (ret)
		*ret = 8;
	return 0;
}
EXPORT(NtQueryEvent);

void nt_sync_init(void)
{
}
