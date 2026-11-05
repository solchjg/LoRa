"""Host tests of the scanner's pure core (components/scan_core)."""

import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CORE = ROOT / "components" / "scan_core"


@pytest.fixture(scope="module")
def binary(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    out = tmp_path_factory.mktemp("scan_core") / "scan_core_test"
    subprocess.run(
        [cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
         "-I", str(CORE / "include"), str(CORE / "scan_core.c"),
         str(pathlib.Path(__file__).with_name("scan_core_test.c")), "-o", str(out)],
        check=True,
    )
    return out


def test_scan_core(binary):
    run = subprocess.run([str(binary)], capture_output=True, text=True, check=False)
    assert run.returncode == 0, run.stdout
