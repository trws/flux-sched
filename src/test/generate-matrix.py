#!/usr/bin/env python3
#
#  Generate a build matrix for use with github workflows
#

import json
import os
import re

docker_run_checks = "src/test/docker/docker-run-checks.sh"

default_args = " --sysconfdir=/etc" " --localstatedir=/var"

DOCKER_REPO = "fluxrm/flux-sched"


class BuildMatrix:
    def __init__(self):
        self.matrix = []
        self.branch = None
        self.tag = None

        #  Set self.branch or self.tag based on GITHUB_REF
        if "GITHUB_REF" in os.environ:
            self.ref = os.environ["GITHUB_REF"]
            match = re.search("^refs/heads/(.*)", self.ref)
            if match:
                self.branch = match.group(1)
            match = re.search("^refs/tags/(.*)", self.ref)
            if match:
                self.tag = match.group(1)

    def create_docker_tag(self, image, env, command, platform):
        """Create docker tag string if this is master branch or a tag"""
        if self.branch == "master" or self.tag:
            tag = f"{DOCKER_REPO}:{image}"
            if self.tag:
                tag += f"-{self.tag}"
            if platform is not None:
                tag += "-" + platform.split("/")[1]
            env["DOCKER_TAG"] = tag
            command += f" --tag={tag}"
            return True, command

        return False, command

    def add_build(
        self,
        name=None,
        image="fedora40",
        args=default_args,
        jobs=4,
        env=None,
        docker_tag=False,
        coverage=False,
        coverage_flags=None,
        recheck=True,
        platform=None,
        command_args="",
    ):
        """Add a build to the matrix.include array"""

        # Extra environment to add to this command:
        env = env or {}

        # hwloc tries to look for opengl devices  by connecting to a port that might
        # sometimes be an x11 port, but more often for us is munge, turn it off
        env["HWLOC_COMPONENTS"] = "-gl"
        # the psm3 connector added to libfabrics in ~1.12 causes errors when allowed to
        # use non-local connectors on a system with virtual NICs, since we're in a
        # docker container, prevent this
        env["PSM3_HAL"] = "loopback"

        needs_buildx = False
        if platform:
            command_args += f"--platform={platform}"
            needs_buildx = True

        # The command to run:
        command = f"{docker_run_checks} -j{jobs} --image={image} {command_args}"

        # Add --recheck option if requested
        if recheck and "DISTCHECK" not in env:
            command += " --recheck"

        if docker_tag:
            #  Only export docker_tag if this is main branch or a tag:
            docker_tag, command = self.create_docker_tag(image, env, command, platform)

        if coverage:
            env["COVERAGE"] = "t"

        create_release = False
        if self.tag and "DISTCHECK" in env:
            create_release = True

        command += f" -- {args}"

        # TODO : remove this when the boost issue is dealt with
        if env.get("CC", "gcc").find("clang") < 0:
            cppflags = env.get("CPPFLAGS", "") + " -Wno-error=maybe-uninitialized"
            env["CPPFLAGS"] = cppflags

        self.matrix.append(
            {
                "name": name,
                "env": env,
                "command": command,
                "image": image,
                "tag": self.tag,
                "branch": self.branch,
                "coverage": coverage,
                "coverage_flags": coverage_flags,
                "docker_tag": docker_tag,
                "needs_buildx": needs_buildx,
                "create_release": create_release,
            }
        )

    def __str__(self):
        """Return compact JSON representation of matrix"""
        return json.dumps(
            {"include": self.matrix}, skipkeys=True, separators=(",", ":")
        )


matrix = BuildMatrix()

# # Debian: 32b -- NOTE: VERY broken right now
# matrix.add_build(
#     name="bookworm - 32 bit",
#     image="bookworm",
#     platform="linux/386",
#     docker_tag=True,
# )

# debian/Fedora40: arm64, expensive, only on master and tags, only install
if matrix.branch == "master" or matrix.tag:
    for d in ("bookworm", "noble", "fedora40"):
        matrix.add_build(
            name=f"{d} - arm64",
            image=f"{d}",
            platform="linux/arm64",
            docker_tag=True,
            command_args="--install-only ",
        )

# builds to match arm64 images must have linux/amd64 platform explicitly

# Debian: gcc-12, distcheck
matrix.add_build(
    name="bookworm - gcc-12,distcheck",
    image="bookworm",
    env=dict(
        CC="gcc-12",
        CXX="g++-12",
        DISTCHECK="t",
    ),
    args="",
)

# fedora40: clang-18
matrix.add_build(
    name="fedora40 - clang-18",
    env=dict(
        CC="clang-18",
        CXX="clang++-18",
        CFLAGS="-O2 -gdwarf-4",
        chain_lint="t",
    ),
)

# coverage
matrix.add_build(
    name="coverage",
    image="bookworm",
    coverage_flags="ci-basic",
    coverage=True,
    jobs=4,
)


# Ubuntu: TEST_INSTALL
matrix.add_build(
    name="noble - test-install",
    image="noble",
    env=dict(
        TEST_INSTALL="t",
    ),
    platform="linux/amd64",
    docker_tag=True,
)

# Debian: TEST_INSTALL
matrix.add_build(
    name="bookworm - test-install",
    image="bookworm",
    env=dict(
        TEST_INSTALL="t",
    ),
    platform="linux/amd64",
    args="",
    docker_tag=True,
)

# Ubuntu 24.04
matrix.add_build(
    name="noble - golang-test",
    image="noble-golang",
    env=dict(
        CC="gcc-10",
        CXX="g++-10",
        WITH_GO="yes",
    ),
    args="",
)

# RHEL8 clone
matrix.add_build(
    name="el8",
    image="el8",
    env=dict(
        # this is _required_ because of valgrind's new dependency on python3.11
        # which confuses rhel8 cmake's detection logic
        PYTHON="/usr/bin/python3.6"
    ),
)

# Fedora 40
matrix.add_build(
    name="fedora40 - gcc-14",
    image="fedora40",
    args=("--prefix=/usr" " --sysconfdir=/etc" " --localstatedir=/var"),
    platform="linux/amd64",
    docker_tag=True,
)


print(matrix)
