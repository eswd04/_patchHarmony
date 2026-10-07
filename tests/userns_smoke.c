// SPDX-License-Identifier: GPL-2.0-only
/* Run as root on the target phone after loading both namespace modules. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/nsfs.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <linux/sched.h>
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static const char identity[] = "0 0 4294967295\n";
static int trace_steps;

static void trace_step(const char *function, const char *step)
{
	if (trace_steps)
		fprintf(stderr, "STEP: %s: %s\n", function, step);
}

#define CHECK(expr) do { \
	trace_step(__func__, #expr); \
	if (!(expr)) { \
	fprintf(stderr, "%s:%d: %s (errno=%d: %s)\n", \
		__func__, __LINE__, #expr, errno, strerror(errno)); return 1; \
} } while (0)

static ssize_t write_file(const char *path, const void *data, size_t size)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t ret;
	int saved;

	if (fd < 0)
		return -1;
	ret = write(fd, data, size);
	saved = errno;
	close(fd);
	errno = saved;
	return ret;
}

static int wait_ok(pid_t pid)
{
	int status;

	if (waitpid(pid, &status, 0) != pid)
		return 1;
	return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}

static int identity_maps(void)
{
	char buf[128];
	unsigned int a, b, c;
	int fd;
	ssize_t n;

	CHECK(unshare(CLONE_NEWUSER | CLONE_FS) == 0);
	fd = open("/proc/self/uid_map", O_RDONLY);
	CHECK(fd >= 0);
	n = 0;
	for (;;) {
		ssize_t part = read(fd, buf + n, 3);
		CHECK(part >= 0 && n + part < (ssize_t)sizeof(buf));
		if (!part)
			break;
		n += part;
	}
	close(fd);
	CHECK(n > 0);
	buf[n] = 0;
	CHECK(sscanf(buf, "%u %u %u", &a, &b, &c) == 3);
	CHECK(a == 0 && b == 0 && c == UINT32_MAX);
	CHECK(write_file("/proc/self/uid_map", "0 1000 1\n", 9) == -1 && errno == EPERM);
	CHECK(write_file("/proc/self/uid_map", "0 0 4294967295\0x", 16) == -1 && errno == EINVAL);
	CHECK(write_file("/proc/self/uid_map", identity, strlen(identity)) == (ssize_t)strlen(identity));
	CHECK(write_file("/proc/self/uid_map", identity, strlen(identity)) == -1 && errno == EPERM);
	CHECK(write_file("/proc/self/gid_map", identity, strlen(identity)) == (ssize_t)strlen(identity));
	CHECK(write_file("/proc/self/gid_map", identity, strlen(identity)) == -1 && errno == EPERM);
	/* Keep the host's own IDs unchanged by namespace creation. */
	CHECK(getuid() == 0 && geteuid() == 0);
	/* Reusing a cached proc inode after nesting must select the new map. */
	CHECK(unshare(CLONE_NEWUSER) == 0);
	CHECK(write_file("/proc/self/uid_map", identity, strlen(identity)) == (ssize_t)strlen(identity));
	return 0;
}

static int groups_policy(void)
{
	CHECK(unshare(CLONE_NEWUSER) == 0);
	CHECK(setgroups(0, NULL) == 0);
	CHECK(write_file("/proc/self/setgroups", "deny\n", 5) == 5);
	CHECK(setgroups(0, NULL) == -1 && errno == EPERM);
	CHECK(write_file("/proc/self/setgroups", "allow\n", 6) == -1 && errno == EPERM);
	CHECK(unshare(CLONE_NEWUSER) == 0);
	CHECK(setgroups(0, NULL) == -1 && errno == EPERM);
	return 0;
}

static int setid_calls(void)
{
	CHECK(unshare(CLONE_NEWUSER) == 0);
	CHECK(prctl(PR_CAPBSET_DROP, CAP_SYS_TIME, 0, 0, 0) == 0);
	CHECK(prctl(PR_CAPBSET_READ, CAP_SYS_TIME, 0, 0, 0) == 0);
	CHECK(setresgid(12345, 12345, 12345) == 0);
	CHECK(setresuid(12345, 12345, 12345) == 0);
	CHECK(getuid() == 12345 && getgid() == 12345);
	return 0;
}

