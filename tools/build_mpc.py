#!/usr/bin/env python3
"""Builds Warpline with the stock MPC Plugin SDK build script, adding what build-extra.json asks for.

The SDK's tools/build.py only passes -I<sdk>/framework and -I<project>/src. Warpline also needs the
include folders and ARM link flags listed in build-extra.json ("includeDirs", "armLinkFlags"). This
wrapper loads the SDK's build.py, extends its flag lists with those entries, and runs it with the same
command-line arguments, so every other option (--target, --define, --toolchain ...) works as usual.

    python plugins/Warpline/tools/build_mpc.py [project] [build.py options]

The project defaults to the folder this script is in, and a relative path works from any folder.
The SDK is found two levels above this script's project.
"""
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))
BUILD_PY = os.path.join(SDK, "tools", "build.py")


def main():
    if not os.path.isfile(BUILD_PY):
        sys.exit("SDK build script not found at %s (put this project in <sdk>/plugins/)" % BUILD_PY)
    # The project named on the command line (default: the one this script is in). Made absolute here,
    # because the SDK script runs from the SDK folder.
    args = sys.argv[1:]
    project = PROJECT
    if args and not args[0].startswith("-"):
        project = os.path.abspath(args[0])
        args[0] = project
    else:
        args.insert(0, project)
    with open(os.path.join(project, "build-extra.json"), encoding="utf-8") as f:
        extra = json.load(f)
    include_flags = ["-I" + os.path.join(project, d.replace("/", os.sep)) for d in extra.get("includeDirs", [])]
    arm_link_flags = list(extra.get("armLinkFlags", []))

    spec = importlib.util.spec_from_file_location("sdk_build", BUILD_PY)
    if spec is None or spec.loader is None:
        sys.exit("cannot load %s" % BUILD_PY)
    build = importlib.util.module_from_spec(spec)
    sys.modules["sdk_build"] = build
    spec.loader.exec_module(build)

    common_flags = build.Builder.common_flags
    link_flags_arm = build.Builder.link_flags_arm
    build.Builder.common_flags = lambda self: common_flags(self) + include_flags
    build.Builder.link_flags_arm = lambda self: link_flags_arm(self) + arm_link_flags
    os.chdir(SDK)
    return build.main(args)


if __name__ == "__main__":
    sys.exit(main())
