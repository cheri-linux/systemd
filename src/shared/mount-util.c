/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "alloc-util.h"
#include "extract-word.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "hashmap.h"
#include "libmount-util.h"
#include "mount-util.h"
#include "mountpoint-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "set.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"

int umount_recursive(const char *prefix, int flags) {
        int n = 0, r;
        bool again;

        /* Try to umount everything recursively below a
         * directory. Also, take care of stacked mounts, and keep
         * unmounting them until they are gone. */

        do {
                _cleanup_(mnt_free_tablep) struct libmnt_table *table = NULL;
                _cleanup_(mnt_free_iterp) struct libmnt_iter *iter = NULL;

                again = false;

                r = libmount_parse("/proc/self/mountinfo", NULL, &table, &iter);
                if (r < 0)
                        return log_debug_errno(r, "Failed to parse /proc/self/mountinfo: %m");

                for (;;) {
                        struct libmnt_fs *fs;
                        const char *path;

                        r = mnt_table_next_fs(table, iter, &fs);
                        if (r == 1)
                                break;
                        if (r < 0)
                                return log_debug_errno(r, "Failed to get next entry from /proc/self/mountinfo: %m");

                        path = mnt_fs_get_target(fs);
                        if (!path)
                                continue;

                        if (!path_startswith(path, prefix))
                                continue;

                        if (umount2(path, flags | UMOUNT_NOFOLLOW) < 0) {
                                log_debug_errno(errno, "Failed to umount %s, ignoring: %m", path);
                                continue;
                        }

                        log_debug("Successfully unmounted %s", path);

                        again = true;
                        n++;

                        break;
                }
        } while (again);

        return n;
}

static int get_mount_flags(
                struct libmnt_table *table,
                const char *path,
                unsigned long *ret) {
        struct libmnt_fs *fs;
        struct statvfs buf;
        const char *opts;
        int r = 0;

        /* Get the mount flags for the mountpoint at "path" from "table". We have a fallback using statvfs()
         * in place (which provides us with mostly the same info), but it's just a fallback, since using it
         * means triggering autofs or NFS mounts, which we'd rather avoid needlessly. */

        fs = mnt_table_find_target(table, path, MNT_ITER_FORWARD);
        if (!fs) {
                log_debug("Could not find '%s' in mount table, ignoring.", path);
                goto fallback;
        }

        opts = mnt_fs_get_vfs_options(fs);
        if (!opts) {
                *ret = 0;
                return 0;
        }

        r = mnt_optstr_get_flags(opts, ret, mnt_get_builtin_optmap(MNT_LINUX_MAP));
        if (r != 0) {
                log_debug_errno(r, "Could not get flags for '%s', ignoring: %m", path);
                goto fallback;
        }

        /* MS_RELATIME is default and trying to set it in an unprivileged container causes EPERM */
        *ret &= ~MS_RELATIME;
        return 0;

fallback:
        if (statvfs(path, &buf) < 0)
                return -errno;

        /* The statvfs() flags and the mount flags mostly have the same values, but for some cases do
         * not. Hence map the flags manually. (Strictly speaking, ST_RELATIME/MS_RELATIME is the most
         * prominent one that doesn't match, but that's the one we mask away anyway, see above.) */

        *ret =
                FLAGS_SET(buf.f_flag, ST_RDONLY) * MS_RDONLY |
                FLAGS_SET(buf.f_flag, ST_NODEV) * MS_NODEV |
                FLAGS_SET(buf.f_flag, ST_NOEXEC) * MS_NOEXEC |
                FLAGS_SET(buf.f_flag, ST_NOSUID) * MS_NOSUID |
                FLAGS_SET(buf.f_flag, ST_NOATIME) * MS_NOATIME |
                FLAGS_SET(buf.f_flag, ST_NODIRATIME) * MS_NODIRATIME;

        return 0;
}

/* Use this function only if you do not have direct access to /proc/self/mountinfo but the caller can open it
 * for you. This is the case when /proc is masked or not mounted. Otherwise, use bind_remount_recursive. */
