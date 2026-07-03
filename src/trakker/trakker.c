/* trakker.c - 2O9 execution sandbox and trace recorder implementation
 *
 * Uses Linux's ptrace API to intercept syscalls made by a child process.
 * All event recording happens at syscall ENTRY time (before the kernel
 * modifies anything), because the child's address space may be replaced
 * by execve before we see the exit.
 *
 * For blocking/redirect, we modify registers or memory at entry time.
 *
 * Architecture:
 *   1. Fork a child process
 *   2. Child calls PTRACE_TRACEME, sends SIGSTOP, then execs
 *   3. Parent catches SIGSTOP, sets PTRACE_O_TRACESYSGOOD + follow-fork
 *   4. Parent enters ptrace loop:
 *    - On syscall-entry (SIGTRAP|0x80): read regs, record event,
 *        optionally block/redirect, then continue
 *    - On plain SIGTRAP (from exec): suppress, continue
 *    - On other signals: deliver to child
 *    - On exit: record and remove from child list
 *   5. On completion: write JSON trace
 *
 * x86_64 only (matching the Arch Linux target).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>

/* syscall numbers (x86_64) */
#include <sys/syscall.h>

#include "trakker.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

static trak_event_t *event_new(trak_event_type_t type)
{
        trak_event_t *e = calloc(1, sizeof(*e));
        if (!e) return NULL;
        e->type = type;
        return e;
}

static void event_append(trak_result_t *result, trak_event_t *e)
{
        if (!e) return;
        trak_event_t **pp = &result->events;
        while (*pp) pp = &(*pp)->next;
        *pp = e;
        result->event_count++;
}

/* Read a NUL-terminated string from the child's address space. */
static char *read_child_string(pid_t child, unsigned long addr)
{
        if (addr == 0) return strdup("<null>");

        char *buf = malloc(PATH_MAX);
        if (!buf) return NULL;
        buf[0] = '\0';

        size_t i = 0;
        while (i < PATH_MAX - sizeof(long)) {
                errno = 0;
                long word = ptrace(PTRACE_PEEKDATA, child,
                                   (void *)(addr + i), NULL);
                if (word == -1 && errno != 0) {
                        buf[i] = '\0';
                        break;
                }
                memcpy(buf + i, &word, sizeof(long));
                for (size_t j = 0; j < sizeof(long); j++) {
                        if (buf[i + j] == '\0')
                                return buf;
                }
                i += sizeof(long);
        }
        buf[PATH_MAX - 1] = '\0';
        return buf;
}

/* Write a string into the child's address space. */
static int write_child_string(pid_t child, unsigned long addr,
                              const char *str)
{
        size_t len = strlen(str) + 1;
        for (size_t i = 0; i < len; i += sizeof(long)) {
                long word = 0;
                size_t chunk = sizeof(long);
                if (len - i < chunk) chunk = len - i;
                memcpy(&word, str + i, chunk);
                if (ptrace(PTRACE_POKEDATA, child,
                           (void *)(addr + i), (void *)word) == -1)
                        return -1;
        }
        return 0;
}

/* Helper: mkdir -p */
static int mkdirs(const char *path)
{
        char tmp[PATH_MAX];
        char *p = NULL;
        snprintf(tmp, sizeof(tmp), "%s", path);
        size_t len = strlen(tmp);
        if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
        for (p = tmp + 1; *p; p++) {
                if (*p == '/') {
                        *p = '\0';
                        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
                        *p = '/';
                }
        }
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
        return 0;
}

