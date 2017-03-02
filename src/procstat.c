#define FUSE_USE_VERSION 26
#include <fuse/fuse_lowlevel.h>
#include <stdbool.h>
#include <errno.h>
#include "procstat.h"
#include "list.h"
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <sys/param.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*a))
#endif

enum {
	STATS_ENTRY_FLAG_REGISTERED  = 1 << 0,
	STATS_ENTRY_FLAG_DIR	     = 1 << 1
};

#define DNAME_INLINE_LEN 32
struct procstat_dynamic_name {
	unsigned zero:8;
	char     *buffer;
};

struct procstat_directory;
struct procstat_item {
	union {
		char iname[DNAME_INLINE_LEN];
		struct procstat_dynamic_name name;
	};
	struct procstat_directory *parent;
	uint32_t 	       name_hash;
	struct list_head       entry;
	int 		       refcnt;
	unsigned 	       flags;
};

struct procstat_directory {
	struct procstat_item base;
	struct list_head       children;
};

struct procstat_context {
	struct procstat_directory root;
	char *mountpoint;
	struct fuse_session *session;
	gid_t	gid;
	uid_t   uid;
	pthread_mutex_t global_lock;
};

static uint32_t string_hash(const char *string)
{
	uint32_t hash = 0;
	unsigned char *i;

	for (i = (unsigned char*)string; *i; ++i)
	hash = 31 * hash + *i;
	return hash;
}

static struct procstat_item *fuse_inode_to_item(struct procstat_context *context, fuse_ino_t inode)
{
	return (inode == FUSE_ROOT_ID) ? &context->root.base : (struct procstat_item *)(inode);
}

static struct procstat_directory *fuse_inode_to_dir(struct procstat_context *context, fuse_ino_t inode)
{
	return (struct procstat_directory *)fuse_inode_to_item(context, inode);
}

static struct procstat_context *request_context(fuse_req_t req)
{
	return (struct procstat_context *)fuse_req_userdata(req);
}

static bool root_directory(struct procstat_context *context, struct procstat_directory *directory)
{
	return &context->root == directory;
}

static bool stats_item_short_name(struct procstat_item *item)
{
	/* file name cannot start with \0, so in case
	 * of long dynamic name we mark first byte with 0
	 * this is how we know*/
	return item->name.zero != 0;
}

static const char *procstat_item_name(struct procstat_item *item)
{
	if (stats_item_short_name(item))
		return item->iname;
	return item->name.buffer;
}

static bool item_registered(struct procstat_item *item)
{
	return item->flags & STATS_ENTRY_FLAG_REGISTERED;
}

static bool item_type_directory(struct procstat_item *item)
{
	return (item->flags & STATS_ENTRY_FLAG_DIR);
}

static void free_item(struct procstat_item *item)
{
	list_del(&item->entry);
	if (item_type_directory(item)) {
		struct procstat_directory *directory = (struct procstat_directory *)item;
		assert(list_empty(&directory->children));
	}

	if (!stats_item_short_name(item))
		free(item->name.buffer);

	free(item);
}

static void free_directory(struct procstat_directory *directory)
{
	free_item(&directory->base);
}


static void fuse_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
	struct procstat_item *item = fuse_inode_to_item(request_context(req), ino);

	assert(item->refcnt >= nlookup);
	item->refcnt -= nlookup;
	if (item->refcnt == 0)
		free_item(item);
	fuse_reply_none(req);
}

#define INODE_BLK_SIZE 4096
static void fill_item_stats(struct procstat_context *context, struct procstat_item *item, struct stat *stat)
{
	stat->st_uid = context->uid;
	stat->st_gid = context->gid;
	stat->st_ino = (__ino_t)item;

	if (item_type_directory(item)) {
		stat->st_mode = S_IFDIR | 0755;
		stat->st_nlink = root_directory(context, (struct procstat_directory *)item) ? 2 : 1;
		return;
	}
	stat->st_mode = S_IFREG | 0444;
	stat->st_nlink = 1;
	stat->st_size = 0;
	stat->st_blocks = 0;
	stat->st_blksize = INODE_BLK_SIZE;
}

static struct procstat_item *lookup_item_locked(struct procstat_directory *parent,
						  const char *name,
						  uint32_t name_hash)
{
	struct procstat_item *item;

	list_for_each_entry(item, &parent->children, entry) {
		if (item->name_hash != name_hash)
			continue;
		if (strcmp(procstat_item_name(item), name) == 0)
			return item;
	}

	return NULL;
}

static void fuse_lookup(fuse_req_t req, fuse_ino_t parent_inode, const char *name)
{
	struct procstat_context *context = request_context(req);
	static struct procstat_directory *parent;
	struct procstat_item *item;
	struct fuse_entry_param fuse_entry;

	memset(&fuse_entry, 0, sizeof(fuse_entry));

	pthread_mutex_lock(&context->global_lock);
	parent = fuse_inode_to_dir(request_context(req), parent_inode);

	item = lookup_item_locked(parent, name, string_hash(name));
	if ((!item) || (!item_registered(item))) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, ENOENT);
		return;
	}
	++item->refcnt;
	pthread_mutex_unlock(&context->global_lock);

	fuse_entry.ino = (uintptr_t)item;
	fill_item_stats(context, item, &fuse_entry.attr);
	fuse_reply_entry(req, &fuse_entry);
}

