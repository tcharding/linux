// SPDX-License-Identifier: GPL-2.0+
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/gfp.h>
#include <linux/debugfs.h>
#include <linux/numa.h>

/*
 * SLUB defragmentation a.k.a. Slab Movable Objects (SMO).
 *
 * This module is used for testing the SLUB allocator.  Enables
 * userspace to run kernel functions via a debugfs file.
 *
 *   debugfs: /sys/kernel/debugfs/smo/callfn (write only)
 *
 * String written to `callfn` is parsed by the module and associated
 * function is called.  See fn_tab for mapping of strings to functions.
 */

/* debugfs commands accept two optional arguments */
#define SMO_CMD_DEFAUT_ARG -1

#define SMO_DEBUGFS_DIR "smo"
struct dentry *smo_debugfs_root;

#define SMO_CACHE_NAME "smo_test"
static struct kmem_cache *cachep;

struct smo_slub_object {
	struct list_head list;
	char buf[32];		/* Unused except to control size of object */
	long id;
};

/* Our list of allocated objects */
LIST_HEAD(objects);

static void list_add_to_objects(struct smo_slub_object *so)
{
	/*
	 * We free from the front of the list so store at the
	 * tail in order to put holes in the cache when we free.
	 */
	list_add_tail(&so->list, &objects);
}

/**
 * smo_object_ctor() - SMO object constructor function.
 * @ptr: Pointer to memory where the object should be constructed.
 */
void smo_object_ctor(void *ptr)
{
	struct smo_slub_object *so = ptr;

	INIT_LIST_HEAD(&so->list);
	memset(so->buf, 0, sizeof(so->buf));
	so->id = -1;
}

/**
 * smo_cache_migrate() - kmem_cache migrate function.
 * @cp: kmem_cache pointer.
 * @objs: Array of pointers to objects to migrate.
 * @size: Number of objects in @objs.
 * @node: NUMA node where the object should be allocated.
 * @private: Pointer returned by kmem_cache_isolate_func().
 */
void smo_cache_migrate(struct kmem_cache *cp, void **objs, int size,
		       int node, void *private)
{
	struct smo_slub_object **so_objs = (struct smo_slub_object **)objs;
	struct smo_slub_object *so_old, *so_new;
	int i;

	for (i = 0; i < size; i++) {
		so_old = so_objs[i];

		so_new = kmem_cache_alloc_node(cachep, GFP_KERNEL, node);
		if (!so_new) {
			pr_debug("kmem_cache_alloc failed\n");
			return;
		}

		/* Copy object */
		so_new->id = so_old->id;

		/* Update references to old object */
		list_del(&so_old->list);
		list_add_to_objects(so_new);

		kmem_cache_free(cachep, so_old);
	}
}

static int smo_enable_cache_mobility(int _unused, int __unused)
{
	/* Enable movable objects: BOOM! */
	kmem_cache_setup_mobility(cachep, NULL, smo_cache_migrate);
	pr_info("smo: kmem_cache %s defrag enabled\n", SMO_CACHE_NAME);
	return 0;
}

/*
 * smo_alloc_objects() - Allocate objects and store reference.
 * @nr_objs: Number of objects to allocate.
 * @node: NUMA node to allocate objects on.
 *
 * Allocates @n smo_slub_objects.  Stores a reference to them in
 * the global list of objects (at the tail of the list).
 *
 * Return: The number of objects allocated.
 */
static int smo_alloc_objects(int nr_objs, int node)
{
	struct smo_slub_object *so;
	int i;

	/* Set sane parameters if no args passed in */
	if (nr_objs == SMO_CMD_DEFAUT_ARG)
		nr_objs = 1;
	if (node == SMO_CMD_DEFAUT_ARG)
		node = NUMA_NO_NODE;

	for (i = 0; i < nr_objs; i++) {
		if (node == NUMA_NO_NODE)
			so = kmem_cache_alloc(cachep, GFP_KERNEL);
		else
			so = kmem_cache_alloc_node(cachep, GFP_KERNEL, node);
		if (!so) {
			pr_err("smo: Failed to alloc object %d of %d\n", i, nr_objs);
			return i;
		}
		list_add_to_objects(so);
	}
	return nr_objs;
}