int bind_remount_recursive_with_mountinfo(
                const char *prefix,
                unsigned long new_flags,
                unsigned long flags_mask,
                char **deny_list,
                FILE *proc_self_mountinfo) {

        _cleanup_set_free_free_ Set *done = NULL;
        _cleanup_free_ char *simplified = NULL;
        int r;

        assert(prefix);
        assert(proc_self_mountinfo);

        /* Recursively remount a directory (and all its submounts) read-only or read-write. If the directory is already
         * mounted, we reuse the mount and simply mark it MS_BIND|MS_RDONLY (or remove the MS_RDONLY for read-write
         * operation). If it isn't we first make it one. Afterwards we apply MS_BIND|MS_RDONLY (or remove MS_RDONLY) to
         * all submounts we can access, too. When mounts are stacked on the same mount point we only care for each
         * individual "top-level" mount on each point, as we cannot influence/access the underlying mounts anyway. We
         * do not have any effect on future submounts that might get propagated, they might be writable. This includes
         * future submounts that have been triggered via autofs.
         *
         * If the "deny_list" parameter is specified it may contain a list of subtrees to exclude from the
         * remount operation. Note that we'll ignore the deny list for the top-level path. */

        simplified = strdup(prefix);
        if (!simplified)
                return -ENOMEM;

        path_simplify(simplified, false);

        done = set_new(&path_hash_ops);
        if (!done)
                return -ENOMEM;

        for (;;) {
                _cleanup_set_free_free_ Set *todo = NULL;
                _cleanup_(mnt_free_tablep) struct libmnt_table *table = NULL;
                _cleanup_(mnt_free_iterp) struct libmnt_iter *iter = NULL;
                bool top_autofs = false;
                char *x;
                unsigned long orig_flags;

                todo = set_new(&path_hash_ops);
                if (!todo)
                        return -ENOMEM;

                rewind(proc_self_mountinfo);

                r = libmount_parse("/proc/self/mountinfo", proc_self_mountinfo, &table, &iter);
                if (r < 0)
                        return log_debug_errno(r, "Failed to parse /proc/self/mountinfo: %m");

                for (;;) {
                        struct libmnt_fs *fs;
                        const char *path, *type;

                        r = mnt_table_next_fs(table, iter, &fs);
                        if (r == 1)
                                break;
                        if (r < 0)
                                return log_debug_errno(r, "Failed to get next entry from /proc/self/mountinfo: %m");

                        path = mnt_fs_get_target(fs);
                        type = mnt_fs_get_fstype(fs);
                        if (!path || !type)
                                continue;

                        if (!path_startswith(path, simplified))
                                continue;

                        /* Ignore this mount if it is deny-listed, but only if it isn't the top-level mount
                         * we shall operate on. */
                        if (!path_equal(path, simplified)) {
                                bool deny_listed = false;
                                char **i;

                                STRV_FOREACH(i, deny_list) {
                                        if (path_equal(*i, simplified))
                                                continue;

                                        if (!path_startswith(*i, simplified))
                                                continue;

                                        if (path_startswith(path, *i)) {
                                                deny_listed = true;
                                                log_debug("Not remounting %s deny-listed by %s, called for %s",
                                                          path, *i, simplified);
                                                break;
                                        }
                                }
                                if (deny_listed)
                                        continue;
                        }

                        /* Let's ignore autofs mounts.  If they aren't
                         * triggered yet, we want to avoid triggering
                         * them, as we don't make any guarantees for
                         * future submounts anyway.  If they are
                         * already triggered, then we will find
                         * another entry for this. */
                        if (streq(type, "autofs")) {
                                top_autofs = top_autofs || path_equal(path, simplified);
                                continue;
                        }

                        if (!set_contains(done, path)) {
                                r = set_put_strdup(&todo, path);
                                if (r < 0)
                                        return r;
                        }
                }

                /* If we have no submounts to process anymore and if
                 * the root is either already done, or an autofs, we
                 * are done */
                if (set_isempty(todo) &&
                    (top_autofs || set_contains(done, simplified)))
                        return 0;

                if (!set_contains(done, simplified) &&
                    !set_contains(todo, simplified)) {
                        /* The prefix directory itself is not yet a mount, make it one. */
                        if (mount(simplified, simplified, NULL, MS_BIND|MS_REC, NULL) < 0)
                                return -errno;

                        orig_flags = 0;
                        (void) get_mount_flags(table, simplified, &orig_flags);

                        if (mount(NULL, simplified, NULL, (orig_flags & ~flags_mask)|MS_BIND|MS_REMOUNT|new_flags, NULL) < 0)
                                return -errno;

                        log_debug("Made top-level directory %s a mount point.", prefix);

                        r = set_put_strdup(&done, simplified);
                        if (r < 0)
                                return r;
                }

                while ((x = set_steal_first(todo))) {

                        r = set_consume(done, x);
                        if (IN_SET(r, 0, -EEXIST))
                                continue;
                        if (r < 0)
                                return r;

                        /* Deal with mount points that are obstructed by a later mount */
                        r = path_is_mount_point(x, NULL, 0);
                        if (IN_SET(r, 0, -ENOENT))
                                continue;
                        if (IN_SET(r, -EACCES, -EPERM)) {
                                /* Even if root user invoke this, submounts under private FUSE or NFS mount points
                                 * may not be acceessed. E.g.,
                                 *
                                 * $ bindfs --no-allow-other ~/mnt/mnt ~/mnt/mnt
                                 * $ bindfs --no-allow-other ~/mnt ~/mnt
                                 *
                                 * Then, root user cannot access the mount point ~/mnt/mnt.
                                 * In such cases, the submounts are ignored, as we have no way to manage them. */
                                log_debug_errno(r, "Failed to determine '%s' is mount point or not, ignoring: %m", x);
                                continue;
                        }
                        if (r < 0)
                                return r;

                        /* Try to reuse the original flag set */
                        orig_flags = 0;
                        (void) get_mount_flags(table, x, &orig_flags);

                        if (mount(NULL, x, NULL, (orig_flags & ~flags_mask)|MS_BIND|MS_REMOUNT|new_flags, NULL) < 0)
                                return -errno;

                        log_debug("Remounted %s read-only.", x);
                }
        }
}