static void fuse_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct stat stat;
	struct procstat_context *context = request_context(req);
	struct procstat_item *item;

	memset(&stat, 0, sizeof(stat));
	pthread_mutex_lock(&context->global_lock);
	item = fuse_inode_to_item(context, ino);
	if (!item_registered(item)) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, ENOENT);
		return;
	}

	fill_item_stats(context, item, &stat);
	pthread_mutex_unlock(&context->global_lock);
	fuse_reply_attr(req, &stat, 1.0);
}

static void fuse_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct procstat_context *context = request_context(req);
	struct procstat_item *item;

	pthread_mutex_lock(&context->global_lock);
	item = fuse_inode_to_item(context, ino);

	if (!item_registered(item)) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, ENOENT);
		return;
	}

	pthread_mutex_unlock(&context->global_lock);
	fuse_reply_open(req, fi);
}

#define DEFAULT_BUFER_SIZE 1024
static void fuse_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
	struct procstat_context *context = request_context(req);
	static struct procstat_directory *dir;
	static struct procstat_item *iter;
	char *reply_buffer = NULL;
	size_t bufsize;
	size_t offset;
	int alloc_factor = 0;

	pthread_mutex_lock(&context->global_lock);
	dir = fuse_inode_to_dir(context, ino);

	if (!item_registered(&dir->base)) {
		pthread_mutex_unlock(&context->global_lock);
		fuse_reply_err(req, ENOENT);
		return;
	}

	/* FIXME: currently it is very naive implementation which requires lots of reallocations
	 * this can be improved with iovec. */
	/**
	 * FIXME: This is very inefficient since for every lookup this is called twice with offset 0 and with
	 * offset past last, We need to "rebuild" it and save it between "opendir" and "releasedir"
	 */

	bufsize = 0;
	offset = 0;
	list_for_each_entry(iter, &dir->children, entry) {
		const char *fname;
		size_t entry_size;
		struct stat stat;

		memset(&stat, 0, sizeof(stat));
		fname = procstat_item_name(iter);
		fill_item_stats(context, iter, &stat);
		entry_size = fuse_add_direntry(req, NULL, 0, fname, NULL, 0);
		if (bufsize <= entry_size + offset) {
			bufsize = DEFAULT_BUFER_SIZE * (1 << alloc_factor);
			char *new_buffer = realloc(reply_buffer, bufsize);

			++alloc_factor;
			if (!new_buffer) {
				pthread_mutex_unlock(&context->global_lock);
				fuse_reply_err(req, ENOMEM);
				goto done;
			}
			reply_buffer = new_buffer;
		}
		fuse_add_direntry(req, reply_buffer + offset, entry_size, fname, &stat, offset + entry_size);
		offset += entry_size;
	}

	pthread_mutex_unlock(&context->global_lock);
	if (off < offset)
		fuse_reply_buf(req, reply_buffer + off, MIN(size, offset - off));
	else
		fuse_reply_buf(req, NULL, 0);
done:
	free(reply_buffer);
	return;
}

static int register_item(struct procstat_context *context,
			 struct procstat_item *item,
			 struct procstat_directory *parent)
{
	pthread_mutex_lock(&context->global_lock);
	if (parent) {
		struct procstat_item *duplicate;

		duplicate = lookup_item_locked(parent, procstat_item_name(item), item->name_hash);
		if (duplicate) {
			pthread_mutex_unlock(&context->global_lock);
			return EEXIST;
		}
		list_add_tail(&item->entry, &parent->children);
	}
	item->flags |= STATS_ENTRY_FLAG_REGISTERED;
	item->refcnt = 1;
	pthread_mutex_unlock(&context->global_lock);
	return 0;
}

static void init_item(struct procstat_item *item, const char *name)
{
	size_t name_len = strlen(name);

	item->name_hash = string_hash(name);
	if (name_len < DNAME_INLINE_LEN)
		strcpy(item->iname, name);
	else {
		item->name.zero = 0;
		item->name.buffer = strdup(name);
	}
	INIT_LIST_HEAD(&item->entry);
}

static int init_directory(struct procstat_context *context,
			  struct procstat_directory *directory,
			  const char *name,
			  struct procstat_directory *parent)
{
	int error;

	init_item(&directory->base, name);
	directory->base.flags = STATS_ENTRY_FLAG_DIR;
	INIT_LIST_HEAD(&directory->children);
	error = register_item(context, &directory->base, parent);
	if (error)
		return error;

	return 0;
}

static void item_put_locked(struct procstat_item *item);
static void item_put_children_locked(struct procstat_directory *directory)
{
	struct procstat_item *iter, *n;
	list_for_each_entry_safe(iter, n, &directory->children, entry) {
		iter->parent = NULL;
		list_del_init(&iter->entry);
		item_put_locked(iter);
	}
}