/*
 * smo_free_object() - Frees n objects from position.
 * @nr_objs: Number of objects to free.
 * @pos: Position in global list to start freeing.
 *
 * Iterates over the global list of objects to position @pos then frees @n
 * objects from there (or to end of list).  Does nothing if @n > list length.
 *
 * Calling with @n==0 frees all objects starting at @pos.
 *
 * Return: Number of objects freed.
 */
static int smo_free_object(int nr_objs, int pos)
{
	struct smo_slub_object *cur, *tmp;
	int deleted = 0;
	int i = 0;

	/* Set sane parameters if no args passed in */
	if (nr_objs == SMO_CMD_DEFAUT_ARG)
		nr_objs = 1;
	if (pos == SMO_CMD_DEFAUT_ARG)
		pos = 0;

	list_for_each_entry_safe(cur, tmp, &objects, list) {
		if (i < pos) {
			i++;
			continue;
		}

		list_del(&cur->list);
		kmem_cache_free(cachep, cur);
		deleted++;
		if (deleted == nr_objs)
			break;
	}
	return deleted;
}

static int index_for_expected_id(long *expected, int size, long id)
{
	int i;

	/* Array is unsorted, just iterate the whole thing */
	for (i = 0; i < size; i++) {
		if (expected[i] == id)
			return i;
	}
	return -1;		/* Not found */
}

static int assert_have_objects(int nr_objs, int keep)
{
	struct smo_slub_object *cur;
	long *expected;		/* Array of expected IDs */
	int nr_ids;		/* Length of array */
	long id;
	int index, i;

	nr_ids = nr_objs / keep + 1;

	expected = kmalloc_array(nr_ids, sizeof(long), GFP_KERNEL);
	if (!expected)
		return -ENOMEM;

	id = 0;
	for (i = 0; i < nr_ids; i++) {
		expected[i] = id;
		id += keep;
	}

	list_for_each_entry(cur, &objects, list) {
		index = index_for_expected_id(expected, nr_ids, cur->id);
		if (index < 0) {
			pr_err("smo: ID not found: %ld\n", cur->id);
			return -1;
		}

		if (expected[index] == -1) {
			pr_err("smo: ID already encountered: %ld\n", cur->id);
			return -1;
		}
		expected[index] = -1;
	}
	return 0;
}

/*
 * smo_run_module_tests() - Runs unit tests from within the module
 * @nr_objs: Number of objects to allocate.
 * @keep: Free all but 1 in @keep objects.
 *
 * Allocates @nr_objects then iterates over the allocated objects
 * freeing all but 1 out of every @keep objects i.e. for @keep==10
 * keeps the first object then frees the next 9.
 *
 * Caller is responsible for ensuring that the cache has at most a
 * single slab on the partial list without any objects in it.  This is
 * easy enough to ensure, just call this when the module is freshly
 * loaded.
 */
