#include <fcntl.h>
#include <err.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>

#include <systemd/sd-event.h>

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

	/* Count number of arguments */
	char *temp = strdup(command);
	if (temp == NULL)
		err(1, "strdup");
	
	cmd_argc = 0;
	for (token = strtok_r(temp, " \t", &saveptr); token != NULL;
	     token = strtok_r(NULL, " \t", &saveptr)) {
		cmd_argc++;
	}
	free(temp);

	/* Allocate argv array (argc + 1 for NULL terminator) */
	cmd_argv = malloc((cmd_argc + 1) * sizeof(char *));
	if (cmd_argv == NULL)
		err(1, "malloc");

	/* Populate argv array */
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

	/* Allocate a copy of argv to modify */
	argv = malloc((cmd_argc + 1) * sizeof(char *));
	if (argv == NULL)
		err(1, "malloc");

	/* Copy argv, replacing %H with hash in each argument */
	for (i = 0; i < cmd_argc; i++) {
		char *arg = cmd_argv[i];
		char *pos = strstr(arg, "%H");

		if (pos != NULL) {
			/* Argument contains %H, expand it */
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
			/* Argument doesn't contain %H, copy as-is */
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
		/* Execute with argv */
		execvp(argv[0], argv);
		err(1, "execvp");
	} else {
		if ((wpid = waitpid(pid, NULL, 0)) < 0) {
			err(1, "waitpid");
		}
	}

	/* Free the argv copy */
	for (i = 0; i < cmd_argc; i++) {
		free(argv[i]);
	}
	free(argv);
}

static int
lsd_handler(sd_event_source *s, int fd, uint32_t revents, void *v)
{
	int rv;
	struct lsd *lsd = v;
	__attribute__((cleanup(lsd_frame_freep))) struct lsd_frame *frame = NULL;
	__attribute__((cleanup(lsd_frame_info_freep))) struct lsd_frame_info *info = NULL;
	struct infohash *ih;
	char hash[INFOHASH_SHA1HEXLEN+1];

	(void)s;
	(void)fd;
	(void)revents;

	rv = lsd_recv(lsd, &frame);
	if (rv < 0) {
		warnx("lsd_recv: %s", strerror(-rv));
		sd_event_exit(sd_event_source_get_event(s), rv);
		return rv;
	}

	rv = lsd_frame_parse(frame, &info);
	if (rv < 0) {
		warnx("lsd_frame_parse: %s", strerror(-rv));
		sd_event_exit(sd_event_source_get_event(s), rv);
		return rv;
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

	return 0;
}

int
main(int argc, char *argv[])
{
	int rv;
	int c;
	__attribute__((cleanup(lsd_freep))) struct lsd *lsdfd = NULL;
	__attribute__((cleanup(sd_event_unrefp))) sd_event *ev = NULL;

	/* Parse command line arguments */
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

	if ((rv = sd_event_default(&ev)) < 0)
		errx(1, "sd_event_default: %s", strerror(-rv));

	if ((rv = sd_event_add_io(ev, NULL, lsdfd->fd, EPOLLIN, lsd_handler, lsdfd)) < 0)
		errx(1, "sd_event_add_io: %s", strerror(-rv));

	rv = sd_event_loop(ev);
	if (rv < 0)
		errx(1, "sd_event_loop: %s", strerror(-rv));

	return 0;
}