int bind_remount_recursive(
                const char *prefix,
                unsigned long new_flags,
                unsigned long flags_mask,
                char **deny_list) {

        _cleanup_fclose_ FILE *proc_self_mountinfo = NULL;
        int r;

        r = fopen_unlocked("/proc/self/mountinfo", "re", &proc_self_mountinfo);
        if (r < 0)
                return r;

        return bind_remount_recursive_with_mountinfo(prefix, new_flags, flags_mask, deny_list, proc_self_mountinfo);
}

int bind_remount_one_with_mountinfo(
                const char *path,
                unsigned long new_flags,
                unsigned long flags_mask,
                FILE *proc_self_mountinfo) {

        _cleanup_(mnt_free_tablep) struct libmnt_table *table = NULL;
        unsigned long orig_flags = 0;
        int r;

        assert(path);
        assert(proc_self_mountinfo);

        rewind(proc_self_mountinfo);

        table = mnt_new_table();
        if (!table)
                return -ENOMEM;

        r = mnt_table_parse_stream(table, proc_self_mountinfo, "/proc/self/mountinfo");
        if (r < 0)
                return r;

        /* Try to reuse the original flag set */
        (void) get_mount_flags(table, path, &orig_flags);

        if (mount(NULL, path, NULL, (orig_flags & ~flags_mask)|MS_BIND|MS_REMOUNT|new_flags, NULL) < 0)
                return -errno;

        return 0;
}

int mount_move_root(const char *path) {
        assert(path);

        if (chdir(path) < 0)
                return -errno;

        if (mount(path, "/", NULL, MS_MOVE, NULL) < 0)
                return -errno;

        if (chroot(".") < 0)
                return -errno;

        if (chdir("/") < 0)
                return -errno;

        return 0;
}

int repeat_unmount(const char *path, int flags) {
        bool done = false;

        assert(path);

        /* If there are multiple mounts on a mount point, this
         * removes them all */

        for (;;) {
                if (umount2(path, flags) < 0) {

                        if (errno == EINVAL)
                                return done;

                        return -errno;
                }

                done = true;
        }
}

