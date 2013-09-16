#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "crtools.h"
#include "util-pie.h"
#include "log.h"
#include "cr-service.h"

struct _cr_service_client *cr_service_client;

static int recv_criu_msg(int socket_fd, CriuReq **msg)
{
	unsigned char buf[MAX_MSG_SIZE];
	int len;

	len = read(socket_fd, buf, MAX_MSG_SIZE);
	if (len == -1) {
		puts("Can't read request");
		return -1;
	}

	*msg = criu_req__unpack(NULL, len, buf);
	if (!*msg) {
		puts("Failed unpacking request");
		return -1;
	}

	return 0;
}

static int send_criu_msg(int socket_fd, CriuResp *msg)
{
	unsigned char buf[MAX_MSG_SIZE];
	int len;

	len = criu_resp__get_packed_size(msg);

	if (criu_resp__pack(msg, buf) != len) {
		pr_perror("Failed packing response");
		return -1;
	}

	if (write(socket_fd, buf, len)  == -1) {
		pr_perror("Can't send response");
		return -1;
	}

	return 0;
}

int send_criu_dump_resp(int socket_fd, bool success, bool restored)
{
	CriuResp msg = CRIU_RESP__INIT;
	CriuDumpResp resp = CRIU_DUMP_RESP__INIT;

	msg.type = CRIU_REQ_TYPE__DUMP;
	msg.success = success;
	msg.dump = &resp;

	resp.restored = restored;

	return send_criu_msg(socket_fd, &msg);
}

static int setup_dump_from_req(CriuDumpReq *req)
{
	struct ucred ids;
	struct stat st;
	socklen_t ids_len = sizeof(struct ucred);
	char images_dir_path[PATH_MAX];

	if (getsockopt(cr_service_client->sk_fd, SOL_SOCKET, SO_PEERCRED,
							  &ids, &ids_len)) {
		pr_perror("Can't get socket options.");
		return -1;
	}

	cr_service_client->pid = ids.pid;
	cr_service_client->uid = ids.uid;

	if (fstat(cr_service_client->sk_fd, &st)) {
		pr_perror("Can't get socket stat");
		return -1;
	}

	cr_service_client->sk_ino = st.st_ino;

	/* going to dir, where to place images*/
	sprintf(images_dir_path, "/proc/%d/fd/%d",
		cr_service_client->pid, req->images_dir_fd);

	if (chdir(images_dir_path)) {
		pr_perror("Can't chdir to images directory");
		return -1;
	}

	if (open_image_dir() < 0)
		return -1;

	log_closedir();

	/* initiate log file in imgs dir */
	opts.output = "./dump.log";

	log_set_loglevel(req->log_level);
	if (log_init(opts.output) == -1) {
		pr_perror("Can't initiate log.");
		return -1;
	}

	/* checking dump flags from client */
	if (req->has_leave_running && req->leave_running)
		opts.final_state = TASK_ALIVE;

	if (!req->has_pid) {
		req->has_pid = true;
		req->pid = ids.pid;
	}

	if (req->has_ext_unix_sk)
		opts.ext_unix_sk = req->ext_unix_sk;

	if (req->has_tcp_established)
		opts.tcp_established_ok = req->tcp_established;

	if (req->has_evasive_devices)
		opts.evasive_devices = req->evasive_devices;

	if (req->has_shell_job)
		opts.shell_job = req->shell_job;

	if (req->has_file_locks)
		opts.handle_file_locks = req->file_locks;

	return 0;
}

static int dump_using_req(CriuDumpReq *req)
{
	bool success = false;

	if (setup_dump_from_req(req) == -1) {
		pr_perror("Arguments treating fail");
		goto exit;
	}

	if (cr_dump_tasks(req->pid) == -1) {
		pr_perror("Dump fail");
		goto exit;
	}

	if (req->has_leave_running && req->leave_running) {
		success = true;
exit:
		if (send_criu_dump_resp(cr_service_client->sk_fd,
					success, false) == -1) {
			pr_perror("Can't send response");
			success = false;
		}
	}

	close(cr_service_client->sk_fd);
	return success ? 0 : 1;
}

int cr_service(bool daemon_mode)
{
	int server_fd;
	int child_pid;

	struct sockaddr_un server_addr;
	struct sockaddr_un client_addr;

	socklen_t server_addr_len;
	socklen_t client_addr_len;

	CriuReq *msg = 0;

	cr_service_client = malloc(sizeof(struct _cr_service_client));

	server_fd = socket(AF_LOCAL, SOCK_SEQPACKET, 0);
	if (server_fd == -1) {
		pr_perror("Can't initialize service socket.");
		return -1;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	memset(&client_addr, 0, sizeof(client_addr));
	server_addr.sun_family = AF_LOCAL;

	if (opts.addr == NULL)
		opts.addr = CR_DEFAULT_SERVICE_ADDRESS;

	strcpy(server_addr.sun_path, opts.addr);

	server_addr_len = strlen(server_addr.sun_path)
			+ sizeof(server_addr.sun_family);
	client_addr_len = sizeof(client_addr);

	unlink(server_addr.sun_path);

	if (bind(server_fd, (struct sockaddr *) &server_addr,
					server_addr_len) == -1) {
		pr_perror("Can't bind.");
		return -1;
	}

	pr_info("The service socket is bound to %s\n", server_addr.sun_path);

	/* change service socket permissions, so anyone can connect to it */
	if (chmod(server_addr.sun_path, 0666)) {
		pr_perror("Can't change permissions of the service socket.");
		return -1;
	}

	if (listen(server_fd, 16) == -1) {
		pr_perror("Can't listen for socket connections.");
		return -1;
	}

	if (daemon_mode) {
		if (daemon(0, 0) == -1) {
			pr_perror("Can't run service server in the background");
			return -errno;
		}
	}

	/* FIXME Do not ignore children's return values */
	signal(SIGCHLD, SIG_IGN);

	while (1) {
		pr_info("Waiting for connection...\n");

		cr_service_client->sk_fd = accept(server_fd,
						  &client_addr,
						  &client_addr_len);
		if (cr_service_client->sk_fd == -1) {
			pr_perror("Can't accept connection.");
			continue;
		}

		pr_info("Connected.\n");

		switch (child_pid = fork()) {
		case -1:
			pr_perror("Can't fork a child.");
			continue;

		case 0:
			if (recv_criu_msg(cr_service_client->sk_fd,
							&msg) == -1) {
				pr_perror("Can't recv request");
				goto err;
			}

			switch (msg->type) {
			case CRIU_REQ_TYPE__DUMP:
				exit(dump_using_req(msg->dump));

			default:
				pr_perror("Invalid request");
				goto err;
			}

err:
			/*
			 * FIXME -- add generic error report
			 */

			close(cr_service_client->sk_fd);
			exit(-1);

		default:
			close(cr_service_client->sk_fd);
		}
	}

	return 0;
}
