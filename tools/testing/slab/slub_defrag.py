#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import subprocess
import sys
from os import path

# SLUB Movable Objects test suite.
#
# Requirements:
#  - CONFIG_SLUB=y
#  - CONFIG_SLUB_DEBUG=y
#  - The slub_defrag module in this directory.

# Test SMO using a kernel module that enables triggering arbitrary
# kernel code from userspace via a debugfs file.
#
# Module code is in ./slub_defrag.c, basically the functionality is as
# follows:
#
#  - Creates debugfs file /sys/kernel/debugfs/smo/callfn
#  - Writes to 'callfn' are parsed as a command string and the function
#    associated with command is called.
#  - Defines 4 commands (all commands operate on smo_test cache):
#     - 'test': Runs module stress tests.
#     - 'alloc N': Allocates N slub objects
#     - 'free N POS': Frees N objects starting at POS (see below)
#     - 'enable': Enables SLUB Movable Objects
#
# The module maintains a list of allocated objects.  Allocation adds
# objects to the tail of the list.  Free'ing frees from the head of the
# list.  This has the effect of creating free slots in the slab.  For
# finer grained control over where in the cache slots are free'd POS
# (position) argument may be used.

# The main() function is reasonably readable; the test suite does the
# following:
#
# 1. Runs the module stress tests.
# 2. Tests the cache without movable objects enabled.
#    - Creates multiple partial slabs as explained above.
#    - Verifies that partial slabs are _not_ removed by shrink (see below).
# 3. Tests the cache with movable objects enabled.
#    - Creates multiple partial slabs as explained above.
#    - Verifies that partial slabs _are_ removed by shrink (see below).

# The sysfs file /sys/kernel/slab/<cache>/shrink enables calling the
# function kmem_cache_shrink() (see mm/slab_common.c and mm/slub.cc).
# Shrinking a cache attempts to consolidate all partial slabs by moving
# objects if object migration is enable for the cache, otherwise
# shrinking a cache simply re-orders the partial list so as most densely
# populated slab are at the head of the list.

# Enable/disable debugging output (also enabled via -d | --debug).
debug = False

# Used in debug messages and when running `insmod`.
MODULE_NAME = "slub_defrag"

# Slab cache created by the test module.
CACHE_NAME = "smo_test"

# Set by get_slab_config()
objects_per_slab = 0
pages_per_slab = 0
debugfs_mounted = False         # Set to true if we mount debugfs.


def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


def dprint(*args, **kwargs):
    if debug:
        print(*args, file=sys.stderr, **kwargs)


def run_shell(cmd):
    return subprocess.call([cmd], shell=True)


def run_shell_get_stdout(cmd):
    return subprocess.check_output([cmd], shell=True)


def assert_root():
    user = run_shell_get_stdout('whoami')
    if user != b'root\n':
        eprint("Please run script as root")
        sys.exit(1)


def mount_debugfs():
    mounted = False

    # Check if debugfs is mounted at a known mount point.
    ret = run_shell('mount -l | grep /sys/kernel/debug > /dev/null 2>&1')
    if ret != 0:
        run_shell('mount -t debugfs none /sys/kernel/debug/')
        mounted = True
        dprint("Mounted debugfs on /sys/kernel/debug")

    return mounted


def umount_debugfs():
    dprint("Un-mounting debugfs")
    run_shell('umount /sys/kernel/debug')


def load_module():
    """Loads the test module.

    We need a clean slab state to start with so module must
    be loaded by the test suite.
    """
    ret = run_shell('lsmod | grep %s > /dev/null' % MODULE_NAME)
    if ret == 0:
        eprint("Please unload slub_defrag module before running test suite")
        return -1

    dprint('Loading module ...')
    ret = run_shell('insmod %s.ko' % MODULE_NAME)
    if ret != 0:                # ret==1 on error
        return -1

    dprint("Slab cache %s created" % CACHE_NAME)
    return 0


def unload_module():
    ret = run_shell('lsmod | grep %s > /dev/null' % MODULE_NAME)
    if ret == 0:
        dprint('Removing module %s ...' % MODULE_NAME)
        run_shell('rmmod %s > /dev/null 2>&1' % MODULE_NAME)


