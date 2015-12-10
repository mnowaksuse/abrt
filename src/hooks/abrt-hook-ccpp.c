/*
    abrt-hook-ccpp.cpp - the hook for C/C++ crashing program

    Copyright (C) 2009	Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009	RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <syslog.h>
#include <fnmatch.h>
#include <sys/utsname.h>
#include "libabrt.h"
#include <selinux/selinux.h>

#define  DUMP_SUID_UNSAFE 1
#define  DUMP_SUID_SAFE 2

static int g_user_core_flags;
static int g_need_nonrelative;

/* I want to use -Werror, but gcc-4.4 throws a curveball:
 * "warning: ignoring return value of 'ftruncate', declared with attribute warn_unused_result"
 * and (void) cast is not enough to shut it up! Oh God...
 */
#define IGNORE_RESULT(func_call) do { if (func_call) /* nothing */; } while (0)

static char* malloc_readlinkat(int dir_fd, const char *linkname)
{
    char buf[PATH_MAX + 1];
    int len;

    len = readlinkat(dir_fd, linkname, buf, sizeof(buf)-1);
    if (len >= 0)
    {
        buf[len] = '\0';
        return xstrdup(buf);
    }
    return NULL;
}

static char* malloc_readlink(const char *linkname)
{
    return malloc_readlinkat(AT_FDCWD, linkname);
}

/* Custom version of copyfd_xyz,
 * one which is able to write into two descriptors at once.
 */
#define CONFIG_FEATURE_COPYBUF_KB 4
static off_t copyfd_sparse(int src_fd, int dst_fd1, int dst_fd2, off_t size2)
{
	off_t total = 0;
	int last_was_seek = 0;
#if CONFIG_FEATURE_COPYBUF_KB <= 4
	char buffer[CONFIG_FEATURE_COPYBUF_KB * 1024];
	enum { buffer_size = sizeof(buffer) };
#else
	char *buffer;
	int buffer_size;

	/* We want page-aligned buffer, just in case kernel is clever
	 * and can do page-aligned io more efficiently */
	buffer = mmap(NULL, CONFIG_FEATURE_COPYBUF_KB * 1024,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON,
			/* ignored: */ -1, 0);
	buffer_size = CONFIG_FEATURE_COPYBUF_KB * 1024;
	if (buffer == MAP_FAILED) {
		buffer = alloca(4 * 1024);
		buffer_size = 4 * 1024;
	}
#endif

	while (1) {
		ssize_t rd = safe_read(src_fd, buffer, buffer_size);
		if (!rd) { /* eof */
			if (last_was_seek) {
				if (lseek(dst_fd1, -1, SEEK_CUR) < 0
				 || safe_write(dst_fd1, "", 1) != 1
				 || (dst_fd2 >= 0
				     && (lseek(dst_fd2, -1, SEEK_CUR) < 0
					 || safe_write(dst_fd2, "", 1) != 1
				        )
				    )
				) {
					perror_msg("Write error");
					total = -1;
					goto out;
				}
			}
			/* all done */
			goto out;
		}
		if (rd < 0) {
			perror_msg("Read error");
			total = -1;
			goto out;
		}

		/* checking sparseness */
		ssize_t cnt = rd;
		while (--cnt >= 0) {
			if (buffer[cnt] != 0) {
				/* not sparse */
				errno = 0;
				ssize_t wr1 = full_write(dst_fd1, buffer, rd);
				ssize_t wr2 = (dst_fd2 >= 0 ? full_write(dst_fd2, buffer, rd) : rd);
				if (wr1 < rd || wr2 < rd) {
					perror_msg("Write error");
					total = -1;
					goto out;
				}
				last_was_seek = 0;
				goto adv;
			}
		}
		/* sparse */
		xlseek(dst_fd1, rd, SEEK_CUR);
		if (dst_fd2 >= 0)
			xlseek(dst_fd2, rd, SEEK_CUR);
		last_was_seek = 1;
 adv:
		total += rd;
		size2 -= rd;
		if (size2 < 0)
			dst_fd2 = -1;
// truncate to 0 or even delete the second file?
// No, kernel does not delete nor truncate core files.
	}
 out:

#if CONFIG_FEATURE_COPYBUF_KB > 4
	if (buffer_size != 4 * 1024)
		munmap(buffer, buffer_size);
#endif
	return total;
}


/* Global data */

static char *user_pwd;
static DIR *proc_cwd;
static struct dump_dir *dd;
static int user_core_fd = -1;
/*
 * %s - signal number
 * %c - ulimit -c value
 * %p - pid
 * %u - uid
 * %g - gid
 * %t - UNIX time of dump
 * %e - executable filename
 * %h - hostname
 * %% - output one "%"
 */
/* Hook must be installed with exactly the same sequence of %c specifiers.
 * Last one, %h, may be omitted (we can find it out).
 */
static const char percent_specifiers[] = "%scpugteh";
static char *core_basename = (char*) "core";

