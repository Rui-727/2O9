/* nix-store --add subprocess spike
 *
 * This is a Phase 0 proof-of-concept: a C program that spawns
 * `nix-store --add <path>` and captures the resulting store path.
 * This is the pattern the store adapter (src/store/) will use.
 *
 * Build:  cc -o spike-nix-add spike-nix-add.c
 * Usage:  ./spike-nix-add <file-to-add>
 *
 * Exit codes:
 *   0  - nix-store succeeded, store path printed to stdout
 *   1  - usage error
 *   2  - nix-store not found
 *   3  - nix-store failed
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <errno.h>

/* 2O9: we never link against libnixstore. The nix toolchain is
 * orchestrated as a subprocess. This is a design constraint from
 * DESIGN.md §2 decision 2 and §9 dependency table.
 *
 * Why posix_spawn over fork/exec:
 *  - Same performance on Linux (vfork under the hood)
 *  - Cleaner error handling
 *  - We don't need to manipulate FDs between fork and exec
 */

extern char **environ;

static int spawn_nix_store_add(const char *path, char **store_path_out)
{
	int pipefd[2];
	if (pipe(pipefd) < 0) {
		perror("pipe");
		return -1;
	}

	/* nix-store --add prints the store path to stdout.
	 * We redirect stdout into our pipe to capture it. */
	pid_t pid;
	char *argv[] = { "nix-store", "--add", (char *)path, NULL };

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, pipefd[0]);
	posix_spawn_file_actions_addclose(&actions, pipefd[1]);

	int ret = posix_spawnp(&pid, "nix-store", &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);

	if (ret != 0) {
		fprintf(stderr, "spike: posix_spawnp: %s\n", strerror(ret));
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}

	/* Close write end in parent */
	close(pipefd[1]);

	/* Read the store path from nix-store's stdout */
	char buf[4096] = {0};
	ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
	close(pipefd[0]);

	int status;
	waitpid(pid, &status, 0);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "spike: nix-store exited %d\n",
		        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		return -1;
	}

	if (n <= 0) {
		fprintf(stderr, "spike: nix-store produced no output\n");
		return -1;
	}

	/* Trim trailing newline */
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	*store_path_out = strdup(buf);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <file-to-add>\n", argv[0]);
		return 1;
	}

	const char *path = argv[1];

	/* Verify the file exists before spawning */
	if (access(path, R_OK) != 0) {
		fprintf(stderr, "spike: cannot read %s: %s\n", path, strerror(errno));
		return 1;
	}

	char *store_path = NULL;
	if (spawn_nix_store_add(path, &store_path) < 0) {
		/* If nix-store is not found, report clearly */
		if (errno == ENOENT || access("/usr/bin/nix-store", X_OK) != 0) {
			fprintf(stderr, "spike: nix-store not found - install nix first\n");
			return 2;
		}
		fprintf(stderr, "spike: nix-store --add failed\n");
		return 3;
	}

	printf("%s\n", store_path);
	free(store_path);
	return 0;
}
