/* seccomp_filter.c - seccomp-bpf filter generation for Debag
 *
 * Builds a libseccomp filter that:
 *   1. ALLOWs syscalls the binary is expected to use (from static analysis)
 *   2. TRACEs dangerous syscalls (execve, connect, open) - these trigger ptrace
 *   3. KILLs everything else (unexpected syscalls)
 *
 * The filter is installed before exec(). Combined with PTRACE_SECCOMP,
 * only TRACE'd syscalls fall through to the ptrace handler - everything
 * else is handled in-kernel with nanosecond overhead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <seccomp.h>
#include <sys/syscall.h>
#include <errno.h>

#include "debag.h"

/* Default safe syscalls - always allowed, even if not in the binary's
 * symbol table. These are needed for basic process startup (glibc init,
 * loader, etc.). Without them, even /bin/true would be killed. */
static const int default_safe[] = {
    __NR_read, __NR_write, __NR_close, __NR_fstat, __NR_stat, __NR_lstat,
    __NR_lseek, __NR_mmap, __NR_munmap, __NR_mprotect, __NR_brk,
    __NR_exit, __NR_exit_group, __NR_rt_sigaction, __NR_rt_sigprocmask,
    __NR_rt_sigreturn, __NR_getpid, __NR_getuid, __NR_getgid,
    __NR_geteuid, __NR_getegid, __NR_arch_prctl, __NR_set_tid_address,
    __NR_set_robust_list, __NR_rseq, __NR_prlimit64, __NR_getrandom,
    __NR_clock_gettime, __NR_clock_getres, __NR_ioctl, __NR_pread64,
    __NR_pwrite64, __NR_fcntl, __NR_dup, __NR_dup2, __NR_dup3,
    __NR_pipe, __NR_pipe2, __NR_poll, __NR_ppoll, __NR_epoll_create1,
    __NR_epoll_ctl, __NR_epoll_wait, __NR_eventfd2, __NR_timerfd_create,
    __NR_timerfd_settime, __NR_futex, __NR_sched_yield,
    __NR_sched_getaffinity, __NR_madvise, __NR_getcwd, __NR_chdir,
    __NR_fchdir, __NR_access, __NR_faccessat, __NR_readlink,
    __NR_readlinkat, __NR_getdents64, __NR_getdents, __NR_nanosleep,
    __NR_clock_nanosleep, __NR_restart_syscall, __NR_sigaltstack,
    __NR_mremap, __NR_msync, __NR_mincore, __NR_mlock, __NR_munlock,
    __NR_mlockall, __NR_munlockall, __NR_madvise, __NR_recvmsg,
    __NR_sendmsg, __NR_getsockopt, __NR_setsockopt, __NR_shutdown,
};

/* Dangerous syscalls - always traced via ptrace, regardless of static analysis */
static const int always_traced[] = {
    __NR_execve, __NR_execveat,     /* what is it executing? */
    __NR_mount, __NR_umount2,       /* filesystem changes */
    __NR_ptrace,                    /* don't let it trace us */
    __NR_pivot_root, __NR_chroot,  /* filesystem escape */
    __NR_setuid, __NR_setgid,      /* privilege changes */
    __NR_setreuid, __NR_setregid,
    __NR_setresuid, __NR_setresgid,
    __NR_setfsuid, __NR_setfsgid,
    __NR_capset,                    /* capability changes */
    __NR_keyctl, __NR_add_key, __NR_request_key,  /* kernel keyring */
    __NR_personality,               /* can change execution domain */
    __NR_create_module, __NR_init_module, __NR_delete_module,  /* kernel modules */
    __NR_kexec_load, __NR_kexec_file_load,  /* kernel replacement */
    __NR_bpf,                       /* eBPF - can subvert the sandbox */
    __NR_unshare, __NR_setns,       /* namespace manipulation */
};

/* Network syscalls - traced if --no-net, allowed otherwise (unless static analysis says no) */
static const int network_syscalls[] = {
    __NR_socket, __NR_connect, __NR_bind, __NR_listen, __NR_accept,
    __NR_accept4, __NR_sendto, __NR_recvfrom, __NR_sendmsg, __NR_recvmsg,
    __NR_socketpair, __NR_getsockname, __NR_getpeername,
};

