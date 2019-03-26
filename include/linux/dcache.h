/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_DCACHE_H
#define __LINUX_DCACHE_H

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/rculist_bl.h>
#include <linux/spinlock.h>
#include <linux/seqlock.h>
#include <linux/cache.h>
#include <linux/rcupdate.h>
#include <linux/lockref.h>
#include <linux/stringhash.h>
#include <linux/wait.h>

struct path;
struct vfsmount;

/*
 * linux/include/linux/dcache.h
 *
 * Dirent cache data structures
 *
 * (C) Copyright 1997 Thomas Schoebel-Theuer,
 * with heavy changes by Linus Torvalds
 */

#define IS_ROOT(x) ((x) == (x)->d_parent)

/* The hash is always the low bits of hash_len */
#ifdef __LITTLE_ENDIAN
 #define HASH_LEN_DECLARE u32 hash; u32 len
 #define bytemask_from_count(cnt)	(~(~0ul << (cnt)*8))
#else
 #define HASH_LEN_DECLARE u32 len; u32 hash
 #define bytemask_from_count(cnt)	(~(~0ul >> (cnt)*8))
#endif

/*
 * "quick string" -- eases parameter passing, but more importantly
 * saves "metadata" about the string (ie length and the hash).
 *
 * hash comes first so it snuggles against d_parent in the
 * dentry.
 */
struct qstr {
	union {
		struct {
			HASH_LEN_DECLARE;
		};
		u64 hash_len;
	};
	const unsigned char *name;
};

#define QSTR_INIT(n,l) { { { .len = l } }, .name = n }

extern const struct qstr empty_name;
extern const struct qstr slash_name;

struct dentry_stat_t {
	long nr_dentry;
	long nr_unused;
	long age_limit;          /* age in seconds */
	long want_pages;         /* pages requested by system */
	long dummy[2];
};
extern struct dentry_stat_t dentry_stat;

/*
 * Try to keep struct dentry aligned on 64 byte cachelines (this will
 * give reasonable cacheline footprint with larger lines without the
 * large memory footprint increase).
 */
#ifdef CONFIG_64BIT
# define DNAME_INLINE_LEN 32 /* 192 bytes */
#else
# ifdef CONFIG_SMP
#  define DNAME_INLINE_LEN 36 /* 128 bytes */
# else
#  define DNAME_INLINE_LEN 40 /* 128 bytes */
# endif
#endif

#define d_lock	d_lockref.lock

struct dentry {
	/* RCU lookup touched fields */
	unsigned int d_flags;		/* protected by d_lock */
	seqcount_t d_seq;		/* per dentry seqlock */
	struct hlist_bl_node d_hash;	/* lookup hash list */
	struct dentry *d_parent;	/* parent directory */
	struct qstr d_name;
	struct inode *d_inode;		/* inode for name - NULL is negative */
	unsigned char d_iname[DNAME_INLINE_LEN];	/* small names */

	/* Ref lookup also touches following */
	struct lockref d_lockref;	/* per-dentry lock and refcount */
	const struct dentry_operations *d_op;
	struct super_block *d_sb;	/* The root of the dentry tree */
	unsigned long d_time;		/* used by d_revalidate */
	void *d_fsdata;			/* fs-specific data */

	union {
		struct list_head d_lru;		/* LRU list */
		wait_queue_head_t *d_wait;	/* in-lookup ones only */
	};
	struct list_head d_child;	/* child of parent list */
	struct list_head d_subdirs;	/* our children */
	/*
	 * d_alias and d_rcu can share memory
	 */
	union {
		struct hlist_node d_alias;	/* inode alias list */
		struct hlist_bl_node d_in_lookup_hash;	/* only for in-lookup ones */
	 	struct rcu_head d_rcu;
	} d_u;
} __randomize_layout;

/*
 * dentry->d_lock spinlock nesting subclasses:
 *
 * 0: normal
 * 1: nested
 */
enum dentry_d_lock_class
{
	DENTRY_D_LOCK_NORMAL, /* implicitly used by plain spin_lock() APIs. */
	DENTRY_D_LOCK_NESTED
};