/* Compute redirected path for writes. */
static char *redirect_path(const char *original, const char *redirect_dir)
{
        if (!redirect_dir || !original) return NULL;
        if (strncmp(original, redirect_dir, strlen(redirect_dir)) == 0)
                return strdup(original);

        char *result = malloc(PATH_MAX);
        if (!result) return NULL;

        if (original[0] == '/') {
                snprintf(result, PATH_MAX, "%s%s", redirect_dir, original);
        } else {
                char cwd[PATH_MAX];
                if (getcwd(cwd, sizeof(cwd)))
                        snprintf(result, PATH_MAX, "%s%s/%s",
                                 redirect_dir, cwd, original);
                else
                        snprintf(result, PATH_MAX, "%s/%s",
                                 redirect_dir, original);
        }
        return result;
}

/* ── Main ptrace loop ────────────────────────────────────────────── */

trak_result_t *trakker_run(const char **argv, size_t argc,
                           const trakker_policy_t *policy)
{
        if (!argv || argc == 0) return NULL;

        trak_result_t *result = calloc(1, sizeof(*result));
        if (!result) return NULL;

        /* Build command string */
        size_t cmd_len = 0;
        for (size_t i = 0; i < argc; i++)
                cmd_len += strlen(argv[i]) + 1;
        result->command = malloc(cmd_len + 1);
        result->command[0] = '\0';
        for (size_t i = 0; i < argc; i++) {
                if (i > 0) strcat(result->command, " ");
                strcat(result->command, argv[i]);
        }

        struct timeval start, end;
        gettimeofday(&start, NULL);

        pid_t child = fork();
        if (child < 0) {
                free(result->command);
                free(result);
                return NULL;
        }

        if (child == 0) {
                /* Child: request tracing, stop, then exec */
                ptrace(PTRACE_TRACEME, 0, NULL, NULL);
                kill(getpid(), SIGSTOP);

                char **exec_argv = malloc((argc + 1) * sizeof(char *));
                for (size_t i = 0; i < argc; i++)
                        exec_argv[i] = (char *)argv[i];
                exec_argv[argc] = NULL;

                execvp(argv[0], exec_argv);
                _exit(127);
        }

        /* Parent: wait for initial SIGSTOP */
        int status;
        waitpid(child, &status, 0);
        if (!WIFSTOPPED(status)) {
                result->exit_code = -1;
                return result;
        }

        /* Set ptrace options */
        ptrace(PTRACE_SETOPTIONS, child, NULL,
               (void *)(long)(PTRACE_O_TRACESYSGOOD |
                        PTRACE_O_TRACECLONE |
                        PTRACE_O_TRACEFORK |
                        PTRACE_O_TRACEVFORK));

        /* Resume child, start syscall tracing */
        ptrace(PTRACE_SYSCALL, child, NULL, NULL);

        /* Per-pid in_syscall tracking */
        #define PID_MAP_SIZE 4096
        int in_syscall_arr[PID_MAP_SIZE];
        memset(in_syscall_arr, 0, sizeof(in_syscall_arr));

        /* Track all traced children */
        pid_t *children = malloc(64 * sizeof(pid_t));
        size_t child_count = 0;
        size_t child_cap = 64;
        children[child_count++] = child;

        while (1) {
                pid_t pid = waitpid(-1, &status, __WALL);
                if (pid < 0) {
                        if (errno == ECHILD) break;
                        continue;
                }

                if (WIFEXITED(status)) {
                        int exit_code = WEXITSTATUS(status);
                        trak_event_t *e = event_new(TRAK_PROC_EXIT);
                        if (e) { e->pid = pid; e->exit_code = exit_code; event_append(result, e); }
                        if (pid == child) result->exit_code = exit_code;

                        for (size_t i = 0; i < child_count; i++) {
                                if (children[i] == pid) {
                                        children[i] = children[--child_count];
                                        break;
                                }
                        }
                        continue;
                }

                if (WIFSIGNALED(status)) {
                        if (pid == child) result->exit_code = -WTERMSIG(status);
                        for (size_t i = 0; i < child_count; i++) {
                                if (children[i] == pid) {
                                        children[i] = children[--child_count];
                                        break;
                                }
                        }
                        continue;
                }

                if (!WIFSTOPPED(status)) continue;

                int sig = WSTOPSIG(status);

                /* ptrace events (clone/fork/vfork) */
                unsigned int event = (status >> 16) & 0xff;
                if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
                    event == PTRACE_EVENT_VFORK) {
                        unsigned long new_pid;
                        ptrace(PTRACE_GETEVENTMSG, pid, NULL, &new_pid);
                        trak_event_t *e = event_new(TRAK_PROC_FORK);
                        if (e) { e->pid = (int)new_pid; e->command = strdup("(fork)"); event_append(result, e); }
                        if (child_count >= child_cap) {
                                child_cap *= 2;
                                children = realloc(children, child_cap * sizeof(pid_t));
                        }
                        children[child_count++] = (pid_t)new_pid;
                        ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
                        continue;
                }

                /* PTRACE_EVENT_EXEC */
                if (event == PTRACE_EVENT_EXEC) {
                        ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
                        /* Reset in_syscall for this pid since exec resets it */
                        in_syscall_arr[pid % PID_MAP_SIZE] = 0;
                        continue;
                }

                /* Plain SIGTRAP from exec - suppress */
                if (sig == SIGTRAP) {
                        ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
                        continue;
                }

                /* Not a syscall stop - deliver signal */
                if (sig != (SIGTRAP | 0x80)) {
                        ptrace(PTRACE_SYSCALL, pid, NULL, (void *)(long)sig);
                        continue;
                }

                /* Syscall stop - read registers */
                struct user_regs_struct regs;
                if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) < 0) {
                        ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
                        continue;
                }

                int idx = pid % PID_MAP_SIZE;
                if (!in_syscall_arr[idx]) {
                        /* ── SYSCALL ENTRY ── */
                        in_syscall_arr[idx] = 1;
                        long syscall_nr = regs.orig_rax;

                        switch (syscall_nr) {
                        case SYS_open:
                        case SYS_openat: {
                                unsigned long path_addr = (syscall_nr == SYS_open)
                                        ? regs.rdi : regs.rsi;
                                unsigned long flags = (syscall_nr == SYS_open)
                                        ? regs.rsi : regs.rdx;

                                char *path = read_child_string(pid, path_addr);
                                if (!path) break;

                                int is_write = (flags & (O_WRONLY | O_RDWR | O_CREAT |
                                                         O_TRUNC | O_APPEND)) != 0;

                                if (is_write) {
                                        if (policy && policy->no_write) {
                                                /* Block: inject -EPERM on exit */
                                                /* We'll set it on the next stop */
                                        } else if (policy && policy->redirect_writes) {
                                                char *new_path = redirect_path(path,
                                                        policy->redirect_writes);
                                                if (new_path) {
                                                        char parent[PATH_MAX];
                                                        snprintf(parent, sizeof(parent), "%s", new_path);
                                                        char *sl = strrchr(parent, '/');
                                                        if (sl) { *sl = '\0'; mkdirs(parent); }
                                                        write_child_string(pid, path_addr, new_path);
                                                        free(new_path);
                                                }
                                        }
                                        trak_event_t *e = event_new(TRAK_FILE_WRITE);
                                        if (e) { e->path = path; event_append(result, e); }
                                        else free(path);
                                } else {
                                        trak_event_t *e = event_new(TRAK_FILE_READ);
                                        if (e) { e->path = path; event_append(result, e); }
                                        else free(path);
                                }
                                break;
                        }

                        case SYS_unlink:
                        case SYS_unlinkat: {
                                unsigned long path_addr = (syscall_nr == SYS_unlink)
                                        ? regs.rdi : regs.rsi;
                                char *path = read_child_string(pid, path_addr);
                                trak_event_t *e = event_new(TRAK_FILE_DELETE);
                                if (e) { e->path = path ? path : strdup("(?)"); event_append(result, e); }
                                else free(path);
                                break;
                        }

                        case SYS_socket: {
                                if (policy && policy->no_net) {
                                        /* Block socket creation */
                                        regs.orig_rax = -1;  /* invalid syscall */
                                        ptrace(PTRACE_SETREGS, pid, NULL, &regs);
                                        trak_event_t *e = event_new(TRAK_NET_BLOCKED);
                                        if (e) { e->address = strdup("socket()"); event_append(result, e); }
                                }
                                break;
                        }

                        case SYS_connect: {
                                if (policy && policy->no_net) {
                                        regs.orig_rax = -1;
                                        ptrace(PTRACE_SETREGS, pid, NULL, &regs);
                                        trak_event_t *e = event_new(TRAK_NET_BLOCKED);
                                        if (e) { e->address = strdup("connect()"); event_append(result, e); }
                                } else {
                                        trak_event_t *e = event_new(TRAK_NET_CONNECT);
                                        if (e) { e->address = strdup("(connect)"); event_append(result, e); }
                                }
                                break;
                        }

                        case SYS_execve: {
                                char *path = read_child_string(pid, regs.rdi);
                                trak_event_t *e = event_new(TRAK_PROC_EXEC);
                                if (e) { e->command = path ? path : strdup("(?)"); e->pid = pid; event_append(result, e); }
                                else free(path);
                                break;
                        }

                        default:
                                break;
                        }
                } else {
                        /* ── SYSCALL EXIT ── */
                        in_syscall_arr[idx] = 0;

                        /* If we blocked a syscall by setting orig_rax to -1,
                         * set the return value to -EPERM */
                        if ((long)regs.rax == -1 && regs.orig_rax == (unsigned long)-1) {
                                struct user_regs_struct exit_regs;
                                if (ptrace(PTRACE_GETREGS, pid, NULL, &exit_regs) == 0) {
                                        if ((long)exit_regs.rax == -ENOSYS) {
                                                exit_regs.rax = (unsigned long)-EPERM;
                                                ptrace(PTRACE_SETREGS, pid, NULL, &exit_regs);
                                        }
                                }
                        }
                }

                ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
        }

        gettimeofday(&end, NULL);
        result->duration_ms = ((end.tv_sec - start.tv_sec) * 1000 +
                               (end.tv_usec - start.tv_usec) / 1000);

        free(children);
        return result;
}

