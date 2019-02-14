/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * zus.h - C Wrappers over the ZUFS_IOCTL Api
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */
#ifndef __ZUS_H__
#define __ZUS_H__

/* sys/stat.h must be included the very first */
#include <sys/stat.h>

#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <sched.h>

/* On old GCC systems we do not yet have STATX stuff in sys/stat.h
 * so in this case include also the kernel header.
 */
#ifndef STATX_MODE
#include <linux/stat.h>
#endif

/* This is a nasty hack for getting O_TMPFILE into centos 7.4
 * it already exists on centos7.4 but with a diffrent name
 * the value is the same
 * kernel support was introduced in kernel 3.11
 */

#ifndef O_TMPFILE
#define O_TMPFILE (__O_TMPFILE | O_DIRECTORY)
#endif

#include "zus_api.h"
#include "_pr.h"
#include "a_list.h"

#ifndef likely
#define likely(x_)	__builtin_expect(!!(x_), 1)
#define unlikely(x_)	__builtin_expect(!!(x_), 0)
#endif

#include "md.h"
#include "movnt.h"

/* utils.c */
void zus_dump_stack(FILE *, bool warn, const char *fmt, ...);
void zus_warn(const char *cond, const char *file, int line);
void zus_bug(const char *cond, const char *file, int line);


#define dump_stack() \
	zus_dump_stack(stderr, false, "<5>%s: (%s:%d)\n", \
			__func__, __FILE__, __LINE__)

#define ZUS_WARN_ON(x_) ({ \
	int __ret_warn_on = !!(x_); \
	if (unlikely(__ret_warn_on)) \
		zus_warn(#x_, __FILE__, __LINE__); \
	unlikely(__ret_warn_on); \
})

#define ZUS_WARN_ON_ONCE(x_) ({				\
	int __ret_warn_on = !!(x_);			\
	static bool __once;				\
	if (unlikely(__ret_warn_on && !__once))	{	\
		zus_warn(#x_, __FILE__, __LINE__); 	\
		__once = true;				\
	}						\
	unlikely(__ret_warn_on);			\
})

#define ZUS_BUG_ON(x_) ({ \
	int __ret_bug_on = !!(x_); \
	if (unlikely(__ret_bug_on)) \
		zus_bug(#x_, __FILE__, __LINE__); \
	unlikely(__ret_bug_on); \
})

/* ~~~~ zus fs_info super_blocks inodes ~~~~ */

struct zus_fs_info;
struct zus_sb_info;

#define ZUS_MAX_POOLS	7
struct pa {
	struct fba pages;
	struct fba data;
	struct a_list_head head;
	size_t size;
	pthread_spinlock_t lock;
};

struct zus_sb_info {
	ulong			flags;
	struct pa		pa[ZUS_MAX_POOLS];
};

enum E_zus_sbi_flags {
	ZUS_SBIF_ERROR = 0,

	ZUS_SBIF_LAST,
};

static inline void zus_sbi_set_flag(struct zus_sb_info *sbi, int flag)
{
	sbi->flags |= (1 << flag);
}

static inline int zus_sbi_test_flag(struct zus_sb_info *sbi, int flag)
{
	return (sbi->flags & (1 << flag));
}

struct zus_zfi_operations {
	struct zus_sb_info *(*sbi_alloc)(struct zus_fs_info *zfi);
	void (*sbi_free)(struct zus_sb_info *sbi);

	int (*sbi_init)(struct zus_sb_info *sbi, struct zufs_ioc_mount *zim);
	int (*sbi_fini)(struct zus_sb_info *sbi);
	int (*sbi_remount)(struct zus_sb_info *sbi, struct zufs_ioc_mount *zim);
};

struct zus_fs_info {
	struct register_fs_info rfi;
	const struct zus_zfi_operations *op;
	const struct zus_sbi_operations *sbi_op;

	uint			user_page_size;
	uint			next_sb_id;
};

/* printz.c */
int zus_add_module_ddbg(const char *fs_name, void *handle);
void zus_free_ddbg_db(void);
int zus_ddbg_read(struct zufs_ddbg_info *zdi);
int zus_ddbg_write(struct zufs_ddbg_info *zdi);

/* pa.c */

/* fba - File backed Allocator
 * Gives user an allocated pointer which is derived from a /tmp/O_TMPFILE mmap.
 * The size is round up to 4K alignment.
 */
int  fba_alloc(struct fba *fba, size_t size);
int  fba_alloc_align(struct fba *fba, size_t size);
void fba_free(struct fba *fba);
int fba_punch_hole(struct fba *fba, ulong index, uint nump);

/* pa - Page Allocator
 * Gives users a page Allocator pa_pages are ref-counted last put
 * frees the page. (pa_free is just a pa_put_page
 */
int pa_init(struct zus_sb_info *sbi);
void pa_fini(struct zus_sb_info *sbi);

struct pa_page {
	unsigned long		flags;
	void			*owner;

	unsigned long		index;
	int			units;
	int			refcount;

	struct a_list_head	list;

	unsigned long		private;
	void			*private2;
} __aligned(64);


#define PA_MAX_ORDER 5

struct pa_page *pa_alloc_order(struct zus_sb_info *sbi, int order);
static inline struct pa_page *pa_alloc(struct zus_sb_info *sbi)
{
	return pa_alloc_order(sbi, 0);
}