/**
 * struct dentry_operations - Describe how a filesystem can overload the
 * standard dentry operations.
 *
 * Dentries and the dcache are the domain of the VFS and the individual
 * filesystem implementations.  Device drivers have no business here.
 * These methods may be set to %NULL, as they are either optional or the
 * VFS uses a default.
 *
 * Each dentry has a pointer to its parent dentry, as well as a hash
 * list of child dentries.  Child dentries are basically like files in a
 * directory.
 */
struct dentry_operations {
	/**
	 * @d_revalidate: Called when the VFS needs to revalidate a
	 * dentry.  This is called whenever a name look-up finds a
	 * dentry in the dcache.  Most local filesystems leave this as
	 * %NULL, because all their dentries in the dcache are valid.
	 * Network filesystems are different since things can change on
	 * the server without the client necessarily being aware of it.
	 *
	 * This function should return a positive value if the dentry is
	 * still valid, and zero or a negative error code if it isn't.
	 *
	 * d_revalidate may be called in rcu-walk mode (flags &
	 * LOOKUP_RCU).  If in rcu-walk mode, the filesystem must
	 * revalidate the dentry without blocking or storing to the
	 * dentry, d_parent and d_inode should not be used without care
	 * (because they can change and, in d_inode case, even become
	 * %NULL under us).
	 *
	 * If a situation is encountered that rcu-walk
	 * cannot handle, return -ECHILD and it will be called again
	 * in ref-walk mode.
	 */
	int (*d_revalidate)(struct dentry *, unsigned int);

	/**
	 * @d_weak_revalidate: Called when the VFS needs to revalidate a
	 * "jumped" dentry.  This is called when a path-walk ends at
	 * dentry that was not acquired by doing a lookup in the parent
	 * directory.  This includes "/", "." and "..", as well as
	 * procfs-style symlinks and mountpoint traversal.
	 *
	 * In this case, we are less concerned with whether the dentry
	 * is still fully correct, but rather that the inode is still
	 * valid.  As with d_revalidate, most local filesystems will set
	 * this to %NULL since their dcache entries are always valid.
	 *
	 * This function has the same return code semantics as
	 * d_revalidate.
	 *
	 * d_weak_revalidate is only called after leaving rcu-walk mode.
	 */
	int (*d_weak_revalidate)(struct dentry *, unsigned int);

	/**
	 * @d_hash: Called when the VFS adds a dentry to the hash table.
	 * The first dentry passed to d_hash is the parent directory
	 * that the name is to be hashed into.
	 *
	 * Same locking and synchronisation rules as d_compare regarding
	 * what is safe to dereference etc.
	 */
	int (*d_hash)(const struct dentry *, struct qstr *);

	/**
	 * @d_compare: Called to compare a dentry name with a given name.  The first
	 * dentry is the parent of the dentry to be compared, the second is
	 * the child dentry.  len and name string are properties of the dentry
	 * to be compared.  qstr is the name to compare it with.
	 *
	 * Must be constant and idempotent, and should not take locks if
	 * possible, and should not or store into the dentry.
	 * Should not dereference pointers outside the dentry without
	 * lots of care (eg.  d_parent, d_inode, d_name should not be used).
	 *
	 * However, our vfsmount is pinned, and RCU held, so the dentries and
	 * inodes won't disappear, neither will our sb or filesystem module.
	 * ->d_sb may be used.
	 *
	 * It is a tricky calling convention because it needs to be called under
	 * "rcu-walk", ie. without any locks or references on things.
	 */
	int (*d_compare)(const struct dentry *,
			unsigned int, const char *, const struct qstr *);

	/**
	 * @d_delete: Called when the last reference to a dentry is
	 * dropped and the dcache is deciding whether or not to cache
	 * it.  Return 1 to delete immediately, or 0 to cache the
	 * dentry.  Default is %NULL which means to always cache a
	 * reachable dentry.  d_delete() must be constant and idempotent.
	 */
	int (*d_delete)(const struct dentry *);

	/**
	 * @d_init: Called when a dentry is allocated.
	 */
	int (*d_init)(struct dentry *);

