/* SPDX-License-Identifier: MIT */
/*
 * Description: test sq/cq ring resizing
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>

#include "liburing.h"
#include "helpers.h"

#define NVECS	128

#define min(a, b)	((a) < (b) ? (a) : (b))

struct data {
	pthread_t thread;
	int fd;
	int nr_writes;
	int failed;
};

static void *thread_fn(void *__data)
{
	struct data *d = __data;
	char buffer[8];
	int to_write = d->nr_writes;

	memset(buffer, 0x5a, sizeof(buffer));
	usleep(10000);
	while (to_write) {
		int ret = write(d->fd, buffer, sizeof(buffer));

		if (ret < 0) {
			perror("write");
			d->failed = 1;
			break;
		} else if (ret != sizeof(buffer)) {
			printf("short write %d\n", ret);
		}
		to_write--;
		usleep(5);
	}
	return NULL;
}

static int test_pipes(struct io_uring *ring, int async)
{
	struct io_uring_params p = { };
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	unsigned long ud = 0;
	struct data d = { };
	int ret, i, fds[2], to_read;
	char buffer[8];
	void *tret;

	p.sq_entries = 128;
	p.cq_entries = 128;
	ret = io_uring_resize_rings(ring, &p);
	if (ret < 0) {
		fprintf(stderr, "Failed to resize ring: %d\n", ret);
		return T_EXIT_FAIL;
	}

	if (pipe(fds) < 0) {
		perror("pipe");
		return T_EXIT_FAIL;
	}

	/*
	 * Put NVECS inflight, then resize while waiting. Repeat until
	 * 'to_read' has been read.
	 */
	d.nr_writes = 4096;
	d.fd = fds[1];
	p.sq_entries = 64;
	p.cq_entries = 256;
	p.flags = 0;

	pthread_create(&d.thread, NULL, thread_fn, &d);

	to_read = d.nr_writes - 128;
	while (to_read && !d.failed) {
		unsigned long start_ud = -1UL, end_ud;
		int to_wait;

		to_wait = NVECS;
		if (to_wait > to_read)
			to_wait = to_read;

		for (i = 0; i < to_wait; i++) {
			sqe = io_uring_get_sqe(ring);
			/* resized smaller */
			if (!sqe)
				break;
			io_uring_prep_read(sqe, fds[0], buffer, sizeof(buffer), 0);
			if (async)
				sqe->flags |= IOSQE_ASYNC;
			if (start_ud == -1UL)
				start_ud = ud;
			sqe->user_data = ++ud;
			to_read--;
		}
		end_ud = ud;
		ret = io_uring_submit(ring);
		if (ret != i) {
			fprintf(stderr, "submitted; %d\n", ret);
			return T_EXIT_FAIL;
		}

		to_wait = i;
		for (i = 0; i < to_wait; i++) {
			if (i == 0) {
				ret = io_uring_resize_rings(ring, &p);
				if (ret < 0) {
					fprintf(stderr, "resize failed: %d\n", ret);
					return T_EXIT_FAIL;
				}
				p.sq_entries = 32;
				p.cq_entries = 128;
				p.flags = 0;
			}
			if (d.failed)
				break;
			ret = io_uring_wait_cqe(ring, &cqe);
			if (ret) {
				fprintf(stderr, "wait cqe: %d\n", ret);
				return T_EXIT_FAIL;
			}
			if (cqe->res < 0) {
				fprintf(stderr, "cqe res %d\n", cqe->res);
				return T_EXIT_FAIL;
			}
			if (cqe->user_data < start_ud ||
			    cqe->user_data > end_ud) {
				fprintf(stderr, "use_data out-of-range: <%lu-%lu>: %lu\n",
					start_ud, end_ud, (long) cqe->user_data);
				return T_EXIT_FAIL;
			}
			io_uring_cqe_seen(ring, cqe);
			if (!(i % 17)) {
				ret = io_uring_resize_rings(ring, &p);
				if (ret < 0) {
					fprintf(stderr, "resize failed: %d\n", ret);
					return T_EXIT_FAIL;
				}
				if (p.sq_entries == 32)
					p.sq_entries = 64;
				else if (p.sq_entries == 64)
					p.sq_entries = 16;
				else
					p.sq_entries = 32;
				if (p.cq_entries == 128)
					p.cq_entries = 256;
				else
					p.cq_entries = 128;
				p.flags = 0;
			}
		}
	}

	pthread_join(d.thread, &tret);
	close(fds[0]);
	close(fds[0]);
	return 0;
}

