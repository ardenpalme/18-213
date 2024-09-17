/**
 * @file tsh.c
 * @brief A tiny shell program with job control
 *
 * This shell program parses command line arguments from the user
 * with builtin command allowing to bring stopped jobs to foreground/background
 * and background jobs to the foreground. There are further built in commands
 * for listing jobs; this shell also supports I/O redirection through Linux's
 * `>` and `<` operators. Furthermore, typed-in commands are executed in a forked
 * process.
 *
 * @author Arden Diakhate <aqd@andrew.cmu.edu>
 */

#include "csapp.h"
#include "tsh_helper.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * If DEBUG is defined, enable contracts and printing on dbg_printf.
 */
#ifdef DEBUG
/* When debugging is enabled, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated for these */
#define dbg_printf(...)
#define dbg_requires(...)
#define dbg_assert(...)
#define dbg_ensures(...)
#endif

//permission bits (req'd combination)
#define PERMIS S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH

/* Function prototypes */
void eval(const char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);
void sigquit_handler(int sig);
void cleanup(void);

/**
 * @brief Establishes the shell, readying it for user-input
 *
 *  This function readies the Tiny shell for user-input by parsing the command
 *  line arguments to detect options, initializing the job list, creating
 *  environment variables, and installing signal handlers. Finally, the shell
 *  begins an infinite loop, where the user can input commands, and can exit.
 *
 *  arguments: argc, argv are computed when the user passes extra arguments
 *  when running the executable
 *
 *  error cases print out relevant messages when encoutered, but before exiting
 *
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE_TSH]; // Cmdline for fgets
    bool emit_prompt = true;   // Emit prompt (default)

    // Redirect stderr to stdout (so that driver will get all output
    // on the pipe connected to stdout)
    if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
        perror("dup2 error");
        exit(1);
    }

    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h': // Prints help message
            usage();
            break;
        case 'v': // Emits additional diagnostic info
            verbose = true;
            break;
        case 'p': // Disables prompt printing
            emit_prompt = false;
            break;
        default:
            usage();
        }
    }

    // Create environment variable
    if (putenv("MY_ENV=42") < 0) {
        perror("putenv error");
        exit(1);
    }

    // Set buffering mode of stdout to line buffering.
    // This prevents lines from being printed in the wrong order.
    if (setvbuf(stdout, NULL, _IOLBF, 0) < 0) {
        perror("setvbuf error");
        exit(1);
    }

    // Initialize the job list while blocking all signals
    sigset_t tmp_gen2, tmp_prev2;
    sigfillset(&tmp_gen2);

    sigprocmask(SIG_BLOCK, &tmp_gen2, &tmp_prev2);
    init_job_list();
    sigprocmask(SIG_SETMASK, &tmp_prev2, NULL);

    // Register a function to clean up the job list on program termination.
    // The function may not run in the case of abnormal termination (e.g. when
    // using exit or terminating due to a signal handler), so in those cases,
    // we trust that the OS will clean up any remaining resources.
    if (atexit(cleanup) < 0) {
        perror("atexit error");
        exit(1);
    }

    // Install the signal handlers
    Signal(SIGINT, sigint_handler);   // Handles Ctrl-C
    Signal(SIGTSTP, sigtstp_handler); // Handles Ctrl-Z
    Signal(SIGCHLD, sigchld_handler); // Handles terminated or stopped child

    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    Signal(SIGQUIT, sigquit_handler);

    // Execute the shell's read/eval loop
    while (true) {
        if (emit_prompt) {
            printf("%s", prompt);

            // We must flush stdout since we are not printing a full line.
            fflush(stdout);
        }

        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin)) {
            perror("fgets error");
            exit(1);
        }

        if (feof(stdin)) {
            // End of file (Ctrl-D)
            printf("\n");
            return 0;
        }

        // Remove any trailing newline
        char *newline = strchr(cmdline, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }

        // Evaluate the command line
        eval(cmdline);
    }

    return -1; // control never reaches here
}

/* @brief error-checking fork() wrapper function
 * Return Value: on success, returns the process id of the newly-created
 * child process to the parent, and 0 to the child.
 *
 * Errors: fork() errors will print an error message before exiting
 */
pid_t Fork(void){
    pid_t tmp_pid;

    if((tmp_pid = fork()) < 0){
        sio_printf("fork error: %s\n",strerror(errno));
        exit(0);
    }
    return tmp_pid;
}