	/**
	 * @d_release: Called when a dentry is really deallocated.
	 */
	void (*d_release)(struct dentry *);

	/**
	 * @d_prune: Called by the VFS to inform the fs that this dentry
	 * is about to be unhashed and destroyed.
	 */
	void (*d_prune)(struct dentry *);

	/**
	 * @d_iput: Called when a dentry loses its inode (just prior to
	 * its being deallocated).  The default when this is %NULL is
	 * that the VFS calls iput().  If you define this method, you
	 * must call iput() yourself.
	 */
	void (*d_iput)(struct dentry *, struct inode *);

	/**
	 * @d_dname: Called when the pathname of a dentry should be
	 * generated.  Useful for some pseudo filesystems (sockfs,
	 * pipefs, ...) to delay pathname generation.  (Instead of doing
	 * it when dentry is created, it's done only when the path is
	 * needed.).  Real filesystems probably dont want to use it,
	 * because their dentries are present in global dcache hash, so
	 * their hash should be an invariant.  As no lock is held,
	 * d_dname() should not try to modify the dentry itself, unless
	 * appropriate SMP safety is used.  CAUTION : d_path() logic is
	 * quite tricky.  The correct way to return for example "Hello"
	 * is to put it at the end of the buffer, and returns a pointer
	 * to the first char.  dynamic_dname() helper function is
	 * provided to take care of this.  (See vfs.rst for an example.)
	 */
	char *(*d_dname)(struct dentry *, char *, int);

	/**
	 * @d_automount: Called when an automount dentry is to be
	 * traversed (optional).  This should create a new VFS mount
	 * record and return the record to the caller.  The caller is
	 * supplied with a path parameter giving the automount directory
	 * to describe the automount target and the parent VFS mount
	 * record to provide inheritable mount parameters.  %NULL should
	 * be returned if someone else managed to make the automount
	 * first.  If the vfsmount creation failed, then an error code
	 * should be returned.  If -EISDIR is returned, then the
	 * directory will be treated as an ordinary directory and
	 * returned to pathwalk to continue walking.  If a vfsmount is
	 * returned, the caller will attempt to mount it on the
	 * mountpoint and will remove the vfsmount from its expiration
	 * list in the case of failure.  The vfsmount should be returned
	 * with 2 refs on it to prevent automatic expiration - the
	 * caller will clean up the additional ref.  This function is
	 * only used if DCACHE_NEED_AUTOMOUNT is set on the dentry.
	 * This is set by __d_instantiate() if S_AUTOMOUNT is set on the
	 * inode being added.
	 */
	struct vfsmount *(*d_automount)(struct path *);

	/**
	 * @d_manage: Called to allow the filesystem to manage the
	 * transition from a dentry (optional).  This allows autofs, for
	 * example, to hold up clients waiting to explore behind a
	 * 'mountpoint' while letting the daemon go past and construct
	 * the subtree there.  0 should be returned to let the calling
	 * process continue.  -EISDIR can be returned to tell pathwalk
	 * to use this directory as an ordinary directory and to ignore
	 * anything mounted on it and not to check the automount flag.
	 * Any other error code will abort pathwalk completely.  If the
	 * 'rcu_walk' parameter is true, then the caller is doing a
	 * pathwalk in RCU-walk mode.  Sleeping is not permitted in this
	 * mode, and the caller can be asked to leave it and call again
	 * by returning -* ECHILD.  -EISDIR may also be returned to tell
	 * pathwalk to ignore d_automount or any mounts.  This function
	 * is only used if DCACHE_MANAGE_TRANSIT is set on the dentry
	 * being transited from.
	 */
	int (*d_manage)(const struct path *, bool);

	/**
	 * @d_real: overlay/union type filesystems implement this method
	 * to return one of the underlying dentries hidden by the
	 * overlay.  It is used in two different modes: Called from
	 * file_dentry() it returns the real dentry matching the inode
	 * argument.  The real dentry may be from a lower layer already
	 * copied up, but still referenced from the file.  This mode is
	 * selected with a non-NULL inode argument.  With %NULL inode
	 * the topmost real underlying dentry is returned.
	 */
	struct dentry *(*d_real)(struct dentry *, const struct inode *);
} ____cacheline_aligned;

