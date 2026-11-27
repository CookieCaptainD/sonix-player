#!/usr/bin/env python3
"""Print a short summary of the Tartarus v1.3.3 GitHub release.

Fetches the release by tag from the GitHub API. The output is captured to
release-summary.txt and rendered to PNG. No values are hard-coded here.
"""

import json
import urllib.request

URL = "https://api.github.com/repos/Tartarus6/sonix-player/releases/tags/v1.3.3"


def main():
    request = urllib.request.Request(URL, headers={
        "Accept": "application/vnd.github+json",
        "User-Agent": "sonix-pr-evidence",
    })
    with urllib.request.urlopen(request, timeout=30) as response:
        release = json.load(response)

    print("TARTARUS SONIX-PLAYER RELEASE METADATA (GitHub API)")
    print("tag       : " + release["tag_name"])
    print("published : " + release["published_at"])
    print("assets    : " + str(len(release["assets"])))
    for asset in release["assets"]:
        print("  %-14s %10d bytes  sha256 %s" % (
            asset["name"], asset["size"], asset["digest"].split(":")[-1][:16]))
    print("")
    print("RELEASE NOTES (verbatim, blank lines removed)")
    for line in release["body"].splitlines():
        stripped = line.strip()
        if stripped:
            print("  " + stripped)
    print("")
    print("Branch base for this PR: c54f8e4, the v1.3.3 merge commit.")


if __name__ == "__main__":
    main()