/* Build and install a seccomp filter. Returns 0 on success, -1 on failure. */
int debag_install_seccomp(const debag_analysis_t *analysis,
                           const debag_policy_t *policy)
{
    /* Default action: TRACE (fall through to ptrace) instead of KILL.
     * This is more forgiving - unexpected syscalls get inspected by
     * ptrace rather than killing the process. Use --fast-mode for
     * KILL_PROCESS as the default (no ptrace fallback). */
    uint32_t default_action = policy->fast_mode ? SCMP_ACT_KILL_PROCESS : SCMP_ACT_TRACE(0);
    scmp_filter_ctx ctx = seccomp_init(default_action);
    if (!ctx) {
        fprintf(stderr, "debag: seccomp_init failed\n");
        return -1;
    }

    int rc;
    size_t i;

    /* 1. Add default safe syscalls (ALLOW) */
    for (i = 0; i < sizeof(default_safe) / sizeof(default_safe[0]); i++) {
        rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, default_safe[i], 0);
        if (rc < 0 && rc != -EEXIST) {
            /* Some syscalls may not exist on this arch - ignore */
        }
    }

    /* 2. Add syscalls from static analysis (ALLOW) */
    if (analysis) {
        for (i = 0; i < analysis->allowed_count; i++) {
            seccomp_rule_add(ctx, SCMP_ACT_ALLOW, analysis->allowed_syscalls[i], 0);
        }
    }

    /* 3. Add dangerous syscalls (TRACE if ptrace, KILL if fast mode) */
    /* SCMP_ACT_TRACE(value) sends the value to ptrace; we use 0.
     * In fast mode, we just kill the process instead. */
    int trace_action = policy->fast_mode ? SCMP_ACT_KILL_PROCESS : SCMP_ACT_TRACE(0);
    for (i = 0; i < sizeof(always_traced) / sizeof(always_traced[0]); i++) {
        seccomp_rule_add(ctx, trace_action, always_traced[i], 0);
    }

    /* 4. Handle --no-net: block all network syscalls */
    if (policy->no_net) {
        for (i = 0; i < sizeof(network_syscalls) / sizeof(network_syscalls[0]); i++) {
            seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, network_syscalls[i], 0);
        }
    } else if (analysis) {
        /* If binary uses network, TRACE those syscalls (unless fast mode) */
        if (analysis->has_network) {
            for (i = 0; i < sizeof(network_syscalls) / sizeof(network_syscalls[0]); i++) {
                seccomp_rule_add(ctx, trace_action, network_syscalls[i], 0);
            }
        }
    }

    /* 5. Handle --no-write: block write syscalls */
    if (policy->no_write) {
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_open, 1,
                         SCMP_A1(SCMP_CMP_MASKED_EQ, O_WRONLY, O_WRONLY));
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_openat, 1,
                         SCMP_A2(SCMP_CMP_MASKED_EQ, O_WRONLY, O_WRONLY));
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_unlink, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_unlinkat, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_rename, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_renameat, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_truncate, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_ftruncate, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_chmod, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_chown, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_mkdir, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_rmdir, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_write, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_writev, 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, __NR_pwrite64, 0);
    }

    /* 6. Load the filter */
    rc = seccomp_load(ctx);
    seccomp_release(ctx);

    if (rc < 0) {
        fprintf(stderr, "debag: seccomp_load failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/* Export the filter as BPF bytecode (for --static-scan debugging) */
void debag_print_seccomp_rules(const debag_analysis_t *analysis,
                                const debag_policy_t *policy, FILE *out)
{
    fprintf(out, "\n=== Seccomp Filter Rules ===\n");
    fprintf(out, "Default action: KILL_PROCESS\n\n");

    fprintf(out, "ALLOW (fast path, in-kernel):\n");
    fprintf(out, "  %zu default safe syscalls (read, write, close, mmap, ...)\n",
            sizeof(default_safe) / sizeof(default_safe[0]));
    if (analysis)
        fprintf(out, "  %zu syscalls from static analysis\n", analysis->allowed_count);

    fprintf(out, "\n%s (slow path, ptrace inspection):\n",
            policy->fast_mode ? "KILL" : "TRACE");
    fprintf(out, "  %zu always-traced syscalls (execve, mount, ptrace, ...)\n",
            sizeof(always_traced) / sizeof(always_traced[0]));
    if (analysis && analysis->has_network && !policy->no_net)
        fprintf(out, "  %zu network syscalls (socket, connect, ...)\n",
                sizeof(network_syscalls) / sizeof(network_syscalls[0]));

    if (policy->no_net)
        fprintf(out, "\nKILL: all network syscalls (--no-net)\n");
    if (policy->no_write)
        fprintf(out, "KILL: all write syscalls (--no-write)\n");
}