/*
 * Locking rules for dentry_operations callbacks are to be found in
 * Documentation/filesystems/Locking. Keep it updated!
 *
 * Further descriptions are found in Documentation/filesystems/vfs.txt.
 * Keep it updated too!
 */

/* d_flags entries */
#define DCACHE_OP_HASH			0x00000001
#define DCACHE_OP_COMPARE		0x00000002
#define DCACHE_OP_REVALIDATE		0x00000004
#define DCACHE_OP_DELETE		0x00000008
#define DCACHE_OP_PRUNE			0x00000010
/*
 * This dentry is possibly not currently connected to the dcache tree,
 * in which case its parent will either be itself, or will have this
 * flag as well.  nfsd will not use a dentry with this bit set, but will
 * first endeavour to clear the bit either by discovering that it is
 * connected, or by performing lookup operations.  Any filesystem which
 * supports nfsd_operations MUST have a lookup function which, if it
 * finds a directory inode with a DCACHE_DISCONNECTED dentry, will
 * d_move that dentry into place and return that dentry rather than the
 * passed one, typically using d_splice_alias.
 */
#define	DCACHE_DISCONNECTED		0x00000020

#define DCACHE_REFERENCED		0x00000040 /* Recently used, don't discard. */
#define DCACHE_RCUACCESS		0x00000080 /* Entry has ever been RCU-visible */

#define DCACHE_CANT_MOUNT		0x00000100
#define DCACHE_GENOCIDE			0x00000200
#define DCACHE_SHRINK_LIST		0x00000400

#define DCACHE_OP_WEAK_REVALIDATE	0x00000800
/* This dentry has been "silly renamed" and has to be deleted on the last dput(). */
#define DCACHE_NFSFS_RENAMED		0x00001000
#define DCACHE_COOKIE			0x00002000 /* For use by dcookie subsystem */
/* Parent inode is watched by some fsnotify listener. */
#define DCACHE_FSNOTIFY_PARENT_WATCHED	0x00004000

#define DCACHE_DENTRY_KILLED		0x00008000

#define DCACHE_MOUNTED			0x00010000 /* Is a mountpoint */
#define DCACHE_NEED_AUTOMOUNT		0x00020000 /* Handle automount on this dir */
#define DCACHE_MANAGE_TRANSIT		0x00040000 /* Manage transit from this dirent */
#define DCACHE_MANAGED_DENTRY \
	(DCACHE_MOUNTED|DCACHE_NEED_AUTOMOUNT|DCACHE_MANAGE_TRANSIT)

#define DCACHE_LRU_LIST			0x00080000

#define DCACHE_ENTRY_TYPE		0x00700000
#define DCACHE_MISS_TYPE		0x00000000 /* Negative dentry (maybe fallthru to nowhere) */
#define DCACHE_WHITEOUT_TYPE		0x00100000 /* Whiteout dentry (stop pathwalk) */
#define DCACHE_DIRECTORY_TYPE		0x00200000 /* Normal directory */
#define DCACHE_AUTODIR_TYPE		0x00300000 /* Lookupless directory (presumed automount) */
#define DCACHE_REGULAR_TYPE		0x00400000 /* Regular file type (or fallthru to such) */
#define DCACHE_SPECIAL_TYPE		0x00500000 /* Other file type (or fallthru to such) */
#define DCACHE_SYMLINK_TYPE		0x00600000 /* Symlink (or fallthru to such) */

#define DCACHE_MAY_FREE			0x00800000
#define DCACHE_FALLTHRU			0x01000000 /* Fall through to lower layer */
#define DCACHE_ENCRYPTED_WITH_KEY	0x02000000 /* dir is encrypted with a valid key */
#define DCACHE_OP_REAL			0x04000000

#define DCACHE_PAR_LOOKUP		0x10000000 /* being looked up (with parent locked shared) */
#define DCACHE_DENTRY_CURSOR		0x20000000

extern seqlock_t rename_lock;

