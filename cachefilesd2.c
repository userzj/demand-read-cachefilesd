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
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include <passfds.h>

#include "internal.h"

static int process_one_req(int devfd, int sockfd)
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

	printf("[HEADER] id %u, opcode %d\t", msg->msg_id, msg->opcode);

	switch (msg->opcode) {
	case CACHEFILES_OP_OPEN:
		return process_open_req(devfd, sockfd, msg);
	case CACHEFILES_OP_CLOSE:
		return process_close_req(devfd, msg);
	case CACHEFILES_OP_READ:
		return process_read_req(devfd, msg);
	default:
		printf("invalid opcode %d\n", msg->opcode);
		return -1;
	}
}

static int handle_requests(int devfd, int sockfd)
{
	int ret = 0;
	struct pollfd pollfd;

	pollfd.fd = devfd;
	pollfd.events = POLLIN;

	printf("child process startup, handling reqs\n");
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
		while (!process_one_req(devfd, sockfd)) {}
	}

	return 0;
}

static int startup_child_process(int devfd, int sockfd)
{
	pid_t pid;
	int wstatus = 0;

restart:
	pid = fork();
	if (pid == 0) {
		/* child process */
		handle_requests(devfd, sockfd);
	} else if (pid > 0)  {
		/* parent process */
		if (pid == wait(&wstatus)) {
			if (WIFSIGNALED(wstatus)) {
				printf("parent: child be killed, now recover kernel req\n");
				ssize_t written = write(devfd, "restore", 7);
				if (written != 7) {
					printf("recover failed\n");
					return -1;
				}
				printf("parent: recover kernel req success, restart child process...\n");
				goto restart;
			}
		}
	} else {
		return pid;
	}

	return 0;
}
void *store_fd_work(void *data) {
	int receive_fd[1] = {0};
	int ret;
	int sockfd = *(int *)data;

	printf("sockfd %d\n", sockfd);
	while (1) {
		ret = recvfds(sockfd, receive_fd, 1);
		if (ret != 1) {
			printf("recvfds failed!\n");
			exit(1);
		}
		printf("supervisor receive fd %d\n", receive_fd[0]);
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	char *fscachedir;
	pthread_t thread;
	int devfd, ret, shm_id;
	int sv[2];

	if (argc != 2) {
		printf("Using example: cachefilesd2 <fscachedir>\n");
		return -1;
	}
	fscachedir = argv[1];

	supervisor_init_shm();

	ret = socketpair(AF_LOCAL, SOCK_STREAM, 0, sv);
	if (ret == -1) {
		perror("socketpair");
		exit(EXIT_FAILURE);
	} else {
		printf("parent sockfd %d , child sockfd %d\n", sv[0], sv[1]);
	}

	/* parent: create a thread to reveice and keep fd fd's reference */
	pthread_create(&thread, 0, store_fd_work, &sv[0]);

	devfd = daemon_get_devfd(fscachedir, "test");
	if (devfd < 0)
		return -1;

	startup_child_process(devfd, sv[1]);
	pthread_join(thread, NULL);

	return 0;
}