def get_sysfs_value(filename):
    """
    Parse slab sysfs files (single line: '20 N0=20')
    """
    path = '/sys/kernel/slab/smo_test/%s' % filename
    f = open(path, "r")
    s = f.readline()
    tokens = s.split(" ")

    return int(tokens[0])


def get_nr_objects_active():
    return get_sysfs_value('objects')


def get_nr_objects_total():
    return get_sysfs_value('total_objects')


def get_nr_slabs_total():
    return get_sysfs_value('slabs')


def get_nr_slabs_partial():
    return get_sysfs_value('partial')


def get_nr_slabs_full():
    return get_nr_slabs_total() - get_nr_slabs_partial()


def get_slab_config():
    """Get relevant information from sysfs."""
    global objects_per_slab

    objects_per_slab = get_sysfs_value('objs_per_slab')
    if objects_per_slab < 0:
        return -1

    dprint("Objects per slab: %d" % objects_per_slab)
    return 0


def verify_state(nr_objects_active, nr_objects_total,
                 nr_slabs_partial, nr_slabs_full, nr_slabs_total, msg=''):
    err = 0
    got_nr_objects_active = get_nr_objects_active()
    got_nr_objects_total = get_nr_objects_total()
    got_nr_slabs_partial = get_nr_slabs_partial()
    got_nr_slabs_full = get_nr_slabs_full()
    got_nr_slabs_total = get_nr_slabs_total()

    if got_nr_objects_active != nr_objects_active:
        err = -1

    if got_nr_objects_total != nr_objects_total:
        err = -2

    if got_nr_slabs_partial != nr_slabs_partial:
        err = -3

    if got_nr_slabs_full != nr_slabs_full:
        err = -4

    if got_nr_slabs_total != nr_slabs_total:
        err = -5

    if err != 0:
        dprint("Verify state: %s" % msg)
        dprint("  what\t\t\twant\tgot")
        dprint("-----------------------------------------")
        dprint("  %s\t%d\t%d" % ('nr_objects_active', nr_objects_active, got_nr_objects_active))
        dprint("  %s\t%d\t%d" % ('nr_objects_total', nr_objects_total, got_nr_objects_total))
        dprint("  %s\t%d\t%d" % ('nr_slabs_partial', nr_slabs_partial, got_nr_slabs_partial))
        dprint("  %s\t\t%d\t%d" % ('nr_slabs_full', nr_slabs_full, got_nr_slabs_full))
        dprint("  %s\t%d\t%d\n" % ('nr_slabs_total', nr_slabs_total, got_nr_slabs_total))

    return err


def exec_via_sysfs(command):
        ret = run_shell('echo %s > /sys/kernel/debug/smo/callfn' % command)
        if ret != 0:
            eprint("Failed to echo command to sysfs: %s" % command)

        return ret


def enable_movable_objects():
    return exec_via_sysfs('enable')


def alloc(n):
    exec_via_sysfs("alloc %d" % n)


def free(n, pos = 0):
    exec_via_sysfs('free %d %d' % (n, pos))


def shrink():
    ret = run_shell('slabinfo smo_test -s')
    if ret != 0:
            eprint("Failed to execute slabinfo -s")


def sanity_checks():
    # Verify everything is 0 to start with.
    return verify_state(0, 0, 0, 0, 0, "sanity check")


def test_non_movable():
    one_over = objects_per_slab + 1

    dprint("testing slab 'smo_test' prior to enabling movable objects ...")

    alloc(one_over)

    objects_active = one_over
    objects_total = objects_per_slab * 2
    slabs_partial = 1
    slabs_full = 1
    slabs_total = 2
    ret = verify_state(objects_active, objects_total,
                       slabs_partial, slabs_full, slabs_total,
                       "non-movable: initial allocation")
    if ret != 0:
        eprint("test_non_movable: failed to verify initial state")
        return -1

    # Free object from first slot of first slab.
    free(1)
    objects_active = one_over - 1
    objects_total = objects_per_slab * 2
    slabs_partial = 2
    slabs_full = 0
    slabs_total = 2
    ret = verify_state(objects_active, objects_total,
                       slabs_partial, slabs_full, slabs_total,
                       "non-movable: after free")
    if ret != 0:
        eprint("test_non_movable: failed to verify after free")
        return -1

    # Non-movable cache, shrink should have no effect.
    shrink()
    ret = verify_state(objects_active, objects_total,
                       slabs_partial, slabs_full, slabs_total,
                       "non-movable: after shrink")
    if ret != 0:
        eprint("test_non_movable: failed to verify after shrink")
        return -1

    # Cleanup
    free(objects_per_slab)
    shrink()

    dprint("verified non-movable slabs are NOT shrinkable")
    return 0