/* @brief this function opens a file with the right permissions in the
 * correct mode, returning its fd
 *
 * [in]argument: filename, a string containing the filename (possibly
 * in an absolute or relative path)
 *
 * Return value: on success, file descriptor of the newly-opened file filename
 *
 * Errors: dealt with by printing the relevant error message before exiting,
 */
int getout_fd(char *filename){
    int fd;
    bool flag= false;
    if((fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, PERMIS)) < 0){
        if(errno == EACCES){
            flag= true;
        }else{
            sio_printf("%s: %s\n", filename, strerror(errno));
            exit(0);
        }
    }
    if(flag){
        if((fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, PERMIS)) < 0){
            sio_printf("%s: %s\n", filename, strerror(errno));
            exit(0);
        }
    }
    return fd;
}

/* @brief evaluates a command line the user typed into the Tiny shell by
 * responding with the correct shell behavior.
 *
 * Handles built-in commands separately before forking a new process in which
 * to execute the user-inputted command line. Signal blocking ensures that
 * functions which are not async-signal safe are not interrupted by signals.
 * File I/O commands are dealt with as part of the child process (descriptors
 * are closed)
 *
 * [in]argument: user-inputted command line as a string
 * Return value:none
 * Errors: dealt with separately with a print to STDOUT of the appropriate
 * error message before exiting
 *
 */
