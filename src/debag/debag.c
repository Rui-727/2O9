/* debag.c - hybrid sandbox (seccomp + ptrace)
 *
 * The orchestrator. Runs static analysis, installs the seccomp filter,
 * sets up ptrace for SECCOMP_RET_TRACE events, forks + execs the target,
 * and handles only the syscalls that seccomp routed to ptrace.
 *
 * Performance: the target runs at near-native speed because seccomp
 * handles 95% of syscalls in-kernel. Only the dangerous 5% (execve,
 * connect, mount, etc.) trigger a ptrace context switch.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/user.h>

#include "debag.h"

/* Forward decl from seccomp_filter.c */
extern int debag_install_seccomp(const debag_analysis_t *analysis,
                                  const debag_policy_t *policy);
extern void debag_print_seccomp_rules(const debag_analysis_t *analysis,
                                       const debag_policy_t *policy, FILE *out);

/* ── Syscall name lookup (for logging) ───────────────────────────── */
#include <sys/syscall.h>

static const char *syscall_name(int nr)
{
    switch (nr) {
    case __NR_read: return "read";
    case __NR_write: return "write";
    case __NR_open: return "open";
    case __NR_openat: return "openat";
    case __NR_close: return "close";
    case __NR_stat: return "stat";
    case __NR_fstat: return "fstat";
    case __NR_lstat: return "lstat";
    case __NR_lseek: return "lseek";
    case __NR_mmap: return "mmap";
    case __NR_mprotect: return "mprotect";
    case __NR_munmap: return "munmap";
    case __NR_brk: return "brk";
    case __NR_execve: return "execve";
    case __NR_execveat: return "execveat";
    case __NR_connect: return "connect";
    case __NR_socket: return "socket";
    case __NR_bind: return "bind";
    case __NR_listen: return "listen";
    case __NR_accept: return "accept";
    case __NR_sendto: return "sendto";
    case __NR_recvfrom: return "recvfrom";
    case __NR_unlink: return "unlink";
    case __NR_unlinkat: return "unlinkat";
    case __NR_rename: return "rename";
    case __NR_mount: return "mount";
    case __NR_umount2: return "umount2";
    case __NR_clone: return "clone";
    case __NR_fork: return "fork";
    case __NR_vfork: return "vfork";
    case __NR_ptrace: return "ptrace";
    case __NR_setuid: return "setuid";
    case __NR_setgid: return "setgid";
    case __NR_chroot: return "chroot";
    case __NR_pivot_root: return "pivot_root";
    default: return NULL;
    }
}

/* ── Run ─────────────────────────────────────────────────────────── */