def test_movable():
    one_over = objects_per_slab + 1

    dprint("testing slab 'smo_test' after enabling movable objects ...")

    alloc(one_over)

    objects_active = one_over
    objects_total = objects_per_slab * 2
    slabs_partial = 1
    slabs_full = 1
    slabs_total = 2
    ret = verify_state(objects_active, objects_total,
                       slabs_partial, slabs_full, slabs_total,
                       "movable: initial allocation")
    if ret != 0:
        eprint("test_movable: failed to verify initial state")
        return -1

    # Free object from first slot of first slab.
    free(1)
    objects_active = one_over - 1
    objects_total = objects_per_slab * 2
    slabs_partial = 2
    slabs_full = 0
    slabs_total = 2
    ret = verify_state(objects_active, objects_total,
                       slabs_partial, slabs_full, slabs_total,
                       "movable: after free")
    if ret != 0:
        eprint("test_movable: failed to verify after free")
        return -1

    # movable cache, shrink should move objects and free slab.
    shrink()
    objects_active = one_over - 1
    objects_total = objects_per_slab * 1
    slabs_partial = 0
    slabs_full = 1
    slabs_total = 1
    ret = verify_state(objects_active, objects_total,
                       slabs_partial, slabs_full, slabs_total,
                       "movable: after shrink")
    if ret != 0:
        eprint("test_movable: failed to verify after shrink")
        return -1

    # Cleanup
    free(objects_per_slab)
    shrink()

    dprint("verified movable slabs are shrinkable")
    return 0


def dprint_start_test(test):
    dprint("Running %s ..." % test)


def dprint_done():
    dprint("")


def run_test(fn, desc):
    dprint_start_test(desc)
    ret = fn()
    if ret < 0:
        fail_test(desc)
    dprint_done()


# Load and unload the module for this test to ensure clean state.
def run_module_stress_test():
    dprint("Running module stress test (see dmesg for additional test output) ...")

    unload_module()
    ret = load_module()
    if ret < 0:
        cleanup_and_exit(ret)

    exec_via_sysfs("test");

    unload_module()

    dprint()


def fail_test(msg):
    eprint("\nFAIL: test failed: '%s' ... aborting\n" % msg)
    cleanup_and_exit(1)


def display_help():
    print("Usage: %s [OPTIONS]\n" % path.basename(sys.argv[0]))
    print("\tRuns defrag test suite (a.k.a. SLUB Movable Objects)\n")
    print("OPTIONS:")
    print("\t-d | --debug       Enable verbose debug output")
    print("\t-h | --help        Print this help and exit")


def cleanup_and_exit(return_code):
    global debugfs_mounted

    if debugfs_mounted == True:
        umount_debugfs()

    unload_module()

    sys.exit(return_code)


def main():
    global debug

    if len(sys.argv) > 1:
        if sys.argv[1] == '-h' or sys.argv[1] == '--help':
            display_help()
            sys.exit(0)

        if sys.argv[1] == '-d' or sys.argv[1] == '--debug':
            debug = True

    assert_root()

    # Use cleanup_and_exit() instead of sys.exit() after mounting debugfs.
    debugfs_mounted = mount_debugfs()

    # Loads and unloads the module.
    run_module_stress_test()

    ret = load_module()
    if (ret < 0):
        cleanup_and_exit(ret)

    ret = get_slab_config()
    if (ret != 0):
        fail_test("get slab config details")

    run_test(sanity_checks, "sanity checks")

    run_test(test_non_movable, "test non-movable")

    ret = enable_movable_objects()
    if (ret != 0):
        fail_test("enable movable objects")

    run_test(test_movable, "test movable")

    cleanup_and_exit(0)

if __name__== "__main__":
  main()
