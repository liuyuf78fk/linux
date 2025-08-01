// SPDX-License-Identifier: GPL-2.0
/*
 * A test of splitting PMD THPs and PTE-mapped THPs from a specified virtual
 * address range in a process via <debugfs>/split_huge_pages interface.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <malloc.h>
#include <stdbool.h>
#include <time.h>
#include "vm_util.h"
#include "../kselftest.h"

uint64_t pagesize;
unsigned int pageshift;
uint64_t pmd_pagesize;

#define SPLIT_DEBUGFS "/sys/kernel/debug/split_huge_pages"
#define SMAP_PATH "/proc/self/smaps"
#define INPUT_MAX 80

#define PID_FMT "%d,0x%lx,0x%lx,%d"
#define PID_FMT_OFFSET "%d,0x%lx,0x%lx,%d,%d"
#define PATH_FMT "%s,0x%lx,0x%lx,%d"

#define PFN_MASK     ((1UL<<55)-1)
#define KPF_THP      (1UL<<22)

int is_backed_by_thp(char *vaddr, int pagemap_file, int kpageflags_file)
{
	uint64_t paddr;
	uint64_t page_flags;

	if (pagemap_file) {
		pread(pagemap_file, &paddr, sizeof(paddr),
			((long)vaddr >> pageshift) * sizeof(paddr));

		if (kpageflags_file) {
			pread(kpageflags_file, &page_flags, sizeof(page_flags),
				(paddr & PFN_MASK) * sizeof(page_flags));

			return !!(page_flags & KPF_THP);
		}
	}
	return 0;
}

static void write_file(const char *path, const char *buf, size_t buflen)
{
	int fd;
	ssize_t numwritten;

	fd = open(path, O_WRONLY);
	if (fd == -1)
		ksft_exit_fail_msg("%s open failed: %s\n", path, strerror(errno));

	numwritten = write(fd, buf, buflen - 1);
	close(fd);
	if (numwritten < 1)
		ksft_exit_fail_msg("Write failed\n");
}

static void write_debugfs(const char *fmt, ...)
{
	char input[INPUT_MAX];
	int ret;
	va_list argp;

	va_start(argp, fmt);
	ret = vsnprintf(input, INPUT_MAX, fmt, argp);
	va_end(argp);

	if (ret >= INPUT_MAX)
		ksft_exit_fail_msg("%s: Debugfs input is too long\n", __func__);

	write_file(SPLIT_DEBUGFS, input, ret + 1);
}

static char *allocate_zero_filled_hugepage(size_t len)
{
	char *result;
	size_t i;

	result = memalign(pmd_pagesize, len);
	if (!result) {
		printf("Fail to allocate memory\n");
		exit(EXIT_FAILURE);
	}

	madvise(result, len, MADV_HUGEPAGE);

	for (i = 0; i < len; i++)
		result[i] = (char)0;

	return result;
}

static void verify_rss_anon_split_huge_page_all_zeroes(char *one_page, int nr_hpages, size_t len)
{
	unsigned long rss_anon_before, rss_anon_after;
	size_t i;

	if (!check_huge_anon(one_page, 4, pmd_pagesize))
		ksft_exit_fail_msg("No THP is allocated\n");

	rss_anon_before = rss_anon();
	if (!rss_anon_before)
		ksft_exit_fail_msg("No RssAnon is allocated before split\n");

	/* split all THPs */
	write_debugfs(PID_FMT, getpid(), (uint64_t)one_page,
		      (uint64_t)one_page + len, 0);

	for (i = 0; i < len; i++)
		if (one_page[i] != (char)0)
			ksft_exit_fail_msg("%ld byte corrupted\n", i);

	if (!check_huge_anon(one_page, 0, pmd_pagesize))
		ksft_exit_fail_msg("Still AnonHugePages not split\n");

	rss_anon_after = rss_anon();
	if (rss_anon_after >= rss_anon_before)
		ksft_exit_fail_msg("Incorrect RssAnon value. Before: %ld After: %ld\n",
		       rss_anon_before, rss_anon_after);
}

void split_pmd_zero_pages(void)
{
	char *one_page;
	int nr_hpages = 4;
	size_t len = nr_hpages * pmd_pagesize;

	one_page = allocate_zero_filled_hugepage(len);
	verify_rss_anon_split_huge_page_all_zeroes(one_page, nr_hpages, len);
	ksft_test_result_pass("Split zero filled huge pages successful\n");
	free(one_page);
}