static void item_put_locked(struct procstat_item *item)
{
	assert(item->refcnt);

	if (--item->refcnt)
		return;

	item->flags &= ~STATS_ENTRY_FLAG_REGISTERED;
	if (item_type_directory(item))
		item_put_children_locked((struct procstat_directory *)item);

	free_item(item);
}

static struct procstat_item *parent_or_root(struct procstat_context *context, struct procstat_item *parent)
{
	if (!parent)
		return &context->root.base;
	else if (item_type_directory(parent))
		return parent;
	return NULL;
}

struct procstat_item *procstat_create_directory(struct procstat_context *context,
					   	struct procstat_item *parent,
						const char *name)
{
	struct procstat_directory *new_directory;
	int error;

	parent = parent_or_root(context, parent);
	if (!parent) {
		errno = EINVAL;
		return NULL;
	}

	new_directory = calloc(1, sizeof(*new_directory));
	if (!new_directory) {
		errno = ENOMEM;
		return NULL;
	}

	error = init_directory(context, new_directory, name, (struct procstat_directory *)parent);
	if (error) {
		free_directory(new_directory);
		errno = error;
		return NULL;
	}

	return &new_directory->base;
}

void procstat_remove(struct procstat_context *context, struct procstat_item *item)
{
	struct procstat_directory *directory;

	assert(context);
	assert(item);

	pthread_mutex_lock(&context->global_lock);
	if (!item_type_directory(item))
		goto remove_item;

	directory = (struct procstat_directory *)item;
	if (root_directory(context, directory)) {
		item_put_children_locked(directory);
		goto done;
	}

remove_item:
	item->flags &= ~STATS_ENTRY_FLAG_REGISTERED;
	item_put_locked(item);
done:
	pthread_mutex_unlock(&context->global_lock);
}

int procstat_remove_by_name(struct procstat_context *context,
			    struct procstat_item *parent,
			    const char *name)
{
	struct procstat_item *item;

	parent = parent_or_root(context, parent);
	if (!parent) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&context->global_lock);
	item = lookup_item_locked((struct procstat_directory *)parent,
				  name, string_hash(name));
	if (!item) {
		pthread_mutex_unlock(&context->global_lock);
		return ENOENT;
	}
	item->flags &= ~STATS_ENTRY_FLAG_REGISTERED;
	item_put_locked(item);
	pthread_mutex_unlock(&context->global_lock);
	return 0;
}

struct procstat_item *procstat_root(struct procstat_context *context)
{
	assert(context);
	return &context->root.base;
}

static struct fuse_lowlevel_ops fops = {
	.forget = fuse_forget,
	.lookup = fuse_lookup,
	.getattr = fuse_getattr,
	.opendir = fuse_opendir,
	.readdir = fuse_readdir,
};

#define ROOT_DIR_NAME "."
struct procstat_context *procstat_create(const char *mountpoint)
{
	struct procstat_context *context;
	char *argv[] = {(char *)"stats", (char *)"-o", (char *)"auto_unmount", (char *)mountpoint};
	struct fuse_args args = FUSE_ARGS_INIT(ARRAY_SIZE(argv), argv);
	char *full_path_mountpoint;
	int error;
	struct fuse_chan *channel;

	error = mkdir(mountpoint, 0755);
	if (error) {
		if (errno != EEXIST)
			return NULL;
	}

	error = fuse_parse_cmdline(&args, &full_path_mountpoint, NULL, NULL);
	if (error) {
		errno = EINVAL;
		return NULL;
	}

	context = calloc(1, sizeof(*context));
	if (!context) {
		errno = ENOMEM;
		return NULL;
	}
	context->mountpoint = full_path_mountpoint;
	context->uid = getuid();
	context->gid = getgid();

	pthread_mutex_init(&context->global_lock, NULL);
	init_directory(context, &context->root, ROOT_DIR_NAME, NULL);

	channel = fuse_mount(context->mountpoint, &args);
	if (!channel) {
		errno = EFAULT;
		goto free_stats;
	}

	context->session = fuse_lowlevel_new(&args, &fops, sizeof(fops), context);
	if (!context->session) {
		errno = EPERM;
		goto free_stats;
	}

	fuse_session_add_chan(context->session, channel);
	return context;
free_stats:
	procstat_destroy(context);
	return NULL;
}

void procstat_destroy(struct procstat_context *context)
{
	struct fuse_session *session;

	assert(context);
	session = context->session;

	pthread_mutex_lock(&context->global_lock);
	if (session) {
		struct fuse_chan *channel = NULL;

		assert(context->mountpoint);
		fuse_session_exit(session);
		channel = fuse_session_next_chan(session, channel);
		assert(channel);
		fuse_session_remove_chan(channel);

		fuse_unmount(context->mountpoint, channel);
		fuse_session_destroy(session);
	}

	item_put_children_locked(&context->root);
	free(context->mountpoint);
	pthread_mutex_unlock(&context->global_lock);
	pthread_mutex_destroy(&context->global_lock);

	/* debug purposes of use after free*/
	context->mountpoint = NULL;
	context->session = NULL;
	free(context);
}

void procstat_loop(struct procstat_context *context)
{
	fuse_session_loop(context->session);
}