static int mounts_and_uts(void)
{
	char dir[] = "/data/local/tmp/dlkm-userns-XXXXXX";
	char hostname[64];
	const char *step;
	int ret = 1;

	CHECK(mkdtemp(dir) != NULL);
	step = "unshare(NEWUSER | NEWNS | NEWUTS)";
	trace_step(__func__, step);
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWUTS) < 0)
		goto out;
	step = "mount(/, MS_REC | MS_PRIVATE)";
	trace_step(__func__, step);
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		goto out;
	step = "sethostname(dlkm-userns-test)";
	trace_step(__func__, step);
	if (sethostname("dlkm-userns-test", 16) < 0)
		goto out;
	step = "gethostname / verify dlkm-userns-test";
	trace_step(__func__, step);
	if (gethostname(hostname, sizeof(hostname)) < 0)
		goto out;
	if (strcmp(hostname, "dlkm-userns-test")) {
		errno = EPROTO;
		goto out;
	}
	step = "mount(tmpfs, size=64k)";
	trace_step(__func__, step);
	if (mount("none", dir, "tmpfs", 0, "size=64k") < 0)
		goto out;
	step = "umount(tmpfs)";
	trace_step(__func__, step);
	ret = umount(dir) < 0;
out:
	if (ret)
		fprintf(stderr, "mount/UTS test failed at %s: errno=%d: %s\n",
			step, errno, strerror(errno));
	trace_step(__func__, "rmdir temporary directory");
	rmdir(dir);
	return ret;
}

static int pid_ipc_owner_files(void)
{
	const char *paths[] = {"/proc/self/ns/ipc", "/proc/self/ns/pid_for_children"};
	struct stat user_stat, owner_stat;
	int user;

	trace_step(__func__, "open /proc/self/ns/user");
	user = open("/proc/self/ns/user", O_RDONLY);
	CHECK(user >= 0 && fstat(user, &user_stat) == 0);
	close(user);
	for (unsigned int i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		int fd;
		int owner;

		trace_step(__func__, paths[i]);
		fd = open(paths[i], O_RDONLY);
		CHECK(fd >= 0);
		owner = ioctl(fd, NS_GET_USERNS);
		close(fd);
		CHECK(owner >= 0 && fstat(owner, &owner_stat) == 0);
		close(owner);
		CHECK(owner_stat.st_ino == user_stat.st_ino && owner_stat.st_dev == user_stat.st_dev);
	}
	return 0;
}

static int pid_ipc_owners(void)
{
	int ready[2], finish[2], fd;
	pid_t child;
	char byte;
	int ret = 1;

	CHECK(unshare(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWIPC) == 0);
	/* Like native Linux, pid_for_children has no target until the first
	 * child establishes this PID namespace's child_reaper. */
	trace_step(__func__, "pid_for_children before first fork: expect ENOENT");
	fd = open("/proc/self/ns/pid_for_children", O_RDONLY);
	if (fd >= 0)
		close(fd);
	CHECK(fd == -1 && errno == ENOENT);
	CHECK(pipe(ready) == 0);
	if (pipe(finish)) {
		close(ready[0]);
		close(ready[1]);
		return 1;
	}
	trace_step(__func__, "fork and keep namespace PID 1 alive for owner checks");
	child = fork();
	if (child < 0) {
		close(ready[0]);
		close(ready[1]);
		close(finish[0]);
		close(finish[1]);
		return 1;
	}
	if (!child) {
		alarm(15);
		close(ready[0]);
		close(finish[1]);
		trace_step(__func__, "child getpid() == 1");
		if (getpid() != 1) {
			fprintf(stderr, "PID namespace first child has PID %d, expected 1\n", getpid());
			_exit(1);
		}
		byte = 'R';
		if (write(ready[1], &byte, 1) != 1)
			_exit(1);
		close(ready[1]);
		_exit(read(finish[0], &byte, 1) != 1);
	}
	close(ready[1]);
	close(finish[0]);
	trace_step(__func__, "wait until child PID 1 is ready");
	if (read(ready[0], &byte, 1) == 1 && byte == 'R')
		ret = pid_ipc_owner_files();
	else
		fprintf(stderr, "PID namespace child did not become ready\n");
	close(ready[0]);
	trace_step(__func__, "release and reap namespace PID 1");
	byte = 'F';
	/* If PID 1 times out/exits, report the failure without SIGPIPE killing
	 * this process before its waitpid cleanup. */
	signal(SIGPIPE, SIG_IGN);
	if (write(finish[1], &byte, 1) != 1)
		ret = 1;
	close(finish[1]);
	if (wait_ok(child))
		ret = 1;
	return ret;
}