/*
 * These are the low-level FS interfaces to the dcache..
 */
extern void d_instantiate(struct dentry *, struct inode *);
extern void d_instantiate_new(struct dentry *, struct inode *);
extern struct dentry * d_instantiate_unique(struct dentry *, struct inode *);
extern struct dentry * d_instantiate_anon(struct dentry *, struct inode *);
extern void __d_drop(struct dentry *dentry);
extern void d_drop(struct dentry *dentry);
extern void d_delete(struct dentry *);
extern void d_set_d_op(struct dentry *dentry, const struct dentry_operations *op);

/* allocate/de-allocate */
extern struct dentry * d_alloc(struct dentry *, const struct qstr *);
extern struct dentry * d_alloc_anon(struct super_block *);
extern struct dentry * d_alloc_pseudo(struct super_block *, const struct qstr *);
extern struct dentry * d_alloc_parallel(struct dentry *, const struct qstr *,
					wait_queue_head_t *);
extern struct dentry * d_splice_alias(struct inode *, struct dentry *);
extern struct dentry * d_add_ci(struct dentry *, struct inode *, struct qstr *);
extern struct dentry * d_exact_alias(struct dentry *, struct inode *);
extern struct dentry *d_find_any_alias(struct inode *inode);
extern struct dentry * d_obtain_alias(struct inode *);
extern struct dentry * d_obtain_root(struct inode *);
extern void shrink_dcache_sb(struct super_block *);
extern void shrink_dcache_parent(struct dentry *);
extern void shrink_dcache_for_umount(struct super_block *);
extern void d_invalidate(struct dentry *);

/* only used at mount-time */
extern struct dentry * d_make_root(struct inode *);

/* <clickety>-<click> the ramfs-type tree */
extern void d_genocide(struct dentry *);

extern void d_tmpfile(struct dentry *, struct inode *);

extern struct dentry *d_find_alias(struct inode *);
extern void d_prune_aliases(struct inode *);

/* test whether we have any submounts in a subdir tree */
extern int path_has_submounts(const struct path *);

/* This adds the entry to the hash queues. */
extern void d_rehash(struct dentry *);

extern void d_add(struct dentry *, struct inode *);

/* used for rename() and baskets */
extern void d_move(struct dentry *, struct dentry *);
extern void d_exchange(struct dentry *, struct dentry *);
extern struct dentry *d_ancestor(struct dentry *, struct dentry *);

/* appendix may either be NULL or be used for transname suffixes */
extern struct dentry *d_lookup(const struct dentry *, const struct qstr *);
extern struct dentry *d_hash_and_lookup(struct dentry *, struct qstr *);
extern struct dentry *__d_lookup(const struct dentry *, const struct qstr *);
extern struct dentry *__d_lookup_rcu(const struct dentry *parent,
				const struct qstr *name, unsigned *seq);

static inline unsigned d_count(const struct dentry *dentry)
{
	return dentry->d_lockref.count;
}

/* Helper function for dentry_operations.d_dname() members. */
extern __printf(4, 5)
char *dynamic_dname(struct dentry *, char *, int, const char *, ...);
extern char *simple_dname(struct dentry *, char *, int);

extern char *__d_path(const struct path *, const struct path *, char *, int);
extern char *d_absolute_path(const struct path *, char *, int);
extern char *d_path(const struct path *, char *, int);
extern char *dentry_path_raw(struct dentry *, char *, int);
extern char *dentry_path(struct dentry *, char *, int);

/* Allocation counts.. */

/**
 * dget_dlock() - Get a reference to a dentry.
 * @dentry: The dentry to get a reference to.
 *
 * Given a dentry or %NULL pointer increment the reference count if
 * appropriate. A dentry will not be destroyed when it has references.
 *
 * Context: Caller must hold the dentry->d_lock.
 * Return: The dentry.
 */
static inline struct dentry *dget_dlock(struct dentry *dentry)
{
	if (dentry)
		dentry->d_lockref.count++;
	return dentry;
}

