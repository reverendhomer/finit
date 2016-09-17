/* Finit TTY handling
 *
 * Copyright (c) 2013  Mattias Walström <lazzer@gmail.com>
 * Copyright (c) 2013  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <lite/lite.h>

#include "config.h"		/* Generated by configure script */
#include "finit.h"
#include "conf.h"
#include "helpers.h"
#include "tty.h"
#include "utmp-api.h"

LIST_HEAD(, tty_node) tty_list = LIST_HEAD_INITIALIZER();
static pid_t fallback = 0;


/* tty [!1-9,S] <DEV> [BAUD[,BAUD,...]] [TERM] */
int tty_register(char *line)
{
	tty_node_t *entry;
	int         insert = 0;
	char       *cmd, *dev, *baud = NULL;
	char       *runlevels = NULL, *term = NULL;

	if (!line) {
		_e("Invalid input argument");
		return errno = EINVAL;
	}

	cmd = strtok(line, " ");
	if (!cmd) {
	incomplete:
		_e("Incomplete tty, cannot register");
		return errno = EINVAL;
	}

	if (cmd[0] == '[') {	/* [runlevels] */
		runlevels = &cmd[0];
		dev = strtok(NULL, " ");
		if (!dev)
			goto incomplete;
	} else {
		dev = cmd;
	}

	cmd = strtok(NULL, " ");
	if (cmd) {
		baud = strdup(cmd);
		term = strtok(NULL, " ");
	}

	entry = tty_find(dev);
	if (!entry) {
		insert = 1;
		entry = calloc(1, sizeof(*entry));
		if (!entry) {
			if (baud)
				free(baud);

			return errno = ENOMEM;
		}
	}

	entry->data.name = strdup(dev);
	entry->data.baud = baud;
	entry->data.term = term ? strdup(term) : NULL;
	entry->data.runlevels = conf_parse_runlevels(runlevels);
	_d("Registering tty %s at %s baud with term=%s on runlevels %s", dev, baud ?: "NULL", term ?: "N/A", runlevels);

	if (insert)
		LIST_INSERT_HEAD(&tty_list, entry, link);

	return 0;
}

tty_node_t *tty_find(char *dev)
{
	tty_node_t *entry;

	LIST_FOREACH(entry, &tty_list, link) {
		if (!strcmp(dev, entry->data.name))
			return entry;
	}

	return NULL;
}

size_t tty_num(void)
{
	size_t num = 0;
	tty_node_t *entry;

	LIST_FOREACH(entry, &tty_list, link)
		num++;

	return num;
}

size_t tty_num_active(void)
{
	size_t num = 0;
	tty_node_t *entry;

	LIST_FOREACH(entry, &tty_list, link) {
		if (entry->data.pid)
			num++;
	}

	return num;
}

tty_node_t *tty_find_by_pid(pid_t pid)
{
	tty_node_t *entry;

	LIST_FOREACH(entry, &tty_list, link) {
		if (entry->data.pid == pid)
			return entry;
	}

	return NULL;
}

static char *canonicalize(char *tty)
{
	struct stat st;
	static char path[80];

	if (!tty)
		return NULL;

	strlcpy(path, tty, sizeof(path));
	if (stat(path, &st)) {
		snprintf(path, sizeof(path), "%s%s", _PATH_DEV, tty);
		if (stat(path, &st))
			return NULL;
	}

	if (!S_ISCHR(st.st_mode))
		return NULL;

	return path;
}

void tty_start(finit_tty_t *fitty)
{
	int is_console = 0;
	char *tty;

	if (fitty->pid)
		return;

	tty = canonicalize(fitty->name);
	if (!tty)
		return;

	if (console && !strcmp(tty, console))
		is_console = 1;

	fitty->pid = run_getty(tty, fitty->baud, fitty->term, is_console);
}

void tty_stop(finit_tty_t *tty)
{
	if (!tty->pid)
		return;

	_d("Stopping TTY %s", tty->name);
	kill(tty->pid, SIGTERM);
	do_sleep(2);
	kill(tty->pid, SIGKILL);
	waitpid(tty->pid, NULL, 0);
	tty->pid = 0;
}

int tty_enabled(finit_tty_t *tty, int runlevel)
{
	if (!tty)
		return 0;

	if (!ISSET(tty->runlevels, runlevel))
		return 0;
	if (fexist(tty->name))
		return 1;

	return 0;
}

/*
 * Fallback shell if no TTYs are active
 */
int tty_fallback(pid_t lost)
{
#ifdef FALLBACK_SHELL
	if (lost == 1) {
		if (fallback) {
			_d("Stopping fallback shell");
			kill(fallback, SIGKILL);
			fallback = 0;
		}

		return 0;
	}

	if (fallback != lost || tty_num_active()) {
		_d("Not starting fallback (%d) shell, lost pid %d", fallback, lost);
		return 0;
	}

	_d("Starting fallback shell");
	fallback = fork();
	if (fallback)
		return 1;

	/*
	 * Become session leader and set controlling TTY
	 * to enable Ctrl-C and job control in shell.
	 */
	setsid();
	ioctl(STDIN_FILENO, TIOCSCTTY, 1);

	_exit(execl(_PATH_BSHELL, _PATH_BSHELL, NULL));
#endif /* FALLBACK_SHELL */

	return 0;
}

/*
 * TTY monitor, called by service_monitor()
 */
int tty_respawn(pid_t pid)
{
	tty_node_t *entry = tty_find_by_pid(pid);

	if (!entry)
		return tty_fallback(pid);

	/* Set DEAD_PROCESS UTMP entry */
	utmp_set_dead(pid);

	/* Clear PID to be able to respawn it. */
	entry->data.pid = 0;

	if (!tty_enabled(&entry->data, runlevel))
		tty_stop(&entry->data);
	else
		tty_start(&entry->data);

	return 1;
}

/* Start all TTYs that exist in the system and are allowed at this runlevel */
void tty_runlevel(int runlevel)
{
	tty_node_t *entry;

	LIST_FOREACH(entry, &tty_list, link) {
		if (!tty_enabled(&entry->data, runlevel))
			tty_stop(&entry->data);
		else
			tty_start(&entry->data);
	}

	/* Start fallback shell if enabled && no TTYs */
	tty_fallback(tty_num_active() > 0 ? 1 : 0);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