static int test_reads(struct io_uring *ring, int fd, int async)
{
	struct io_uring_params p = { };
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct iovec vecs[NVECS];
	unsigned long to_read;
	unsigned long ud = 0;
	unsigned long offset;
	int ret, i;

	if (fd == -1)
		return T_EXIT_SKIP;

	p.sq_entries = 128;
	p.cq_entries = 128;
	ret = io_uring_resize_rings(ring, &p);
	if (ret < 0) {
		fprintf(stderr, "Failed to resize ring: %d\n", ret);
		return T_EXIT_FAIL;
	}

	for (i = 0; i < NVECS; i++) {
		if (posix_memalign(&vecs[i].iov_base, 4096, 4096))
			return T_EXIT_FAIL;
		vecs[i].iov_len = 4096;
	}

	/*
	 * Put NVECS inflight, then resize while waiting. Repeat until
	 * 'to_read' has been read.
	 */
	to_read = 64*1024*1024;
	p.sq_entries = 64;
	p.cq_entries = 256;
	p.flags = 0;
	offset = 0;
	while (to_read) {
		unsigned long start_ud = -1UL, end_ud;
		int to_wait;

		for (i = 0; i < NVECS; i++) {
			sqe = io_uring_get_sqe(ring);
			/* resized smaller */
			if (!sqe)
				break;
			io_uring_prep_read(sqe, fd, vecs[i].iov_base,
						vecs[i].iov_len, offset);
			if (async)
				sqe->flags |= IOSQE_ASYNC;
			offset += 8192;
			if (start_ud == -1UL)
				start_ud = ud;
			sqe->user_data = ++ud;
		}
		end_ud = ud;
		ret = io_uring_submit(ring);
		if (ret != i) {
			fprintf(stderr, "submitted; %d\n", ret);
			return T_EXIT_FAIL;
		}

		to_wait = i;
		for (i = 0; i < to_wait; i++) {
			if (i == 0) {
				ret = io_uring_resize_rings(ring, &p);
				if (ret < 0) {
					fprintf(stderr, "resize failed: %d\n", ret);
					return T_EXIT_FAIL;
				}
				p.sq_entries = 32;
				p.cq_entries = 128;
				p.flags = 0;
			}
			ret = io_uring_wait_cqe(ring, &cqe);
			if (ret) {
				fprintf(stderr, "wait cqe: %d\n", ret);
				return T_EXIT_FAIL;
			}
			if (cqe->res < 0) {
				fprintf(stderr, "cqe res %d\n", cqe->res);
				return T_EXIT_FAIL;
			}
			if (cqe->user_data < start_ud ||
			    cqe->user_data > end_ud) {
				fprintf(stderr, "use_data out-of-range: <%lu-%lu>: %lu\n",
					start_ud, end_ud, (long) cqe->user_data);
				return T_EXIT_FAIL;
			}
			io_uring_cqe_seen(ring, cqe);
			if (to_read)
				to_read -= min(to_read, 4096);
			if (!(i % 17)) {
				ret = io_uring_resize_rings(ring, &p);
				if (ret < 0) {
					fprintf(stderr, "resize failed: %d\n", ret);
					return T_EXIT_FAIL;
				}
				if (p.sq_entries == 32)
					p.sq_entries = 64;
				else if (p.sq_entries == 64)
					p.sq_entries = 16;
				else
					p.sq_entries = 32;
				if (p.cq_entries == 128)
					p.cq_entries = 256;
				else
					p.cq_entries = 128;
				p.flags = 0;
			}
		}
	}

	return 0;
}

static int test_basic(struct io_uring *ring, int async)
{
	struct io_uring_params p = { };
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int i, ret;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_nop(sqe);
	if (async)
		sqe->flags |= IOSQE_ASYNC;
	sqe->user_data = 1;
	io_uring_submit(ring);

	p.sq_entries = 32;
	p.cq_entries = 64;
	ret = io_uring_resize_rings(ring, &p);
	if (ret == -EINVAL)
		return T_EXIT_SKIP;

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_nop(sqe);
	if (async)
		sqe->flags |= IOSQE_ASYNC;
	sqe->user_data = 2;
	io_uring_submit(ring);

	for (i = 0; i < 2; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait cqe %d\n", ret);
			return T_EXIT_FAIL;
		}
		if (cqe->user_data != i + 1) {
			fprintf(stderr, "bad user_data %ld\n", (long) cqe->user_data);
			return T_EXIT_FAIL;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return T_EXIT_PASS;
}

static int test(int flags, int fd, int async)
{
	struct io_uring_params p = {
		.flags = flags,
	};
	struct io_uring ring;
	int ret;

	ret = io_uring_queue_init_params(8, &ring, &p);
	if (ret < 0) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return T_EXIT_FAIL;
	}

	ret = test_basic(&ring, async);
	if (ret == T_EXIT_SKIP) {
		return T_EXIT_SKIP;
	} else if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test_basic %x failed\n", flags);
		return T_EXIT_FAIL;
	}

	ret = test_reads(&ring, fd, async);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test_reads %x failed\n", flags);
		return T_EXIT_FAIL;
	}

	ret = test_pipes(&ring, async);
	if (ret == T_EXIT_FAIL) {
		fprintf(stderr, "test_pipes %x failed\n", flags);
		return T_EXIT_FAIL;
	}

	io_uring_queue_exit(&ring);
	return T_EXIT_PASS;
}

int main(int argc, char *argv[])
{
	int ret, fd = -1;

	if (argc > 1)
		fd = open("/dev/nvme0n1", O_RDONLY | O_DIRECT);

	ret = test(0, fd, 0);
	if (ret == T_EXIT_SKIP)
		return T_EXIT_SKIP;
	else if (ret == T_EXIT_FAIL)
		return T_EXIT_FAIL;

	ret = test(0, fd, 1);
	if (ret == T_EXIT_FAIL)
		return T_EXIT_FAIL;

	ret = test(IORING_SETUP_SQPOLL, fd, 0);
	if (ret == T_EXIT_FAIL)
		return T_EXIT_FAIL;

	ret = test(IORING_SETUP_SQPOLL, fd, 1);
	if (ret == T_EXIT_FAIL)
		return T_EXIT_FAIL;

	ret = test(IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN, fd, 0);
	if (ret == T_EXIT_FAIL)
		return T_EXIT_FAIL;

	ret = test(IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN, fd, 1);
	if (ret == T_EXIT_FAIL)
		return T_EXIT_FAIL;


	return T_EXIT_PASS;
}