static char* get_executable(pid_t pid, int *fd_p)
{
    char buf[sizeof("/proc/%lu/exe") + sizeof(long)*3];

    sprintf(buf, "/proc/%lu/exe", (long)pid);
    *fd_p = open(buf, O_RDONLY); /* might fail and return -1, it's ok */
    char *executable = malloc_readlink(buf);
    if (!executable)
        return NULL;
    /* find and cut off " (deleted)" from the path */
    char *deleted = executable + strlen(executable) - strlen(" (deleted)");
    if (deleted > executable && strcmp(deleted, " (deleted)") == 0)
    {
        *deleted = '\0';
        log("File '%s' seems to be deleted", executable);
    }
    /* find and cut off prelink suffixes from the path */
    char *prelink = executable + strlen(executable) - strlen(".#prelink#.XXXXXX");
    if (prelink > executable && strncmp(prelink, ".#prelink#.", strlen(".#prelink#.")) == 0)
    {
        log("File '%s' seems to be a prelink temporary file", executable);
        *prelink = '\0';
    }
    return executable;
}

static DIR *open_cwd(pid_t pid)
{
    char buf[sizeof("/proc/%lu/cwd") + sizeof(long)*3];
    sprintf(buf, "/proc/%lu/cwd", (long)pid);

    DIR *cwd = opendir(buf);
    if (cwd == NULL)
        perror_msg("Can't open process's CWD for CompatCore");

    return cwd;
}

static char* get_cwd(pid_t pid)
{
    char buf[sizeof("/proc/%lu/cwd") + sizeof(long)*3];
    sprintf(buf, "/proc/%lu/cwd", (long)pid);
    return malloc_readlink(buf);
}

static char* get_rootdir(pid_t pid)
{
    char buf[sizeof("/proc/%lu/root") + sizeof(long)*3];
    sprintf(buf, "/proc/%lu/root", (long)pid);
    return malloc_readlink(buf);
}

static int get_proc_fs_id(pid_t pid, char type)
{
    const char *scanf_format = "%*cid:\t%d\t%d\t%d\t%d\n";
    char id_type[] = "_id";
    id_type[0] = type;

    int real, e_id, saved;
    int fs_id = 0;

    char filename[sizeof("/proc/%lu/status") + sizeof(long)*3];
    sprintf(filename, "/proc/%lu/status", (long)pid);
    FILE *file = fopen(filename, "r");
    if (!file)
        /* rather bail out than create core with wrong permission */
        perror_msg_and_die("Can't open %s", filename);

    char line[128];
    while (fgets(line, sizeof(line), file) != NULL)
    {
        if (strncmp(line, id_type, 3) == 0)
        {
            int n = sscanf(line, scanf_format, &real, &e_id, &saved, &fs_id);
            if (n != 4)
            {
                perror_msg_and_die("Can't parse %cid: line in %s", type, filename);
            }

            fclose(file);
            return fs_id;
        }
    }
    fclose(file);

    perror_msg_and_die("Failed to get file system %cID of the crashed process", type);
}

static int get_fsuid(pid_t pid)
{
    return get_proc_fs_id(pid, /*UID*/'U');
}

static int get_fsgid(pid_t pid)
{
    return get_proc_fs_id(pid, /*GID*/'G');
}

static int dump_suid_policy()
{
    /*
     - values are:
       0 - don't dump suided programs - in this case the hook is not called by kernel
       1 - create coredump readable by fs_uid
       2 - create coredump readable by root only
    */
    int c;
    int suid_dump_policy = 0;
    const char *filename = "/proc/sys/fs/suid_dumpable";
    FILE *f  = fopen(filename, "r");
    if (!f)
    {
        log("Can't open %s", filename);
        return suid_dump_policy;
    }

    c = fgetc(f);
    fclose(f);
    if (c != EOF)
        suid_dump_policy = c - '0';

    //log("suid dump policy is: %i", suid_dump_policy);
    return suid_dump_policy;
}

/* Computes a security context of new file created by the given process with
 * pid in the given directory represented by file descriptor.
 *
 * On errors returns negative number. Returns 0 if the function succeeds and
 * computes the context and returns positive number and assigns NULL to newcon
 * if the security context is not needed (SELinux disabled).
 */
static int compute_selinux_con_for_new_file(pid_t pid, int dir_fd, security_context_t *newcon)
{
    security_context_t srccon;
    security_context_t dstcon;

    const int r = is_selinux_enabled();
    if (r == 0)
    {
        *newcon = NULL;
        return 1;
    }
    else if (r == -1)
    {
        perror_msg("Couldn't get state of SELinux");
        return -1;
    }
    else if (r != 1)
        error_msg_and_die("Unexpected SELinux return value: %d", r);


    if (getpidcon_raw(pid, &srccon) < 0)
    {
        perror_msg("getpidcon_raw(%d)", pid);
        return -1;
    }

    if (fgetfilecon_raw(dir_fd, &dstcon) < 0)
    {
        perror_msg("getfilecon_raw(%s)", user_pwd);
        return -1;
    }

    if (security_compute_create_raw(srccon, dstcon, string_to_security_class("file"), newcon) < 0)
    {
        perror_msg("security_compute_create_raw(%s, %s, 'file')", srccon, dstcon);
        return -1;
    }

    return 0;
}