static int clone_child(void *unused)
{
	int fd = open("/proc/self/ns/user", O_RDONLY);
	int type;

	(void)unused;
	CHECK(fd >= 0);
	type = ioctl(fd, NS_GET_NSTYPE);
	close(fd);
	CHECK(type == CLONE_NEWUSER);
	return 0;
}

static int clone_calls(void)
{
	char *stack = malloc(65536);
	struct clone_args args = { .flags = CLONE_NEWUSER, .exit_signal = SIGCHLD };
	pid_t pid;

	CHECK(stack != NULL);
	pid = clone(clone_child, stack + 65536, CLONE_NEWUSER | SIGCHLD, NULL);
	CHECK(pid >= 0 && wait_ok(pid) == 0);
	free(stack);
	pid = syscall(SYS_clone3, &args, sizeof(args));
	CHECK(pid >= 0);
	if (!pid)
		_exit(clone_child(NULL));
	CHECK(wait_ok(pid) == 0);
	return 0;
}

static int setns_calls(void)
{
	int ready[2], finish[2], fd, host, owner;
	pid_t target, joiner;
	char path[80], byte;
	int ret;

	CHECK(pipe(ready) == 0 && pipe(finish) == 0);
	host = open("/proc/self/ns/user", O_RDONLY);
	CHECK(host >= 0);
	target = fork();
	CHECK(target >= 0);
	if (!target) {
		alarm(20);
		close(ready[0]);
		close(finish[1]);
		ret = unshare(CLONE_NEWUSER);
		byte = ret ? 'F' : 'R';
		if (write(ready[1], &byte, 1) != 1)
			_exit(1);
		if (read(finish[0], &byte, 1) != 1)
			_exit(1);
		_exit(ret != 0);
	}
	close(ready[1]);
	close(finish[0]);
	ret = 1;
	if (read(ready[0], &byte, 1) != 1 || byte != 'R')
		goto out;
	snprintf(path, sizeof(path), "/proc/%d/ns/user", target);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		goto out;
	owner = ioctl(fd, NS_GET_USERNS);
	if (owner < 0) {
		close(fd);
		goto out;
	}
	close(owner);
	joiner = fork();
	if (!joiner) {
		if (setns(fd, CLONE_NEWNET) != -1 || errno != EINVAL)
			_exit(1);
		if (setns(fd, 0) || setns(fd, CLONE_NEWUSER) != -1 || errno != EINVAL)
			_exit(1);
		if (setns(host, CLONE_NEWUSER) != -1)
			_exit(1);
		if (ioctl(fd, NS_GET_PARENT) != -1 || errno != EPERM)
			_exit(1);
		_exit(0);
	}
	close(fd);
	if (joiner > 0)
		ret = wait_ok(joiner);
out:
	if (ret)
		fprintf(stderr, "setns/ioctl test failed: %s\n", strerror(errno));
	byte = 'D';
	if (write(finish[1], &byte, 1) != 1)
		ret = 1;
	close(finish[1]);
	close(ready[0]);
	close(host);
	return wait_ok(target) || ret;
}