void eval(const char *cmdline) {
    parseline_return parse_result;
    struct cmdline_tokens token;

    pid_t pid;
    sigset_t mask_all, mask_none;
    sigset_t prev, prev_one;

    //file descriptors for input and output redirection
    int in_fd= -1;
    int out_fd= -1;

    sigemptyset(&mask_all);
    sigemptyset(&prev);
    sigemptyset(&prev_one);
    sigaddset(&mask_all, SIGCHLD);
    sigaddset(&mask_all, SIGINT);
    sigaddset(&mask_all, SIGTSTP);
    sigemptyset(&mask_none);

    //blocked signals are restored before each ret instruction
    sigprocmask(SIG_BLOCK, &mask_all, &prev_one);

    // Parse command line
    parse_result = parseline(cmdline, &token);

    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY) {
        sigprocmask(SIG_SETMASK, &prev_one, NULL);
        return;
    }

    // check built-in command: quit
    if(token.builtin == BUILTIN_QUIT){
        exit(0);
    }

    //check built-in command: jobs
    if(token.builtin == BUILTIN_JOBS){
        //output redirection
        if(token.outfile != NULL){
            out_fd= getout_fd(token.outfile);
            list_jobs(out_fd);
            close(out_fd);
            sigprocmask(SIG_SETMASK, &prev_one, NULL);
            return;
        }

        list_jobs(1);
        sigprocmask(SIG_SETMASK, &prev_one, NULL);
        return;
    }

    //check built-in command: bg
    if(token.builtin == BUILTIN_BG){
        if(token.argc == 1){
            sio_printf("bg command requires PID or %%jobid argument\n");
            sigprocmask(SIG_SETMASK, &prev_one, NULL);
            return;
        }

        jid_t tmp_jid;
        jid_t tmp_pid;
        if(token.argv[1][0] == '%'){
            tmp_jid= atoi((char*)token.argv[1] + 1); //parse out the job id
            if(!job_exists(tmp_jid)){
                sio_printf("%%%d: No such job\n",tmp_jid);
                sigprocmask(SIG_SETMASK, &prev, NULL);
                return;
            }

            tmp_pid= job_get_pid(tmp_jid);
        }else{
            if(token.argv[1][0] < 0x30 || token.argv[1][0] > 0x39){
                sio_printf("bg: argument must be a PID or %%jobid\n");
                sigprocmask(SIG_SETMASK, &prev, NULL);
                return;
            }

            tmp_pid= atoi(token.argv[1]);
            if(tmp_pid == 0){
                sio_printf("bg command requires PID or %%jobid argument");
                sigprocmask(SIG_SETMASK, &prev, NULL);
                return;
            }
            tmp_jid= job_from_pid(tmp_pid);
        }

        job_set_state(tmp_jid, BG);
        if(killpg(tmp_pid, SIGCONT) < 0){
            sio_printf("Error in killpg: %s", strerror(errno));
            exit(0);
        }

        sio_printf("[%d] (%d) %s\n", tmp_jid, tmp_pid, job_get_cmdline(tmp_jid));
        sigprocmask(SIG_SETMASK, &prev_one, NULL);
        return;
    }

    //check built-in command: fg
    if(token.builtin == BUILTIN_FG){
        if(token.argc == 1){
            sio_printf("fg command requires PID or %%jobid argument\n");
            sigprocmask(SIG_SETMASK, &prev_one, NULL);
            return;
        }

        jid_t tmp_jid;
        jid_t tmp_pid;

        if(token.argv[1][0] == '%'){
            tmp_jid= atoi((char*)token.argv[1] + 1); //parse out the job id
            if(!job_exists(tmp_jid)){
                sio_printf("%%%d: No such job\n",tmp_jid);
                sigprocmask(SIG_SETMASK, &prev, NULL);
                return;
            }

            tmp_pid= job_get_pid(tmp_jid);
        }else{
            if(token.argv[1][0] < 0x30 || token.argv[1][0] > 0x39){
                sio_printf("fg: argument must be a PID or %%jobid\n");
                sigprocmask(SIG_SETMASK, &prev, NULL);
                return;
            }

            tmp_pid= atoi(token.argv[1]);
            if(tmp_pid == 0){
                sio_printf("fg command requires PID or %%jobid argument");
                sigprocmask(SIG_SETMASK, &prev, NULL);
                return;
            }
            tmp_jid= job_from_pid(tmp_pid);
        }

        job_set_state(tmp_jid, FG);
        if(killpg(tmp_pid, SIGCONT) < 0){
            sio_printf("Error in killpg: %s", strerror(errno));
            exit(0);
        }


        while(job_exists(tmp_jid) && job_get_state(tmp_jid) == FG){
            sigsuspend(&mask_none); //signals in mask_all already blocked here
        }

        sigprocmask(SIG_SETMASK, &prev_one, NULL);
        return;
    }

    if((pid = Fork()) == 0){
        //set the process group id of the child process to child process id
        setpgid(0,0);

        //check for I/O redirection
        if(token.outfile != NULL){
            out_fd= getout_fd(token.outfile);

            if(dup2(out_fd, STDOUT_FILENO) < 0){
                sio_printf("Error in dup2: %s", strerror(errno));
                exit(0);
            }
            if(close(out_fd) < 0){
                sio_printf("Error in close: %s", strerror(errno));
                exit(0);
            }
            if(dup2(STDOUT_FILENO, STDOUT_FILENO) < 0){
                sio_printf("Error in dup2: %s", strerror(errno));
                exit(0);
            }
        }
        if(token.infile != NULL){
            if((in_fd = open(token.infile, O_RDONLY, PERMIS)) < 0){
                sio_printf("%s: %s\n", token.infile, strerror(errno));
                //file doesn't exist error
                if(errno == 2){
                    exit(0);
                }
                sigprocmask(SIG_SETMASK, &prev_one, NULL);
                return;
            }

            if(dup2(in_fd, STDIN_FILENO) < 0){
                sio_printf("Error in dup2: %s", strerror(errno));
                exit(0);
            }
            if(close(in_fd) < 0){
                sio_printf("Error in close: %s", strerror(errno));
                exit(0);
            }
            if(dup2(STDIN_FILENO, STDIN_FILENO) < 0){
                sio_printf("Error in dup2: %s", strerror(errno));
                exit(0);
            }
        }


        sigprocmask(SIG_SETMASK, &prev_one, NULL);
        if(execve(token.argv[0], token.argv, environ) < 0){
            sio_printf("%s: %s\n", token.argv[0], strerror(errno));
            exit(0);
        }
    }

    //original command run with suffix `&`
    if(parse_result == PARSELINE_BG){
        add_job(pid, BG, cmdline);
        sio_printf("[%d] (%d) %s\n",job_from_pid(pid), pid, cmdline);
    }

    if(parse_result == PARSELINE_FG){
        add_job(pid, FG, cmdline);
        jid_t tmp= job_from_pid(pid);

        while(job_exists(tmp) && job_get_state(tmp) == FG){
            sigsuspend(&mask_none); //signals in mask_all already blocked here
        }
    }

    sigprocmask(SIG_SETMASK, &prev_one, NULL);
    return;
}

/* @brief Reaps zombie children setting states appropriately based on the
 * state of processes encountered.
 *
 * Blocking other signals which may interrupt this signal handler, the
 * sigchld handler loops through all terminated/stopped child proceeses
 * in the wait set, exiting the loop if none have terminated/stopped yet;
 * cases on various process status.
 *
 * [in]argument: signal which triggered the handler
 * Return value:none
 * Errors: dealt with separately with a print to STDOUT of the appropriate
 * error message before exiting
 */