static int open_user_core(uid_t uid, uid_t fsuid, pid_t pid, char **percent_values)
{
    proc_cwd = open_cwd(pid);
    if (proc_cwd == NULL)
        return -1;

    errno = 0;

    /* http://article.gmane.org/gmane.comp.security.selinux/21842 */
    security_context_t newcon;
    if (compute_selinux_con_for_new_file(pid, dirfd(proc_cwd), &newcon) < 0)
    {
        error_msg("Not going to create a user core due to SELinux errors");
        return -1;
    }

    if (strcmp(core_basename, "core") == 0)
    {
        /* Mimic "core.PID" if requested */
        char buf[] = "0\n";
        int fd = open("/proc/sys/kernel/core_uses_pid", O_RDONLY);
        if (fd >= 0)
        {
            IGNORE_RESULT(read(fd, buf, sizeof(buf)));
            close(fd);
        }
        if (strcmp(buf, "1\n") == 0)
        {
            core_basename = xasprintf("%s.%lu", core_basename, (long)pid);
        }
    }
    else
    {
        /* Expand old core pattern, put expanded name in core_basename */
        core_basename = xstrdup(core_basename);
        unsigned idx = 0;
        while (1)
        {
            char c = core_basename[idx];
            if (!c)
                break;
            idx++;
            if (c != '%')
                continue;

            /* We just copied %, look at following char and expand %c */
            c = core_basename[idx];
            unsigned specifier_num = strchrnul(percent_specifiers, c) - percent_specifiers;
            if (percent_specifiers[specifier_num] != '\0') /* valid %c (might be %% too) */
            {
                const char *val = "%";
                if (specifier_num > 0) /* not %% */
                    val = percent_values[specifier_num - 1];
                //log("c:'%c'", c);
                //log("val:'%s'", val);

                /* Replace %c at core_basename[idx] by its value */
                idx--;
                char *old = core_basename;
                core_basename = xasprintf("%.*s%s%s", idx, core_basename, val, core_basename + idx + 2);
                //log("pos:'%*s|'", idx, "");
                //log("new:'%s'", core_basename);
                //log("old:'%s'", old);
                free(old);
                idx += strlen(val);
            }
            /* else: invalid %c, % is already copied verbatim,
             * next loop iteration will copy c */
        }
    }

    if (g_need_nonrelative && core_basename[0] != '/')
    {
        error_msg("Current suid_dumpable policy prevents from saving core dumps according to relative core_pattern");
        return -1;
    }

    /* Open (create) compat core file.
     * man core:
     * There are various circumstances in which a core dump file
     * is not produced:
     *
     * [skipped obvious ones]
     * The process does not have permission to write the core file.
     * ...if a file with the same name exists and is not writable
     * or is not a regular file (e.g., it is a directory or a symbolic link).
     *
     * A file with the same name already exists, but there is more
     * than one hard link to that file.
     *
     * The file system where the core dump file would be created is full;
     * or has run out of inodes; or is mounted read-only;
     * or the user has reached their quota for the file system.
     *
     * The RLIMIT_CORE or RLIMIT_FSIZE resource limits for the process
     * are set to zero.
     * [we check RLIMIT_CORE, but how can we check RLIMIT_FSIZE?]
     *
     * The binary being executed by the process does not have
     * read permission enabled. [how we can check it here?]
     *
     * The process is executing a set-user-ID (set-group-ID) program
     * that is owned by a user (group) other than the real
     * user (group) ID of the process. [TODO?]
     * (However, see the description of the prctl(2) PR_SET_DUMPABLE operation,
     * and the description of the /proc/sys/fs/suid_dumpable file in proc(5).)
     */

    int user_core_fd = -1;
    int selinux_fail = 1;

    /*
     * These calls must be reverted as soon as possible.
     */
    xsetegid(get_fsgid(pid));
    xseteuid(fsuid);

    /* Set SELinux context like kernel when creating core dump file.
     * This condition is TRUE if */
    if (/* SELinux is disabled  */ newcon == NULL
     || /* or the call succeeds */ setfscreatecon_raw(newcon) >= 0)
    {
        /* Do not O_TRUNC: if later checks fail, we do not want to have file already modified here */
        user_core_fd = openat(dirfd(proc_cwd), core_basename, O_WRONLY | O_CREAT | O_NOFOLLOW | g_user_core_flags, 0600); /* kernel makes 0600 too */

        /* Do the error check here and print the error message in order to
         * avoid interference in 'errno' usage caused by SELinux functions */
        if (user_core_fd < 0)
            perror_msg("Can't open '%s' at '%s'", core_basename, user_pwd);

        /* Fail if SELinux is enabled and the call fails */
        if (newcon != NULL && setfscreatecon_raw(NULL) < 0)
            perror_msg("setfscreatecon_raw(NULL)");
        else
            selinux_fail = 0;
    }
    else
        perror_msg("setfscreatecon_raw(%s)", newcon);

    /*
     * DON'T JUMP OVER THIS REVERT OF THE UID/GID CHANGES
     */
    xsetegid(0);
    xseteuid(0);

    if (user_core_fd < 0 || selinux_fail)
        goto user_core_fail;

    struct stat sb;
    if (fstat(user_core_fd, &sb) != 0
     || !S_ISREG(sb.st_mode)
     || sb.st_nlink != 1
     || sb.st_uid != fsuid
    ) {
        perror_msg("'%s' at '%s' is not a regular file with link count 1 owned by UID(%d)", core_basename, user_pwd, fsuid);
        goto user_core_fail;
    }
    if (ftruncate(user_core_fd, 0) != 0) {
        /* perror first, otherwise unlink may trash errno */
        perror_msg("Can't truncate '%s' at '%s' to size 0", core_basename, user_pwd);
        goto user_core_fail;
    }

    return user_core_fd;

user_core_fail:
    if (user_core_fd >= 0)
        close(user_core_fd);
    return -1;
}

