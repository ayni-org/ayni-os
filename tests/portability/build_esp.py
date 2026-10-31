"""Build the identical normal application with the pinned ESP-IDF, without running a device."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
from source_sdk import source_sdk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-revision", default="working-tree")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir()  # A new directory only; preserve all earlier artifacts/media.
    for relative in ("common", "esp32_s3"):
        shutil.copytree(args.source / "tests/portability" / relative, output / "consumer" / relative,
                        ignore=shutil.ignore_patterns("managed_components", "sdkconfig", "sdkconfig.old"))
    project = output / "consumer/esp32_s3"
    sdk = output / "sdk"
    with source_sdk(args.source, sdk, "esp_idf"):
        build = output / "build"
        subprocess.run(["idf.py", "--ccache", "-B", str(build), "-D", f"AYNI_SOURCE_DIR={sdk}",
                        "-D", f"AYNI_SOURCE_REVISION={args.source_revision}", "reconfigure"], cwd=project, check=True)
        # idf.py invokes Ninja directly and does not honor CMAKE_BUILD_PARALLEL_LEVEL.
        # CMake --build applies the explicit job count to the actual build tool.
        jobs = os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "2")
        subprocess.run(["cmake", "--build", str(build), "--parallel", jobs], check=True)
        subprocess.run(["cmake", "--build", str(build), "--target", "size", "--parallel", jobs], check=True)
    print(f"Independent ESP compile passed; no hardware execution: {output}")


if __name__ == "__main__":
    main()
