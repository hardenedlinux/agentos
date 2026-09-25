#!/usr/bin/env python3
"""
clean_events.py
================

Standalone cleanup for AgentOS's event mailbox
(agentos_home()/events/ — job.phase_changed/job.step_changed files that
a real Bridge is expected to drain and delete itself).

Use this when testing an actual Bridge/web service against AgentOS
(rather than the one-shot verify_deliverable_kind.py script) — run it
before a test session to start from a clean mailbox, without needing to
route it through any other test script.

Usage:
    python clean_events.py              # delete leftover event files
    python clean_events.py --dry-run    # just report how many there are
    python clean_events.py --home /path/to/.agentos   # override location
"""

import argparse
import os
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--home",
        default=os.environ.get("AGENTOS_HOME", "~/.agentos"),
        help="AgentOS home directory (default: $AGENTOS_HOME or ~/.agentos)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="only report the count, don't actually delete anything",
    )
    args = parser.parse_args()

    events_dir = os.path.join(os.path.expanduser(args.home), "events")

    if not os.path.isdir(events_dir):
        print(f"No events directory at {events_dir} — nothing to clean.")
        return

    entries = [
        name for name in os.listdir(events_dir)
        if os.path.isfile(os.path.join(events_dir, name))
    ]

    if not entries:
        print(f"{events_dir} is already empty.")
        return

    if args.dry_run:
        print(f"{len(entries)} leftover event file(s) in {events_dir} "
              f"(dry run — nothing deleted):")
        for name in sorted(entries):
            print(f"  {name}")
        return

    removed = 0
    failed = 0
    for name in entries:
        path = os.path.join(events_dir, name)
        try:
            os.remove(path)
            removed += 1
        except OSError as e:
            failed += 1
            print(f"  could not remove {path}: {e}", file=sys.stderr)

    print(f"Removed {removed} event file(s) from {events_dir}"
          + (f" ({failed} failed)" if failed else ""))


if __name__ == "__main__":
    main()