static int close_user_core(int user_core_fd, off_t core_size)
{
    if (user_core_fd >= 0 && (fsync(user_core_fd) != 0 || close(user_core_fd) != 0 || core_size < 0))
    {
        perror_msg("Error writing '%s' at '%s'", core_basename, user_pwd);
        return -1;
    }
    return 0;
}

static bool dump_fd_info(const char *dest_filename, char *source_filename, int source_base_ofs, uid_t uid, gid_t gid)
{
    FILE *fp = fopen(dest_filename, "wx");
    if (!fp)
        return false;

    unsigned fd = 0;
    while (fd <= 99999) /* paranoia check */
    {
        sprintf(source_filename + source_base_ofs, "fd/%u", fd);
        char *name = malloc_readlink(source_filename);
        if (!name)
            break;
        fprintf(fp, "%u:%s\n", fd, name);
        free(name);

        sprintf(source_filename + source_base_ofs, "fdinfo/%u", fd);
        fd++;
        FILE *in = fopen(source_filename, "r");
        if (!in)
            continue;
        char buf[128];
        while (fgets(buf, sizeof(buf)-1, in))
        {
            /* in case the line is not terminated, terminate it */
            char *eol = strchrnul(buf, '\n');
            eol[0] = '\n';
            eol[1] = '\0';
            fputs(buf, fp);
        }
        fclose(in);
    }

    const int dest_fd = fileno(fp);
    if (fchown(dest_fd, uid, gid) < 0)
    {
        perror_msg("Can't change '%s' ownership to %lu:%lu", dest_filename, (long)uid, (long)gid);
        fclose(fp);
        unlink(dest_filename);
        return false;
    }

    fclose(fp);
    return true;
}

/* Like xopen, but on error, unlocks and deletes dd and user core */
static int create_or_die(const char *filename)
{
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0640);
    if (fd >= 0)
    {
        IGNORE_RESULT(fchown(fd, dd->dd_uid, dd->dd_gid));
        return fd;
    }

    int sv_errno = errno;
    if (dd)
        dd_delete(dd);
    if (user_core_fd >= 0)
        close(user_core_fd);

    errno = sv_errno;
    perror_msg_and_die("Can't open '%s'", filename);
}

static char *get_dev_log_socket_inode(void)
{
    char *dev_log_path = xstrdup("/dev/log");
    char *tmp = NULL;
    while ((tmp = malloc_readlink(dev_log_path)) != NULL)
    {
        free(dev_log_path);
        dev_log_path = tmp;
    }

    if (errno != EINVAL)
    {
        free(dev_log_path);
        return NULL;
    }

    FILE *fpnu = fopen("/proc/net/unix", "r");
    if (fpnu == NULL)
        return NULL;

    /* skip the first line with column headers */
    int c = 0;
    while ((c = fgetc(fpnu)) != EOF && c != '\n')
        ;

    char *socket_inode = NULL;
    long inode_pos = 0;
    while (feof(fpnu) == 0 && ferror(fpnu) == 0)
    {
        /* Read fields delimited by single ' ', the 7th field might be
         * delimited with several ' '. */
        int field = 0;
        while ((c = fgetc(fpnu)) != EOF && c != '\n')
        {
            if (c != ' ')
                continue;

            ++field;
            if (field == 6)
            {
                /* We are going to read the 7th field, so we have to ignore
                 * multiple occurrences of ' '. Thanks jstancek! */
                while ((c = fgetc(fpnu)) != EOF && c == ' ')
                    ;

                /* Must be greater than 0, because we are going to decrement it
                 * to move inode_pos at beginnig of the 7th field (skipping of
                 * multiple ' ' moved us to the 2nd byte of the 7th field). */
                if ((inode_pos = ftell(fpnu)) <= 0)
                    goto cleanup;
                --inode_pos;
            }
            else if (field == 7)
                break;
        }

        if (c != '\n' && field == 7)
        {
            /* inode_pos is the white space right before the inode value */
            const long cur_pos = ftell(fpnu);
            if (cur_pos <= inode_pos)
                goto cleanup;

            const long inode_len = (cur_pos - inode_pos) - 1;

            const char *path_iter = dev_log_path;
            while ((c = fgetc(fpnu)) != EOF && c != '\n' && c == *path_iter)
                ++path_iter;

            if (c == '\n' && path_iter[0] == '\0')
            {
                socket_inode = xmalloc(inode_len + 1);

                if (fseek(fpnu, inode_pos, SEEK_SET))
                    goto cleanup;

                if (fread(socket_inode, 1, inode_len, fpnu) != inode_len)
                {
                    free(socket_inode);
                    socket_inode = NULL;
                }

                goto cleanup;
            }
        }

        while ((c = fgetc(fpnu)) != EOF && c != '\n')
            ;
    }

cleanup:
    fclose(fpnu);
    free(dev_log_path);

    return socket_inode;
}