/* ── JSON output ─────────────────────────────────────────────────── */

int trakker_result_write_json(const trak_result_t *result, FILE *out)
{
        if (!result || !out) return -1;

        fprintf(out, "{\n");

        /* Command (escaped) */
        fprintf(out, "  \"command\": \"");
        if (result->command) {
                for (const char *p = result->command; *p; p++) {
                        switch (*p) {
                        case '"':  fprintf(out, "\\\""); break;
                        case '\\': fprintf(out, "\\\\"); break;
                        case '\n': fprintf(out, "\\n"); break;
                        case '\t': fprintf(out, "\\t"); break;
                        default:   fputc(*p, out); break;
                        }
                }
        }
        fprintf(out, "\",\n");
        fprintf(out, "  \"exit_code\": %d,\n", result->exit_code);
        fprintf(out, "  \"duration_ms\": %lu,\n", result->duration_ms);

        /* Collect event counts */
        size_t reads = 0, writes = 0, creates = 0, deletes = 0;
        size_t connects = 0, blocked = 0, forks = 0, execs = 0;
        for (trak_event_t *e = result->events; e; e = e->next) {
                switch (e->type) {
                case TRAK_FILE_READ:   reads++; break;
                case TRAK_FILE_WRITE:  writes++; break;
                case TRAK_FILE_CREATE: creates++; break;
                case TRAK_FILE_DELETE: deletes++; break;
                case TRAK_NET_CONNECT: connects++; break;
                case TRAK_NET_BLOCKED: blocked++; break;
                case TRAK_PROC_FORK:   forks++; break;
                case TRAK_PROC_EXEC:   execs++; break;
                default: break;
                }
        }

        fprintf(out, "  \"files\": {\n");
        fprintf(out, "    \"read\": [");
        int first = 1;
        for (trak_event_t *e = result->events; e; e = e->next) {
                if (e->type == TRAK_FILE_READ) {
                        if (!first) fprintf(out, ", ");
                        first = 0;
                        fprintf(out, "\"%s\"", e->path ? e->path : "?");
                }
        }
        fprintf(out, "],\n    \"write\": [");
        first = 1;
        for (trak_event_t *e = result->events; e; e = e->next) {
                if (e->type == TRAK_FILE_WRITE) {
                        if (!first) fprintf(out, ", ");
                        first = 0;
                        fprintf(out, "\"%s\"", e->path ? e->path : "?");
                }
        }
        fprintf(out, "],\n    \"create\": [");
        first = 1;
        for (trak_event_t *e = result->events; e; e = e->next) {
                if (e->type == TRAK_FILE_CREATE) {
                        if (!first) fprintf(out, ", ");
                        first = 0;
                        fprintf(out, "\"%s\"", e->path ? e->path : "?");
                }
        }
        fprintf(out, "],\n    \"delete\": [");
        first = 1;
        for (trak_event_t *e = result->events; e; e = e->next) {
                if (e->type == TRAK_FILE_DELETE) {
                        if (!first) fprintf(out, ", ");
                        first = 0;
                        fprintf(out, "\"%s\"", e->path ? e->path : "?");
                }
        }
        fprintf(out, "]\n  },\n");

        fprintf(out, "  \"network\": {\n    \"allowed\": [");
        first = 1;
        for (trak_event_t *e = result->events; e; e = e->next) {
                if (e->type == TRAK_NET_CONNECT) {
                        if (!first) fprintf(out, ", ");
                        first = 0;
                        fprintf(out, "\"%s\"", e->address ? e->address : "?");
                }
        }
        fprintf(out, "],\n    \"blocked\": [");
        first = 1;
        for (trak_event_t *e = result->events; e; e = e->next) {
                if (e->type == TRAK_NET_BLOCKED) {
                        if (!first) fprintf(out, ", ");
                        first = 0;
                        fprintf(out, "\"%s\"", e->address ? e->address : "?");
                }
        }
        fprintf(out, "]\n  },\n");

        fprintf(out, "  \"processes\": {\n    \"forked\": [");
        first = 1;
        for (trak_event_t *e = result->events; e; e = e->next) {
                if (e->type == TRAK_PROC_FORK) {
                        if (!first) fprintf(out, ", ");
                        first = 0;
                        fprintf(out, "{\"pid\": %d}", e->pid);
                }
        }
        fprintf(out, "],\n    \"exec\": [");
        first = 1;
        for (trak_event_t *e = result->events; e; e = e->next) {
                if (e->type == TRAK_PROC_EXEC) {
                        if (!first) fprintf(out, ", ");
                        first = 0;
                        fprintf(out, "\"%s\"", e->command ? e->command : "?");
                }
        }
        fprintf(out, "]\n  }\n}\n");

        return 0;
}

/* ── Free ────────────────────────────────────────────────────────── */

static void trakker_event_free(trak_event_t *e)
{
        while (e) {
                trak_event_t *next = e->next;
                free(e->path);
                free(e->address);
                free(e->command);
                free(e);
                e = next;
        }
}

void trakker_result_free(trak_result_t *result)
{
        if (!result) return;
        free(result->command);
        trakker_event_free(result->events);
        free(result);
}

void trakker_policy_free(trakker_policy_t *policy)
{
        if (!policy) return;
        free(policy->redirect_writes);
        if (policy->allow_net_ports) {
                for (size_t i = 0; i < policy->allow_net_count; i++)
                        free(policy->allow_net_ports[i]);
                free(policy->allow_net_ports);
        }
}
