#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "share/dbg.h"
#include "share/hpc.h"
#include "share/sys.h"

#include "recorder/recorder.h"
#include "recorder/write_trace.h"
#include "recorder/rec_sched.h"
#include "replayer/replayer.h"
#include "replayer/read_trace.h"
#include "replayer/rep_sched.h"


static pid_t child;

#define MAX_ARGC_LEN	16
#define MAX_ENVC_LEN	128
#define MAX_ARGV_LEN	128
#define MAX_ENVP_LEN	1500
#define MAX_EXEC_LEN    64

#define INVALID			0
#define RECORD			1
#define REPLAY			2

#define NO_REDIRECT		0
#define REDIRECT		1

static char** __argv;
static char** __envp;
static char* __executable;

static void alloc_argc(int argc)
{
	int i;
	assert(argc -1 < MAX_ARGC_LEN);
	__argv = sys_malloc(MAX_ARGC_LEN * sizeof(char*));

	for (i = 0; i < MAX_ARGC_LEN; i++) {
		__argv[i] = sys_malloc(MAX_ARGV_LEN);
	}

}

static void alloc_envp(char** envp)
{
	int i;

	__envp = sys_malloc(MAX_ENVC_LEN * sizeof(char*));
	for (i = 0; i < MAX_ENVP_LEN; i++) {
		__envp[i] = sys_malloc(MAX_ENVP_LEN);
	}
}

static void copy_argv(int argc, char* argv[])
{
	int i;
	for (i = 0; i < argc - 2; i++) {
		int arglen = strlen(argv[i + 2]);
		assert(arglen + 1 < MAX_ARGV_LEN);
		strncpy(__argv[i], argv[i + 2], arglen + 1);
	}
	__argv[i] = NULL;
}

static void copy_envp(char** envp)
{
	int i = 0;
	while (envp[i] != NULL) {
		assert (i < MAX_ENVC_LEN);
		int arglen = strlen(envp[i]);
		assert(arglen < MAX_ENVP_LEN);
		strncpy(__envp[i], envp[i], arglen + 1);
		i++;
	}
	__envp[i] = 0;
}

static void alloc_executable()
{
	__executable = sys_malloc(MAX_EXEC_LEN);
}

static void copy_executable(char* exec)
{
	assert (strlen(exec) < MAX_EXEC_LEN);
	strcpy(__executable, exec);
}

/**
 * used to stop child process when the parent process bails out
 */
static void sig_child(int sig)
{
	printf("got signal %d\n", sig);
	kill(child, SIGQUIT);
	kill(getpid(), SIGQUIT);
}

void print_usage()
{
	printf("rr: missing/incorrect operands. usage is: rr --{record,replay} [--redirect_output] [--dump_memory=<syscall_num>] executable [args].\n");
}

static void install_signal_handler()
{
	signal(SIGINT, sig_child);
}

/**
 * main replayer method
 */
