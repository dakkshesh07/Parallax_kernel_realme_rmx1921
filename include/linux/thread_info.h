/* thread_info.h: common low-level thread information accessors
 *
 * Copyright (C) 2002  David Howells (dhowells@redhat.com)
 * - Incorporating suggestions made by Linus Torvalds
 */

#ifndef _LINUX_THREAD_INFO_H
#define _LINUX_THREAD_INFO_H

#include <linux/types.h>
#include <linux/bug.h>
#include <linux/restart_block.h>
#include <linux/errno.h>

struct timespec;
struct compat_timespec;

#ifdef CONFIG_THREAD_INFO_IN_TASK
/*
 * For CONFIG_THREAD_INFO_IN_TASK kernels we need <asm/current.h> for the
 * definition of current, but for !CONFIG_THREAD_INFO_IN_TASK kernels,
 * including <asm/current.h> can cause a circular dependency on some platforms.
 */
#include <asm/current.h>
#define current_thread_info() ((struct thread_info *)current)
#endif

#include <linux/bitops.h>
#include <asm/thread_info.h>

#ifdef __KERNEL__

#ifndef arch_set_restart_data
#define arch_set_restart_data(restart) do { } while (0)
#endif

static inline long set_restart_fn(struct restart_block *restart,
					long (*fn)(struct restart_block *))
{
	restart->fn = fn;
	arch_set_restart_data(restart);
	return -ERESTART_RESTARTBLOCK;
}

#define THREADINFO_GFP	(GFP_KERNEL_ACCOUNT | __GFP_NOTRACK | __GFP_ZERO)

/*
 * flag set/clear/test wrappers
 * - pass TIF_xxxx constants to these functions
 */

static inline void set_ti_thread_flag(struct thread_info *ti, int flag)
{
	/* set_bit() with release semantics */
	smp_mb__before_atomic();
	set_bit(flag, (unsigned long *)&ti->flags);
}

static inline void clear_ti_thread_flag(struct thread_info *ti, int flag)
{
	/* clear_bit() with release semantics */
	smp_mb__before_atomic();
	clear_bit(flag, (unsigned long *)&ti->flags);
}

static inline int test_and_set_ti_thread_flag(struct thread_info *ti, int flag)
{
	atomic_long_t *p = (atomic_long_t *)&ti->flags;
	const unsigned long mask = BIT_MASK(flag);
	long old;

	/* test_and_set_bit() sans the unordered test */
	p += BIT_WORD(flag);
	old = atomic_long_fetch_or(mask, p);
	return !!(old & mask);
}

static inline int test_and_clear_ti_thread_flag(struct thread_info *ti, int flag)
{
	atomic_long_t *p = (atomic_long_t *)&ti->flags;
	const unsigned long mask = BIT_MASK(flag);
	long old;

	/* test_and_clear_bit() sans the unordered test */
	p += BIT_WORD(flag);
	old = atomic_long_fetch_andnot(mask, p);
	return !!(old & mask);
}

static inline int test_ti_thread_flag(struct thread_info *ti, int flag)
{
	const atomic_long_t *p = (atomic_long_t *)&ti->flags;

	/* test_bit() with acquire semantics */
	p += BIT_WORD(flag);
	return !!(atomic_long_read_acquire(p) & BIT_MASK(flag));
}

#define set_thread_flag(flag) \
	set_ti_thread_flag(current_thread_info(), flag)
#define clear_thread_flag(flag) \
	clear_ti_thread_flag(current_thread_info(), flag)
#define test_and_set_thread_flag(flag) \
	test_and_set_ti_thread_flag(current_thread_info(), flag)
#define test_and_clear_thread_flag(flag) \
	test_and_clear_ti_thread_flag(current_thread_info(), flag)
#define test_thread_flag(flag) \
	test_ti_thread_flag(current_thread_info(), flag)

#define tif_need_resched() test_thread_flag(TIF_NEED_RESCHED)

#ifndef CONFIG_HAVE_ARCH_WITHIN_STACK_FRAMES
static inline int arch_within_stack_frames(const void * const stack,
					   const void * const stackend,
					   const void *obj, unsigned long len)
{
	return 0;
}
#endif

#ifdef CONFIG_HARDENED_USERCOPY
extern void __check_object_size(const void *ptr, unsigned long n,
					bool to_user);

static __always_inline void check_object_size(const void *ptr, unsigned long n,
					      bool to_user)
{
	if (!__builtin_constant_p(n))
		__check_object_size(ptr, n, to_user);
}
#else
static inline void check_object_size(const void *ptr, unsigned long n,
				     bool to_user)
{ }
#endif /* CONFIG_HARDENED_USERCOPY */

#endif	/* __KERNEL__ */

#endif /* _LINUX_THREAD_INFO_H */