/**
 * dget() - Get a reference to a dentry.
 * @dentry: The dentry to get a reference to.
 *
 * Given a dentry or %NULL pointer increment the reference count if
 * appropriate. A dentry will not be destroyed when it has references.
 *
 * Context: Takes the dentry->d_lock.
 * Return: The dentry.
 */
static inline struct dentry *dget(struct dentry *dentry)
{
	if (dentry)
		lockref_get(&dentry->d_lockref);
	return dentry;
}

extern struct dentry *dget_parent(struct dentry *dentry);

/**
 * d_unhashed() - Is dentry unhashed.
 * @dentry: The dentry to check.
 *
 * Return: True if the dentry passed is not currently hashed.
 */
static inline int d_unhashed(const struct dentry *dentry)
{
	return hlist_bl_unhashed(&dentry->d_hash);
}

static inline int d_unlinked(const struct dentry *dentry)
{
	return d_unhashed(dentry) && !IS_ROOT(dentry);
}

static inline int cant_mount(const struct dentry *dentry)
{
	return (dentry->d_flags & DCACHE_CANT_MOUNT);
}

static inline void dont_mount(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	dentry->d_flags |= DCACHE_CANT_MOUNT;
	spin_unlock(&dentry->d_lock);
}

extern void __d_lookup_done(struct dentry *);

static inline int d_in_lookup(const struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_PAR_LOOKUP;
}

static inline void d_lookup_done(struct dentry *dentry)
{
	if (unlikely(d_in_lookup(dentry))) {
		spin_lock(&dentry->d_lock);
		__d_lookup_done(dentry);
		spin_unlock(&dentry->d_lock);
	}
}

extern void dput(struct dentry *);

static inline bool d_managed(const struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_MANAGED_DENTRY;
}

static inline bool d_mountpoint(const struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_MOUNTED;
}

/*
 * Directory cache entry type accessor functions.
 */
static inline unsigned __d_entry_type(const struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_ENTRY_TYPE;
}

static inline bool d_is_miss(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_MISS_TYPE;
}

static inline bool d_is_whiteout(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_WHITEOUT_TYPE;
}

static inline bool d_can_lookup(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_DIRECTORY_TYPE;
}

static inline bool d_is_autodir(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_AUTODIR_TYPE;
}

static inline bool d_is_dir(const struct dentry *dentry)
{
	return d_can_lookup(dentry) || d_is_autodir(dentry);
}

static inline bool d_is_symlink(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_SYMLINK_TYPE;
}

static inline bool d_is_reg(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_REGULAR_TYPE;
}

static inline bool d_is_special(const struct dentry *dentry)
{
	return __d_entry_type(dentry) == DCACHE_SPECIAL_TYPE;
}

static inline bool d_is_file(const struct dentry *dentry)
{
	return d_is_reg(dentry) || d_is_special(dentry);
}

static inline bool d_is_negative(const struct dentry *dentry)
{
	// TODO: check d_is_whiteout(dentry) also.
	return d_is_miss(dentry);
}

static inline bool d_is_positive(const struct dentry *dentry)
{
	return !d_is_negative(dentry);
}

/**
 * d_really_is_negative - Determine if a dentry is really negative (ignoring fallthroughs)
 * @dentry: The dentry in question
 *
 * Returns true if the dentry represents either an absent name or a name that
 * doesn't map to an inode (ie. ->d_inode is NULL).  The dentry could represent
 * a true miss, a whiteout that isn't represented by a 0,0 chardev or a
 * fallthrough marker in an opaque directory.
 *
 * Note!  (1) This should be used *only* by a filesystem to examine its own
 * dentries.  It should not be used to look at some other filesystem's
 * dentries.  (2) It should also be used in combination with d_inode() to get
 * the inode.  (3) The dentry may have something attached to ->d_lower and the
 * type field of the flags may be set to something other than miss or whiteout.
 */
static inline bool d_really_is_negative(const struct dentry *dentry)
{
	return dentry->d_inode == NULL;
}

