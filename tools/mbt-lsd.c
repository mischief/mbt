#include <fcntl.h>
#include <err.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "event.h"
#include "infohash.h"
#include "lsd.h"

char *cmd = NULL;
char **cmd_argv = NULL;
int cmd_argc = 0;

static void
parse_cmd(const char *command)
{
	char *cmd_copy, *token, *saveptr;
	int i = 0;

	if (command == NULL)
		return;

	cmd_copy = strdup(command);
	if (cmd_copy == NULL)
		err(1, "strdup");

	char *temp = strdup(command);
	if (temp == NULL)
		err(1, "strdup");
	
	cmd_argc = 0;
	for (token = strtok_r(temp, " \t", &saveptr); token != NULL;
	     token = strtok_r(NULL, " \t", &saveptr)) {
		cmd_argc++;
	}
	free(temp);

	cmd_argv = malloc((cmd_argc + 1) * sizeof(char *));
	if (cmd_argv == NULL)
		err(1, "malloc");

	saveptr = NULL;
	for (token = strtok_r(cmd_copy, " \t", &saveptr); token != NULL;
	     token = strtok_r(NULL, " \t", &saveptr)) {
		cmd_argv[i] = strdup(token);
		if (cmd_argv[i] == NULL)
			err(1, "strdup");
		i++;
	}
	cmd_argv[cmd_argc] = NULL;

	free(cmd_copy);
}

static void
do_exec(char *s)
{
	pid_t pid, wpid;
	char **argv;
	int i;

	if (cmd_argv == NULL)
		return;

	argv = malloc((cmd_argc + 1) * sizeof(char *));
	if (argv == NULL)
		err(1, "malloc");

	for (i = 0; i < cmd_argc; i++) {
		char *arg = cmd_argv[i];
		char *pos = strstr(arg, "%H");

		if (pos != NULL) {
			size_t before_len = pos - arg;
			size_t hash_len = strlen(s);
			size_t after_len = strlen(pos + 2);
			size_t total_len = before_len + hash_len + after_len;

			argv[i] = malloc(total_len + 1);
			if (argv[i] == NULL)
				err(1, "malloc");

			strncpy(argv[i], arg, before_len);
			strcpy(argv[i] + before_len, s);
			strcpy(argv[i] + before_len + hash_len, pos + 2);
		} else {
			argv[i] = strdup(arg);
			if (argv[i] == NULL)
				err(1, "strdup");
		}
	}
	argv[cmd_argc] = NULL;

	pid = fork();
	if (pid == -1) {
		err(1, "fork");
	} else if (pid == 0) {
		execvp(argv[0], argv);
		err(1, "execvp");
	} else {
		if ((wpid = waitpid(pid, NULL, 0)) < 0) {
			err(1, "waitpid");
		}
	}

	for (i = 0; i < cmd_argc; i++) {
		free(argv[i]);
	}
	free(argv);
}

struct lsd_tool_ctx {
	struct lsd *lsd;
	struct mbt_ev *ev;
};

static void
lsd_handler(struct mbt_ev_io *io, int fd, uint32_t events, void *v)
{
	int rv;
	struct lsd_tool_ctx *ctx = v;
	struct lsd *lsd = ctx->lsd;
	__attribute__((cleanup(lsd_frame_freep))) struct lsd_frame *frame = NULL;
	__attribute__((cleanup(lsd_frame_info_freep))) struct lsd_frame_info *info = NULL;
	struct infohash *ih;
	char hash[INFOHASH_SHA1HEXLEN+1];

	(void)io;
	(void)fd;
	(void)events;

	rv = lsd_recv(lsd, &frame);
	if (rv < 0) {
		warnx("lsd_recv: %s", strerror(-rv));
		mbt_ev_stop(ctx->ev);
		return;
	}

	rv = lsd_frame_parse(frame, &info);
	if (rv < 0) {
		warnx("lsd_frame_parse: %s", strerror(-rv));
		mbt_ev_stop(ctx->ev);
		return;
	}

	printf("received LSD frame from %s\n", info->remote);
	printf("bt port %hu\n", info->port);
	printf("cookie: %s\n", info->cookie);
	printf("hashes:\n");
	STAILQ_FOREACH(ih, &info->infohashes, next) {
		if ((rv = infohash_string(ih, hash, sizeof(hash))) < 0) {
			warnx("infohash_string: %s", strerror(-rv));
		} else {
			printf("%s\n", hash);
			do_exec(hash);
		}
	}
}

int
main(int argc, char *argv[])
{
	int rv;
	int c;
	struct mbt_ev *ev = NULL;
	struct mbt_ev_io *lsd_io = NULL;
	__attribute__((cleanup(lsd_freep))) struct lsd *lsdfd = NULL;

	while ((c = getopt(argc, argv, "c:")) != -1) {
		switch (c) {
		case 'c':
			parse_cmd(optarg);
			break;
		default:
			fprintf(stderr, "Usage: %s [-c command]\n", argv[0]);
			return 1;
		}
	}

	if ((rv = lsd_init(&lsdfd, O_NONBLOCK)) < 0)
		errx(1, "lsd_init: %s", strerror(-rv));

#ifdef HAVE_SD_EVENT
	rv = mbt_ev_sd_new(&ev);
#elif defined(HAVE_LIBEVENT)
	rv = mbt_ev_libevent_new(&ev);
#else
#error "No event backend configured"
#endif
	if (rv < 0)
		errx(1, "mbt_ev_new: %s", strerror(-rv));

	struct lsd_tool_ctx ctx = { .lsd = lsdfd, .ev = ev };

	if ((rv = mbt_ev_io_new(ev, &lsd_io, lsdfd->fd, MBT_EV_READ, lsd_handler, &ctx)) < 0)
		errx(1, "mbt_ev_io_new: %s", strerror(-rv));

	rv = mbt_ev_run(ev);
	if (rv < 0)
		errx(1, "mbt_ev_run: %s", strerror(-rv));

	mbt_ev_io_free(lsd_io);
	mbt_ev_destroy(ev);

	return 0;
}
