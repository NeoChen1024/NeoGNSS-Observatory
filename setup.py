# SPDX-License-Identifier: GPL-3.0-only
"""Build the native batch-processing extension with the project's CMake targets."""

import subprocess
import sys
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext


class CMakeBuild(build_ext):
    def build_extension(self, ext):
        root = Path(__file__).parent.resolve()
        build = Path(self.build_temp).resolve() / "cmake"
        output = Path(self.get_ext_fullpath(ext.name)).resolve().parent
        subprocess.run(
            [
                "cmake",
                "-S",
                str(root),
                "-B",
                str(build),
                "-DCMAKE_BUILD_TYPE=Release",
                "-DBUILD_TESTING=OFF",
                "-DCPPGNSS_BUILD_EXAMPLES=OFF",
                "-DNEOGNSS_BUILD_RINEX_TOOLS=OFF",
                f"-DPython3_EXECUTABLE={sys.executable}",
                f"-DPython_EXECUTABLE={sys.executable}",
                f"-DNEOGNSS_PYTHON_OUTPUT={output}",
            ],
            check=True,
        )
        subprocess.run(["cmake", "--build", str(build), "--target", "_native", "--parallel", "4"], check=True)


setup(ext_modules=[Extension("neognss_observatory._native", sources=[])], cmdclass={"build_ext": CMakeBuild})
