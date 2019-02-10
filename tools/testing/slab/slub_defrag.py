#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0
#

import subprocess
import sys
from os import path

#
# SLUB Movable objects test suite.
#
# Requirements:
#  - CONFIG_SLUB=y
#  - CONFIG_SLUB_DEBUG=y
#  - The slub_defrag module in this directory.
#

# Enable/disable debugging output (also enabled via -d | --debug).
debug = False

# Used in debug messages and also used by `modprobe` if MODULE_PATH
# is not set.  If you set MODULE_PATH ensure names match.
MODULE_NAME = "slub_defrag"
# Set this to use `insmod` instead of `modprobe`.
#MODULE_PATH = ""
MODULE_PATH = "./slub_defrag.ko"

# Set by get_slab_config()
objects_per_slab = 0
pages_per_slab = 0

def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


def dprint(*args, **kwargs):
    if debug:
        print("DEBUG: ", end='', file=sys.stderr)
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
    ret = run_shell('ls /sys/kernel/debug/smo > /dev/null 2>&1')
    if ret != 0:
        run_shell('mount -t debugfs none /sys/kernel/debug/')
        mounted = True
        dprint("Mounted debugfs on /sys/kernel/debug")

    return mounted


def unmount_debugfs():
    dprint("Un-mounting debugfs")
    run_shell('umount /sys/kernel/debug')


def load_module():
    """Loads the test module

    Return: 0 if module was already loaded
            1 if module we loaded the module
           -1 on error
    """
    loaded = False
    ret = run_shell('lsmod | grep %s > /dev/null' % MODULE_NAME)
    if ret == 0:
        return 0

    dprint('Loading module')
    if MODULE_PATH != "":
        ret = run_shell('insmod %s' % MODULE_PATH)
    else:
        ret = run_shell('modprobe %s' % MODULE_NAME)

    if ret < 0:
        return ret

    dprint("Slab cache 'smo_test' created")
    return 1


def unload_module():
    dprint('Un-loading module')
    run_shell('rmmod %s' % MODULE_NAME)


def get_sysfs_single_value(filename):
    path = '/sys/kernel/slab/smo_test/%s' % filename
#    return int.from_bytes(run_shell_get_stdout(
#        'cat /sys/kernel/slab/smo_test/%s' % filename), byteorder='little')
    with open(path) as f:
        return [int(x) for x in f][0]

def get_sysfs_value(filename):
    """
    Parse slab sysfs files (single line: '20 N0=20')
    """
    path = '/sys/kernel/slab/smo_test/%s' % filename
    f = open(path, "r")
    s = f.readline()
    tokens = s.split(" ")

    return int(tokens[0])


#
# Fix get_nr_objects_*
#

# X N0=X
def get_nr_objects_active():
    return get_sysfs_value('objects')

# X N0=X
def get_nr_objects_total():
    return get_sysfs_value('total_objects')


def get_nr_slabs_total():
    return get_sysfs_value('slabs')


def get_nr_slabs_partial():
    return get_sysfs_value('slabs')


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
        err -1

    if got_nr_objects_total != nr_objects_total:
        err -2

    if got_nr_slabs_partial != nr_slabs_partial:
        err -3

    if got_nr_slabs_full != nr_slabs_full:
        err -4

    if got_nr_slabs_total != nr_slabs_total:
        err -5

    if err != 0:
        print("", file=sys.stderr)
        dprint("Verify state: %s" % msg)
        dprint("\t<what>\t\t\t<want>\t<got>")
        dprint("%s\t%d\t%d" % ('nr_objects_active', nr_objects_active, got_nr_objects_active))
        dprint("%s\t\t%d\t%d" % ('nr_objects_total', nr_objects_total, got_nr_objects_total))
        dprint("%s\t\t%d\t%d" % ('nr_slabs_partial', nr_slabs_partial, got_nr_slabs_partial))
        dprint("%s\t\t%d\t%d" % ('nr_slabs_full', nr_slabs_full, got_nr_slabs_full))
        dprint("%s\t\t%d\t%d\n" % ('nr_slabs_total', nr_slabs_total, got_nr_slabs_total))

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
    return 0


def test_movable():
    one_over = objects_per_slab + 1

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
    return 0


def run_test(fn, desc):
    dprint("Running %s ..." % desc, end='')
    ret = fn()
    if ret < 0:
        fail_test(desc)
    if debug:
        print("done", file=sys.stderr)


def fail_test(msg):
    eprint("\nFAIL: test failed: '%s' ... aborting\n" % msg)
    sys.exit(1)


def display_help():
    print("Usage: %s [OPTIONS]\n" % path.basename(sys.argv[0]))
    print("\tRuns defrag test suite (a.k.a. SLUB Movable Objects)\n")
    print("OPTIONS:")
    print("\t-d | --debug       Enable verbose debug output")
    print("\t-h | --help        Print this help and exit")


def main():
    global debug

    if len(sys.argv) > 1:
        if sys.argv[1] == '-h' or sys.argv[1] == '--help':
            display_help()
            sys.exit(0)

        if sys.argv[1] == '-d' or sys.argv[1] == '--debug':
            debug = True

    assert_root()

    module_loaded = load_module()
    if (module_loaded < 0):
        fail_test("load module %s" % MODULE_NAME)

    debugfs_mounted = mount_debugfs()

    ret = get_slab_config()
    if (ret != 0):
        fail_test("get slab config details")

    run_test(sanity_checks, "sanity checks")
    run_test(test_non_movable, "test non-movable")

    ret = enable_movable_objects()
    if (ret != 0):
        fail_test("enable movable objects")

    run_test(test_movable, "test movable")

    # TODO We can't test this yet because userspace cannot
    # trigger defrag (only shrink).
#    ret = test_defrag_used_ratio()
#    if (ret != 0):
#        fail_test("test defrag used ration")

    if debugfs_mounted == True:
        unmount_debugfs()

    if module_loaded == 1:
        unload_module()

    sys.exit(0)


if __name__== "__main__":
  main()