static int unprivileged_rejected(void)
{
	CHECK(setgroups(0, NULL) == 0);
	CHECK(setgid(65534) == 0 && setuid(65534) == 0);
	CHECK(unshare(CLONE_NEWUSER) == -1 && errno == EPERM);
	return 0;
}

static void *waiting_thread(void *arg)
{
	char byte;
	int fd = *(int *)arg;

	(void)read(fd, &byte, 1);
	return NULL;
}

static int threaded_rejected(void)
{
	pthread_t thread;
	int fds[2];
	int ret, saved;

	CHECK(pipe(fds) == 0);
	CHECK(pthread_create(&thread, NULL, waiting_thread, &fds[0]) == 0);
	ret = unshare(CLONE_NEWUSER);
	saved = errno;
	close(fds[1]);
	CHECK(pthread_join(thread, NULL) == 0);
	close(fds[0]);
	CHECK(ret == -1 && saved == EINVAL);
	return 0;
}

static int invalid_flags(void)
{
	char before[128], after[128];
	ssize_t a, b;

	a = readlink("/proc/self/ns/user", before, sizeof(before));
	CHECK(a > 0);
	CHECK(unshare(CLONE_NEWUSER | (1UL << 31)) == -1 && errno == EINVAL);
	b = readlink("/proc/self/ns/user", after, sizeof(after));
	CHECK(a == b && !memcmp(before, after, a));
	return 0;
}

int main(int argc, char **argv)
{
	const struct { const char *name; int (*run)(void); } tests[] = {
		{ "identity maps / single writes", identity_maps },
		{ "setgroups deny and inheritance", groups_policy },
		{ "setresuid / setresgid", setid_calls },
		{ "combined unshare / tmpfs / UTS", mounts_and_uts },
		{ "PID / IPC namespace owners", pid_ipc_owners },
		{ "clone / clone3", clone_calls },
		{ "setns / nsfs ioctls", setns_calls },
		{ "unprivileged rejection", unprivileged_rejected },
		{ "failed unshare preserves namespace", invalid_flags },
		{ "multithreaded unshare rejection", threaded_rejected },
	};
	unsigned int failures = 0;
	unsigned int count = sizeof(tests) / sizeof(tests[0]);
	unsigned int first = 0, last = count;
	int arg = 1;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	if (arg < argc && !strcmp(argv[arg], "--trace")) {
		trace_steps = 1;
		arg++;
	}
	if (arg < argc && !strcmp(argv[arg], "--list") && arg + 1 == argc) {
		for (unsigned int i = 0; i < count; i++)
			printf("%u: %s\n", i + 1, tests[i].name);
		return 0;
	}
	if (arg < argc && !strcmp(argv[arg], "--case") && arg + 2 == argc) {
		char *end;
		unsigned long selected = strtoul(argv[arg + 1], &end, 10);
		if (!*argv[arg + 1] || *end || selected < 1 || selected > count)
			goto usage;
		first = selected - 1;
		last = selected;
	} else if (!(arg < argc && !strcmp(argv[arg], "--all") && arg + 1 == argc)) {
		goto usage;
	}

	fprintf(stderr, "START: module status preflight\n");
	if (geteuid() != 0 || access("/proc/droid_lkm_misc/status", R_OK)) {
		fprintf(stderr, "Run as root on the phone after loading droid_lkm and droid_lkm_misc.\n");
		return 2;
	}
	for (unsigned int i = first; i < last; i++) {
		pid_t pid;
		int failed;

		fprintf(stderr, "START: case %u: %s (fork)\n", i + 1, tests[i].name);
		pid = fork();
		if (!pid) {
			alarm(20);
			fprintf(stderr, "RUN: case %u: %s\n", i + 1, tests[i].name);
			_exit(tests[i].run());
		}
		failed = pid < 0 || wait_ok(pid);
		printf("%s: %s\n", failed ? "FAIL" : "PASS", tests[i].name);
		failures += failed;
	}
	return failures != 0;
usage:
	fprintf(stderr, "Usage: %s [--trace] --list | --case N | --all\n", argv[0]);
	return 2;
}
