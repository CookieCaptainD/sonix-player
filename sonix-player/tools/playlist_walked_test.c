// Host test for library_path_in_walk() -- GitHub issue #180.
//
// Includes ONLY the header under test: the walk predicate must stand on its
// own, with no dependency on library.c globals.
//
// Build from sonix-player/ with -I. and C11; no other headers are needed.
#include "src/system/library/walked_folders.h"

// Tiny local stand-in for playlist_is_junk_name(): the AppleDouble sidecars
// and the desktop junk list. The leading-dot rule belongs to the walk itself.
static bool test_is_junk(const char *name) {
	if (!name || !name[0]) {
		return true;
	}
	if (name[0] == '.' && name[1] == '_') {
		return true;
	}
	static const char *const JUNK[] = {".DS_Store", "__MACOSX", ".Spotlight-V100", ".Trashes",
									   ".fseventsd", "$RECYCLE.BIN", "System Volume Information"};
	for (size_t i = 0; i < sizeof(JUNK) / sizeof(JUNK[0]); i++) {
		if (strcasecmp(name, JUNK[i]) == 0) {
			return true;
		}
	}
	return false;
}

static int checks;
static int failures;

static void check(const char *name, bool got, bool expected) {
	checks++;
	if (got == expected) {
		printf("ok   - %s\n", name);
	} else {
		failures++;
		printf("FAIL - %s (got %s, expected %s)\n", name, got ? "true" : "false",
			   expected ? "true" : "false");
	}
}

int main(void) {
	const char *root = "/mnt/sdcard";
	const char *chosen[] = {"Music", "FLAC"};

	// Whole-card scan (the default): no filter, root "/" + relative path.
	check("whole-card: plain file directly in root",
		  library_path_in_walk("/mnt/sdcard/Song.flac", root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"), true);
	check("whole-card: normal nested album file",
		  library_path_in_walk("/mnt/sdcard/Music/Album/Song.flac", root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"),
		  true);

	// The bug: dot-prefixed entries are skipped by the walk at every depth.
	check("dot folder .basstest at depth 0 is not walked",
		  library_path_in_walk("/mnt/sdcard/.basstest/Song.flac", root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"),
		  false);
	check("dot file .hidden.flac at depth 0 is not walked",
		  library_path_in_walk("/mnt/sdcard/.hidden.flac", root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"),
		  false);
	check("dot folder nested below a chosen folder is not walked",
		  library_path_in_walk("/mnt/sdcard/Music/.basstest/Song.flac", root, chosen, 2, test_is_junk, "Audiobooks",
							   "Podcast"),
		  false);
	check("junk folder __MACOSX at depth 0 is not walked",
		  library_path_in_walk("/mnt/sdcard/__MACOSX/Song.flac", root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"),
		  false);
	check("junk file .DS_Store at depth 0 is not walked",
		  library_path_in_walk("/mnt/sdcard/.DS_Store", root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"), false);
	check("AppleDouble ._Song.flac is not walked",
		  library_path_in_walk("/mnt/sdcard/Music/._Song.flac", root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"),
		  false);

	// Depth-0-only rules: special folders are skipped at the root, but the
	// same name under a folder is an ordinary folder the walk does enter.
	check("special folder Audiobooks at depth 0 is not walked",
		  library_path_in_walk("/mnt/sdcard/Audiobooks/Book.flac", root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"),
		  false);
	check("special folder Podcast at depth 0 is not walked",
		  library_path_in_walk("/mnt/sdcard/Podcast/Episode.flac", root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"),
		  false);
	check("Audiobooks at depth 1 under chosen Music is walked",
		  library_path_in_walk("/mnt/sdcard/Music/Audiobooks/Book.flac", root, chosen, 2, test_is_junk, "Audiobooks",
							   "Podcast"),
		  true);
	check("Podcast at depth 1 whole-card is walked",
		  library_path_in_walk("/mnt/sdcard/Music/Podcast/Episode.flac", root, NULL, 0, test_is_junk, "Audiobooks",
							   "Podcast"),
		  true);

	// Chosen-folder filter: case-blind, root loose files excluded.
	check("filter: chosen folder accepted",
		  library_path_in_walk("/mnt/sdcard/Music/Song.flac", root, chosen, 2, test_is_junk, "Audiobooks", "Podcast"),
		  true);
	check("filter: chosen folder accepted with different case",
		  library_path_in_walk("/mnt/sdcard/MUSIC/Song.flac", root, chosen, 2, test_is_junk, "Audiobooks", "Podcast"),
		  true);
	check("filter: unchosen folder rejected",
		  library_path_in_walk("/mnt/sdcard/Videos/Song.flac", root, chosen, 2, test_is_junk, "Audiobooks", "Podcast"),
		  false);
	check("filter: loose file in root rejected",
		  library_path_in_walk("/mnt/sdcard/Song.flac", root, chosen, 2, test_is_junk, "Audiobooks", "Podcast"), false);
	check("filter: deeper path under chosen folder accepted",
		  library_path_in_walk("/mnt/sdcard/FLAC/Album/Song.flac", root, chosen, 2, test_is_junk, "Audiobooks", "Podcast"),
		  true);

	// Root boundary and cue-track paths.
	check("path outside root rejected",
		  library_path_in_walk("/mnt/other/Music/Song.flac", root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"), false);
	check("path equal to root rejected", library_path_in_walk("/mnt/sdcard", root, NULL, 0, test_is_junk, "Audiobooks",
															 "Podcast"),
		  false);
	check("cue track path accepted",
		  library_path_in_walk("/mnt/sdcard/Music/Album/file.flac?track=2", root, chosen, 2, test_is_junk, "Audiobooks",
							   "Podcast"),
		  true);

	// Overlong paths cannot have been indexed by the 512-byte scan buffer.
	char long_path[4096];
	memset(long_path, 'A', sizeof(long_path) - 1);
	long_path[sizeof(long_path) - 1] = '\0';
	memcpy(long_path, root, strlen(root));
	long_path[strlen(root)] = '/';
	check("overlong path rejected without overflowing",
		  library_path_in_walk(long_path, root, NULL, 0, test_is_junk, "Audiobooks", "Podcast"), false);

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
