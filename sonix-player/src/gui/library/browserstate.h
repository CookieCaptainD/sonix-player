#ifndef BROWSERSTATE_H
#define BROWSERSTATE_H

#include <stdbool.h>
#include <stddef.h>

// The browser's folder and scroll offset are kept beside the settings file in
// a small binary of their own, not in the INI: a card path can be longer than
// a settings value (capped at 128 bytes) and can contain bytes an INI line
// cannot carry.
bool browser_state_save(const char *file, const char *folder, int scroll);
bool browser_state_load(const char *file, char *folder, size_t size, int *scroll);

// Only an existing directory inside the card is accepted, after realpath, so a
// ".." or a symlink cannot walk the browser off it. The answer is rebased onto
// the configured spelling of the root, which is what the browser navigates by.
bool browser_state_directory(const char *root, const char *folder, char *out, size_t size);

// Chooses the folder the browser opens: the saved one, then the local track's
// parent when the track still exists, then <root>/Music, then the root. The
// scroll is returned only for the saved folder.
bool browser_state_choose(const char *root, const char *saved, int saved_scroll, const char *track, char *out, size_t size, int *scroll);

#endif /* BROWSERSTATE_H */
