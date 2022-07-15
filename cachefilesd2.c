#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>

#include "internal.h"

#define FSCACHETHREADS	3

static int process_one_req(int devfd)
{
	char buf[CACHEFILES_MSG_MAX_SIZE];
	struct cachefiles_msg *msg;
	size_t len;
	int ret;

	memset(buf, 0, sizeof(buf));

	ret = read(devfd, buf, sizeof(buf));
	if (ret < 0)
		printf("read devnode failed\n");
	if (ret <= 0)
		return -1;

	msg = (void *)buf;
	if (ret != msg->len) {
		printf("invalid message length %d (readed %d)\n", msg->len, ret);
		return -1;
	}

	printf("[HEADER] id %u, opcode %d tid %lu\t", msg->msg_id, msg->opcode, pthread_self());

	switch (msg->opcode) {
	case CACHEFILES_OP_OPEN:
		return process_open_req(devfd, msg);
	case CACHEFILES_OP_CLOSE:
		return process_close_req(devfd, msg);
	case CACHEFILES_OP_READ:
		return process_read_req(devfd, msg);
	default:
		printf("invalid opcode %d\n", msg->opcode);
		return -1;
	}
}

static int handle_requests(int devfd)
{
	int ret = 0;
	struct pollfd pollfd;

	pollfd.fd = devfd;
	pollfd.events = POLLIN;

	printf("\t\tthread startup, tid %lu\n", pthread_self());
	while (1) {
		ret = poll(&pollfd, 1, -1);
		if (ret < 0) {
			printf("poll failed\n");
			return -1;
		}

		if (ret == 0 || !(pollfd.revents & POLLIN)) {
			printf("poll returned %d (%x)\n", ret, pollfd.revents);
			continue;
		}

		/* process all pending read requests */
		while (!process_one_req(devfd)) {}
	}

	return 0;
}

static void *thread_work(void *data)
{
	int devfd = *(int *)data;

	handle_requests(devfd);

	return NULL;
}

static int fork_and_start_thread(int devfd)
{
	pthread_t tid[10];
	int i;

	pid_t pid = fork();
	if (pid == 0) {
		init_shm_address();
		for (i = 0; i < FSCACHETHREADS; i++) {
			pthread_create(&tid[i], 0, thread_work, &devfd);
		}
		for (i = 0; i < FSCACHETHREADS; i++) {
			pthread_join(tid[i], 0);
		}
	}
	return pid;
}

int main(int argc, char *argv[])
{
	char *fscachedir;
	int devfd, ret;
	int wstatus = 0;
	pid_t pid;

	if (argc != 2) {
		printf("Using example: cachefilesd2 <fscachedir>\n");
		return -1;
	}
	fscachedir = argv[1];

	supervisor_init_shm();

	devfd = daemon_get_devfd(fscachedir, "test");
	if (devfd < 0)
		return -1;

restart:
	printf("supervisor[pid %d]: fork child process\n", getpid());
	pid = fork_and_start_thread(devfd);
	if (pid < 0) {
		printf("fork and start thread failed\n");
		return -1;
	}
	if (pid == wait(&wstatus)) {
		ssize_t written = write(devfd, "restore", 7);
		if (written != 7) {
			printf("supervisor: write restore cmd recover failed\n");
			return -1;
		} else {
			printf("supervisor: restore in-flight IO success\n");
		}
		goto restart;
	}

	return 0;
}
