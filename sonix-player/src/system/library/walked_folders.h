#ifndef SONIX_LIBRARY_WALKED_FOLDERS_H
#define SONIX_LIBRARY_WALKED_FOLDERS_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// Whether a media path lies in what the library walk actually covered.
// Parameterised so it mirrors the walk's rules without depending on library.c
// globals, and so a host test can include just this header.
//
// The walk skips dot-prefixed and junk names at every depth, and at the card
// root also skips the audiobook/podcast folders and (with a filter active)
// unchosen folders. A path in one of those branches is no more "walked" than
// a path outside the root, even though it sits under it. `is_junk` stands in
// for playlist_is_junk_name(); folder comparisons are case-blind, as the card
// is, matching the root-prefix comparison in the original check.
static inline bool library_path_in_walk(const char *path, const char *root, const char *const *folders,
										int folder_count, bool (*is_junk)(const char *name),
										const char *audiobooks_folder, const char *podcasts_folder) {
	if (!path || !root) {
		return false;
	}
	size_t root_len = strlen(root);
	if (strncmp(path, root, root_len) != 0 || path[root_len] != '/') {
		return false;
	}

	const char *relative = path + root_len + 1;
	size_t rel_len = strlen(relative);
	// The walk builds each child in a fixed 512-byte buffer and skips paths
	// that do not fit; 2048 leaves room for cue-track suffixes and needs no
	// allocation. Anything longer cannot be an indexed file either.
	char buf[2048];
	if (rel_len == 0 || rel_len >= sizeof(buf)) {
		return false;
	}
	memcpy(buf, relative, rel_len + 1);

	char *component = buf;
	for (;;) {
		char *slash = strchr(component, '/');
		bool last = (slash == NULL);
		if (!last) {
			*slash = '\0';
		}

		// A file directly in the root is a component too: hidden files such
		// as ._Song.flac are skipped by the scan just like hidden folders.
		if (component[0] == '\0' || component[0] == '.' || (is_junk && is_junk(component))) {
			return false;
		}

		if (component == buf) { // depth 0
			// These are skipped only as root folders; the same name deeper
			// down is an ordinary folder the walk does enter.
			if (!last && ((audiobooks_folder && strcasecmp(component, audiobooks_folder) == 0) ||
						  (podcasts_folder && strcasecmp(component, podcasts_folder) == 0))) {
				return false;
			}
			if (folder_count > 0) {
				if (last) {
					return false; // loose files at the root belong to no chosen folder
				}
				bool chosen = false;
				for (int i = 0; i < folder_count; i++) {
					if (folders && folders[i] && strcasecmp(component, folders[i]) == 0) {
						chosen = true;
						break;
					}
				}
				if (!chosen) {
					return false;
				}
			}
		}

		if (last) {
			break;
		}
		component = slash + 1;
	}
	return true;
}

#endif