void split_pmd_thp_to_order(int order)
{
	char *one_page;
	size_t len = 4 * pmd_pagesize;
	size_t i;

	one_page = memalign(pmd_pagesize, len);
	if (!one_page)
		ksft_exit_fail_msg("Fail to allocate memory: %s\n", strerror(errno));

	madvise(one_page, len, MADV_HUGEPAGE);

	for (i = 0; i < len; i++)
		one_page[i] = (char)i;

	if (!check_huge_anon(one_page, 4, pmd_pagesize))
		ksft_exit_fail_msg("No THP is allocated\n");

	/* split all THPs */
	write_debugfs(PID_FMT, getpid(), (uint64_t)one_page,
		(uint64_t)one_page + len, order);

	for (i = 0; i < len; i++)
		if (one_page[i] != (char)i)
			ksft_exit_fail_msg("%ld byte corrupted\n", i);


	if (!check_huge_anon(one_page, 0, pmd_pagesize))
		ksft_exit_fail_msg("Still AnonHugePages not split\n");

	ksft_test_result_pass("Split huge pages to order %d successful\n", order);
	free(one_page);
}

void split_pte_mapped_thp(void)
{
	char *one_page, *pte_mapped, *pte_mapped2;
	size_t len = 4 * pmd_pagesize;
	uint64_t thp_size;
	size_t i;
	const char *pagemap_template = "/proc/%d/pagemap";
	const char *kpageflags_proc = "/proc/kpageflags";
	char pagemap_proc[255];
	int pagemap_fd;
	int kpageflags_fd;

	if (snprintf(pagemap_proc, 255, pagemap_template, getpid()) < 0)
		ksft_exit_fail_msg("get pagemap proc error: %s\n", strerror(errno));

	pagemap_fd = open(pagemap_proc, O_RDONLY);
	if (pagemap_fd == -1)
		ksft_exit_fail_msg("read pagemap: %s\n", strerror(errno));

	kpageflags_fd = open(kpageflags_proc, O_RDONLY);
	if (kpageflags_fd == -1)
		ksft_exit_fail_msg("read kpageflags: %s\n", strerror(errno));

	one_page = mmap((void *)(1UL << 30), len, PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (one_page == MAP_FAILED)
		ksft_exit_fail_msg("Fail to allocate memory: %s\n", strerror(errno));

	madvise(one_page, len, MADV_HUGEPAGE);

	for (i = 0; i < len; i++)
		one_page[i] = (char)i;

	if (!check_huge_anon(one_page, 4, pmd_pagesize))
		ksft_exit_fail_msg("No THP is allocated\n");

	/* remap the first pagesize of first THP */
	pte_mapped = mremap(one_page, pagesize, pagesize, MREMAP_MAYMOVE);

	/* remap the Nth pagesize of Nth THP */
	for (i = 1; i < 4; i++) {
		pte_mapped2 = mremap(one_page + pmd_pagesize * i + pagesize * i,
				     pagesize, pagesize,
				     MREMAP_MAYMOVE|MREMAP_FIXED,
				     pte_mapped + pagesize * i);
		if (pte_mapped2 == MAP_FAILED)
			ksft_exit_fail_msg("mremap failed: %s\n", strerror(errno));
	}

	/* smap does not show THPs after mremap, use kpageflags instead */
	thp_size = 0;
	for (i = 0; i < pagesize * 4; i++)
		if (i % pagesize == 0 &&
		    is_backed_by_thp(&pte_mapped[i], pagemap_fd, kpageflags_fd))
			thp_size++;

	if (thp_size != 4)
		ksft_exit_fail_msg("Some THPs are missing during mremap\n");

	/* split all remapped THPs */
	write_debugfs(PID_FMT, getpid(), (uint64_t)pte_mapped,
		      (uint64_t)pte_mapped + pagesize * 4, 0);

	/* smap does not show THPs after mremap, use kpageflags instead */
	thp_size = 0;
	for (i = 0; i < pagesize * 4; i++) {
		if (pte_mapped[i] != (char)i)
			ksft_exit_fail_msg("%ld byte corrupted\n", i);

		if (i % pagesize == 0 &&
		    is_backed_by_thp(&pte_mapped[i], pagemap_fd, kpageflags_fd))
			thp_size++;
	}

	if (thp_size)
		ksft_exit_fail_msg("Still %ld THPs not split\n", thp_size);

	ksft_test_result_pass("Split PTE-mapped huge pages successful\n");
	munmap(one_page, len);
	close(pagemap_fd);
	close(kpageflags_fd);
}

void split_file_backed_thp(int order)
{
	int status;
	int fd;
	char tmpfs_template[] = "/tmp/thp_split_XXXXXX";
	const char *tmpfs_loc = mkdtemp(tmpfs_template);
	char testfile[INPUT_MAX];
	ssize_t num_written, num_read;
	char *file_buf1, *file_buf2;
	uint64_t pgoff_start = 0, pgoff_end = 1024;
	int i;

	ksft_print_msg("Please enable pr_debug in split_huge_pages_in_file() for more info.\n");

	file_buf1 = (char *)malloc(pmd_pagesize);
	file_buf2 = (char *)malloc(pmd_pagesize);

	if (!file_buf1 || !file_buf2) {
		ksft_print_msg("cannot allocate file buffers\n");
		goto out;
	}

	for (i = 0; i < pmd_pagesize; i++)
		file_buf1[i] = (char)i;
	memset(file_buf2, 0, pmd_pagesize);

	status = mount("tmpfs", tmpfs_loc, "tmpfs", 0, "huge=always,size=4m");

	if (status)
		ksft_exit_fail_msg("Unable to create a tmpfs for testing\n");

	status = snprintf(testfile, INPUT_MAX, "%s/thp_file", tmpfs_loc);
	if (status >= INPUT_MAX) {
		ksft_exit_fail_msg("Fail to create file-backed THP split testing file\n");
		goto cleanup;
	}

	fd = open(testfile, O_CREAT|O_RDWR, 0664);
	if (fd == -1) {
		ksft_perror("Cannot open testing file");
		goto cleanup;
	}

	/* write pmd size data to the file, so a file-backed THP can be allocated */
	num_written = write(fd, file_buf1, pmd_pagesize);

	if (num_written == -1 || num_written != pmd_pagesize) {
		ksft_perror("Failed to write data to testing file");
		goto close_file;
	}

	/* split the file-backed THP */
	write_debugfs(PATH_FMT, testfile, pgoff_start, pgoff_end, order);

	/* check file content after split */
	status = lseek(fd, 0, SEEK_SET);
	if (status == -1) {
		ksft_perror("Cannot lseek file");
		goto close_file;
	}

	num_read = read(fd, file_buf2, num_written);
	if (num_read == -1 || num_read != num_written) {
		ksft_perror("Cannot read file content back");
		goto close_file;
	}

	if (strncmp(file_buf1, file_buf2, pmd_pagesize) != 0) {
		ksft_print_msg("File content changed\n");
		goto close_file;
	}

	close(fd);
	status = unlink(testfile);
	if (status) {
		ksft_perror("Cannot remove testing file");
		goto cleanup;
	}

	status = umount(tmpfs_loc);
	if (status) {
		rmdir(tmpfs_loc);
		ksft_exit_fail_msg("Unable to umount %s\n", tmpfs_loc);
	}

	status = rmdir(tmpfs_loc);
	if (status)
		ksft_exit_fail_msg("cannot remove tmp dir: %s\n", strerror(errno));

	ksft_print_msg("Please check dmesg for more information\n");
	ksft_test_result_pass("File-backed THP split to order %d test done\n", order);
	return;

close_file:
	close(fd);
cleanup:
	umount(tmpfs_loc);
	rmdir(tmpfs_loc);
out:
	ksft_exit_fail_msg("Error occurred\n");
}

bool prepare_thp_fs(const char *xfs_path, char *thp_fs_template,
		const char **thp_fs_loc)
{
	if (xfs_path) {
		*thp_fs_loc = xfs_path;
		return false;
	}

	*thp_fs_loc = mkdtemp(thp_fs_template);

	if (!*thp_fs_loc)
		ksft_exit_fail_msg("cannot create temp folder\n");

	return true;
}

void cleanup_thp_fs(const char *thp_fs_loc, bool created_tmp)
{
	int status;

	if (!created_tmp)
		return;

	status = rmdir(thp_fs_loc);
	if (status)
		ksft_exit_fail_msg("cannot remove tmp dir: %s\n",
				   strerror(errno));
}

int create_pagecache_thp_and_fd(const char *testfile, size_t fd_size, int *fd,
		char **addr)
{
	size_t i;
	unsigned char buf[1024];

	srand(time(NULL));

	*fd = open(testfile, O_CREAT | O_RDWR, 0664);
	if (*fd == -1)
		ksft_exit_fail_msg("Failed to create a file at %s\n", testfile);

	assert(fd_size % sizeof(buf) == 0);
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char)i;
	for (i = 0; i < fd_size; i += sizeof(buf))
		write(*fd, buf, sizeof(buf));

	close(*fd);
	sync();
	*fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
	if (*fd == -1) {
		ksft_perror("open drop_caches");
		goto err_out_unlink;
	}
	if (write(*fd, "3", 1) != 1) {
		ksft_perror("write to drop_caches");
		goto err_out_unlink;
	}
	close(*fd);

	*fd = open(testfile, O_RDWR);
	if (*fd == -1) {
		ksft_perror("Failed to open testfile\n");
		goto err_out_unlink;
	}

	*addr = mmap(NULL, fd_size, PROT_READ|PROT_WRITE, MAP_SHARED, *fd, 0);
	if (*addr == (char *)-1) {
		ksft_perror("cannot mmap");
		goto err_out_close;
	}
	madvise(*addr, fd_size, MADV_HUGEPAGE);

	for (size_t i = 0; i < fd_size; i++)
		FORCE_READ((*addr + i));

	if (!check_huge_file(*addr, fd_size / pmd_pagesize, pmd_pagesize)) {
		ksft_print_msg("No large pagecache folio generated, please provide a filesystem supporting large folio\n");
		munmap(*addr, fd_size);
		close(*fd);
		unlink(testfile);
		ksft_test_result_skip("Pagecache folio split skipped\n");
		return -2;
	}
	return 0;
err_out_close:
	close(*fd);
err_out_unlink:
	unlink(testfile);
	ksft_exit_fail_msg("Failed to create large pagecache folios\n");
	return -1;
}