/* Must not be used by users, private to pa implementation */
void __pa_free(struct pa_page *page);

#define ZONE_BITLEN	4
#define ZONE_SHIFT	(64 - ZONE_BITLEN) /* last 4 bits */
#define ZONE_MASK	(((1UL << ZONE_BITLEN)-1) << ZONE_SHIFT)

#define NODES_BITLEN	4
#define NODES_PGSHIFT	(ZONE_SHIFT - NODES_BITLEN) /* 4 bits */
#define NODES_MASK	(((1UL << NODES_BITLEN)-1) << NODES_PGSHIFT)

static inline ulong get_bit_range(ulong x, int start_bit, ulong mask)
{
	return (x & mask) >> start_bit;
}

static inline void set_bit_range(ulong *x, int start_bit, ulong mask, ulong val)
{
	*x &= ~mask;
	*x |= (val << start_bit);
}

static inline void pa_set_page_zone(struct pa_page *page, int zone)
{
	ZUS_WARN_ON_ONCE(zone >> ZONE_BITLEN);
	set_bit_range(&page->flags, ZONE_SHIFT, ZONE_MASK, zone);
}

static inline int pa_page_zone(struct pa_page *page)
{
	return get_bit_range(page->flags, ZONE_SHIFT, ZONE_MASK);
}

static inline void pa_page_nid_set(struct pa_page *page, unsigned long node)
{
	ZUS_WARN_ON_ONCE(node >> NODES_BITLEN);
	set_bit_range(&page->flags, NODES_PGSHIFT, NODES_MASK, node);
}

static inline int pa_page_to_nid(struct pa_page *page)
{
	return get_bit_range(page->flags, NODES_PGSHIFT, NODES_MASK);
}

static inline int _zu_atomic_add_unless(int *v, int a, int u)
{
	int c, old;

	c = __atomic_load_n(v, __ATOMIC_SEQ_CST);
	old = c;
	while (c != u) {
		if (__atomic_compare_exchange_n(v, &old, c + a, false,
						__ATOMIC_SEQ_CST,
						__ATOMIC_SEQ_CST))
			break;

		c = old;
	}
	return c;
}

static inline int __atomic_dec_testzero(int *v)
{
	return (__atomic_fetch_sub(v, 1, __ATOMIC_SEQ_CST) == 1);
}

/* Return the incremented refcount, unless 0 */
static inline int pa_get_page(struct pa_page *page)
{
	return _zu_atomic_add_unless(&page->refcount, 1, 0);
}

static inline int pa_page_count(struct pa_page *page)
{
	return __atomic_load_n(&page->refcount, __ATOMIC_SEQ_CST);
}

/* Return true if the refcount droped to 0 */
static inline int pa_put_page(struct pa_page *page)
{
	if (__atomic_dec_testzero(&page->refcount)) {
		__pa_free(page);
		return 1;
	}
	return 0;
}

static inline void pa_free(struct zus_sb_info *sbi, struct pa_page *page)
{
	pa_put_page(page);
}

/* TODO: let the user decide on the pool number */
#define POOL_NUM	1

static inline struct pa_page *pa_bn_to_page(struct zus_sb_info *sbi, ulong bn)
{
	return sbi->pa[POOL_NUM].pages.ptr + bn * sizeof(struct pa_page);
}

static inline ulong pa_page_to_bn(struct zus_sb_info *sbi, struct pa_page *page)
{
	return ((void *)page - sbi->pa[POOL_NUM].pages.ptr) / sizeof(*page);
}

static inline void *pa_page_address(struct zus_sb_info *sbi,
				    struct pa_page *page)
{
	ulong bn = pa_page_to_bn(sbi, page);

	return sbi->pa[POOL_NUM].data.ptr + bn * PAGE_SIZE;
}

static inline bool _pa_valid_addr(struct pa *pa, void *addr)
{
	if (ZUS_WARN_ON((addr < pa->data.ptr) ||
			((pa->data.ptr + pa->size * PAGE_SIZE) <= addr))) {
		ERROR("Invalid address=%p data.ptr=%p data.end=%p\n",
		      addr, pa->data.ptr,
		      (pa->data.ptr + pa->size * PAGE_SIZE));
		return false;
	}
	return true;
}

static inline
struct pa_page *pa_virt_to_page(struct zus_sb_info *sbi, void *addr)
{
	struct pa *pa = &sbi->pa[POOL_NUM];

	if (!_pa_valid_addr(pa, addr))
		return NULL;

	return pa_bn_to_page(sbi, md_o2p(addr - pa->data.ptr));
}

static inline
ulong pa_addr_to_offset(struct zus_sb_info *sbi, void *addr)
{
	struct pa *pa = &sbi->pa[POOL_NUM];

	if (!_pa_valid_addr(pa, addr))
		return 0;

	return (addr - pa->data.ptr);
}

static inline void *pa_addr(struct zus_sb_info *sbi, ulong offset)
{
	struct pa *pa = &sbi->pa[POOL_NUM];

	return offset ? pa->data.ptr + offset : NULL;
}

#define ZUS_LIBFS_MAX_NR	16	/* see also MAX_LOCKDEP_FSs in zuf */
#define ZUS_LIBFS_MAX_PATH	256

#endif /* define __ZUS_H__ */