static bool is_path_ignored(const GList *list, const char *path)
{
    const GList *li;
    for (li = list; li != NULL; li = g_list_next(li))
    {
        if (fnmatch((char*)li->data, path, /*flags:*/ 0) == 0)
        {
            return true;
        }
    }
    return false;
}

/* Like other glibc functions this one also return 0 on logical true, positive
 * number on logical false and negative number on error. */
static int process_is_syslog(pid_t pid)
{
    char *socket_inode = NULL;
    int r = 1;

    char proc_pid_fd_path[sizeof("/proc/%lu/fd") + sizeof(long)*3];
    sprintf(proc_pid_fd_path, "/proc/%lu/fd", (long)pid);

    DIR *proc_fd_dir = opendir(proc_pid_fd_path);
    if (!proc_fd_dir)
        goto cleanup;

    while (1)
    {
        errno = 0;
        struct dirent *dent = readdir(proc_fd_dir);
        if (dent == NULL)
        {
            if (errno > 0)
            {
                r = -errno;
                goto cleanup;
            }
            break;
        }
        else if (dot_or_dotdot(dent->d_name))
            continue;

        char *fdname = malloc_readlinkat(dirfd(proc_fd_dir), dent->d_name);

        if (prefixcmp(fdname, /*prefix*/"socket:[") == 0)
        {
            if (socket_inode == NULL)
            {
                /* get_dev_log_socket_inode() returns NULL on errors */
                socket_inode = get_dev_log_socket_inode();

                if (socket_inode == NULL)
                {
                    free(fdname);
                    r = -1;
                    break;
                }

                /* Abuse trailing '\0' and replace it with ']'. But then we have to
                 * use a function which does not require a null-terminated string. */
                socket_inode[strlen(socket_inode)] = ']';
            }

            const char *fdsocket = fdname + strlen("socket:[");
            /* Only the second argument must be a null-terminated string */
            if (prefixcmp(socket_inode, /*prefix*/fdsocket) == 0)
            {
                free(fdname);
                r = 0;
                break;
            }
        }

        free(fdname);
    }

cleanup:
    free(socket_inode);
    closedir(proc_fd_dir);

    return r;
}

static void error_msg_not_process_crash(const char *pid_str, const char *process_str,
        long unsigned uid, int signal_no, const char *signame, const char *message, ...)
{
    va_list p;
    va_start(p, message);
    char *message_full = xvasprintf(message, p);
    va_end(p);

    if (signame)
        error_msg("Process %s (%s) of user %lu killed by SIG%s - %s", pid_str,
                        process_str, uid, signame, message_full);
    else
        error_msg("Process %s (%s) of user %lu killed by signal %d - %s", pid_str,
                        process_str, uid, signal_no, message_full);

    free(message_full);

    return;
}

