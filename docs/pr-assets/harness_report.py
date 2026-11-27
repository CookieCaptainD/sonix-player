#!/usr/bin/env python3
"""Print the recorded AVRCP target browsing gate results as a terminal report.

Reads results.json, computes the totals, and writes a fixed-width report. The
output is captured to harness-summary.txt and rendered to PNG. No device data
is generated here: the device section is the user report stored in the JSON.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    with open(os.path.join(HERE, "results.json"), encoding="utf-8") as handle:
        data = json.load(handle)

    print("AVRCP TARGET BROWSING - HOST GATE SUMMARY")
    print("source : " + data["source"])
    print("base   : " + data["base_commit"])
    print("")
    print("%-20s %7s %7s  %s" % ("GATE", "CASES", "PASS", "SCOPE"))
    print("-" * 94)

    total_cases = 0
    total_pass = 0
    for gate in data["gates"]:
        total_cases += gate["cases"]
        total_pass += gate["passed"]
        status = "PASS" if gate["passed"] == gate["cases"] else "FAIL"
        print("%-20s %7d %7d  %s" % (
            gate["name"], gate["cases"], gate["passed"], status))
        print("%-20s %7s %7s  %s" % ("", "", "", gate["scope"]))

    print("-" * 94)
    print("TOTAL                %7d %7d  %s" % (
        total_cases, total_pass,
        "PASS" if total_pass == total_cases else "FAIL"))
    print("")
    print("DEVICE REPORT (user, real car head unit, not CI)")
    for item in data["device"]["confirmed"]:
        print("  confirmed: " + item)
    print("")
    print("Evidence note: this is harness output and a user report.")
    print("No device screenshot was captured or invented.")

    return 0 if total_pass == total_cases else 1


if __name__ == "__main__":
    sys.exit(main())