int mode_to_inaccessible_node(
                const char *runtime_dir,
                mode_t mode,
                char **ret) {

        /* This function maps a node type to a corresponding inaccessible file node. These nodes are created
         * during early boot by PID 1. In some cases we lacked the privs to create the character and block
         * devices (maybe because we run in an userns environment, or miss CAP_SYS_MKNOD, or run with a
         * devices policy that excludes device nodes with major and minor of 0), but that's fine, in that
         * case we use an AF_UNIX file node instead, which is not the same, but close enough for most
         * uses. And most importantly, the kernel allows bind mounts from socket nodes to any non-directory
         * file nodes, and that's the most important thing that matters.
         *
         * Note that the runtime directory argument shall be the top-level runtime directory, i.e. /run/ if
         * we operate in system context and $XDG_RUNTIME_DIR if we operate in user context. */

        _cleanup_free_ char *d = NULL;
        const char *node = NULL;
        bool fallback = false;

        assert(ret);

        if (!runtime_dir)
                runtime_dir = "/run";

        switch(mode & S_IFMT) {
                case S_IFREG:
                        node = "/systemd/inaccessible/reg";
                        break;

                case S_IFDIR:
                        node = "/systemd/inaccessible/dir";
                        break;

                case S_IFCHR:
                        node = "/systemd/inaccessible/chr";
                        fallback = true;
                        break;

                case S_IFBLK:
                        node = "/systemd/inaccessible/blk";
                        fallback = true;
                        break;

                case S_IFIFO:
                        node = "/systemd/inaccessible/fifo";
                        break;

                case S_IFSOCK:
                        node = "/systemd/inaccessible/sock";
                        break;
        }
        if (!node)
                return -EINVAL;

        d = path_join(runtime_dir, node);
        if (!d)
                return -ENOMEM;

        if (fallback && access(d, F_OK) < 0) {
                free(d);
                d = path_join(runtime_dir, "/systemd/inaccessible/sock");
                if (!d)
                        return -ENOMEM;
        }

        *ret = TAKE_PTR(d);
        return 0;
}

#define FLAG(name) (flags & name ? STRINGIFY(name) "|" : "")
static char* mount_flags_to_string(long unsigned flags) {
        char *x;
        _cleanup_free_ char *y = NULL;
        long unsigned overflow;

        overflow = flags & ~(MS_RDONLY |
                             MS_NOSUID |
                             MS_NODEV |
                             MS_NOEXEC |
                             MS_SYNCHRONOUS |
                             MS_REMOUNT |
                             MS_MANDLOCK |
                             MS_DIRSYNC |
                             MS_NOATIME |
                             MS_NODIRATIME |
                             MS_BIND |
                             MS_MOVE |
                             MS_REC |
                             MS_SILENT |
                             MS_POSIXACL |
                             MS_UNBINDABLE |
                             MS_PRIVATE |
                             MS_SLAVE |
                             MS_SHARED |
                             MS_RELATIME |
                             MS_KERNMOUNT |
                             MS_I_VERSION |
                             MS_STRICTATIME |
                             MS_LAZYTIME);

        if (flags == 0 || overflow != 0)
                if (asprintf(&y, "%lx", overflow) < 0)
                        return NULL;

        x = strjoin(FLAG(MS_RDONLY),
                    FLAG(MS_NOSUID),
                    FLAG(MS_NODEV),
                    FLAG(MS_NOEXEC),
                    FLAG(MS_SYNCHRONOUS),
                    FLAG(MS_REMOUNT),
                    FLAG(MS_MANDLOCK),
                    FLAG(MS_DIRSYNC),
                    FLAG(MS_NOATIME),
                    FLAG(MS_NODIRATIME),
                    FLAG(MS_BIND),
                    FLAG(MS_MOVE),
                    FLAG(MS_REC),
                    FLAG(MS_SILENT),
                    FLAG(MS_POSIXACL),
                    FLAG(MS_UNBINDABLE),
                    FLAG(MS_PRIVATE),
                    FLAG(MS_SLAVE),
                    FLAG(MS_SHARED),
                    FLAG(MS_RELATIME),
                    FLAG(MS_KERNMOUNT),
                    FLAG(MS_I_VERSION),
                    FLAG(MS_STRICTATIME),
                    FLAG(MS_LAZYTIME),
                    y);
        if (!x)
                return NULL;
        if (!y)
                x[strlen(x) - 1] = '\0'; /* truncate the last | */
        return x;
}