void split_thp_in_pagecache_to_order_at(size_t fd_size, const char *fs_loc,
		int order, int offset)
{
	int fd;
	char *addr;
	size_t i;
	char testfile[INPUT_MAX];
	int err = 0;

	err = snprintf(testfile, INPUT_MAX, "%s/test", fs_loc);

	if (err < 0)
		ksft_exit_fail_msg("cannot generate right test file name\n");

	err = create_pagecache_thp_and_fd(testfile, fd_size, &fd, &addr);
	if (err)
		return;
	err = 0;

	if (offset == -1)
		write_debugfs(PID_FMT, getpid(), (uint64_t)addr,
			      (uint64_t)addr + fd_size, order);
	else
		write_debugfs(PID_FMT_OFFSET, getpid(), (uint64_t)addr,
			      (uint64_t)addr + fd_size, order, offset);

	for (i = 0; i < fd_size; i++)
		if (*(addr + i) != (char)i) {
			ksft_print_msg("%lu byte corrupted in the file\n", i);
			err = EXIT_FAILURE;
			goto out;
		}

	if (!check_huge_file(addr, 0, pmd_pagesize)) {
		ksft_print_msg("Still FilePmdMapped not split\n");
		err = EXIT_FAILURE;
		goto out;
	}

out:
	munmap(addr, fd_size);
	close(fd);
	unlink(testfile);
	if (offset == -1) {
		if (err)
			ksft_exit_fail_msg("Split PMD-mapped pagecache folio to order %d failed\n", order);
		ksft_test_result_pass("Split PMD-mapped pagecache folio to order %d passed\n", order);
	} else {
		if (err)
			ksft_exit_fail_msg("Split PMD-mapped pagecache folio to order %d at in-folio offset %d failed\n", order, offset);
		ksft_test_result_pass("Split PMD-mapped pagecache folio to order %d at in-folio offset %d passed\n", order, offset);
	}
}

