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
/* Used by stress_xarray()*/
DEFINE_XARRAY(stress_xa);

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
			pr_debug("kmem_cache_alloc failded\n");
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
	const int keep = 3; /* Free 4 out of 5 items */
	const int nr_items = 1000;
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

	/* Now free 2/3 of the items, putting holes in both caches */
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
	 *   slabinfo radix_tree_node -s
	 *   slabinfo smox_test -s
	 */

	return 0;
}

/*
 * stress_xarray() - Stress test the xarray cache.
 *
 * Allocate and free a _bunch_ of elements then shrink the
 * radix_tree_node cache.
 */
static int stress_xarray(void)
{
	const int keep = 5; /* Free 4 out of 5 items */
	unsigned long value = 0;
	unsigned long expected;
	unsigned long index;
	int nr_items = 1000;
	void *entry;
	int i;

	for (i = 0; i < nr_items; i++, value++) {
		entry = xa_store(&stress_xa, i, xa_mk_value(value), GFP_KERNEL);
		if (xa_is_err(entry)) {
			pr_err("smox: failed to store at index: %d\n", i);
			return -1;
		}
	}

	for (i = 0; i < nr_items; i++) {
		if (i % keep == 0)
			continue;

		entry = xa_erase(&stress_xa, i);
		if (xa_is_err(entry))
			pr_err("smox: failed dummy erase: %d\n", i);
	}

	expected = 0;
	xa_for_each(&stress_xa, index, entry) {
		value = xa_to_value(entry);
		if (value != expected || (unsigned long)index != expected) {
			pr_err("smox: error; got %ld want %ld at %ld\n",
			       value, expected, index);
			return -1;
		}
		expected += (unsigned long)keep;
	}

	/*
	 * Leave cache sparsely allocated.  Shrink cache manually with:
	 *
	 *   slabinfo radix_tree_node -s
	 *
	 * Unloading module destroys the XArray
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

	pr_info("smo_xarray: module loaded\n");

	/*
	 * Running this test irrevocable consumes memory unless you
	 * shrink the radix_tree_node cache manually with `slabinfo`.
	 */
	ret = test_smo_xarray();
	if (ret)
		pr_warn("test_smo_xarray failed: %d\n", ret);

	/*
	 * Running this test irrevocable consumes even more memory unless
	 * you shrink the radix_tree_node cache manually with `slabinfo`.
	 */
	ret = stress_xarray();
	if (ret)
		pr_warn("test_smo_xarray stress test failed: %d\n", ret);

	return 0;
}
module_init(smox_init);

static void __exit smox_exit(void)
{
	xa_destroy(&stress_xa);	/* stress_xa only stores values */
	smox_cache_cleanup();

	pr_info("smo_xarray: module removed\n");
}
module_exit(smox_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobin C. Harding");
MODULE_DESCRIPTION("SMO XArray test module.");
