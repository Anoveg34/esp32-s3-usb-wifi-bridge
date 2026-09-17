#!/usr/bin/env python3
"""Build and flash both USB stacks: ota_0 = RNDIS, ota_1 = NCM."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IDF_PATH = Path(os.environ.get("IDF_PATH", r"D:\esp\v6.1-beta1\esp-idf"))
IDF_TOOLS_PATH = Path(os.environ.get("IDF_TOOLS_PATH", r"D:\Espressif"))
PYTHON = sys.executable


def find_idf_python() -> Path:
    env = os.environ.get("IDF_PYTHON")
    if env and Path(env).exists():
        return Path(env)

    scripts = "Scripts" if os.name == "nt" else "bin"
    allowed = {"python.exe", "python3.exe", "python3", "python"}
    candidates: list[Path] = []
    python_root = IDF_TOOLS_PATH / "tools" / "python"
    if python_root.exists():
        candidates.extend(sorted(python_root.glob(f"*/venv/{scripts}/python*"), reverse=True))
    candidates.append(IDF_TOOLS_PATH / "python_env" / "idf6.1_py3.12_env" / scripts / "python.exe")

    for cand in candidates:
        if cand.is_file() and cand.name.lower() in allowed:
            return cand

    raise SystemExit(
        "找不到 ESP-IDF 的 Python 虚拟环境。请设置 IDF_TOOLS_PATH，或在 ESP-IDF 终端里运行本脚本。"
    )


def idf_version() -> str:
    header = IDF_PATH / "components" / "esp_common" / "include" / "esp_idf_version.h"
    text = header.read_text(encoding="utf-8", errors="replace")
    major = re.search(r"#define\s+ESP_IDF_VERSION_MAJOR\s+(\d+)", text)
    minor = re.search(r"#define\s+ESP_IDF_VERSION_MINOR\s+(\d+)", text)
    if not major or not minor:
        raise SystemExit(f"无法从 {header} 读取 ESP-IDF 版本")
    return f"{major.group(1)}.{minor.group(1)}"


def apply_idf_export() -> None:
    """Load cmake/ninja/compiler PATH the same way export.ps1 does."""
    cmd = [
        PYTHON,
        str(IDF_PATH / "tools" / "idf_tools.py"),
        "--non-interactive",
        "export",
        "--format",
        "key-value",
    ]
    out = subprocess.check_output(cmd, cwd=ROOT, text=True, env=os.environ.copy())
    for line in out.splitlines():
        if not line or "=" not in line:
            continue
        key, val = line.split("=", 1)
        if key and val:
            os.environ[key] = val


def use_idf_python() -> None:
    global PYTHON
    idf_python = find_idf_python()
    PYTHON = str(idf_python)
    os.environ["IDF_PATH"] = str(IDF_PATH)
    os.environ["IDF_TOOLS_PATH"] = str(IDF_TOOLS_PATH)
    os.environ["IDF_PYTHON_CHECK_CONSTRAINTS"] = "0"
    os.environ["IDF_PYTHON"] = PYTHON
    os.environ["IDF_PYTHON_ENV_PATH"] = str(idf_python.parent.parent)
    os.environ["ESP_IDF_VERSION"] = idf_version()
    scripts = str(idf_python.parent)
    path = os.environ.get("PATH", "")
    if scripts.lower() not in path.lower():
        os.environ["PATH"] = scripts + os.pathsep + path
    print(f"Using IDF Python: {PYTHON}", flush=True)
    print(f"ESP_IDF_VERSION={os.environ['ESP_IDF_VERSION']}", flush=True)
    apply_idf_export()


def run_idf(*args: str, sdkconfig: Path, build_dir: Path) -> None:
    cmd = [
        PYTHON,
        str(IDF_PATH / "tools" / "idf.py"),
        "-B",
        str(build_dir),
        "-D",
        f"SDKCONFIG={sdkconfig}",
        *args,
    ]
    print("+", " ".join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=ROOT)


def patch_usb_mode(src: Path, dst: Path, ncm: bool) -> None:
    text = src.read_text(encoding="utf-8", errors="replace")

    def set_flag(buf: str, key: str, enabled: bool) -> str:
        on = f"{key}=y"
        off = f"# {key} is not set"
        if enabled:
            buf = re.sub(rf"^{re.escape(off)}$", on, buf, flags=re.M)
            if not re.search(rf"^{re.escape(on)}$", buf, flags=re.M):
                buf += f"\n{on}\n"
        else:
            buf = re.sub(rf"^{re.escape(on)}$", off, buf, flags=re.M)
            if not re.search(rf"^{re.escape(off)}$", buf, flags=re.M):
                buf += f"\n{off}\n"
        return buf

    text = set_flag(text, "CONFIG_EXAMPLE_USB_NET_MODE_NCM", ncm)
    text = set_flag(text, "CONFIG_EXAMPLE_USB_NET_MODE_RNDIS", not ncm)
    text = set_flag(text, "CONFIG_TINYUSB_NET_MODE_NCM", ncm)
    text = set_flag(text, "CONFIG_TINYUSB_NET_MODE_ECM_RNDIS", not ncm)
    text = set_flag(text, "CONFIG_PARTITION_TABLE_TWO_OTA_LARGE", True)
    text = set_flag(text, "CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE", False)
    dst.write_text(text, encoding="utf-8")


def part_offset(build_dir: Path, name: str) -> str:
    parttool = IDF_PATH / "components" / "partition_table" / "parttool.py"
    table = build_dir / "partition_table" / "partition-table.bin"
    out = subprocess.check_output(
        [
            PYTHON,
            str(parttool),
            "--partition-table-file",
            str(table),
            "get_partition_info",
            "--partition-name",
            name,
            "--info",
            "offset",
        ],
        cwd=ROOT,
        text=True,
    ).strip()
    return out.splitlines()[-1].strip()


def build() -> tuple[Path, Path]:
    base_sdk = ROOT / "sdkconfig"
    if not base_sdk.exists():
        raise SystemExit("sdkconfig not found; run a normal idf.py build once first")

    rndis_sdk = ROOT / "sdkconfig.rndis"
    ncm_sdk = ROOT / "sdkconfig.ncm"
    patch_usb_mode(base_sdk, rndis_sdk, ncm=False)
    patch_usb_mode(base_sdk, ncm_sdk, ncm=True)

    rndis_dir = ROOT / "build_rndis"
    ncm_dir = ROOT / "build_ncm"
    run_idf("build", sdkconfig=rndis_sdk, build_dir=rndis_dir)
    run_idf("build", sdkconfig=ncm_sdk, build_dir=ncm_dir)
    return rndis_dir, ncm_dir


def sources_newer_than(bin_path: Path) -> bool:
    if not bin_path.exists():
        return True
    newest = bin_path.stat().st_mtime
    roots = [ROOT / "main", ROOT / "sdkconfig.defaults", ROOT / "CMakeLists.txt"]
    files: list[Path] = []
    for item in roots:
        if item.is_file():
            files.append(item)
        elif item.is_dir():
            files.extend(item.rglob("*.c"))
            files.extend(item.rglob("*.h"))
            files.extend(item.rglob("*.html"))
    return any(p.exists() and p.stat().st_mtime > newest for p in files)


def flash(port: str, rndis_dir: Path, ncm_dir: Path) -> None:
    boot = rndis_dir / "bootloader" / "bootloader.bin"
    table = rndis_dir / "partition_table" / "partition-table.bin"
    otadata = rndis_dir / "ota_data_initial.bin"
    rndis_bin = rndis_dir / "esp32-s3-wired-to-wifi.bin"
    ncm_bin = ncm_dir / "esp32-s3-wired-to-wifi.bin"
    for p in (boot, table, otadata, rndis_bin, ncm_bin):
        if not p.exists():
            raise SystemExit(f"missing {p}, run: python tools/dual_fw.py build")

    otadata_off = part_offset(rndis_dir, "otadata")
    ota0 = part_offset(rndis_dir, "ota_0")
    ota1 = part_offset(rndis_dir, "ota_1")

    cmd = [
        PYTHON,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "-p",
        port,
        "-b",
        "460800",
        "--before",
        "default-reset",
        "--after",
        "hard-reset",
        "write-flash",
        "--flash-mode",
        "dio",
        "--flash-size",
        "16MB",
        "--flash-freq",
        "80m",
        "0x0",
        str(boot),
        "0x8000",
        str(table),
        otadata_off,
        str(otadata),
        ota0,
        str(rndis_bin),
        ota1,
        str(ncm_bin),
    ]
    print("+", " ".join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=ROOT)
    print("Flashed ota_0=RNDIS, ota_1=NCM. Default boot is RNDIS; switch on the web page.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["build", "flash", "all"], nargs="?", default="all")
    parser.add_argument("-p", "--port", default=os.environ.get("ESPPORT", "COM40"))
    args = parser.parse_args()

    use_idf_python()

    rndis_dir = ROOT / "build_rndis"
    ncm_dir = ROOT / "build_ncm"
    if args.action in ("build", "all"):
        rndis_dir, ncm_dir = build()
    if args.action in ("flash", "all"):
        rndis_bin = rndis_dir / "esp32-s3-wired-to-wifi.bin"
        if args.action == "flash" and sources_newer_than(rndis_bin):
            print("Source newer than build_rndis firmware, rebuilding both images...", flush=True)
            rndis_dir, ncm_dir = build()
        elif not rndis_bin.exists():
            rndis_dir, ncm_dir = build()
        flash(args.port, rndis_dir, ncm_dir)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        sys.exit(exc.returncode)
