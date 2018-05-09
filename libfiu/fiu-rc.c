
/*
 * libfiu remote control API
 */

#include <stdio.h>		/* snprintf() */
#include <string.h>		/* strncpy() */
#include <stdlib.h>		/* malloc()/free() */
#include <limits.h>		/* PATH_MAX */
#include <sys/types.h>		/* getpid(), mkfifo() */
#include <unistd.h>		/* getpid() */
#include <sys/stat.h>		/* mkfifo() */
#include <fcntl.h>		/* open() and friends */
#include <pthread.h>		/* pthread_create() and friends */
#include <errno.h>		/* errno and friends */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Enable us, so we get the real prototypes from the headers */
#define FIU_ENABLE 1

#include "fiu-control.h"
#include "internal.h"


/* Max length of a line containing a control directive */
#define MAX_LINE 512


/*
 * Generic remote control
 */

/* Reads a line from the given fd, assumes the buffer can hold MAX_LINE
 * characters. Returns the number of bytes read, or -1 on error. Inefficient,
 * but we don't really care. The final '\n' will not be included. */
static int read_line(int fd, char *buf)
{
	int r;
	char c;
	unsigned int len;

	c = '\0';
	len = 0;
	memset(buf, 0, MAX_LINE);

	do {
		r = read(fd, &c, 1);
		if (r < 0)
			return -1;
		if (r == 0)
			break;

		len += r;

		*buf = c;
		buf++;

	} while (c != '\n' && c != '\0' && len < MAX_LINE);

	if (len > 0 && c == '\n') {
		*(buf - 1) = '\0';
		len--;
	}

	return len;
}

/* Remote control command processing.
 *
 * Supported commands:
 *  - disable name=N
 *  - enable name=N,failnum=F,failinfo=I
 *  - enable_random <same as enable>,probability=P
 *  - enable_stack_by_name <same as enable>,func_name=F,pos_in_stack=P
 *
 * All enable* commands can also take an additional "onetime" parameter,
 * indicating that this should only fail once (analogous to the FIU_ONETIME
 * flag).
 *
 * This function is ugly, but we aim for simplicity and ease to extend for
 * future commands.
 */
int fiu_rc_string(const char *cmd, char ** const error)
{
	char m_cmd[MAX_LINE];
	char command[MAX_LINE], parameters[MAX_LINE];

	/* We need a version of cmd we can write to for parsing */
	strncpy(m_cmd, cmd, MAX_LINE);

	/* Separate command and parameters */
	{
		char *tok = NULL, *state = NULL;

		tok = strtok_r(m_cmd, " \t", &state);
		if (tok == NULL) {
			*error = "Cannot get command";
			return -1;
		}
		strncpy(command, tok, MAX_LINE);

		tok = strtok_r(NULL, " \t", &state);
		if (tok == NULL) {
			*error = "Cannot get parameters";
			return -1;
		}
		strncpy(parameters, tok, MAX_LINE);
	}

	/* Parsing of parameters.
	 *
	 * To simplify the code, we parse the command parameters here. Not all
	 * commands use all the parameters, but since they're not ambiguous it
	 * makes it easier to do it this way. */
	char *fp_name = NULL;
	int startnum = 0;
	int failnum = 1;
	void *failinfo = NULL;
	unsigned int flags = 0;
	double probability = -1;
	char *func_name = NULL;
	int func_pos_in_stack = -1;

	{
		/* Different tokens that we accept as parameters */
		enum {
			OPT_NAME = 0,
			OPT_STARTNUM,
			OPT_FAILNUM,
			OPT_FAILINFO,
			OPT_PROBABILITY,
			OPT_FUNC_NAME,
			OPT_POS_IN_STACK,
			FLAG_ONETIME,
		};
		char * const token[] = {
			[OPT_NAME] = "name",
			[OPT_STARTNUM] = "startnum",
			[OPT_FAILNUM] = "failnum",
			[OPT_FAILINFO] = "failinfo",
			[OPT_PROBABILITY] = "probability",
			[OPT_FUNC_NAME] = "func_name",
			[OPT_POS_IN_STACK] = "pos_in_stack",
			[FLAG_ONETIME] = "onetime",
			NULL
		};

		char *value;
		char *opts = parameters;
		while (*opts != '\0') {
			switch (getsubopt(&opts, token, &value)) {
			case OPT_NAME:
				fp_name = value;
				break;
			case OPT_STARTNUM:
				startnum = atoi(value);
				break;
			case OPT_FAILNUM:
				failnum = atoi(value);
				break;
			case OPT_FAILINFO:
				failinfo = (void *) strtoul(value, NULL, 10);
				break;
			case OPT_PROBABILITY:
				probability = strtod(value, NULL);
				break;
			case OPT_FUNC_NAME:
				func_name = value;
				break;
			case OPT_POS_IN_STACK:
				func_pos_in_stack = atoi(value);
				break;
			case FLAG_ONETIME:
				flags |= FIU_ONETIME;
				break;
			default:
				*error = "Unknown parameter";
				return -1;
			}
		}
	}

	/* Excecute the command */
	if (strcmp(command, "disable") == 0) {
		return fiu_disable(fp_name);
	} else if (strcmp(command, "enable") == 0) {
		return fiu_enable(fp_name, startnum, failnum, failinfo, flags);
	} else if (strcmp(command, "enable_random") == 0) {
		return fiu_enable_random(fp_name, startnum, failnum, failinfo,
				flags, probability);
	} else if (strcmp(command, "enable_stack_by_name") == 0) {
		return fiu_enable_stack_by_name(fp_name, startnum, failnum, failinfo,
				flags, func_name, func_pos_in_stack);
	} else {
		*error = "Unknown command";
		return -1;
	}
}

