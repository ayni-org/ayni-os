"""Run the full normal application as an independent read-only source SDK consumer."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from source_sdk import source_sdk


def run(command):
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--generator", default="Ninja")
    parser.add_argument("--sanitize", default="")
    parser.add_argument("--build-type", default="Debug")
    parser.add_argument("--source-revision", default="working-tree")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ayni-normal-consumer-") as temporary:
        root = Path(temporary)
        sdk = root / "sdk"
        consumer = root / "consumer"
        for relative in ("common", "native"):
            shutil.copytree(args.source / "tests/portability" / relative, consumer / relative)
        with source_sdk(args.source, sdk, "native"):
            build = root / "build"
            configure = ["cmake", "-G", args.generator, "-S", str(consumer / "native"), "-B", str(build),
                         f"-DAYNI_SOURCE_DIR={sdk}", f"-DAYNI_SOURCE_REVISION={args.source_revision}", f"-DCMAKE_BUILD_TYPE={args.build_type}",
                         "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_CURL=TRUE",
                         "-DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=TRUE"]
            if args.sanitize:
                flags = f"-fsanitize={args.sanitize} -fno-omit-frame-pointer -fno-sanitize-recover=all"
                configure += [f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags}", f"-DCMAKE_EXE_LINKER_FLAGS={flags}"]
            run(configure)
            run(["cmake", "--build", str(build), "--parallel", os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "2")])
            commands = json.loads((build / "compile_commands.json").read_text())
            # Consumer compilation sees public targets only. The SDK itself may
            # use its private Core implementation, without exporting those includes.
            for command in commands:
                if Path(command["file"]).is_relative_to(consumer):
                    assert str(sdk / "core") not in command["command"], command
                    assert "/runtime/native_node" not in command["command"], command
                    assert "/tests/unit" not in command["command"], command
            state = root / "state"
            state.mkdir()
            run([str(build / "ayni-portable"), str(state)])
            assert (state / "snapshot_0").is_file(), "Privacy compaction must publish both snapshot slots"
            assert (state / "snapshot_1").is_file(), "Privacy compaction must publish both snapshot slots"
    print("Independent full normal SDK consumer passed")


if __name__ == "__main__":
    main()
