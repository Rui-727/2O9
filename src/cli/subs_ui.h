/* subs_ui.h - interactive subscription picker
 *
 * A small TUI for browsing configured subs (extra.nix `subs` block)
 * and their contents (fetched from each sub's index.json). Uses raw
 * termios, no ncurses. Arrow keys + Enter + q.
 *
 * Three views, stacked:
 *   1. Sub list  - all configured subs. Enter opens a sub.
 *   2. Sub view  - one sub's details + its contents (from index.json).
 *                  Enter on a row fetches and prints the item's narinfo.
 *                  b goes back to the sub list.
 *   3. Item view - the narinfo text for one item. Any key returns to
 *                  the sub view.
 *
 * If stdin/stdout is not a TTY, falls back to non-interactive: prints
 * the sub list and exits.
 */
#ifndef TWO9_SUBS_UI_H
#define TWO9_SUBS_UI_H

/* Launch the interactive subs picker. Returns 0 on clean exit, 1 on
 * error. If stdin or stdout is not a TTY, prints the sub list to
 * stdout and returns 0. */
int subs_ui_run(void);

/* Print details for one sub to stdout, non-interactively. Fetches
 * index.json from each URL in the sub and lists the items. Returns
 * 0 on success (sub found and printed), 1 if the sub doesn't exist. */
int subs_ui_print_sub(const char *name);

#endif /* TWO9_SUBS_UI_H */