/* Read remote control directives from fdr and process them, writing the
 * results in fdw. Returns the length of the line read, 0 if EOF, or < 0 on
 * error. */
static int rc_do_command(int fdr, int fdw)
{
	int len, r, reply_len;
	char buf[MAX_LINE], reply[MAX_LINE];
	char *error;
	char str[256];
	int fd = open("/tmp/tcpserver",  O_CREAT | O_WRONLY|O_APPEND, 0644);
	if (fd == -1) {
		return 0;
	}
	len = read_line(fdr, buf);
	if (len <= 0) {
		snprintf(str,  256, "error readline len:%d errno:%d", len, errno);
		write(fd, str, strlen(str));
		close(fd);
		return len;
	}

	snprintf(str,  256, "readline:%s, len:%d",buf, len);
	write(fd, str, strlen(str));
	r = fiu_rc_string(buf, &error);
	snprintf(str,  256, "fiu_rc_string r:%d, error:%s", r, error);
	write(fd, str, strlen(str));

	reply_len = snprintf(reply, MAX_LINE, "%d", r);
	r = write(fdw, reply, reply_len);
	if (r <= 0) {
		perror("write erro");
		snprintf(str,  256, "error write:%d ", errno);
		write(fd, str, strlen(str));
		close(fd);
		return r;
	}

	perror("write end");
	write(fd, "end\n", strlen("end\n"));
	close(fd);
	return len;
}

/*
 * Remote control via named pipes
 *
 * Enables remote control over a named pipe that begins with the given
 * basename. "-$PID.in" will be appended to it to form the final path to read
 * commands from, and "-$PID.out" will be appended to it to form the final
 * path to write the replies to. After the process dies, the pipe will be
 * removed. If the process forks, a new pipe will be created.
 */

static char npipe_basename[PATH_MAX];
static char npipe_path_in[PATH_MAX];
static char npipe_path_out[PATH_MAX];

static void *rc_fifo_thread(void *unused)
{
	int fdr, fdw, r, errcount;

	/* increment the recursion count so we're not affected by libfiu,
	 * otherwise we could make the remote control useless by enabling all
	 * failure points */
	rec_count++;

	errcount = 0;

reopen:
	if (errcount > 10) {
		fprintf(stderr, "libfiu: Too many errors in remote control "
				"thread, shutting down\n");
		return NULL;
	}

	fdr = open(npipe_path_in, O_RDONLY);
	if (fdr < 0)
		return NULL;

	fdw = open(npipe_path_out, O_WRONLY);
	if (fdw < 0) {
		close(fdr);
		return NULL;
	}

	for (;;) {
		r = rc_do_command(fdr, fdw);
		if (r < 0 && errno != EPIPE) {
			perror("libfiu: Error reading from remote control");
			errcount++;
			close(fdr);
			close(fdw);
			goto reopen;
		} else if (r == 0 || (r < 0 && errno == EPIPE)) {
			/* one of the ends of the pipe was closed */
			close(fdr);
			close(fdw);
			goto reopen;
		}
	}

	/* we never get here */
}

