.. SPDX-License-Identifier: GPL-2.0

=========================================
Overview of the Linux Virtual File System
=========================================

Original author: Richard Gooch <rgooch@atnf.csiro.au>

Last updated on June 24, 2007.

- Copyright (C) 1999 Richard Gooch
- Copyright (C) 2005 Pekka Enberg


Introduction
============

The Virtual File System (also known as the Virtual Filesystem Switch) is
the software layer in the kernel that provides the filesystem interface
to userspace programs.  It also provides an abstraction within the
kernel which allows different filesystem implementations to coexist.

VFS system calls open(2), stat(2), read(2), write(2), chmod(2) and so on
are called from a process context.  Filesystem locking is described in
the document Documentation/filesystems/Locking.


Directory Entry Cache (dcache)
------------------------------

The VFS implements the open(2), stat(2), chmod(2), and similar system
calls.  The pathname argument that is passed to them is used by the VFS
to search through the directory entry cache (also known as the dentry
cache or dcache).  This provides a very fast look-up mechanism to
translate a pathname (filename) into a specific dentry.  Dentries live
in RAM and are never saved to disc: they exist only for performance.

The dentry cache is meant to be a view into your entire filespace.  As
most computers cannot fit all dentries in the RAM at the same time, some
bits of the cache are missing.  In order to resolve your pathname into a
dentry, the VFS may have to resort to creating dentries along the way,
and then loading the inode.  This is done by looking up the inode.


The Inode Object
----------------

An individual dentry usually has a pointer to an inode.  Inodes are
filesystem objects such as regular files, directories, FIFOs and other
beasts.  They live either on the disc (for block device filesystems) or in
the memory (for pseudo filesystems).  Inodes that live on the disc are
copied into memory when required and changes to the inode are written back
to disc.  A single inode can be pointed to by multiple dentries (hard
links, for example, do this).

To look up an inode requires that the VFS calls the lookup() method of
the parent directory inode.  This method is installed by the specific
filesystem implementation that the inode lives in.  Once the VFS has the
required dentry (and hence the inode), we can do all those boring things
like open(2) the file, or stat(2) it to peek at the inode data.  The
stat(2) operation is fairly simple: once the VFS has the dentry, it
peeks at the inode data and passes some of it back to userspace.


The File Object
---------------

Opening a file requires another operation: allocation of a file
structure (this is the kernel-side implementation of file descriptors).
The freshly allocated file structure is initialized with a pointer to
the dentry and a set of file operation member functions.  These are
taken from the inode data.  The open() file method is then called so the
specific filesystem implementation can do its work.  You can see that
this is another switch performed by the VFS.  The file structure is
placed into the file descriptor table for the process.

Reading, writing and closing files (and other assorted VFS operations)
is done by using the userspace file descriptor to grab the appropriate
file structure, and then calling the required file structure method to
do whatever is required.  For as long as the file is open, it keeps the
dentry in use, which in turn means that the VFS inode is still in use.


Registering and Mounting a Filesystem
=====================================

To register and unregister a filesystem, use the following API
functions:

.. code-block:: c

   #include <linux/fs.h>

   extern int register_filesystem(struct file_system_type *);
   extern int unregister_filesystem(struct file_system_type *);

The passed struct file_system_type describes your filesystem.  When a
request is made to mount a filesystem onto a directory in your
namespace, the VFS will call the appropriate mount() method for the
specific filesystem.  New vfsmount referring to the tree returned by
->mount() will be attached to the mountpoint, so that when pathname
resolution reaches the mountpoint it will jump into the root of that
vfsmount.

You can see all filesystems that are registered to the kernel in the
file /proc/filesystems.


struct file_system_type
-----------------------

.. kernel-doc:: include/linux/fs.h
   :functions: struct file_system_type

.. _vfs_file_system_type:

The mount() method must return the root dentry of the tree requested by
caller.  An active reference to its superblock must be grabbed and the
superblock must be locked.  On failure it should return ERR_PTR(error).

The arguments match those of mount(2) and their interpretation depends
on filesystem type.  E.g. for block filesystems, dev_name is interpreted
as block device name, that device is opened and if it contains a
suitable filesystem image the method creates and initializes struct
super_block accordingly, returning its root dentry to caller.

->mount() may choose to return a subtree of existing filesystem - it
doesn't have to create a new one.  The main result from the caller's
point of view is a reference to dentry at the root of (sub)tree to be
attached; creation of new superblock is a common side effect.

