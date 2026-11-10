#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""The kernel source of an OS profile as the fork holds it (docs/xnu-fork.md), for the tools that read it.

  tools/xnu-tree.py path [PROFILE]      a cached, pristine checkout of the profile's fork commit (no drivers, no configuration:
                                        what `git archive` of Scion-Project/xnu at that commit would give), printed: xnu-src/fork-<12>
  tools/xnu-tree.py added [PROFILE]     a directory of just the files the fork adds to Apple's XNU, at their paths in the tree (links into
                                        the checkout): xnu-src/fork-<12>-added. For -I: the checkout itself has Apple's headers, which
                                        shadow the host's (<sys/types.h>), and these are Scion's own
  tools/xnu-tree.py changed [PROFILE] [--diff-filter=ACMRTD]
                                        the files the fork's commit adds or changes relative to Apple's commit (the profile's
                                        xnu_commit), one per line; --diff-filter=A is the files that are Scion's own
  tools/xnu-tree.py diff [PROFILE] [FILE...]
                                        `git diff -U0` of the fork's commit against Apple's commit, for the files named (default: all)
  tools/xnu-tree.py log [PROFILE]       the fork's commits on Apple's, oldest first: "<commit> <parent> <subject>" (the parent of the
                                        first is Apple's commit); fetches the branch's history, which `path` does not

PROFILE is OS_PROFILE, else the active profile (tools/os-profile.py name). The fork comes from the profile's xnu_fork, or
XNU_FORK_URL (a local clone that has the commit will do); Apple's commit is fetched from the same place, which has it, and from
XNU_URL (Apple's repository) if that does not. The cache is kept in xnu-src/ (not in git); it needs the network the first time.

From Python: load this file (importlib) and call tree(profile), added(profile), changed(profile, diff_filter), diff(profile, files),
commits(profile) and git_out(profile, *args) (git run in the checkout).
"""
import importlib.util
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPLE = "https://github.com/apple-oss-distributions/xnu.git"


_PROFILE = []


def profile_get(profile, key, required=True):
    """The profile's value of `key` (tools/os-profile.py get), read in this process; None if the profile has none and it is not required."""
    if not _PROFILE:
        spec = importlib.util.spec_from_file_location("os_profile", os.path.join(ROOT, "tools", "os-profile.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _PROFILE.append(mod)
    mod = _PROFILE[0]
    name = profile or mod.chosen()
    value = mod.read(name).get(key)        # exits with its own message for a profile that does not exist
    if value is None and required:
        sys.exit("xnu-tree: the profile %s has no %s\n  (the kernel tree comes from the fork: docs/xnu-fork.md)" % (name, key))
    return value


def git(cwd, *args, check=True, capture=False):
    r = subprocess.run(["git", "-c", "advice.detachedHead=false"] + list(args), cwd=cwd, check=False,
                       stdout=subprocess.PIPE if capture else None, stderr=None if check else subprocess.DEVNULL,
                       universal_newlines=True)
    if check and r.returncode != 0:
        sys.exit("xnu-tree: git %s failed in %s" % (" ".join(args[:2]), cwd))
    return r


def head(path):
    """The commit checked out in `path` (None if there is no checkout): read from .git, without a git process, when it can be."""
    if not os.path.isdir(os.path.join(path, ".git")):
        return None
    try:
        ref = open(os.path.join(path, ".git", "HEAD")).read().strip()
        if not ref.startswith("ref: "):
            return ref
        return open(os.path.join(path, ".git", ref[5:])).read().strip()
    except OSError:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=path, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, universal_newlines=True)
        return r.stdout.strip() if r.returncode == 0 else None


def tree(profile=None):
    """Path of the pristine checkout of the profile's fork commit; made (and fetched) if it is not there."""
    commit = profile_get(profile, "fork_commit")
    url = os.environ.get("XNU_FORK_URL") or profile_get(profile, "xnu_fork")
    path = os.path.join(ROOT, "xnu-src", "fork-" + commit[:12])
    if head(path) == commit:
        return path
    tmp = "%s.tmp%d" % (path, os.getpid())
    subprocess.run(["rm", "-rf", tmp], check=True)
    os.makedirs(tmp)
    git(tmp, "init", "-q")
    git(tmp, "remote", "add", "origin", url)
    sys.stderr.write("xnu-tree: fetching %s at %s\n" % (url, commit[:12]))
    git(tmp, "fetch", "-q", "--depth", "1", "origin", commit)
    git(tmp, "checkout", "-q", "-b", "fork", commit)
    if head(tmp) != commit:
        sys.exit("xnu-tree: %s gave %s, expected %s" % (url, head(tmp), commit))
    if os.path.isdir(path):
        subprocess.run(["rm", "-rf", path], check=True)
    os.rename(tmp, path)
    return path


def added(profile=None):
    """Path of a directory of the files the fork adds to Apple's XNU, at their paths in the tree, as links into the checkout."""
    path = tree(profile)
    out = path + "-added"
    commit = head(path)
    if os.path.isfile(os.path.join(out, ".commit")) and open(os.path.join(out, ".commit")).read().strip() == commit:
        return out
    tmp = "%s.tmp%d" % (out, os.getpid())
    subprocess.run(["rm", "-rf", tmp], check=True)
    for f in changed(profile, "A"):
        os.makedirs(os.path.dirname(os.path.join(tmp, f)), exist_ok=True)
        os.symlink(os.path.join(path, f), os.path.join(tmp, f))
    with open(os.path.join(tmp, ".commit"), "w") as fh:
        fh.write(commit + "\n")
    subprocess.run(["rm", "-rf", out], check=True)
    os.rename(tmp, out)
    return out


def apple_commit(profile, path):
    """Apple's commit (the profile's xnu_commit) in the cache repository, fetched if it is not there yet."""
    base = profile_get(profile, "xnu_commit")
    if git(path, "cat-file", "-e", base + "^{commit}", check=False).returncode == 0:
        return base
    url = os.environ.get("XNU_FORK_URL") or profile_get(profile, "xnu_fork")
    for u in (url, os.environ.get("XNU_URL") or APPLE):
        sys.stderr.write("xnu-tree: fetching Apple's %s from %s\n" % (base[:12], u))
        if git(path, "fetch", "-q", "--depth", "1", u, base, check=False).returncode == 0:
            return base
    sys.exit("xnu-tree: cannot fetch Apple's commit %s" % base)


def changed(profile=None, diff_filter=None):
    """The files the fork's commit adds or changes relative to Apple's commit, sorted."""
    path = tree(profile)
    base = apple_commit(profile, path)
    cmd = ["diff", "--name-only", "--no-renames"] + (["--diff-filter=" + diff_filter] if diff_filter else []) + [base, head(path)]
    return sorted(l for l in git(path, *cmd, capture=True).stdout.split("\n") if l)


def diff(profile=None, files=()):
    """`git diff -U0` of the fork's commit against Apple's, for the files given (all if none)."""
    path = tree(profile)
    base = apple_commit(profile, path)
    cmd = ["diff", "-U0", "--no-renames", "--no-color", base, head(path)] + (["--"] + list(files) if files else [])
    return git(path, *cmd, capture=True).stdout


def git_out(profile, *args):
    """The output of `git ARGS` run in the profile's checkout."""
    return git(tree(profile), *args, capture=True).stdout


def commits(profile=None):
    """[(commit, parent, subject)] of the fork's commits on Apple's, oldest first; the first one's parent is Apple's commit."""
    path = tree(profile)
    base = apple_commit(profile, path)
    tip = head(path)
    def rev():
        log = git(path, "log", "--reverse", "--format=%H\t%s", base + ".." + tip, capture=True).stdout
        return [l.partition("\t")[::2] for l in log.split("\n") if l]
    revs = rev()
    if len(revs) < 2 and git(path, "rev-list", "--count", tip, capture=True).stdout.strip() == "1":
        url = os.environ.get("XNU_FORK_URL") or profile_get(profile, "xnu_fork")
        git(path, "fetch", "-q", "--shallow-exclude=" + profile_get(profile, "xnu_tag"), url, tip)
        revs = rev()
    out, parent = [], base
    for sha, subject in revs:
        out.append((sha, parent, subject))
        parent = sha
    return out


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ("path", "changed", "diff", "log", "added"):
        sys.exit(__doc__)
    cmd, rest = args[0], args[1:]
    flt = None
    for a in list(rest):
        if a.startswith("--diff-filter="):
            flt = a.split("=", 1)[1]
            rest.remove(a)
    profile = rest[0] if rest and cmd != "diff" else None
    if cmd == "diff":
        if rest and rest[0] != "--" and not os.sep in rest[0] and not "." in rest[0] and os.path.isfile(os.path.join(ROOT, "profiles", rest[0] + ".conf")):
            profile, rest = rest[0], rest[1:]
        sys.stdout.write(diff(profile, rest))
    elif cmd == "log":
        for sha, parent, subject in commits(profile):
            print(sha, parent, subject)
    elif cmd == "path":
        print(tree(profile))
    elif cmd == "added":
        print(added(profile))
    else:
        for f in changed(profile, flt):
            print(f)


if __name__ == "__main__":
    main()