/**
 * d_really_is_positive - Determine if a dentry is really positive (ignoring fallthroughs)
 * @dentry: The dentry in question
 *
 * Returns true if the dentry represents a name that maps to an inode
 * (ie. ->d_inode is not NULL).  The dentry might still represent a whiteout if
 * that is represented on medium as a 0,0 chardev.
 *
 * Note!  (1) This should be used *only* by a filesystem to examine its own
 * dentries.  It should not be used to look at some other filesystem's
 * dentries.  (2) It should also be used in combination with d_inode() to get
 * the inode.
 */
static inline bool d_really_is_positive(const struct dentry *dentry)
{
	return dentry->d_inode != NULL;
}

static inline int simple_positive(const struct dentry *dentry)
{
	return d_really_is_positive(dentry) && !d_unhashed(dentry);
}

extern void d_set_fallthru(struct dentry *dentry);

static inline bool d_is_fallthru(const struct dentry *dentry)
{
	return dentry->d_flags & DCACHE_FALLTHRU;
}


extern int sysctl_vfs_cache_pressure;

static inline unsigned long vfs_pressure_ratio(unsigned long val)
{
	return mult_frac(val, sysctl_vfs_cache_pressure, 100);
}

/**
 * d_inode() - Get the actual inode of this dentry.
 * @dentry: The dentry to query
 *
 * This is the helper normal filesystems should use to get at their own inodes
 * in their own dentries and ignore the layering superimposed upon them.
 */
static inline struct inode *d_inode(const struct dentry *dentry)
{
	return dentry->d_inode;
}

/**
 * d_inode_rcu() - Get the actual inode of this dentry with READ_ONCE().
 * @dentry: The dentry to query
 *
 * This is the helper normal filesystems should use to get at their own inodes
 * in their own dentries and ignore the layering superimposed upon them.
 */
static inline struct inode *d_inode_rcu(const struct dentry *dentry)
{
	return READ_ONCE(dentry->d_inode);
}

/**
 * d_backing_inode() - Get upper or lower inode we should be using.
 * @upper: The upper layer
 *
 * This is the helper that should be used to get at the inode that will be used
 * if this dentry were to be opened as a file.  The inode may be on the upper
 * dentry or it may be on a lower dentry pinned by the upper.
 *
 * Normal filesystems should not use this to access their own inodes.
 */
static inline struct inode *d_backing_inode(const struct dentry *upper)
{
	struct inode *inode = upper->d_inode;

	return inode;
}

/**
 * d_backing_dentry() - Get upper or lower dentry we should be using.
 * @upper: The upper layer
 *
 * This is the helper that should be used to get the dentry of the inode that
 * will be used if this dentry were opened as a file.  It may be the upper
 * dentry or it may be a lower dentry pinned by the upper.
 *
 * Normal filesystems should not use this to access their own dentries.
 */
static inline struct dentry *d_backing_dentry(struct dentry *upper)
{
	return upper;
}

/**
 * d_real() - Return the real dentry.
 * @dentry: the dentry to query
 * @inode: inode to select the dentry from multiple layers (can be %NULL)
 *
 * If dentry is on a union/overlay, then return the underlying, real dentry.
 * Otherwise return the dentry itself.
 *
 * See also: Documentation/filesystems/vfs.txt
 */
static inline struct dentry *d_real(struct dentry *dentry,
				    const struct inode *inode)
{
	if (unlikely(dentry->d_flags & DCACHE_OP_REAL))
		return dentry->d_op->d_real(dentry, inode);
	else
		return dentry;
}

/**
 * d_real_inode() - Return the real inode.
 * @dentry: The dentry to query
 *
 * If dentry is on a union/overlay, then return the underlying, real inode.
 * Otherwise return d_inode().
 */
static inline struct inode *d_real_inode(const struct dentry *dentry)
{
	/* This usage of d_real() results in const dentry */
	return d_backing_inode(d_real((struct dentry *) dentry, NULL));
}

struct name_snapshot {
	const unsigned char *name;
	unsigned char inline_name[DNAME_INLINE_LEN];
};
void take_dentry_name_snapshot(struct name_snapshot *, struct dentry *);
void release_dentry_name_snapshot(struct name_snapshot *);

#endif	/* __LINUX_DCACHE_H */