void sigchld_handler(int sig){
    int old_errno= errno;

    sigset_t mask_all, prev, mask_none;
    sigaddset(&mask_all, SIGINT);
    sigaddset(&mask_all, SIGTSTP);
    sigemptyset(&mask_none);

    pid_t pid;
    int status;
    //reap all zombie children, returns immediately on stopped/terminated procs
    while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0){
        sigprocmask(SIG_BLOCK, &mask_all, &prev);

        //terminated normally
        if(WIFEXITED(status)){
            jid_t tmp= job_from_pid(pid);
            delete_job(tmp);
        }

        if(WIFSTOPPED(status)){
            jid_t tmp2= job_from_pid(pid);
            sio_printf("Job [%d] (%d) stopped by signal %d\n", tmp2, pid, WSTOPSIG(status));
            job_set_state(tmp2, ST);
        }

        if(WIFSIGNALED(status)){
            jid_t tmp1= job_from_pid(pid);
            sio_printf("Job [%d] (%d) terminated by signal %d\n", tmp1, pid, WTERMSIG(status));
            delete_job(tmp1);
        }

    }

    sigprocmask(SIG_SETMASK, &prev, NULL);
    errno= old_errno; //restore
}

/* @brief sends a SIGINT signal to all processes in the foreground process
 * group, blocks other signals from interrupting this function
 *
 * This function only sends SIGINT to all processes in fg proc group, if
 * there is no function process, skip.
 *
 * [in]argument: signal which triggered the handler
 * Return value:none
 * Errors: dealt with separately with a print to STDOUT of the appropriate
 * error message before exiting
 */
void sigint_handler(int sig) {
    int old_errno= errno;

    sigset_t mask_all, prev;
    sigaddset(&mask_all, SIGTSTP);
    sigaddset(&mask_all, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask_all, &prev);

    if(fg_job() > 0){
        pid_t pid= job_get_pid(fg_job());
        pid_t tmp= getpgid(pid);

        if(pid == tmp){
            if(killpg(tmp, SIGINT) < 0){
                sio_printf("Error in killpg: %s", strerror(errno));
                exit(0);
            }
        }
    }

    sigprocmask(SIG_SETMASK, &prev, NULL);
    errno= old_errno; //restore errno
}

/* @brief sends a SIGTSTP signal to all processes in the foreground process
 * group, blocks other signals from interrupting this function
 *
 * This function only sends SIGTSTP to all processes in fg proc group, if
 * there is no function process, skip.
 *
 * [in]argument: signal which triggered the handler
 * Return value:none
 * Errors: dealt with separately with a print to STDOUT of the appropriate
 * error message before exiting
 */
void sigtstp_handler(int sig) {
    int old_errno= errno;

    sigset_t mask_all, prev;
    sigaddset(&mask_all, SIGINT);
    sigaddset(&mask_all, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask_all, &prev);

    if(fg_job() > 0){
        pid_t pid= job_get_pid(fg_job());
        pid_t tmp= getpgid(pid);

        if(pid == tmp){
            if(killpg(tmp, SIGTSTP) < 0){
                sio_printf("Error in killpg: %s", strerror(errno));
                exit(0);
            }
        }

    }

    sigprocmask(SIG_SETMASK, &prev, NULL);
    errno= old_errno; //restore errno
}

/**
 * @brief Attempt to clean up global resources when the program exits.
 *
 * In particular, the job list must be freed at this time, since it may
 * contain leftover buffers from existing or even deleted jobs.
 */
void cleanup(void) {
    // Signals handlers need to be removed before destroying the joblist
    Signal(SIGINT, SIG_DFL);  // Handles Ctrl-C
    Signal(SIGTSTP, SIG_DFL); // Handles Ctrl-Z
    Signal(SIGCHLD, SIG_DFL); // Handles terminated or stopped child

    //delete(free) the job list while blocking async-signals
    sigset_t tmp_gen1, tmp_prev1;
    sigfillset(&tmp_gen1);

    sigprocmask(SIG_BLOCK, &tmp_gen1, &tmp_prev1);
    destroy_job_list();
    sigprocmask(SIG_SETMASK, &tmp_prev1, NULL);
}