static int smo_run_module_tests(int nr_objs, int keep)
{
	struct smo_slub_object *so;
	struct smo_slub_object *cur, *tmp;
	long i;

	if (!list_empty(&objects)) {
		pr_err("smo: test requires clean module state\n");
		return -1;
	}

	/* Set sane parameters if no args passed in */
	if (nr_objs == SMO_CMD_DEFAUT_ARG)
		nr_objs = 1000;
	if (keep == SMO_CMD_DEFAUT_ARG)
		keep = 10;

	pr_info("smo: test using nr_objs: %d keep: %d\n", nr_objs, keep);

	/* Perhaps we got called like this 'test 1000' */
	if (keep == 0) {
		pr_err("Usage: test <nr_objs> <keep>\n");
		return -1;
	}

	/* Test constructor */
	so = kmem_cache_alloc(cachep, GFP_KERNEL);
	if (!so) {
		pr_err("smo: Failed to alloc object\n");
		return -1;
	}
	if (so->id != -1) {
		pr_err("smo: Initial state incorrect");
		return -1;
	}
	kmem_cache_free(cachep, so);

	/*
	 * Test that object migration is correctly implemented by module
	 *
	 * This gives us confidence that if new code correctly enables
	 * object migration (via correct implementation of migrate and
	 * isolate functions) then the slub allocator code that does
	 * object migration is correct.
	 */

	for (i = 0; i < nr_objs; i++) {
		so = kmem_cache_alloc(cachep, GFP_KERNEL);
		if (!so) {
			pr_err("smo: Failed to alloc object %ld of %d\n",
			       i, nr_objs);
			return -1;
		}
		so->id = (long)i;
		list_add_to_objects(so);
	}

	assert_have_objects(nr_objs, 1);

	i = 0;
	list_for_each_entry_safe(cur, tmp, &objects, list) {
		if (i++ % keep == 0)
			continue;

		list_del(&cur->list);
		kmem_cache_free(cachep, cur);
	}

	/* Verify shrink does nothing when migration is not enabled */
	kmem_cache_shrink(cachep);
	assert_have_objects(nr_objs, 1);

	/* Now test shrink */
	kmem_cache_setup_mobility(cachep, NULL, smo_cache_migrate);
	kmem_cache_shrink(cachep);
	/*
	 * Because of how migrate function deletes and adds objects to
	 * the objects list we have no way of knowing the order.  We
	 * want to confirm that we have all the objects after shrink
	 * that we had before we did the shrink.
	 */
	assert_have_objects(nr_objs, keep);

	/* cleanup */
	list_for_each_entry_safe(cur, tmp, &objects, list) {
		list_del(&cur->list);
		kmem_cache_free(cachep, cur);
	}
	kmem_cache_shrink(cachep); /* Remove empty slabs from partial list */

	pr_info("smo: Module tests completed successfully\n");
	return 0;
}

/*
 * struct functions() - Map command to a function pointer.
 */
struct functions {
	char *fn_name;
	int (*fn_ptr)(int arg0, int arg1);
} fn_tab[] = {
	/*
	 * Because of the way we parse the function table no command
	 * may have another command as its prefix.
	 *  i.e. this will break: 'foo'  and 'foobar'
	 */
	{"enable", smo_enable_cache_mobility},
	{"alloc", smo_alloc_objects},
	{"free", smo_free_object},
	{"test", smo_run_module_tests},
};

#define FN_TAB_SIZE (sizeof(fn_tab) / sizeof(struct functions))

/*
 * parse_cmd_buf() - Gets command and arguments command string.
 * @buf: Buffer containing the command string.
 * @cmd: Out parameter, pointer to the command.
 * @arg1: Out parameter, stores the first argument.
 * @arg2: Out parameter, stores the second argument.
 *
 * Parses and tokenizes the input command buffer. Stores a pointer to the
 * command (start of @buf) in @cmd.  Stores the converted long values for
 * argument 1 and 2 in the respective out parameters @arg1 and @arg2.
 *
 * Since arguments are optional, if they are not found the default values are
 * returned.  In order for the caller to differentiate defaults from arguments
 * of the same value the number of arguments parsed is returned.
 *
 * Return: Number of arguments found.
 */
static int parse_cmd_buf(char *buf, char **cmd, long *arg1, long *arg2)
{
	int found;
	char *ptr;
	int ret;

	*arg1 = SMO_CMD_DEFAUT_ARG;
	*arg2 = SMO_CMD_DEFAUT_ARG;
	found = 0;

	/* Jump over the command, check if there are any args */
	ptr = strsep(&buf, " ");
	if (!ptr || !buf)
		return found;

	ptr = strsep(&buf, " ");
	ret = kstrtol(ptr, 10, arg1);
	if (ret < 0) {
		pr_err("failed to convert arg, defaulting to %d. (%s)\n",
		       SMO_CMD_DEFAUT_ARG, ptr);
		return found;
	}
	found++;
	if (!buf)		/* No second arg */
		return found;

	ptr = strsep(&buf, " ");
	ret = kstrtol(ptr, 10, arg2);
	if (ret < 0) {
		pr_err("failed to convert arg, defaulting to %d. (%s)\n",
		       SMO_CMD_DEFAUT_ARG, ptr);
		return found;
	}
	found++;

	return found;
}

