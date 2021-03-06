/*
 * Break - a software debugger PoC
 *
 * Copyright 2018 Orange
 * <camille.oudot@orange.com>
 *
 */

#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>

#include "break_utils.h"


enum tracee_status {
	received_not_handled_signal,
	reached_breakpoint,
};

static void setup_tracee(void);
static pid_t launch_tracee(char *argv[]);
static void setup_debugger(pid_t tracee);
static enum tracee_status handle_tracee_signal(pid_t tracee, int sig);
static void debug(pid_t tracee);
static void debugger_console(pid_t tracee);
static void insert_breakpoint(pid_t tracee, void *address);
static void remove_breakpoint(pid_t tracee, void *address) ;

static void usage(char *name) {
	printf("Runs the specified PROGRAM with the optional ARGS, and inserts "
			"a software breakpoint at the given ADDRESS.\n"
			"Usage:\n"
			"\t%s -a ADDRESS [--] PROGRAM [ARGS...]\n",
			name);
}

void *break_address = NULL;

int main(int argc, char *argv[]) {
	int c;
	pid_t tracee_pid;

	while ((c = getopt(argc, argv, "a:h")) != -1) {
		switch (c) {
			case 'a':
				break_address = (void*)strtoull(optarg, NULL, 0);
				break;
			case 'h':
				usage(*argv);
				exit(0);
				break;
			default:
				break;
		}
	}

	tracee_pid = launch_tracee(argv + optind);

	if (tracee_pid > 0) {
		setup_debugger(tracee_pid);
		debug(tracee_pid);
	}

	return 0;
}

static pid_t launch_tracee(char *argv[]) {
	pid_t child = fork();

	if (child == 0) { /* in the tracee */
		setup_tracee();
		raise(SIGSTOP);
		if (execvp(*argv, argv)) {
			perror(*argv);
			exit(1);
		}
	} else if (child < 0) { /* fork() failed */
		perror("fork");
	}

	return child;
}

static void setup_tracee(void) {
	DBG("init tracee %d\n", getpid());
	if (ptrace(PTRACE_TRACEME) != 0) {
		perror("PTRACE_TRACEME");
	}
}

static void setup_debugger(pid_t tracee) {
	int wstatus;
	DBG("init debugging of PID %d\n", tracee);

	wait_for_signal(tracee, SIGSTOP, &wstatus);
	print_wait_status_infos(wstatus);

	if (ptrace(PTRACE_SETOPTIONS, tracee, NULL, PTRACE_O_TRACEEXEC) != 0) {
		perror("PTRACE_SETOPTIONS(PTRACE_O_TRACEEXEC)");
	}

	ptrace(PTRACE_CONT, tracee, NULL, NULL);

	wait_for_signal(tracee, SIGTRAP, &wstatus);
	print_wait_status_infos(wstatus);

	while (break_address == NULL) {
		break_address = prompt_address("enter the breakpoint address: ");
	}

	insert_breakpoint(tracee, break_address);

	ptrace(PTRACE_CONT, tracee, NULL, NULL);
}

static void debug(pid_t tracee) {
	int wstatus;

	for (;;) {
		if (waitpid(tracee, &wstatus, 0) == -1) {
			perror("waitpid");
			break;
		}

		puts("");
		print_wait_status_infos(wstatus);

		if (WIFSTOPPED(wstatus)) {

			if (handle_tracee_signal(tracee, WSTOPSIG(wstatus))
					== reached_breakpoint)
			{
				DBG(">>> PID %d reached the breakpoint at "
						"address %p\n",
						tracee,
						break_address);

				debugger_console(tracee);

				set_rip(tracee, break_address);
				remove_breakpoint(tracee, break_address);

				DBG("<<< resuming PID %d\n", tracee);
				ptrace(PTRACE_CONT, tracee, NULL, NULL);
			}
		}

		if (WIFEXITED(wstatus)) {
			break;
		}
	}
}

static void debugger_console(pid_t tracee) {
	int exit = 0;
	char command, _;

	printf("enter a command: 'c': continue, 'r': print registers\n");

	while (!exit) {
		printf("(PID %d)> ", tracee);
		if (scanf("%c", &command) == 1) {
			if (command == '\n') continue; /* no command */
			while (scanf("%c", &_) == 1 && _ != '\n'); /* empty the keyboad buffer */

			switch (command) {
			case 'c':
				return;
				break;
			case 'r':
				print_registers(tracee);
				break;
			default:
				printf("invalid command: '%c'\n",
						(int)command);
				break;
			}
		}
	}
}

static enum tracee_status handle_tracee_signal(pid_t tracee, int sig) {
	int ret = received_not_handled_signal;
	siginfo_t si;

	if (sig == SIGTRAP) {
		if (ptrace(PTRACE_GETSIGINFO, tracee, NULL, &si) != 0) {
			perror("PTRACE_GETSIGINFO");
		} else {
			if (si.si_code == SI_KERNEL
					&& (void*)get_rip(tracee) == break_address + 1) {
				ret = reached_breakpoint;
			}
		}
	}

	return ret;
}

static const unsigned char int3 = 0xcc;
static unsigned char breakpoint_backup;

static void insert_breakpoint(pid_t tracee, void *address) {
	DBG("inserting breakpoint at address %p\n", address);
	peek_mem(tracee, address, &breakpoint_backup, 1);
	poke_mem(tracee, address, &int3, 1);
}

static void remove_breakpoint(pid_t tracee, void *address) {
	poke_mem(tracee, address, &breakpoint_backup, 1);
}
