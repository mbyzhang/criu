#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <string.h>

#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "types.h"

#include "compiler.h"
#include "crtools.h"
#include "util.h"
#include "log.h"
#include "sockets.h"

static struct cr_options opts;
struct page_entry zero_page_entry;
char image_dir[PATH_MAX];

/*
 * The cr fd set is the set of files where the information
 * about dumped processes is stored. Each file carries some
 * small portion of info about the whole picture, see below
 * for more details.
 */

struct cr_fd_desc_tmpl fdset_template[CR_FD_MAX] = {

	 /* info about file descriptiors */
	[CR_FD_FDINFO] = {
		.fmt	= FMT_FNAME_FDINFO,
		.magic	= FDINFO_MAGIC,
	},

	/* private memory pages data */
	[CR_FD_PAGES] = {
		.fmt	= FMT_FNAME_PAGES,
		.magic	= PAGES_MAGIC,
	},

	/* shared memory pages data */
	[CR_FD_PAGES_SHMEM] = {
		.fmt	= FMT_FNAME_PAGES_SHMEM,
		.magic	= PAGES_MAGIC,
	},

	/* core data, such as regs and vmas and such */
	[CR_FD_CORE] = {
		.fmt	= FMT_FNAME_CORE,
		.magic	= CORE_MAGIC,
	},

	/* info about pipes - fds, pipe id and pipe data */
	[CR_FD_PIPES] = {
		.fmt	= FMT_FNAME_PIPES,
		.magic	= PIPES_MAGIC,
	},

	 /* info about process linkage */
	[CR_FD_PSTREE] = {
		.fmt	= FMT_FNAME_PSTREE,
		.magic	= PSTREE_MAGIC,
	},

	/* info about which memory areas are shared */
	[CR_FD_SHMEM] = {
		.fmt	= FMT_FNAME_SHMEM,
		.magic	= SHMEM_MAGIC,
	},

	/* info about signal handlers */
	[CR_FD_SIGACT] = {
		.fmt	= FMT_FNAME_SIGACTS,
		.magic	= SIGACT_MAGIC,
	},

	/* info about unix sockets */
	[CR_FD_UNIXSK] = {
		.fmt	= FMT_FNAME_UNIXSK,
		.magic	= UNIXSK_MAGIC,
	},
};

static struct cr_fdset *alloc_cr_fdset(void)
{
	struct cr_fdset *cr_fdset;
	unsigned int i;

	cr_fdset = xmalloc(sizeof(*cr_fdset));
	if (cr_fdset)
		for (i = 0; i < CR_FD_MAX; i++)
			cr_fdset->fds[i] = -1;
	return cr_fdset;
}

struct cr_fdset *prep_cr_fdset_for_dump(int pid, unsigned long use_mask)
{
	unsigned int i;
	int ret = -1;
	char path[PATH_MAX];
	struct cr_fdset *cr_fdset;

	cr_fdset = alloc_cr_fdset();
	if (!cr_fdset)
		goto err;

	for (i = 0; i < CR_FD_MAX; i++) {
		if (!(use_mask & CR_FD_DESC_USE(i)))
			continue;

		ret = get_image_path(path, sizeof(path),
				fdset_template[i].fmt, pid);
		if (ret)
			goto err;

		ret = unlink(path);
		if (ret && errno != ENOENT) {
			pr_perror("Unable to unlink %s (%s)\n", path, strerror(errno));
			goto err;
		} 

		ret = open(path, O_RDWR | O_CREAT | O_EXCL, CR_FD_PERM);
		if (ret < 0) {
			pr_perror("Unable to open %s (%s)\n", path, strerror(errno));
			goto err;
		}

		pr_debug("Opened %s with %d\n", path, ret);
		write_ptr_safe(ret, &fdset_template[i].magic, err);
		cr_fdset->fds[i] = ret;
	}
err:
	return cr_fdset;
}

struct cr_fdset *prep_cr_fdset_for_restore(int pid, unsigned long use_mask)
{
	unsigned int i;
	int ret = -1;
	char path[PATH_MAX];
	u32 magic;
	struct cr_fdset *cr_fdset;

	cr_fdset = alloc_cr_fdset();
	if (!cr_fdset)
		goto err;

	for (i = 0; i < CR_FD_MAX; i++) {
		if (!(use_mask & CR_FD_DESC_USE(i)))
			continue;

		ret = get_image_path(path, sizeof(path),
				fdset_template[i].fmt, pid);
		if (ret)
			goto err;

		ret = open(path, O_RDWR, CR_FD_PERM);
		if (ret < 0) {
			pr_perror("Unable to open %s (%s)\n", path, strerror(errno));
			goto err;
		}

		pr_debug("Opened %s with %d\n", path, ret);
		read_ptr_safe(ret, &magic, err);
		if (magic != fdset_template[i].magic) {
			close(ret);
			pr_err("Magic doesn't match for %s\n", path);
			goto err;
		}

		cr_fdset->fds[i] = ret;
	}
err:
	return cr_fdset;
}