/*
 * call_function() - Calls the function described by str.
 * @str: '<cmd> [<arg>]'
 *
 * Does table lookup on <cmd>, calls appropriate function passing
 * <arg> as a the argument.  Optional arg defaults to 1.
 */
static void call_function(char *str)
{
	char *cmd;
	long arg1 = 0;
	long arg2 = 0;
	int i;

	if (!str)
		return;

	(void)parse_cmd_buf(str, &cmd, &arg1, &arg2);

	for (i = 0; i < FN_TAB_SIZE; i++) {
		char *fn_name = fn_tab[i].fn_name;

		if (strcmp(fn_name, str) == 0) {
			fn_tab[i].fn_ptr(arg1, arg2);
			return;	/* All done */
		}
	}

	pr_err("failed to call function for cmd: %s\n", str);
}

/*
 * smo_callfn_debugfs_write() - debugfs write function.
 * @file: User file
 * @user_buf: Userspace buffer
 * @len: Length of the user space buffer
 * @off: Offset within the file
 *
 * Used for triggering functions by writing command to debugfs file.
 *
 *   echo '<cmd> <arg>'  > /sys/kernel/debug/smo/callfn
 *
 * Return: Number of bytes copied if request succeeds,
 *	   the corresponding error code otherwise.
 */
static ssize_t smo_callfn_debugfs_write(struct file *file,
					const char __user *ubuf,
					size_t len,
					loff_t *off)
{
	char *kbuf;
	int nbytes = 0;

	if (*off != 0 || len == 0)
		return -EINVAL;

	kbuf = kzalloc(len, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	nbytes = strncpy_from_user(kbuf, ubuf, len);
	if (nbytes < 0)
		goto out;

	if (kbuf[nbytes - 1] == '\n')
		kbuf[nbytes - 1] = '\0';

	call_function(kbuf);	/* Tokenizes kbuf */
out:
	kfree(kbuf);
	return nbytes;
}

const struct file_operations fops_callfn_debugfs = {
	.owner = THIS_MODULE,
	.write = smo_callfn_debugfs_write,
};

static int __init smo_debugfs_init(void)
{
	struct dentry *d;

	smo_debugfs_root = debugfs_create_dir(SMO_DEBUGFS_DIR, NULL);
	d = debugfs_create_file("callfn", 0200, smo_debugfs_root, NULL,
				&fops_callfn_debugfs);
	if (IS_ERR(d))
		return PTR_ERR(d);

	return 0;
}

static void __exit smo_debugfs_cleanup(void)
{
	debugfs_remove_recursive(smo_debugfs_root);
}

static int __init smo_cache_init(void)
{
	cachep = kmem_cache_create(SMO_CACHE_NAME,
				   sizeof(struct smo_slub_object),
				   0, 0, smo_object_ctor);
	if (!cachep)
		return -1;

	return 0;
}

static void __exit smo_cache_cleanup(void)
{
	struct smo_slub_object *cur, *tmp;

	list_for_each_entry_safe(cur, tmp, &objects, list) {
		list_del(&cur->list);
		kmem_cache_free(cachep, cur);
	}
	kmem_cache_destroy(cachep);
}

static int __init smo_init(void)
{
	int ret;

	ret = smo_cache_init();
	if (ret) {
		pr_err("smo: Failed to create cache\n");
		return ret;
	}
	pr_info("smo: Created kmem_cache: %s\n", SMO_CACHE_NAME);

	ret = smo_debugfs_init();
	if (ret) {
		pr_err("smo: Failed to init debugfs\n");
		return ret;
	}
	pr_info("smo: Created debugfs directory: /sys/kernel/debugfs/%s\n",
		SMO_DEBUGFS_DIR);

	pr_info("smo: Test module loaded\n");
	return 0;
}
module_init(smo_init);

static void __exit smo_exit(void)
{
	smo_debugfs_cleanup();
	smo_cache_cleanup();

	pr_info("smo: Test module removed\n");
}
module_exit(smo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobin C. Harding");
MODULE_DESCRIPTION("SLUB Movable Objects test module.");
