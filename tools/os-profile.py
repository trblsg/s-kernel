#!/usr/bin/env python3
"""The OS profile a build is for: profiles/<name>.conf. docs/two-bases.md says how profiles, XNU bases and devices fit together.

  tools/os-profile.py get KEY     a key's value (xnu_tag, xnu_commit, base, ...); fails if the profile does not build
  tools/os-profile.py get src     the kernel tree the profile builds in: xnu-src/<xnu_tag>
  tools/os-profile.py get builder who builds it, when not xnu-build.sh: dxb-ios (ios-27); fails when the profile has no such line
  tools/os-profile.py require     fails with the reason if the profile does not build
  tools/os-profile.py name        the profile chosen (see below)
  tools/os-profile.py list        every profile and its status
  tools/os-profile.py for-device NAME   write .os-profile from devices/NAME/device.conf's `profile` line, or remove it

The profile is OS_PROFILE if it is set, else the one the active configuration's device names (the file .os-profile, which
`make NAME_defconfig` writes from devices/NAME/device.conf's `profile` line), else ios-16.5.

status: real (builds, and has booted on a phone), experimental (the base and its series are there, nothing of it has booted:
a build says so on stderr), not-implemented (fails).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = "ios-16.5"
KEYS = {"status", "arch", "darwin", "xnu_tag", "xnu_commit", "base", "machine", "sdk", "hfs", "adt", "userland"}
# where the kernel tree comes from (docs/xnu-fork.md): Scion-Project/xnu, a fork of Apple's repository, at one commit of one branch,
# and what is still staged at build time on top of it (inputs that may not be in git: the Kernel Debug Kit's headers)
# `drivers none`: the profile's fork carries its own drivers, so none is added from drivers/ (xnu/apply-series.sh)
# and who builds it (`builder`: absent, xnu-build.sh in xnu-src/ with xnu-deps/; dxb-ios, darwin-xnu-build in build/dxb-12377 as an
# iOS-target XNU with the Kernel Debug Kit and Xcode you bring: tools/ios27/build-xnu-ios.sh, which tools/make-kernel.sh runs)
OPTIONAL = {"xnu_fork", "fork_branch", "fork_commit", "fork_stage", "fork_copy", "drivers", "builder"}
BUILDERS = ("dxb-ios",)
STATUSES = ("real", "experimental", "not-implemented")


def read(name):
    path = os.path.join(ROOT, "profiles", name + ".conf")
    if not os.path.isfile(path):
        sys.exit("os-profile: no profile %s (tools/os-profile.py list)" % name)
    fields = {}
    for line in open(path, encoding="utf-8"):
        if line.startswith("\t"):
            key, _, value = line.strip().partition(" ")
            if key.startswith("#"):      # a comment indented like a key (the file's top comments need no tab)
                continue
            if key not in KEYS | OPTIONAL:
                sys.exit("os-profile: %s: unknown key %s" % (path, key))
            fields[key] = value.strip()
    missing = KEYS - set(fields)
    if missing:
        sys.exit("os-profile: %s lacks %s" % (path, ", ".join(sorted(missing))))
    if fields["status"] not in STATUSES:
        sys.exit("os-profile: %s: status %s is not one of %s" % (path, fields["status"], ", ".join(STATUSES)))
    if fields.get("builder", BUILDERS[0]) not in BUILDERS:
        sys.exit("os-profile: %s: builder %s is not one of %s" % (path, fields["builder"], ", ".join(BUILDERS)))
    fields["src"] = os.path.join("xnu-src", fields["xnu_tag"])
    return fields


def require(name, fields):
    if fields["status"] == "real":
        return
    if fields["status"] == "experimental":
        if not os.environ.get("OS_PROFILE_QUIET"):
            sys.stderr.write("os-profile: %s is experimental: %s (%s) with the fork's %s; nothing built from it has booted (docs/two-bases.md)\n"
                             % (name, fields["xnu_tag"], fields["arch"], fields.get("fork_branch", "branch")))
        return
    sys.exit("os-profile: %s is %s: this tree has no kernel for %s (%s). Only ios-16.5 builds; docs/modular-kernel.md has what is missing."
             % (name, fields["status"], fields["xnu_tag"], fields["arch"]))


def chosen():
    name = os.environ.get("OS_PROFILE")
    if not name:
        mark = os.path.join(ROOT, ".os-profile")
        if os.path.isfile(mark):
            name = open(mark, encoding="utf-8").read().strip()
    name = name or DEFAULT
    if not re.fullmatch(r"[a-z0-9.-]+", name):
        sys.exit("os-profile: bad name %r" % name)
    return name


def for_device(device):
    """`make NAME_defconfig` calls this: the device's profile (device.conf: `\tprofile NAME`) becomes the default."""
    mark = os.path.join(ROOT, ".os-profile")
    conf = os.path.join(ROOT, "devices", device, "device.conf")
    profile = None
    if os.path.isfile(conf):
        for line in open(conf, encoding="utf-8"):
            words = line.split()
            if line.startswith("\t") and len(words) == 2 and words[0] == "profile":
                profile = words[1]
    if profile:
        read(profile)
        with open(mark, "w", encoding="utf-8") as f:
            f.write(profile + "\n")
        print("os-profile: %s builds for %s (.os-profile)" % (device, profile))
    elif os.path.exists(mark):
        os.remove(mark)


def main():
    name = chosen()
    command = sys.argv[1] if len(sys.argv) > 1 else "list"
    if command == "list":
        for conf in sorted(os.listdir(os.path.join(ROOT, "profiles"))):
            if conf.endswith(".conf"):
                f = read(conf[:-5])
                print("%-10s %-16s %-6s %s" % (conf[:-5], f["status"], f["arch"], f["xnu_tag"]))
    elif command == "for-device" and len(sys.argv) == 3:
        for_device(sys.argv[2])
    elif command == "name":
        print(name)
    elif command == "require":
        require(name, read(name))
    elif command == "get" and len(sys.argv) == 3:
        fields = read(name)
        require(name, fields)
        if sys.argv[2] not in fields:
            if sys.argv[2] in OPTIONAL:
                sys.exit(1)
            sys.exit("os-profile: no key %s" % sys.argv[2])
        print(fields[sys.argv[2]])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