static void start(int option, int argc, char* argv[], char** envp, int redirect_output, int dump_memory)
{
	pid_t pid;
	int status, fake_argc;


	if (option == RECORD) {
		copy_executable(argv[2]);
		if (access(__executable, X_OK)) {
			printf("The specified file '%s' does not exist or is not executable\n", __executable);
			return;
		}

		/* create directory for trace files */
		setup_trace_dir(0);

		/* initialize trace files */
		open_trace_files();
		init_trace_files();
		copy_argv(argc, argv);
		copy_envp(envp);
		record_argv_envp(argc, __argv, __envp);
		close_trace_files();

		pid = sys_fork();

		//read_child_initial_memory_end_exit(pid,__executable,__argv);

		if (pid == 0) { /* child process */
			sys_start_trace(__executable, __argv, __envp);
		} else { /* parent process */
			child = pid;

			/* make sure that the child process dies when the master process gets interrupted */
			install_signal_handler();

			/* sync with the child process */
			sys_waitpid(pid, &status);

			/* configure the child process to get a message upon a thread start, fork(), etc. */
			sys_ptrace_setup(pid);

			/* initialize stuff */
			init_libpfm();
			/* initialize the trace file here -- we need to record argc and envp */
			open_trace_files();

			/* register thread at the scheduler and start the HPC */
			rec_sched_register_thread(0, pid);

			/* perform the action recording */
			fprintf(stderr, "start recording...\n");
			start_recording(dump_memory);
			fprintf(stderr, "done recording -- cleaning up\n");
			/* cleanup all initialized data-structures */
			close_trace_files();
			close_libpfm();
		}

		/* replayer code comes here */
	} else if (option == REPLAY) {
		init_environment(argv[2], &fake_argc, __argv, __envp);

		copy_executable(__argv[0]);
		if (access(__executable, X_OK)) {
			printf("The specified file '%s' does not exist or is not executable\n", __executable);
			return;
		}

		pid = sys_fork();

		//read_child_initial_memory_end_exit(pid,__executable,__argv);

		if (pid == 0) { /* child process */
			sys_start_trace(__executable, __argv, __envp);
		} else { /* parent process */
			child = pid;
			/* make sure that the child process dies when the master process gets interrupted */
			install_signal_handler();

			sys_waitpid(pid, &status);
			sys_ptrace_setup(pid);


			/* initialize stuff */
			init_libpfm();
			rep_sched_init();
			/* sets the file pointer to the first trace entry */

			read_trace_init(argv[2]);

			pid_t rec_main_thread = get_recorded_main_thread();
			rep_sched_register_thread(pid, rec_main_thread);

			/* main loop */
			replay(redirect_output, dump_memory);
			/* thread wants to exit*/
			close_libpfm();
			read_trace_close();
			rep_sched_close();
		}
	}
}

void check_prerequisites() {
	FILE *aslr_file = fopen("/proc/sys/kernel/randomize_va_space","r");
	int aslr_val;
	fscanf(aslr_file,"%d",&aslr_val);
	if (aslr_val != 0)
		assert(0 && "ASLR not disabled, exiting.");

	FILE *ptrace_scope_file = fopen("/proc/sys/kernel/yama/ptrace_scope","r");
	int ptrace_scope_val;
	fscanf(ptrace_scope_file,"%d",&ptrace_scope_val);
	if (ptrace_scope_val != 0)
		assert(0 && "Can't write to process memory, exiting.");
}

/**
 * This is where recorder and the repalyer start
 */
int main(int argc, char* argv[], char** envp)
{
	int option = INVALID;
	int redirect_output = NO_REDIRECT;
	int dump_memory = 0;

	/* check prerequisites for rr to run */
	check_prerequisites();

	/* check for sufficient amount of arguments */
	if (argc < 3) {
		print_usage();
		return 0;
	}

	if (strncmp("--record", argv[1], 8) == 0) {
		option = RECORD;
	} else if (strncmp("--replay", argv[1], 8) == 0) {
		option = REPLAY;
	}

	if (option == INVALID) {
		print_usage();
		return 0;
	}

	if  (argc > 3 && strncmp("--redirect_output", argv[2], 17) == 0) {
		redirect_output = REDIRECT;
		/* we can now ignore the option string */
		argv[2] = argv[3];
		argc--;
	}

	if  (argc > 3 && strncmp("--dump_memory=", argv[2], 14) == 0) {
		sscanf(argv[2],"--dump_memory=%d",&dump_memory);
		/* we can now ignore the option string */
		argv[2] = argv[3];
		argc--;
	}

	/* allocate memory for the arguments that are passed to the
	 * client application. This is the first thing that has to be
	 * done to ensure that the pointers that are passed to the client
	 * are the same in the recorder/replayer.*/
	alloc_argc(argc);
	alloc_envp(envp);
	alloc_executable();

	start(option, argc, argv, envp, redirect_output, dump_memory);

	return 0;

}

