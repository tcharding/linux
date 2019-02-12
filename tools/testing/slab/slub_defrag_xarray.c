// SPDX-License-Identifier: GPL-2.0+
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/gfp.h>
#include <linux/xarray.h>

#define SMOX_CACHE_NAME "smox_test"
static struct kmem_cache *cachep;

/*
 * Declare XArrays globally so we can clean them up on module unload.
 */

/* Used by test_smo_xarray()*/
DEFINE_XARRAY(things);

/* Thing to store pointers to in the XArray */
struct smox_thing {
	long id;
};

/* It's up to the caller to ensure id is unique */
static struct smox_thing *alloc_thing(int id)
{
	struct smox_thing *thing;

	thing = kmem_cache_alloc(cachep, GFP_KERNEL);
	if (!thing)
		return ERR_PTR(-ENOMEM);

	thing->id = id;
	return thing;
}

/**
 * smox_object_ctor() - SMO object constructor function.
 * @ptr: Pointer to memory where the object should be constructed.
 */
void smox_object_ctor(void *ptr)
{
	struct smox_thing *thing = ptr;

	thing->id = -1;
}

/**
 * smox_cache_migrate() - kmem_cache migrate function.
 * @cp: kmem_cache pointer.
 * @objs: Array of pointers to objects to migrate.
 * @size: Number of objects in @objs.
 * @node: NUMA node where the object should be allocated.
 * @private: Pointer returned by kmem_cache_isolate_func().
 */
void smox_cache_migrate(struct kmem_cache *cp, void **objs, int size,
			int node, void *private)
{
	struct smox_thing **ptrs = (struct smox_thing **)objs;
	struct smox_thing *old, *new;
	struct smox_thing *thing;
	unsigned long index;
	void *entry;
	int i;

	for (i = 0; i < size; i++) {
		old = ptrs[i];

		new = kmem_cache_alloc(cachep, GFP_KERNEL);
		if (!new) {
			pr_debug("kmem_cache_alloc failed\n");
			return;
		}

		new->id = old->id;

		/* Update reference the brain dead way */
		xa_for_each(&things, index, thing) {
			if (thing == old) {
				entry = xa_store(&things, index, new, GFP_KERNEL);
				if (entry != old) {
					pr_err("failed to exchange new/old\n");
					return;
				}
			}
		}
		kmem_cache_free(cachep, old);
	}
}

/*
 * test_smo_xarray() - Run some tests using an XArray.
 */
static int test_smo_xarray(void)
{
	const int keep = 6; /* Free 5 out of 6 items */
	const int nr_items = 10000;
	struct smox_thing *thing;
	unsigned long index;
	void *entry;
	int expected;
	int i;

	/*
	 * Populate XArray, this adds to the radix_tree_node cache as
	 * well as the smox_test cache.
	 */
	for (i = 0; i < nr_items; i++) {
		thing = alloc_thing(i);
		entry = xa_store(&things, i, thing, GFP_KERNEL);
		if (xa_is_err(entry)) {
			pr_err("smox: failed to allocate entry: %d\n", i);
			return -ENOMEM;
		}
	}

	/* Now free  items, putting holes in both caches. */
	for (i = 0; i < nr_items; i++) {
		if (i % keep == 0)
			continue;

		thing = xa_erase(&things, i);
		if (xa_is_err(thing))
			pr_err("smox: error erasing entry: %d\n", i);
		kmem_cache_free(cachep, thing);
	}

	expected = 0;
	xa_for_each(&things, index, thing) {
		if (thing->id != expected || index != expected) {
			pr_err("smox: error; got %ld want %d at %ld\n",
			       thing->id, expected, index);
			return -1;
		}
		expected += keep;
	}

	/*
	 * Leave caches sparsely allocated.  Shrink caches manually with:
	 *
	 *   slabinfo radix_tree_node --shrink
	 *   slabinfo smox_test --shrink
	 */

	return 0;
}

static int __init smox_cache_init(void)
{
	cachep = kmem_cache_create(SMOX_CACHE_NAME,
				   sizeof(struct smox_thing),
				   0, 0, smox_object_ctor);
	if (!cachep)
		return -1;

	return 0;
}

static void __exit smox_cache_cleanup(void)
{
	struct smox_thing *thing;
	unsigned long i;

	xa_for_each(&things, i, thing) {
		kmem_cache_free(cachep, thing);
	}
	xa_destroy(&things);
	kmem_cache_destroy(cachep);
}

static int __init smox_init(void)
{
	int ret;

	ret = smox_cache_init();
	if (ret) {
		pr_err("smo_xarray: failed to create cache\n");
		return ret;
	}
	pr_info("smo_xarray: created kmem_cache: %s\n", SMOX_CACHE_NAME);

	kmem_cache_setup_mobility(cachep, NULL, smox_cache_migrate);
	pr_info("smo_xarray: kmem_cache %s defrag enabled\n", SMOX_CACHE_NAME);

	/*
	 * Running this test consumes memory unless you shrink the
	 * radix_tree_node cache manually with `slabinfo`.
	 */
	ret = test_smo_xarray();
	if (ret)
		pr_warn("test_smo_xarray failed: %d\n", ret);

	pr_info("smo_xarray: module loaded successfully\n");
	return 0;
}
module_init(smox_init);

static void __exit smox_exit(void)
{
	smox_cache_cleanup();

	pr_info("smo_xarray: module removed\n");
}
module_exit(smox_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobin C. Harding");
MODULE_DESCRIPTION("SMO XArray test module.");
