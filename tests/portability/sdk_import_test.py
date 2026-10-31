"""ADR-0089: configure an independent consumer against an isolated, read-only SDK."""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import tempfile


def run(command, *, success=True):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (result.returncode == 0) != success:
        raise RuntimeError(f"Unexpected result: {command}\n{result.stdout}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--optional", action="store_true")
    parser.add_argument("--download-dir", default="")
    parser.add_argument("--sanitize", default="")
    parser.add_argument("--generator", default="Ninja")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ayni-consumer-") as temporary:
        root = pathlib.Path(temporary)
        sdk = root / "sdk"
        for relative in ("cmake", "include", "core", "strategies/builtin", "providers/software",
                         "providers/mock", "providers/mock_execution", "control/permit_gate",
                         "applications/assistant", "applications/software_workflow"):
            shutil.copytree(args.source / relative, sdk / relative)
        (sdk / "tools").mkdir()
        shutil.copy2(args.source / "tools/architecture.json", sdk / "tools/architecture.json")
        consumer = root / "consumer"
        shutil.copytree(args.source / "tests/portability/sdk_import", consumer)
        for path in sdk.rglob("*"):
            path.chmod(0o555 if path.is_dir() else 0o444)
        sdk.chmod(0o555)
        try:
            configure = ["cmake", "-G", args.generator, "-S", str(consumer), "-B", str(root / "build"),
                         f"-DAYNI_SOURCE_DIR={sdk}", "-DCMAKE_DISABLE_FIND_PACKAGE_CURL=TRUE",
                         "-DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=TRUE",
                         "-DCMAKE_DISABLE_FIND_PACKAGE_Threads=TRUE"]
            if args.download_dir:
                configure.append(f"-DAYNI_DOWNLOAD_DIR={args.download_dir}")
            if args.sanitize:
                flags = f"-fsanitize={args.sanitize} -fno-omit-frame-pointer -fno-sanitize-recover=all"
                configure.extend([f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags}",
                                  f"-DCMAKE_EXE_LINKER_FLAGS={flags}"])
            if args.optional:
                configure.append("-DPROBE_COMPONENTS=software_provider;permit_gate;assistant;"
                                 "software_workflow;mock_provider;mock_execution")
            run(configure)
            run(["cmake", "--build", str(root / "build"), "--parallel",
                 os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "2")])
            run(["ctest", "--test-dir", str(root / "build"), "--output-on-failure"])
            commands = json.loads((root / "build/compile_commands.json").read_text())
            for command in commands:
                if pathlib.Path(command["file"]).is_relative_to(consumer):
                    assert str(sdk / "core") not in command["command"], command
                    assert "/runtime/" not in command["command"], command
            bad = run(configure + ["-DPROBE_COMPONENTS=unknown"], success=False)
            assert "Unknown Ayni component" in bad, bad
            foreign = root / "foreign"
            shutil.copytree(sdk / "cmake", foreign / "cmake")
            mixed = run(configure + ["-DPROBE_COMPONENTS=core",
                                     f"-DPROBE_SECOND_SOURCE={foreign}"], success=False)
            assert "cannot mix Ayni sources" in mixed, mixed
        finally:
            for path in sdk.rglob("*"):
                if path.is_dir():
                    path.chmod(0o755)
            sdk.chmod(0o755)
    print("Independent SDK consumption passed")


if __name__ == "__main__":
    main()