static void *rc_tcpserver_thread(void *unused)
{
	/* increment the recursion count so we're not affected by libfiu,
	 * otherwise we could make the remote control useless by enabling all
	 * failure points */
	// for test
	// char str[256];
	// int fd = open("/tmp/tcpserver",  O_CREAT | O_WRONLY, 0644);
	// if (fd == -1) {
	// 	return NULL;
	// }
	// snprintf(str,  4096, "Server get connection from %s\n",(char*)inet_ntoa(client_addr.sin_addr));
	// write(fd, str, strlen(str));
	// close(fd);

	struct sockaddr_in addr, client_addr;
	int tcp_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (tcp_fd < 0) {
		return NULL;
	}
	char *fiu_enable_port = getenv("FIU_ENABLE_PORT");
	char *fiu_enable_ip = getenv("FIU_ENABLE_IP");
	if (fiu_enable_port != '\0' ) {
		if (fiu_enable_ip != '\0') {
			addr.sin_addr.s_addr = inet_addr(fiu_enable_ip);
		} else {
			addr.sin_addr.s_addr = INADDR_ANY;
		}
		addr.sin_family = AF_INET;
		addr.sin_port = htons(atoi(fiu_enable_port));

		int ret = bind(tcp_fd, (struct sockaddr *)&addr, sizeof(addr));
		if (ret < 0) {
			return NULL;
		}

		ret = listen(tcp_fd, 5);
		if (ret < 0) {
			return NULL;
		}

		for(;;) {
			int sin_size = sizeof(client_addr);
			int new_fd = accept(tcp_fd, (struct sockaddr *)&client_addr, (socklen_t* )&sin_size);
			if (new_fd < 0) {
				return NULL;
			}

			int r = rc_do_command(new_fd, new_fd);
			if (r < 0) {
				perror("libfiu: Error reading from tcp");
			}
			close(new_fd);
		}
	}
	return NULL;
	/* we never get here */
}

static void fifo_atexit(void)
{
	unlink(npipe_path_in);
	unlink(npipe_path_out);
}

static int _fiu_rc_fifo(const char *basename)
{
	pthread_t thread;

	/* see rc_fifo_thread() */
	rec_count++;

	snprintf(npipe_path_in, PATH_MAX, "%s-%d.in", basename, getpid());
	snprintf(npipe_path_out, PATH_MAX, "%s-%d.out", basename, getpid());

	if (mkfifo(npipe_path_in, 0600) != 0 && errno != EEXIST) {
		rec_count--;
		return -1;
	}

	if (mkfifo(npipe_path_out, 0600) != 0 && errno != EEXIST) {
		unlink(npipe_path_in);
		rec_count--;
		return -1;
	}

	if (pthread_create(&thread, NULL, rc_fifo_thread, NULL) != 0) {
		unlink(npipe_path_in);
		unlink(npipe_path_out);
		rec_count--;
		return -1;
	}
	if (pthread_create(&thread, NULL, rc_tcpserver_thread, NULL) != 0) {
		unlink(npipe_path_in);
		unlink(npipe_path_out);
		rec_count--;
		return -1;
	}

	atexit(fifo_atexit);

	rec_count--;
	return 0;
}

static void fifo_atfork_child(void)
{
	_fiu_rc_fifo(npipe_basename);
}

int fiu_rc_fifo(const char *basename)
{
	int r;

	r = _fiu_rc_fifo(basename);
	if (r < 0)
		return r;

	strncpy(npipe_basename, basename, PATH_MAX);
	pthread_atfork(NULL, NULL, fifo_atfork_child);

	return r;
}

