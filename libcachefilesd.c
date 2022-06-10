#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "internal.h"


#define NAME_MAX 512

struct fd_path_link {
	int object_id;
	int fd;
	int size;
	char path[NAME_MAX];
};

struct shm_link {
	struct fd_path_link links[32];
	unsigned int link_num;
};

int g_shm_id = -1;

void supervisor_init_shm(void)
{
	if ((g_shm_id = shmget(IPC_PRIVATE, sizeof(struct shm_link), IPC_CREAT | 0777)) < 0)
	{
		perror("shmget");
		exit(1);
	}
	void *addr = shmat(g_shm_id, NULL, 0);
	memset(addr, 0, sizeof(struct shm_link));
	printf("init shm success id %d\n", g_shm_id);
}

static struct fd_path_link *find_fd_path_link(int object_id)
{
	struct shm_link *link;
	int i;

	link = shmat(g_shm_id, 0, 0);

	for (i = 0; i < link->link_num; i++) {
		if (link->links[i].object_id == object_id)
			return &link->links[i];
	}
	return NULL;
}

int process_open_req(int devfd, struct cachefiles_msg *msg)
{
	struct cachefiles_open *load;
	struct shm_link *shm_link;
	struct fd_path_link *link;
	char *volume_key, *cookie_key;
	struct stat stats;
	char cmd[32];
	int ret;
	unsigned long long size;

	load = (void *)msg->data;
	volume_key = load->data;
	cookie_key = load->data + load->volume_key_size;

	printf("[OPEN] volume key %s (volume_key_size %lu), cookie key %s (cookie_key_size %lu), "
	       "object id %d, fd %d, flags %u\n",
		volume_key, load->volume_key_size, cookie_key, load->cookie_key_size,
		msg->object_id, load->fd, load->flags);

	ret = stat(cookie_key, &stats);
	if (ret) {
		printf("stat %s failed, %d (%s)\n", cookie_key, errno, strerror(errno));
		return -1;
	}
	size = stats.st_size;

	snprintf(cmd, sizeof(cmd), "copen %u,%llu", msg->msg_id, size);
	printf("Writing cmd: %s\n", cmd);

	ret = write(devfd, cmd, strlen(cmd));
	if (ret < 0) {
		printf("write [copen] failed\n");
		return -1;
	}

	shm_link = shmat(g_shm_id, 0, 0);
	if (shm_link == (void *) -1) {
		printf("get shm address failed %s g_shm_id = %d\n", strerror(errno),g_shm_id);
	}
	if (shm_link->link_num >= 32) {
		printf("shm_link->link_num >= 32\n");
		return -1;
	}

	link = (struct fd_path_link *)(&shm_link->links[shm_link->link_num]);
	shm_link->link_num++;

	link->size = size;
	link->object_id = msg->object_id;
	strncpy(link->path, cookie_key, NAME_MAX);
	link->fd = load->fd;

	return 0;
}

int process_close_req(int devfd, struct cachefiles_msg *msg)
{
	struct fd_path_link *link;

	link = find_fd_path_link(msg->object_id);
	if (!link) {
		printf("invalid object id %d\n", msg->object_id);
		return -1;
	}

	printf("[CLOSE] object_id %d, fd %d\n", msg->object_id, link->fd);
	close(link->fd);
	return 0;
}

/* 2MB buffer aligned with 512 (logical block size) for DIRECT IO  */
#define BUF_SIZE (2*1024*1024)
static char readbuf[BUF_SIZE] __attribute__((aligned(512)));

static int do_process_read_req(int devfd, struct cachefiles_msg *msg, int ra)
{
	struct cachefiles_read *read;
	struct fd_path_link *link;
	int i, ret, retval = -1;
	int dst_fd, src_fd;
	char *src_path = NULL;
	size_t len;
	unsigned long id;

	read = (void *)msg->data;

	link = find_fd_path_link(msg->object_id);
	if (!link) {
		printf("invalid object id %d\n", msg->object_id);
		return -1;
	}
	src_path = link->path;
	dst_fd = link->fd;

	printf("[READ] object_id %d, fd %d, src_path %s, off %llx, len %llx\n",
			msg->object_id, dst_fd, src_path, read->off, read->len);

	src_fd = open(src_path, O_RDONLY);
	if (src_fd < 0) {
		printf("open src_path %s failed\n", src_path);
		return -1;
	}

	len = read->len;
	if (BUF_SIZE < len) {
		printf("buffer overflow\n");
		close(src_fd);
		return -1;
	}

	if (ra && read->off + BUF_SIZE <= link->size)
		len = BUF_SIZE;

	ret = pread(src_fd, readbuf, len, read->off);
	if (ret != len) {
		printf("read src image failed, ret %d, %d (%s)\n", ret, errno, strerror(errno));
		close(src_fd);
		return -1;
	}

	ret = pwrite(dst_fd, readbuf, len, read->off);
	if (ret != len) {
		printf("write dst image failed, ret %d, %d (%s)\n", ret, errno, strerror(errno));
		close(src_fd);
		return -1;
	}

	id = msg->msg_id;
	ret = ioctl(dst_fd, CACHEFILES_IOC_CREAD, id);
	if (ret < 0) {
		printf("send cread failed, %d (%s)\n", errno, strerror(errno));
		close(src_fd);
		return -1;
	}

	close(src_fd);
	return 0;
}

int process_read_req(int devfd, struct cachefiles_msg *msg)
{
	return do_process_read_req(devfd, msg, 0);
}

int daemon_get_devfd(const char *fscachedir, const char *tag)
{
	char *cmd;
	char cmdbuf[128];
	int fd, ret;

	if (!fscachedir)
		return -1;

	fd = open("/dev/cachefiles", O_RDWR);
	if (fd < 0) {
		printf("open /dev/cachefiles failed %s\n", strerror(errno));
		return -1;
	}

	snprintf(cmdbuf, sizeof(cmdbuf), "dir %s", fscachedir);
	ret = write(fd, cmdbuf, strlen(cmdbuf));
	if (ret < 0) {
		printf("write dir failed, %d\n", errno);
		goto error;
	}

	if (tag) {
		snprintf(cmdbuf, sizeof(cmdbuf), "tag %s", tag);
		ret = write(fd, cmdbuf, strlen(cmdbuf));
		if (ret < 0) {
			printf("write tag failed, %d\n", errno);
			goto error;
		}
	}

	cmd = "bind ondemand";
	ret = write(fd, cmd, strlen(cmd));
	if (ret < 0) {
		printf("bind failed %s\n", strerror(errno));
		goto error;
	}

	return fd;
error:
	close(fd);
	return -1;
}
