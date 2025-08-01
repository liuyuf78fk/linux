// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <inttypes.h>
#include <api/fs/tracing_path.h>
#include <linux/err.h>
#include <linux/string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "thread_map.h"
#include "evsel.h"
#include "debug.h"
#include "tests.h"
#include "util/counts.h"

static int test__openat_syscall_event(struct test_suite *test __maybe_unused,
				      int subtest __maybe_unused)
{
	int err = TEST_FAIL, fd;
	struct evsel *evsel;
	unsigned int nr_openat_calls = 111, i;
	struct perf_thread_map *threads = thread_map__new_by_tid(getpid());
	char sbuf[STRERR_BUFSIZE];
	char errbuf[BUFSIZ];

	if (threads == NULL) {
		pr_debug("thread_map__new\n");
		return TEST_FAIL;
	}

	evsel = evsel__newtp("syscalls", "sys_enter_openat");
	if (IS_ERR(evsel)) {
		tracing_path__strerror_open_tp(errno, errbuf, sizeof(errbuf), "syscalls", "sys_enter_openat");
		pr_debug("%s\n", errbuf);
		err = TEST_SKIP;
		goto out_thread_map_delete;
	}

	if (evsel__open_per_thread(evsel, threads) < 0) {
		pr_debug("failed to open counter: %s, "
			 "tweak /proc/sys/kernel/perf_event_paranoid?\n",
			 str_error_r(errno, sbuf, sizeof(sbuf)));
		err = TEST_SKIP;
		goto out_evsel_delete;
	}

	for (i = 0; i < nr_openat_calls; ++i) {
		fd = openat(0, "/etc/passwd", O_RDONLY);
		close(fd);
	}

	if (evsel__read_on_cpu(evsel, 0, 0) < 0) {
		pr_debug("evsel__read_on_cpu\n");
		goto out_close_fd;
	}

	if (perf_counts(evsel->counts, 0, 0)->val != nr_openat_calls) {
		pr_debug("evsel__read_on_cpu: expected to intercept %d calls, got %" PRIu64 "\n",
			 nr_openat_calls, perf_counts(evsel->counts, 0, 0)->val);
		goto out_close_fd;
	}

	err = TEST_OK;
out_close_fd:
	perf_evsel__close_fd(&evsel->core);
out_evsel_delete:
	evsel__delete(evsel);
out_thread_map_delete:
	perf_thread_map__put(threads);
	return err;
}

static struct test_case tests__openat_syscall_event[] = {
	TEST_CASE_REASON("Detect openat syscall event",
			 openat_syscall_event,
			 "permissions"),
	{	.name = NULL, }
};

struct test_suite suite__openat_syscall_event = {
	.desc = "Detect openat syscall event",
	.test_cases = tests__openat_syscall_event,
};