int main(int argc, char **argv)
{
	int i;
	size_t fd_size;
	char *optional_xfs_path = NULL;
	char fs_loc_template[] = "/tmp/thp_fs_XXXXXX";
	const char *fs_loc;
	bool created_tmp;
	int offset;

	ksft_print_header();

	if (geteuid() != 0) {
		ksft_print_msg("Please run the benchmark as root\n");
		ksft_finished();
	}

	if (argc > 1)
		optional_xfs_path = argv[1];

	ksft_set_plan(1+8+1+9+9+8*4+2);

	pagesize = getpagesize();
	pageshift = ffs(pagesize) - 1;
	pmd_pagesize = read_pmd_pagesize();
	if (!pmd_pagesize)
		ksft_exit_fail_msg("Reading PMD pagesize failed\n");

	fd_size = 2 * pmd_pagesize;

	split_pmd_zero_pages();

	for (i = 0; i < 9; i++)
		if (i != 1)
			split_pmd_thp_to_order(i);

	split_pte_mapped_thp();
	for (i = 0; i < 9; i++)
		split_file_backed_thp(i);

	created_tmp = prepare_thp_fs(optional_xfs_path, fs_loc_template,
			&fs_loc);
	for (i = 8; i >= 0; i--)
		split_thp_in_pagecache_to_order_at(fd_size, fs_loc, i, -1);

	for (i = 0; i < 9; i++)
		for (offset = 0;
		     offset < pmd_pagesize / pagesize;
		     offset += MAX(pmd_pagesize / pagesize / 4, 1 << i))
			split_thp_in_pagecache_to_order_at(fd_size, fs_loc, i, offset);
	cleanup_thp_fs(fs_loc, created_tmp);

	ksft_finished();

	return 0;
}
