#!/usr/bin/env python3
"""Turn an emulator boot log into a scrubbed list of problems, and optionally into issues.

    python scripts/triage-log.py boot.log                 # print the report
    python scripts/triage-log.py boot.log --file-issues   # also open / update GitHub issues

Each problem gets a stable signature. With --file-issues, a problem whose signature is not in
any existing issue gets a new issue labelled `needs-triage`; a problem that already has an open
issue gets a "still seen" comment; one whose issue was closed gets a regression comment and is
reopened. Nothing is posted unless the text passes scripts/policy-check.sh.

Only derived facts leave the machine: error messages, symbol ids, shader hashes and counts.
Paths are removed. The log itself is never uploaded.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from collections import Counter, OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PATH_RE = re.compile(r"(?:[A-Za-z]:[\\/]|/(?:home|Users|mnt|media)/)\S*")
SRC_RE = re.compile(r"\S*[\\/](src[\\/]\S+)")
HEX_RE = re.compile(r"0x[0-9a-fA-F]+")


def scrub(text):
    """Remove anything that can identify a dump or a user: keep repo-relative source paths."""
    text = re.sub(r"program=.*$", "program=<scrubbed>", text)
    text = SRC_RE.sub(lambda m: m.group(1).replace("\\", "/"), text)
    text = PATH_RE.sub("<path>", text)
    return text.strip()


def short_hash(text):
    return hashlib.sha1(text.encode("utf-8")).hexdigest()[:10]


class Problem:
    def __init__(self, sig, title, labels, summary):
        self.sig = sig
        self.title = title
        self.labels = labels
        self.summary = summary
        self.count = 0
        self.examples = []

    def add(self, example=None):
        self.count += 1
        if example and example not in self.examples and len(self.examples) < 6:
            self.examples.append(example)


def parse(path):
    problems = OrderedDict()

    def get(sig, title, labels, summary):
        if sig not in problems:
            problems[sig] = Problem(sig, title, labels, summary)
        return problems[sig]

    build = None
    in_error = False
    in_build = False
    details = {"resource tracking failed": set(), "resource specialization failed": set()}
    with open(path, "r", encoding="utf-8", errors="replace") as log:
        for raw in log:
            line = raw.rstrip("\r\n")

            if line.startswith("--- Build ---"):
                in_build = True
                continue
            if in_build:
                build = scrub(line)
                in_build = False
                continue
            if line.startswith("--- Error ---"):
                in_error = True
                continue
            if in_error:
                in_error = False
                msg = scrub(line)
                m = re.search(r"unsupported family=(\w+) opcode=(0x[0-9a-f]+).*?reason=(.*?)(?: in |$)", msg)
                if m:
                    family, opcode, reason = m.groups()
                    sig = "shader-decode:%s:%s:%s" % (family, opcode, short_hash(reason))
                    title = "shader: unsupported %s opcode %s (%s)" % (family, opcode, reason.strip())
                    labels = ["blocker", "area: shader"]
                else:
                    sig = "fatal:" + short_hash(HEX_RE.sub("0x?", msg))
                    title = "fatal: " + HEX_RE.sub("0x?", msg)[:90]
                    labels = ["blocker"]
                get(sig, title, labels, "The emulator aborted with this error.").add(msg)
                continue

            m = re.match(r"Unresolved import stub called: (\S+?)\[(\w+)\]\[(\w+)\]\[(\w+)\]", line)
            if m:
                nid, lib = m.group(1), m.group(2)
                get("unresolved-import:%s:%s" % (lib, nid),
                    "%s: implement the unresolved export %s" % (lib.lower(), nid),
                    ["blocker", "area: libs"],
                    "The title calls a function that is not implemented; the stub returns zero.").add()
                continue
            m = re.match(r"\s*caller=0x[0-9a-f]+ rdi=.*", line)
            if m and problems:
                last = next(reversed(problems.values()))
                if last.sig.startswith("unresolved-import:") and not last.examples:
                    last.examples.append(line.strip())
                continue

            m = re.match(r"shader resource tracking: hash=0x[0-9a-f]+ stage=compute pc=0x[0-9a-f]+ (.*)", line)
            if m:
                # Printed next to the skip line for the same shader, in either order.
                details["resource tracking failed"].add(m.group(1).strip())
                continue
            m = re.match(r"shader resource tracking: hash=(0x[0-9a-f]+) pc=(0x[0-9a-f]+) untraceable sampler", line)
            if m:
                get("sampler-fallback", "shader: sampler state could not be traced",
                    ["workaround-debt", "area: shader"],
                    "A default sampler was substituted; filtering may be wrong.").add(
                        "%s pc=%s" % (m.group(1), m.group(2)))
                continue
            m = re.match(r"shader resource specialization failed: (indirect buffer) ", line)
            if m:
                details["resource specialization failed"].add("too many indirect buffer candidates")
                continue
            m = re.search(r"TEMPORARY COMPUTE DISPATCH SKIP !!! shader=(0x[0-9a-f]+) reason=([a-z ]+)", line)
            if m:
                shader, reason = m.group(1), m.group(2).strip()
                get("compute-skip:" + reason.replace(" ", "-"),
                    "shader: compute dispatch skipped (%s)" % reason,
                    ["workaround-debt", "area: shader"],
                    "Compute shaders were skipped by the temporary workaround.").add(shader)
                continue
            m = re.search(r"TEMPORARY DRAW SKIP !!! shader=(0x[0-9a-f]+) stage=\d+ reason=([a-z ]+)", line)
            if m:
                shader, reason = m.group(1), m.group(2).strip()
                get("draw-skip:" + reason.replace(" ", "-"),
                    "renderer: draw skipped (%s)" % reason,
                    ["workaround-debt", "area: shader"],
                    "Draws were dropped by the temporary workaround because a vertex or pixel "
                    "shader could not be used.").add(shader)
                continue

            m = re.match(r"\s*Equeue wait timedout: (.+)", line)
            if m:
                name = m.group(1).strip()
                get("equeue-timeout:" + name.lower().replace(" ", "-"),
                    "kernel: event queue wait keeps timing out (%s)" % name,
                    ["investigation"],
                    "A guest thread waits for this event and never receives it.").add()
                continue
    for reason, found in details.items():
        p = problems.get("compute-skip:" + reason.replace(" ", "-"))
        if p and found:
            p.summary += " Reported cause: " + "; ".join(sorted(found)) + "."
    return build, list(problems.values())


def policy_ok(text):
    proc = subprocess.run(["bash", os.path.join(HERE, "policy-check.sh"), "text", "triage report"],
                          input=text.encode("utf-8"), cwd=ROOT)
    return proc.returncode == 0


def marker(sig):
    return "<!-- triage-sig: %s -->" % sig


def issue_body(p, build):
    lines = [p.summary, "", "Seen %d time(s) in one run" % p.count
             + (" of build `%s`." % build if build else "."), ""]
    if p.examples:
        lines += ["```text"] + p.examples + ["```", ""]
    lines += ["_Filed by `scripts/triage-log.py`. Needs a human to confirm, add where to look, "
              "and set a milestone._", "", marker(p.sig)]
    return "\n".join(lines)


def report(build, problems):
    out = ["# Boot log triage", ""]
    if build:
        out += ["Build: `%s`" % build, ""]
    if not problems:
        out.append("No known problem patterns found.")
    for p in problems:
        out.append("- **%s** (x%d) `%s`" % (p.title, p.count, p.sig))
        for e in p.examples[:3]:
            out.append("  - `%s`" % e)
    return "\n".join(out)


def gh(*args, stdin=None):
    proc = subprocess.run(["gh"] + list(args), input=stdin, capture_output=True, text=True,
                          encoding="utf-8", cwd=ROOT)
    if proc.returncode != 0:
        sys.exit("gh %s failed: %s" % (" ".join(args[:2]), proc.stderr.strip()))
    return proc.stdout


def file_issues(build, problems, repo):
    repo_args = ["-R", repo] if repo else []
    existing = json.loads(gh("issue", "list", "--state", "all", "--limit", "1000",
                             "--json", "number,state,body", *repo_args))
    for p in problems:
        match = [i for i in existing if marker(p.sig) in (i.get("body") or "")]
        seen = "Seen %d time(s)" % p.count + (" in build `%s`." % build if build else ".")
        if not match:
            url = gh("issue", "create", "--title", p.title, "--label",
                     ",".join(p.labels + ["needs-triage"]), "--body-file", "-", *repo_args,
                     stdin=issue_body(p, build)).strip()
            print("new      %s  %s" % (url, p.title))
            continue
        issue = sorted(match, key=lambda i: i["state"] != "OPEN")[0]
        number = str(issue["number"])
        if issue["state"] == "OPEN":
            gh("issue", "comment", number, "--body", "Still present. " + seen, *repo_args)
            print("open     #%s  %s" % (number, p.title))
        else:
            gh("issue", "reopen", number, "--comment", "Regression: this is back. " + seen, *repo_args)
            print("reopened #%s  %s" % (number, p.title))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--file-issues", action="store_true", help="open or update GitHub issues")
    ap.add_argument("--repo", help="owner/name; defaults to the gh default for this checkout")
    args = ap.parse_args()

    build, problems = parse(args.log)
    text = report(build, problems)
    posted = text + "\n" + "\n".join(p.title + "\n" + issue_body(p, build) for p in problems)
    if not policy_ok(posted):
        sys.exit("The triage output names a restricted title; refusing to print or post it. "
                 "Fix the scrubbing in scripts/triage-log.py.")
    print(text)
    if args.file_issues:
        print()
        file_issues(build, problems, args.repo)


if __name__ == "__main__":
    main()
