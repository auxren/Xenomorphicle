Import("env")

import subprocess

def get_git_rev():
    git_rev = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"]).decode().strip()
    git_status = subprocess.check_output(["git", "status", "-s", "--untracked-files=no"]).decode()
    suffix = "dirty" if git_status else ""

    return git_rev + suffix

def get_version():
    with open("src/OC_version.h") as file:
        last_line = file.readlines()[-1].strip().replace('"', '')

    return last_line 

tag = get_git_rev()

build_flags = env.ParseFlags(env['BUILD_FLAGS'])
defines = build_flags.get("CPPDEFINES")

env.Append(BUILD_FLAGS=[ f'-DOC_BUILD_TAG=\\"{tag}\\"' ])

# Unix time this image was built. setup() uses it to set the RTC when the
# clock reads earlier than the firmware it is running, which is the only
# way it can be wrong in that direction. Without this the RTC sits at its
# 2019-01-01 power-on default and every file timestamp is uptime, not a
# date -- which is exactly what made three stray preset slots undateable.
import datetime as _dt
# LOCAL time, not UTC. Teensy's Time library treats whatever Teensy3Clock
# holds as local, so handing it a UTC epoch makes the module read hours
# ahead of the clock on the wall next to it.
_now = _dt.datetime.now()
_offset = _now.astimezone().utcoffset().total_seconds()
env.Append(BUILD_FLAGS=[ f'-DOC_BUILD_EPOCH={int(_now.timestamp() + _offset)}' ])

if "USB_AUDIO" in defines:
    tag += "+audio"
if "USB_MTPDISK" in defines:
    tag += "+MTP"

if "T41" not in env['PIOENV']:
    version = get_version()
    for item in defines:
        if item[0] == 'OC_VERSION_EXTRA':
            version += item[1].strip('"')
    # This fork is Xenomorphicle, not Phazerville. Note this branch is dead
    # for every environment here -- they all have "T41" in the name, so the
    # guard above skips it, and the release image is named by
    # assemble-multiboot.py instead. Corrected anyway so it cannot come back
    # wrong if an env is ever added without "T41" in its name.
    env.Replace(PROGNAME=f"xenomorphicle-{version}-{tag}")