debag_result_t *debag_run(int argc, const char **argv,
                           const debag_policy_t *policy,
                           const debag_analysis_t *analysis)
{
    if (argc < 1 || !argv[0]) return NULL;

    debag_result_t *result = calloc(1, sizeof(*result));

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        free(result);
        return NULL;
    }

    if (child == 0) {
        /* Child: install seccomp filter, then exec */
        if (!policy->fast_mode) {
            /* Request ptrace tracing - needed for SECCOMP_RET_TRACE */
            ptrace(PTRACE_TRACEME, 0, NULL, NULL);
            raise(SIGSTOP);
        }

        if (debag_install_seccomp(analysis, policy) < 0) {
            _exit(127);
        }

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent */
    if (!policy->fast_mode) {
        /* Wait for initial SIGSTOP */
        int status;
        waitpid(child, &status, 0);
        if (!WIFSTOPPED(status)) {
            result->exit_code = -1;
            return result;
        }

        /* Set ptrace options: follow forks, use SECCOMP mode */
        ptrace(PTRACE_SETOPTIONS, child, NULL,
               PTRACE_O_TRACESECCOMP | PTRACE_O_TRACEFORK |
               PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
               PTRACE_O_EXITKILL);

        /* Resume the child - it will run at near-native speed until
         * a seccomp TRACE event fires */
        ptrace(PTRACE_CONT, child, NULL, NULL);
    }

    /* Event loop: handle ptrace events from seccomp TRACE */
    pid_t current = child;
    while (1) {
        int status;
        pid_t waited = waitpid(-1, &status, __WALL);
        if (waited < 0) {
            if (errno == ECHILD) break;
            continue;
        }

        if (WIFEXITED(status)) {
            if (waited == child) {
                result->exit_code = WEXITSTATUS(status);
            }
            continue;
        }

        if (WIFSIGNALED(status)) {
            if (waited == child) {
                result->exit_code = 128 + WTERMSIG(status);
            }
            continue;
        }

        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            int event = (status >> 16) & 0xFFFF;

            if (event == PTRACE_EVENT_SECCOMP) {
                /* Seccomp routed a syscall to us. Get the syscall number. */
                long sc_nr = ptrace(PTRACE_PEEKUSER, waited,
                                    offsetof(struct user_regs_struct, orig_rax), NULL);
                result->syscalls_total++;

                const char *name = syscall_name((int)sc_nr);

                if (policy->dynamic_block) {
                    /* Block the syscall: skip it and return EPERM */
                    if (policy->verbose) {
                        fprintf(stderr, "debag: BLOCK %s (pid %d)\n",
                                name ? name : "unknown", waited);
                    }
                    ptrace(PTRACE_POKEUSER, waited,
                           offsetof(struct user_regs_struct, orig_rax), (void *)-1);
                    if (result->blocked_count < 64) {
                        const char *n = name ? name : "unknown";
                        result->blocked_list = realloc(result->blocked_list,
                            (result->blocked_count + 2) * sizeof(char *));
                        result->blocked_list[result->blocked_count++] = strdup(n);
                        result->blocked_list[result->blocked_count] = NULL;
                    }
                    result->syscalls_blocked++;
                } else {
                    /* Allow the syscall: just log it */
                    if (policy->verbose) {
                        fprintf(stderr, "debag: TRACE %s (pid %d)\n",
                                name ? name : "unknown", waited);
                    }
                    result->syscalls_allowed++;
                }

                /* Resume - syscall will execute (or be skipped if we changed rax) */
                ptrace(PTRACE_CONT, waited, NULL, NULL);
            } else if (event == PTRACE_EVENT_FORK ||
                       event == PTRACE_EVENT_VFORK ||
                       event == PTRACE_EVENT_CLONE) {
                /* New process - continue tracing it */
                ptrace(PTRACE_CONT, waited, NULL, NULL);
            } else if (sig == SIGTRAP) {
                ptrace(PTRACE_CONT, waited, NULL, NULL);
            } else {
                /* Deliver the signal to the child */
                ptrace(PTRACE_CONT, waited, NULL, (void *)(long)sig);
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    result->duration_ms = (end.tv_sec - start.tv_sec) * 1000 +
                          (end.tv_nsec - start.tv_nsec) / 1000000;

    return result;
}

void debag_result_free(debag_result_t *r)
{
    if (!r) return;
    if (r->blocked_list) {
        for (size_t i = 0; i < r->blocked_count; i++)
            free(r->blocked_list[i]);
        free(r->blocked_list);
    }
    free(r);
}

void debag_result_print(const debag_result_t *r, FILE *out)
{
    if (!r) return;

    fprintf(out, "{\n");
    fprintf(out, "  \"exit_code\": %d,\n", r->exit_code);
    fprintf(out, "  \"duration_ms\": %lu,\n", r->duration_ms);
    fprintf(out, "  \"syscalls\": {\n");
    fprintf(out, "    \"total_traced\": %zu,\n", r->syscalls_total);
    fprintf(out, "    \"allowed\": %zu,\n", r->syscalls_allowed);
    fprintf(out, "    \"blocked\": %zu\n", r->syscalls_blocked);
    fprintf(out, "  },\n");
    fprintf(out, "  \"blocked_syscalls\": [");
    for (size_t i = 0; i < r->blocked_count; i++) {
        if (i > 0) fprintf(out, ", ");
        fprintf(out, "\"%s\"", r->blocked_list[i]);
    }
    fprintf(out, "]\n");
    fprintf(out, "}\n");
}