int mount_verbose(
                int error_log_level,
                const char *what,
                const char *where,
                const char *type,
                unsigned long flags,
                const char *options) {

        _cleanup_free_ char *fl = NULL, *o = NULL;
        unsigned long f;
        int r;

        r = mount_option_mangle(options, flags, &f, &o);
        if (r < 0)
                return log_full_errno(error_log_level, r,
                                      "Failed to mangle mount options %s: %m",
                                      strempty(options));

        fl = mount_flags_to_string(f);

        if ((f & MS_REMOUNT) && !what && !type)
                log_debug("Remounting %s (%s \"%s\")...",
                          where, strnull(fl), strempty(o));
        else if (!what && !type)
                log_debug("Mounting %s (%s \"%s\")...",
                          where, strnull(fl), strempty(o));
        else if ((f & MS_BIND) && !type)
                log_debug("Bind-mounting %s on %s (%s \"%s\")...",
                          what, where, strnull(fl), strempty(o));
        else if (f & MS_MOVE)
                log_debug("Moving mount %s ??? %s (%s \"%s\")...",
                          what, where, strnull(fl), strempty(o));
        else
                log_debug("Mounting %s on %s (%s \"%s\")...",
                          strna(type), where, strnull(fl), strempty(o));
        if (mount(what, where, type, f, o) < 0)
                return log_full_errno(error_log_level, errno,
                                      "Failed to mount %s (type %s) on %s (%s \"%s\"): %m",
                                      strna(what), strna(type), where, strnull(fl), strempty(o));
        return 0;
}

int umount_verbose(const char *what) {
        log_debug("Umounting %s...", what);
        if (umount(what) < 0)
                return log_error_errno(errno, "Failed to unmount %s: %m", what);
        return 0;
}

int mount_option_mangle(
                const char *options,
                unsigned long mount_flags,
                unsigned long *ret_mount_flags,
                char **ret_remaining_options) {

        const struct libmnt_optmap *map;
        _cleanup_free_ char *ret = NULL;
        const char *p;
        int r;

        /* This extracts mount flags from the mount options, and store
         * non-mount-flag options to '*ret_remaining_options'.
         * E.g.,
         * "rw,nosuid,nodev,relatime,size=1630748k,mode=700,uid=1000,gid=1000"
         * is split to MS_NOSUID|MS_NODEV|MS_RELATIME and
         * "size=1630748k,mode=700,uid=1000,gid=1000".
         * See more examples in test-mount-utils.c.
         *
         * Note that if 'options' does not contain any non-mount-flag options,
         * then '*ret_remaining_options' is set to NULL instead of empty string.
         * Note that this does not check validity of options stored in
         * '*ret_remaining_options'.
         * Note that if 'options' is NULL, then this just copies 'mount_flags'
         * to '*ret_mount_flags'. */

        assert(ret_mount_flags);
        assert(ret_remaining_options);

        map = mnt_get_builtin_optmap(MNT_LINUX_MAP);
        if (!map)
                return -EINVAL;

        p = options;
        for (;;) {
                _cleanup_free_ char *word = NULL;
                const struct libmnt_optmap *ent;

                r = extract_first_word(&p, &word, ",", EXTRACT_UNQUOTE);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                for (ent = map; ent->name; ent++) {
                        /* All entries in MNT_LINUX_MAP do not take any argument.
                         * Thus, ent->name does not contain "=" or "[=]". */
                        if (!streq(word, ent->name))
                                continue;

                        if (!(ent->mask & MNT_INVERT))
                                mount_flags |= ent->id;
                        else if (mount_flags & ent->id)
                                mount_flags ^= ent->id;

                        break;
                }

                /* If 'word' is not a mount flag, then store it in '*ret_remaining_options'. */
                if (!ent->name && !strextend_with_separator(&ret, ",", word, NULL))
                        return -ENOMEM;
        }

        *ret_mount_flags = mount_flags;
        *ret_remaining_options = TAKE_PTR(ret);

        return 0;
}