void close_cr_fdset(struct cr_fdset *cr_fdset)
{
	unsigned int i;

	if (!cr_fdset)
		return;

	for (i = 0; i < CR_FD_MAX; i++) {
		if (cr_fdset->fds[i] == -1)
			continue;

		pr_debug("Closed %d/%d\n", i, cr_fdset->fds[i]);
		close(cr_fdset->fds[i]);
		cr_fdset->fds[i] = -1;
	}

	free(cr_fdset);
}

int get_image_path(char *path, int size, const char *fmt, int pid)
{
	int image_dir_size = strlen(image_dir);
	int ret;

	strncpy(path, image_dir, size);
	path[image_dir_size] = '/';
	size -= image_dir_size + 1;

	ret = snprintf(path + image_dir_size + 1, size, fmt, pid);
	if (ret == -1 || ret >= size) {
		pr_err("can't get image path\n");
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	pid_t pid = 0;
	int ret = -1;
	int opt, idx;
	int action = -1;
	int log_inited = 0;

	static const char short_opts[] = "drsf:p:t:hcD:o:";
	static const struct option long_opts[] = {
		{ "dump",	no_argument, NULL, 'd' },
		{ "restore",	no_argument, NULL, 'r' },
		{ "show",	no_argument, NULL, 's' },
		{ NULL,		no_argument, NULL, 0 }
	};

	BUILD_BUG_ON(PAGE_SIZE != PAGE_IMAGE_SIZE);

	if (argc < 3)
		goto usage;

	memzero_p(&zero_page_entry);

	/* Default options */
	opts.final_state = CR_TASK_KILL;

	for (opt = getopt_long(argc, argv, short_opts, long_opts, &idx); opt != -1;
	     opt = getopt_long(argc, argv, short_opts, long_opts, &idx)) {
		switch (opt) {
		case 'p':
			pid = atoi(optarg);
			opts.leader_only = true;
			break;
		case 't':
			pid = atoi(optarg);
			opts.leader_only = false;
			break;
		case 'd':
			action = opt;
			break;
		case 'r':
			action = opt;
			break;
		case 's':
			action = opt;
			break;
		case 'c':
			opts.show_pages_content	= true;
			opts.final_state = CR_TASK_RUN;
			break;
		case 'f':
			opts.show_dump_file = optarg;
			break;
		case 'D':
			if (chdir(optarg)) {
				pr_perror("can't change working directory");
				return -1;
			}
			break;
		case 'o':
			if (init_log(optarg))
				return -1;
			log_inited = 1;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (!log_inited) {
		ret = init_log(NULL);
		if (ret)
			return ret;
	}

	if (!getcwd(image_dir, sizeof(image_dir))) {
		pr_perror("can't get currect directory\n");
		return -1;
	}

	if (!pid && (action != 's' || !opts.show_dump_file))
		goto opt_pid_missing;

	switch (action) {
	case 'd':
		ret = cr_dump_tasks(pid, &opts);
		break;
	case 'r':
		ret = cr_restore_tasks(pid, &opts);
		break;
	case 's':
		ret = cr_show(pid, &opts);
		break;
	default:
		goto usage;
		break;
	}

	return ret;

usage:
	printk("\nUsage:\n");
	printk("  %s --dump|-d [-c] -p|-t pid\n", argv[0]);
	printk("  %s --restore|-r -p|-t pid\n", argv[0]);
	printk("  %s --show|-s [-c] (-p|-t pid)|(-f file)\n", argv[0]);

	printk("\nGeneral parameters:\n");
	printk("  --dump,-d      checkpoint a process identified by pid\n");
	printk("  --restore,-r   restore a process identified by pid\n");
	printk("  --show,-s      show dump contents of a process identified by pid\n");
	printk("  -p             checkpoint/restore only a single process identified by pid\n");
	printk("  -t             checkpoint/restore the whole process tree identified by pid\n");
	printk("  -f             show contents of a checkpoint file\n");
	printk("  -c             in case of checkpoint -- continue running the process after\n"
	       "                 checkpoint complete, in case of showing file contents --\n"
	       "                 show contents of pages dumped in hexdump format\n");

	printk("\nAdditional common parameters:\n");
	printk("  -D dir         save checkpoint files in specified directory\n");
	printk("\n");

	return -1;

opt_pid_missing:
	printk("No pid specified, -t or -p option missed?\n");
	return -1;
}