int main(int argc, char** argv)
{
    int err = 1;
    /* Kernel starts us with all fd's closed.
     * But it's dangerous:
     * fprintf(stderr) can dump messages into random fds, etc.
     * Ensure that if any of fd 0,1,2 is closed, we open it to /dev/null.
     */
    int fd = xopen("/dev/null", O_RDWR);
    while (fd < 2)
	fd = xdup(fd);
    if (fd > 2)
	close(fd);

    if (argc < 8)
    {
        /* percent specifier:         %s    %c             %p  %u  %g  %t   %e          %h */
        /* argv:                  [0] [1]   [2]            [3] [4] [5] [6]  [7]         [8]*/
        error_msg_and_die("Usage: %s SIGNO CORE_SIZE_LIMIT PID UID GID TIME BINARY_NAME [HOSTNAME]", argv[0]);
    }

    /* Not needed on 2.6.30.
     * At least 2.6.18 has a bug where
     * argv[1] = "DUMPDIR SIGNO CORE_SIZE_LIMIT ..."
     * argv[2] = "SIGNO CORE_SIZE_LIMIT ..."
     * and so on. Fixing it:
     */
    if (strchr(argv[1], ' '))
    {
        int i;
        for (i = 1; argv[i]; i++)
        {
            strchrnul(argv[i], ' ')[0] = '\0';
        }
    }

    const char *pid_str = argv[3];
    pid_t pid = xatoi_positive(argv[3]);

    if (process_is_syslog(pid) == 0)
    {
        logmode = LOGMODE_STDIO;
    }
    else
    {
        openlog("abrt", LOG_PID, LOG_DAEMON);
        logmode = LOGMODE_SYSLOG;
    }

    /* Parse abrt.conf */
    load_abrt_conf();
    /* ... and plugins/CCpp.conf */
    bool setting_MakeCompatCore;
    bool setting_SaveBinaryImage;
    GList *setting_ignored_paths = NULL;
    {
        map_string_t *settings = new_map_string();
        load_abrt_plugin_conf_file("CCpp.conf", settings);
        const char *value;
        value = get_map_string_item_or_NULL(settings, "MakeCompatCore");
        setting_MakeCompatCore = value && string_to_bool(value);
        value = get_map_string_item_or_NULL(settings, "SaveBinaryImage");
        setting_SaveBinaryImage = value && string_to_bool(value);
        value = get_map_string_item_or_NULL(settings, "VerboseLog");
        if (value)
            g_verbose = xatoi_positive(value);
        value = get_map_string_item_or_NULL(settings, "IgnoredPaths");
        if (value)
            setting_ignored_paths = parse_list(value);
        free_map_string(settings);
    }

    errno = 0;
    const char* signal_str = argv[1];
    int signal_no = xatoi_positive(signal_str);
    const char *signame = NULL;
    bool signal_is_fatal_bool = signal_is_fatal(signal_no, &signame);
    off_t ulimit_c = strtoull(argv[2], NULL, 10);
    if (ulimit_c < 0) /* unlimited? */
    {
        /* set to max possible >0 value */
        ulimit_c = ~((off_t)1 << (sizeof(off_t)*8-1));
    }
    uid_t uid = xatoi_positive(argv[4]);
    if (errno || pid <= 0)
    {
        perror_msg_and_die("PID '%s' or limit '%s' is bogus", argv[3], argv[2]);
    }

    FILE *saved_core_pattern = fopen(VAR_RUN"/abrt/saved_core_pattern", "r");
    if (saved_core_pattern)
    {
        char *s = xmalloc_fgetline(saved_core_pattern);
        fclose(saved_core_pattern);
        /* If we have a saved pattern and it's not a "|PROG ARGS" thing... */
        if (s && s[0] != '|')
            core_basename = s;
        else
            free(s);
    }

    struct utsname uts;
    if (!argv[8]) /* no HOSTNAME? */
    {
        uname(&uts);
        argv[8] = uts.nodename;
    }

    int src_fd_binary;
    char *executable = get_executable(pid, &src_fd_binary);
    if (executable && strstr(executable, "/abrt-hook-ccpp"))
    {
        error_msg_and_die("PID %lu is '%s', not dumping it to avoid recursion",
                        (long)pid, executable);
    }

    if (executable && is_path_ignored(setting_ignored_paths, executable))
    {
        error_msg_not_process_crash(pid_str, argv[7], (long unsigned)uid, signal_no,
                signame, "ignoring (listed in 'IgnoredPaths')");

        return 0;
    }

    user_pwd = get_cwd(pid); /* may be NULL on error */
    VERB1 log("user_pwd:'%s'", user_pwd);

    if (!setting_SaveBinaryImage && src_fd_binary >= 0)
    {
        close(src_fd_binary);
        src_fd_binary = -1;
    }

    uid_t fsuid = uid;
    uid_t tmp_fsuid = get_fsuid(pid);
    int suid_policy = dump_suid_policy();
    if (tmp_fsuid != uid)
    {
        /* use root for suided apps unless it's explicitly set to UNSAFE */
        fsuid = 0;
        if (suid_policy == DUMP_SUID_UNSAFE)
            fsuid = tmp_fsuid;
        else
        {
            g_user_core_flags = O_EXCL;
            g_need_nonrelative = 1;
        }
    }

    /* If PrivateReports is on, root owns all problem directories */
    const uid_t dduid = g_settings_privatereports ? 0 : fsuid;

    /* Open a fd to compat coredump, if requested and is possible */
    if (setting_MakeCompatCore && ulimit_c != 0)
        /* note: checks "user_pwd == NULL" inside; updates core_basename */
        user_core_fd = open_user_core(uid, fsuid, pid, &argv[1]);

    if (executable == NULL)
    {
        /* readlink on /proc/$PID/exe failed, don't create abrt dump dir */
        error_msg("Can't read /proc/%lu/exe link", (long)pid);
        goto create_user_core;
    }

    const char *signame = NULL;
    switch (signal_no)
    {
        case SIGILL : signame = "ILL" ; break;
        case SIGFPE : signame = "FPE" ; break;
        case SIGSEGV: signame = "SEGV"; break;
        case SIGBUS : signame = "BUS" ; break; //Bus error (bad memory access)
        case SIGABRT: signame = "ABRT"; break; //usually when abort() was called
    // We have real-world reports from users who see buggy programs
    // dying with SIGTRAP, uncommented it too:
        case SIGTRAP: signame = "TRAP"; break; //Trace/breakpoint trap
    // These usually aren't caused by bugs:
      //case SIGQUIT: signame = "QUIT"; break; //Quit from keyboard
      //case SIGSYS : signame = "SYS" ; break; //Bad argument to routine (SVr4)
      //case SIGXCPU: signame = "XCPU"; break; //CPU time limit exceeded (4.2BSD)
      //case SIGXFSZ: signame = "XFSZ"; break; //File size limit exceeded (4.2BSD)
        default: goto create_user_core; // not a signal we care about
    }

    if (!daemon_is_ok())
    {
        /* not an error, exit with exit code 0 */
        log("abrtd is not running. If it crashed, "
            "/proc/sys/kernel/core_pattern contains a stale value, "
            "consider resetting it to 'core'"
        );
        goto create_user_core;
    }

    if (g_settings_nMaxCrashReportsSize > 0)
    {
        /* x1.25 and round up to 64m: go a bit up, so that usual in-daemon trimming
         * kicks in first, and we don't "fight" with it:
         */
        unsigned maxsize = g_settings_nMaxCrashReportsSize + g_settings_nMaxCrashReportsSize / 4;
        maxsize |= 63;
        check_free_space(maxsize, g_settings_dump_location);
    }

    char path[PATH_MAX];

    /* Check /var/spool/abrt/last-ccpp marker, do not dump repeated crashes
     * if they happen too often. Else, write new marker value.
     */
    snprintf(path, sizeof(path), "%s/last-ccpp", g_settings_dump_location);
    if (check_recent_crash_file(path, executable))
    {
        /* It is a repeating crash */
        if (setting_MakeCompatCore)
            goto create_user_core;
        return 1;
    }

    const char *last_slash = strrchr(executable, '/');
    if (last_slash && strncmp(++last_slash, "abrt", 4) == 0)
    {
        /* If abrtd/abrt-foo crashes, we don't want to create a _directory_,
         * since that can make new copy of abrtd to process it,
         * and maybe crash again...
         * Unlike dirs, mere files are ignored by abrtd.
         */
        if (snprintf(path, sizeof(path), "%s/%s-coredump", g_settings_dump_location, last_slash) >= sizeof(path))
            error_msg_and_die("Error saving '%s': truncated long file path", path);

        int abrt_core_fd = xopen3(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        off_t core_size = copyfd_eof(STDIN_FILENO, abrt_core_fd, COPYFD_SPARSE);
        if (core_size < 0 || fsync(abrt_core_fd) != 0)
        {
            unlink(path);
            /* copyfd_eof logs the error including errno string,
             * but it does not log file name */
            error_msg_and_die("Error saving '%s'", path);
        }
        log("Saved core dump of pid %lu (%s) to %s (%llu bytes)", (long)pid, executable, path, (long long)core_size);
        err = 0;
        goto finito;
    }

    unsigned path_len = snprintf(path, sizeof(path), "%s/ccpp-%s-%lu.new",
            g_settings_dump_location, iso_date_string(NULL), (long)pid);
    if (path_len >= (sizeof(path) - sizeof("/"FILENAME_COREDUMP)))
    {
        return 1;
    }

    /* use dduid (either fsuid or 0) instead of uid, so we don't expose any
     * information of suided app in /var/spool/abrt
     *
     * dd_create_skeleton() creates a new directory and leaves ownership to
     * the current user, hence, we have to call dd_reset_ownership() after the
     * directory is populated.
     */
    dd = dd_create_skeleton(path, dduid, 0640, /*no flags*/0);
    if (dd)
    {
        char *rootdir = get_rootdir(pid);

        /* This function uses fsuid only for its value. The function stores fsuid in a file name 'uid'*/
        dd_create_basic_files(dd, fsuid, NULL);

        char source_filename[sizeof("/proc/%lu/somewhat_long_name") + sizeof(long)*3];
        int source_base_ofs = sprintf(source_filename, "/proc/%lu/smaps", (long)pid);
        source_base_ofs -= strlen("smaps");
        char *dest_filename = concat_path_file(dd->dd_dirname, "also_somewhat_longish_name");
        char *dest_base = strrchr(dest_filename, '/') + 1;

        // Disabled for now: /proc/PID/smaps tends to be BIG,
        // and not much more informative than /proc/PID/maps:
        //copy_file_ext(source_filename, dest_filename, 0640, dd->dd_uid, dd->dd_gid, O_RDONLY, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL);

        strcpy(source_filename + source_base_ofs, "maps");
        strcpy(dest_base, FILENAME_MAPS);
        copy_file_ext(source_filename, dest_filename, 0640, dd->dd_uid, dd->dd_gid, O_RDONLY, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL);

        strcpy(source_filename + source_base_ofs, "limits");
        strcpy(dest_base, FILENAME_LIMITS);
        copy_file_ext(source_filename, dest_filename, 0640, dd->dd_uid, dd->dd_gid, O_RDONLY, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL);

        strcpy(source_filename + source_base_ofs, "cgroup");
        strcpy(dest_base, FILENAME_CGROUP);
        copy_file_ext(source_filename, dest_filename, 0640, dd->dd_uid, dd->dd_gid, O_RDONLY, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL);

        strcpy(dest_base, FILENAME_OPEN_FDS);
        dump_fd_info(dest_filename, source_filename, source_base_ofs, dd->dd_uid, dd->dd_gid);

        free(dest_filename);

        dd_save_text(dd, FILENAME_ANALYZER, "CCpp");
        dd_save_text(dd, FILENAME_TYPE, "CCpp");
        dd_save_text(dd, FILENAME_EXECUTABLE, executable);
        dd_save_text(dd, FILENAME_PID, pid_str);
        if (user_pwd)
            dd_save_text(dd, FILENAME_PWD, user_pwd);
        if (rootdir)
        {
            if (strcmp(rootdir, "/") != 0)
                dd_save_text(dd, FILENAME_ROOTDIR, rootdir);
        }

        char *reason = xasprintf("Process %s was killed by signal %s (SIG%s)",
                                 executable, signal_str, signame ? signame : signal_str);
        dd_save_text(dd, FILENAME_REASON, reason);
        free(reason);

        char *cmdline = get_cmdline(pid);
        dd_save_text(dd, FILENAME_CMDLINE, cmdline ? : "");
        free(cmdline);

        char *environ = get_environ(pid);
        dd_save_text(dd, FILENAME_ENVIRON, environ ? : "");
        free(environ);

        dd_save_text(dd, "abrt_version", VERSION);

        if (src_fd_binary > 0)
        {
            strcpy(path + path_len, "/"FILENAME_BINARY);
            int dst_fd = create_or_die(path);
            off_t sz = copyfd_eof(src_fd_binary, dst_fd, COPYFD_SPARSE);
            if (fsync(dst_fd) != 0 || close(dst_fd) != 0 || sz < 0)
            {
                dd_delete(dd);
                error_msg_and_die("Error saving '%s'", path);
            }
            close(src_fd_binary);
        }

        strcpy(path + path_len, "/"FILENAME_COREDUMP);
        int abrt_core_fd = create_or_die(path);

        /* We write both coredumps at once.
         * We can't write user coredump first, since it might be truncated
         * and thus can't be copied and used as abrt coredump;
         * and if we write abrt coredump first and then copy it as user one,
         * then we have a race when process exits but coredump does not exist yet:
         * $ echo -e '#include<signal.h>\nmain(){raise(SIGSEGV);}' | gcc -o test -x c -
         * $ rm -f core*; ulimit -c unlimited; ./test; ls -l core*
         * 21631 Segmentation fault (core dumped) ./test
         * ls: cannot access core*: No such file or directory <=== BAD
         */
        off_t core_size = copyfd_sparse(STDIN_FILENO, abrt_core_fd, user_core_fd, ulimit_c);

        close_user_core(user_core_fd, core_size);

        if (fsync(abrt_core_fd) != 0 || close(abrt_core_fd) != 0 || core_size < 0)
        {
            unlink(path);
            dd_delete(dd);
            /* copyfd_sparse logs the error including errno string,
             * but it does not log file name */
            error_msg_and_die("Error writing '%s'", path);
        }

/* Provisional code, pending discussion with JVM people */
#if 0
        /* Save JVM crash log if it exists. (JVM's coredump per se
         * is nearly useless for JVM developers)
         */
        if (user_pwd)
        {
            char *java_log = xasprintf("%s/hs_err_pid%lu.log", user_pwd, (long)pid);
            int src_fd = open(java_log, O_RDONLY);
            free(java_log);
            if (src_fd >= 0)
            {
                strcpy(path + path_len, "/hs_err.log");
                int dst_fd = create_or_die(path);
                off_t sz = copyfd_eof(src_fd, dst_fd, COPYFD_SPARSE);
                if (close(dst_fd) != 0 || sz < 0)
                {
                    dd_delete(dd);
                    error_msg_and_die("Error saving '%s'", path);
                }
                close(src_fd);
            }
        }
#endif

        /* And finally set the right uid and gid */
        dd_reset_ownership(dd);

        /* We close dumpdir before we start catering for crash storm case.
         * Otherwise, delete_dump_dir's from other concurrent
         * CCpp's won't be able to delete our dump (their delete_dump_dir
         * will wait for us), and we won't be able to delete their dumps.
         * Classic deadlock.
         */
        dd_close(dd);
        path[path_len] = '\0'; /* path now contains only directory name */
        char *newpath = xstrndup(path, path_len - (sizeof(".new")-1));
        if (rename(path, newpath) == 0)
            strcpy(path, newpath);
        free(newpath);

        log("Saved core dump of pid %lu (%s) to %s (%llu bytes)", (long)pid, executable, path, (long long)core_size);

        /* rhbz#539551: "abrt going crazy when crashing process is respawned" */
        if (g_settings_nMaxCrashReportsSize > 0)
        {
            /* x1.25 and round up to 64m: go a bit up, so that usual in-daemon trimming
             * kicks in first, and we don't "fight" with it:
             */
            unsigned maxsize = g_settings_nMaxCrashReportsSize + g_settings_nMaxCrashReportsSize / 4;
            maxsize |= 63;
            trim_problem_dirs(g_settings_dump_location, maxsize * (double)(1024*1024), path);
        }

        free(rootdir);

        err = 0;
        goto finito;
    }

    /* We didn't create abrt dump, but may need to create compat coredump */
 create_user_core:
    if (user_core_fd >= 0)
    {
        off_t core_size = copyfd_size(STDIN_FILENO, user_core_fd, ulimit_c, COPYFD_SPARSE);
        if (close_user_core(user_core_fd, core_size) != 0)
            goto finito;

        err = 0;
        log("Saved core dump of pid %lu to %s at %s (%llu bytes)", (long)pid, core_basename, user_pwd, (long long)core_size);
    }

 finito:
    if (proc_cwd != NULL)
        closedir(proc_cwd);

    return err;
}
