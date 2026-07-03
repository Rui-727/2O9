/* trakker.h - 2O9 execution sandbox and trace recorder
 *
 * Trakker runs a command inside 2O9's sandbox, recording everything
 * it does and optionally restricting what it's allowed to do.
 *
 * From DESIGN.md §7:
 * - Records: file I/O, network, process, memory (mmap summary)
 * - Restricts: --no-net, --redirect-writes <dir>, --no-write,
 *     --allow-net port=443
 * - Uses ptrace to intercept syscalls
 * - Output: JSON trace log
 *
 * Usage: 209 <pkg> trakker [--no-net] [--redirect-writes /tmp/trakker] \
 *                           [--no-write] [--allow-net port=443] <command>
 */

#ifndef TWO9_TRAKKER_H
#define TWO9_TRAKKER_H

#include <stddef.h>

/* ── Restriction flags ───────────────────────────────────────────── */

typedef struct trakker_policy {
        int no_net;              /* Block all network access */
        int no_write;            /* Block all file writes */
        char *redirect_writes;   /* Redirect writes to this directory (or NULL) */
        char **allow_net_ports;  /* NULL-terminated list of allowed port strings */
        size_t allow_net_count;  /* Number of allowed ports */
} trakker_policy_t;

/* ── Trace records ───────────────────────────────────────────────── */

typedef enum {
        TRAK_FILE_READ,
        TRAK_FILE_WRITE,
        TRAK_FILE_CREATE,
        TRAK_FILE_DELETE,
        TRAK_NET_CONNECT,
        TRAK_NET_BLOCKED,
        TRAK_PROC_FORK,
        TRAK_PROC_EXEC,
        TRAK_PROC_EXIT,
        TRAK_MEM_MMAP,
} trak_event_type_t;

typedef struct trak_event {
        trak_event_type_t type;
        char *path;            /* For file events: file path */
        char *address;         /* For network events: IP address */
        int port;              /* For network events: port number */
        char *command;         /* For process events: command string */
        int pid;               /* For process events: PID */
        int exit_code;         /* For exit events */
        struct trak_event *next;
} trak_event_t;

/* ── Trace result ────────────────────────────────────────────────── */

typedef struct trak_result {
        char *command;         /* The command that was run */
        int exit_code;         /* Exit code of the traced process */
        unsigned long duration_ms; /* Wall-clock duration */
        trak_event_t *events;  /* Linked list of recorded events */
        size_t event_count;
} trak_result_t;

/* ── API ─────────────────────────────────────────────────────────── */

/* Run a command under trakker with the given policy.
 * Returns a trace result, or NULL on failure.
 * Caller must free with trakker_result_free(). */
trak_result_t *trakker_run(const char **argv, size_t argc,
                           const trakker_policy_t *policy);

/* Free a trace result and all its events */
void trakker_result_free(trak_result_t *result);

/* Write a trace result as JSON to a file.
 * Returns 0 on success, -1 on failure. */
int trakker_result_write_json(const trak_result_t *result, FILE *out);

/* Free a policy and its strings */
void trakker_policy_free(trakker_policy_t *policy);

#endif /* TWO9_TRAKKER_H */