The most interesting member of the superblock structure that the mount()
method fills in is the "s_op" field.  This is a pointer to a "struct
super_operations" which describes the next level of the filesystem
implementation.

Usually, a filesystem uses one of the generic mount() implementations
and provides a fill_super() callback instead.  The generic variants are:

- ``mount_bdev()``: Mount a filesystem residing on a block device.

- ``mount_nodev()``: Mount a filesystem that is not backed by a device.

- ``mount_single()``: Mount a filesystem which shares the instance between
  all mounts.

A fill_super() callback implementation has the following arguments:

- ``struct super_block *sb``: The superblock structure.  The callback must
  initialize this properly.

- ``void *data``: Arbitrary mount options, usually comes as an ASCII string
  (see "Mount Options" section).

- ``int silent``: Whether or not to be silent on error.


The Superblock Object
=====================

A superblock object represents a mounted filesystem.


struct super_operations
-----------------------

.. kernel-doc:: include/linux/fs.h
   :functions: struct super_operations


struct xattr_handlers
---------------------

On filesystems that support extended attributes (xattrs), the s_xattr
superblock field points to a NULL-terminated array of xattr handlers.
Extended attributes are name:value pairs.

- name: Indicates that the handler matches attributes with the specified
  name (such as ``system.posix_acl_access``); the prefix field must be
  NULL.

- prefix: Indicates that the handler matches all attributes with the
  specified name prefix (such as "user."); the name field must be NULL.

- list: Determine if attributes matching this xattr handler should be
  listed for a particular dentry.  Used by some listxattr
  implementations like generic_listxattr.

- get: Called by the VFS to get the value of a particular extended
  attribute.  This method is called by the ``getxattr(2)`` system call.

- set: Called by the VFS to set the value of a particular extended
  attribute. When the new value is NULL, called to remove a particular
  extended attribute.  This method is called by the ``setxattr(2)`` and
  removexattr(2) system calls.

When none of the xattr handlers of a filesystem match the specified
attribute name or when a filesystem doesn't support extended attributes,
the various ``*xattr(2)`` system calls return -EOPNOTSUPP.


The Inode Object
================

An inode object represents an object within the filesystem.


struct inode_operations
-----------------------

.. kernel-doc:: include/linux/fs.h
   :functions: struct inode_operations


The Address Space Object
========================

The address space object is used to group and manage pages in the page
cache.  It can be used to keep track of the pages in a file (or anything
else) and also track the mapping of sections of the file into process
address spaces.

There are a number of distinct yet related services that an
address-space can provide.  These include communicating memory pressure,
page lookup by address, and keeping track of pages tagged as Dirty or
Writeback.

The first can be used independently to the others.  The VM can try to
either write dirty pages in order to clean them, or release clean pages
in order to reuse them.  To do this it can call the ->writepage method
on dirty pages, and ->releasepage on clean pages with PagePrivate set.
Clean pages without PagePrivate and with no external references will be
released without notice being given to the address_space.

To achieve this functionality, pages need to be placed on an LRU with
lru_cache_add and mark_page_active needs to be called whenever the page
is used.

Pages are normally kept in a radix tree index by ->index.  This tree
maintains information about the PG_Dirty and PG_Writeback status of each
page, so that pages with either of these flags can be found quickly.

The Dirty tag is primarily used by mpage_writepages - the default
->writepages method.  It uses the tag to find dirty pages to call
->writepage on.  If mpage_writepages is not used (i.e. the address
provides its own ->writepages) , the PAGECACHE_TAG_DIRTY tag is almost
unused.  write_inode_now and sync_inode do use it (through
__sync_single_inode) to check if ->writepages has been successful in
writing out the whole address_space.

The Writeback tag is used by filemap*wait* and sync_page* functions, via
filemap_fdatawait_range, to wait for all writeback to complete.

An address_space handler may attach extra information to a page,
typically using the 'private' field in the 'struct page'.  If such
information is attached, the PG_Private flag should be set.  This will
cause various VM routines to make extra calls into the address_space
handler to deal with that data.

An address space acts as an intermediate between storage and
application.  Data is read into the address space a whole page at a
time, and provided to the application either by copying of the page, or
by memory-mapping the page.  Data is written into the address space by
the application, and then written-back to storage typically in whole
pages, however the address_space has finer control of write sizes.

The read process essentially only requires 'readpage'.  The write
process is more complicated and uses write_begin/write_end or
set_page_dirty to write data into the address_space, and writepage and
writepages to writeback data to storage.

Adding and removing pages to/from an address_space is protected by the
inode's i_mutex.

When data is written to a page, the PG_Dirty flag should be set.  It
typically remains set until writepage asks for it to be written.  This
should clear PG_Dirty and set PG_Writeback.  It can be actually written
at any point after PG_Dirty is clear.  Once it is known to be safe,
PG_Writeback is cleared.

Writeback makes use of a writeback_control structure to direct the
operations.  This gives the writepage and writepages operations some
information about the nature of and reason for the writeback request,
and the constraints under which it is being done.  It is also used to
return information back to the caller about the result of a writepage or
writepages request.


Handling errors during writeback
--------------------------------

Most applications that do buffered I/O will periodically call a file
synchronization call (fsync, fdatasync, msync or sync_file_range) to
ensure that data written has made it to the backing store.  When there
is an error during writeback, they expect that error to be reported when
a file sync request is made.  After an error has been reported on one
request, subsequent requests on the same file descriptor should return
0, unless further writeback errors have occurred since the previous file
syncronization.

Ideally, the kernel would report errors only on file descriptions on
which writes were done that subsequently failed to be written back.  The
generic pagecache infrastructure does not track the file descriptions
that have dirtied each individual page however, so determining which
file descriptors should get back an error is not possible.

Instead, the generic writeback error tracking infrastructure in the
kernel settles for reporting errors to fsync on all file descriptions
that were open at the time that the error occurred.  In a situation with
multiple writers, all of them will get back an error on a subsequent
fsync, even if all of the writes done through that particular file
descriptor succeeded (or even if there were no writes on that file
descriptor at all).

Filesystems that wish to use this infrastructure should call
mapping_set_error to record the error in the address_space when it
occurs.  Then, after writing back data from the pagecache in their
file->fsync operation, they should call file_check_and_advance_wb_err to
ensure that the struct file's error cursor has advanced to the correct
point in the stream of errors emitted by the backing device(s).


struct address_space_operations
-------------------------------

.. kernel-doc:: include/linux/fs.h
   :functions: struct address_space_operations


The File Object
===============

A file object represents a file opened by a process.  This is also known
as an "open file description" in POSIX parlance.


struct file_operations
----------------------

.. kernel-doc:: include/linux/fs.h
   :functions: struct file_operations


Directory Entry Cache (dcache)
==============================


struct dentry_operations
------------------------

.. kernel-doc:: include/linux/dcache.h
   :functions: struct dentry_operations

.. _d_dname_example:

Example implementation of the d_dname method:

.. code-block:: c

   static char *pipefs_dname(struct dentry *dent, char *buffer, int buflen)
   {
	   return dynamic_dname(dentry, buffer, buflen, "pipe:[%lu]",
				dentry->d_inode->i_ino);
   }


.. _vfs_mount_options:

Mount Options
=============


Parsing options
---------------

On mount and remount the filesystem is passed a string containing a
comma separated list of mount options.  The options can have either of
these forms:

  option
  option=value

The <linux/parser.h> header defines an API that helps parse these
options.  There are plenty of examples on how to use it in existing
filesystems.


Showing options
---------------

If a filesystem accepts mount options, it must define show_options() to
show all the currently active options.  The rules are:

- Options MUST be shown which are not default or their values differ
  from the default.

- Options MAY be shown which are enabled by default or have their
  default value.

Options used only internally between a mount helper and the kernel (such
as file descriptors), or which only have an effect during the mounting
(such as ones controlling the creation of a journal) are exempt from the
above rules.

The underlying reason for the above rules is to make sure, that a mount
can be accurately replicated (e.g. umounting and mounting again) based
on the information found in /proc/mounts.


Resources
=========

(Note some of these resources are not up-to-date with the latest kernel
 version.)

Creating Linux virtual filesystems. 2002
    <http://lwn.net/Articles/13325/>

The Linux Virtual File-system Layer by Neil Brown. 1999
    <http://www.cse.unsw.edu.au/~neilb/oss/linux-commentary/vfs.html>

A tour of the Linux VFS by Michael K. Johnson. 1996
    <http://www.tldp.org/LDP/khg/HyperNews/get/fs/vfstour.html>

A small trail through the Linux kernel by Andries Brouwer. 2001
    <http://www.win.tue.nl/~aeb/linux/vfs/trail.html>
