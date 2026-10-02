#!/usr/bin/env python3
"""Package one ESP-IDF CI build as a traceable ESP32-P4 flash archive."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import zipfile
from datetime import datetime, timezone
from pathlib import Path


BOARD = "ESP32-P4-WIFI6-Touch-LCD-4.3"
CHIP = "esp32p4"
BAUD = 921600
MAX_FLASH_BYTES = 32 * 1024 * 1024
WORKFLOW = "esp-idf-examples.yml"
SAFE_SEGMENT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
FRAMEWORK_VERSION = re.compile(r"^v\d+\.\d+\.\d+$")
PROTECTED_ESPTOOL_TOKENS = {
    "--port", "-p", "--chip", "-c", "--baud", "-b",
    "write_flash", "write-flash", "erase_flash", "erase-flash",
}


def slugify(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-") or "firmware"


def safe_single_segment(value: str, description: str, *, version: bool = False) -> str:
    """Require metadata that remains one safe archive-name segment."""
    if (
        not isinstance(value, str)
        or not value
        or any(ord(character) < 32 or ord(character) == 127 for character in value)
        or "/" in value
        or "\\" in value
        or ".." in value
        or not SAFE_SEGMENT.fullmatch(value)
        or (version and not FRAMEWORK_VERSION.fullmatch(value))
    ):
        raise ValueError(f"unsafe {description}: {value!r}")
    return value


def reject_protected_esptool_tokens(values: list[str], description: str) -> list[str]:
    for value in values:
        token = value.lower().split("=", 1)[0]
        if token in PROTECTED_ESPTOOL_TOKENS:
            raise ValueError(f"flasher_args.json {description} must not override {token}")
    return values


def strip_verified_chip_option(values: list[str]) -> list[str]:
    """Accept exactly one generated ESP32-P4 chip option, then own it ourselves."""
    result: list[str] = []
    chips: list[str] = []
    index = 0
    while index < len(values):
        value = values[index]
        if value.lower() == "--chip":
            if index + 1 >= len(values):
                raise ValueError("flasher_args.json --chip is missing its value")
            chips.append(values[index + 1])
            index += 2
        elif value.lower().startswith("--chip="):
            chips.append(value.split("=", 1)[1])
            index += 1
        else:
            result.append(value)
            index += 1
    if len(chips) != 1 or chips[0].lower() != CHIP:
        raise ValueError("flasher_args.json must contain exactly one --chip esp32p4 option")
    return result


def contained_path(root: Path, candidate: Path, description: str) -> Path:
    root = root.resolve()
    candidate = candidate.resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise ValueError(f"{description} resolves outside {root}: {candidate}") from error
    return candidate


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_offset(value: str | int) -> int:
    result = int(str(value), 0)
    if result < 0:
        raise ValueError(f"negative flash offset: {value}")
    return result


def manifest_git_sha() -> str:
    value = os.environ.get("PACKAGE_GIT_SHA") or os.environ.get("GITHUB_SHA")
    if not value:
        try:
            value = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL
            ).strip()
        except (OSError, subprocess.CalledProcessError):
            value = "unknown"
    if os.environ.get("CI", "").lower() == "true" and not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise ValueError("CI packaging requires a complete 40-character PACKAGE_GIT_SHA or GITHUB_SHA")
    return value.lower() if re.fullmatch(r"[0-9a-fA-F]{40}", value) else value


def manifest_run_id() -> int:
    value = os.environ.get("PACKAGE_RUN_ID", "")
    if not re.fullmatch(r"[1-9][0-9]*", value):
        raise ValueError("packaging requires a positive PACKAGE_RUN_ID")
    return int(value)


def archive_name(path: Path, used: set[str]) -> str:
    candidate = f"bin/{path.name}"
    stem, suffix, counter = path.stem, path.suffix, 2
    while candidate in used:
        candidate = f"bin/{stem}-{counter}{suffix}"
        counter += 1
    used.add(candidate)
    return candidate


def generated_esptool_args(values: object, framework_version: str) -> list[str]:
    if not isinstance(values, dict) or set(values) - {"before", "after", "stub", "chip"}:
        raise ValueError("flasher_args.json extra_esptool_args has unsupported fields")
    if values.get("chip") != CHIP or not isinstance(values.get("stub", False), bool):
        raise ValueError("flasher_args.json must specify esp32p4 and a boolean stub setting")
    result: list[str] = []
    idf_v5 = framework_version.startswith("v5.")
    for name, allowed in (
        ("before", {"default_reset", "no_reset", "default-reset", "no-reset"}),
        ("after", {"hard_reset", "no_reset", "hard-reset", "no-reset"}),
    ):
        value = values.get(name)
        if value is not None:
            if value not in allowed or (idf_v5 and "-" in value):
                raise ValueError(f"unsupported {name} setting")
            result.extend((f"--{name}", value))
    if values.get("stub"):
        result.append("--stub")
    return result


def generated_write_args(values: object) -> list[str]:
    allowed = {"flash_mode": {"qio", "qout", "dio", "dout", "keep"}, "flash_freq": {"keep", "20m", "26m", "40m", "80m"}, "flash_size": {"keep", "detect", "2MB", "4MB", "8MB", "16MB", "32MB"}, "compress": {True, False}}
    if not isinstance(values, dict) or set(values) - set(allowed):
        raise ValueError("flasher_args.json flash_settings has unsupported fields")
    result: list[str] = []
    for name in ("flash_mode", "flash_freq", "flash_size"):
        if name in values:
            if values[name] not in allowed[name]: raise ValueError(f"unsupported {name} setting")
            result.extend((f"--{name}", values[name]))
    if values.get("compress") is True: result.append("--compress")
    if "compress" in values and type(values["compress"]) is not bool: raise ValueError("compress must be boolean")
    return result


def flash_helpers(files: list[dict[str, object]], esptool_args: list[str], write_args: list[str]) -> tuple[str, str, str]:
    pairs = " ".join(f"{item['offset']} {item['archive_path']}" for item in files)
    quoted_pairs = " ".join(f"{item['offset']} \"$SCRIPT_DIR/{item['archive_path']}\"" for item in files)
    batch_pairs = " ".join(
        f"{item['offset']} \"%~dp0{str(item['archive_path']).replace('/', chr(92))}\"" for item in files
    )
    global_args = " ".join(esptool_args)
    write = " ".join(write_args)
    command = f"python -m esptool --chip {CHIP} --baud {BAUD} {global_args} write_flash {write} {pairs}".replace("  ", " ")
    shell = (
        "#!/usr/bin/env sh\nset -eu\n"
        'SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
        f"python -m esptool \"$@\" --chip {CHIP} --baud {BAUD} {global_args} write_flash {write} {quoted_pairs}\n".replace("  ", " ")
    )
    batch = (
        "@echo off\r\n"
        f"python -m esptool %* --chip {CHIP} --baud {BAUD} {global_args} write_flash {write} {batch_pairs}\r\n".replace("  ", " ")
        + "if errorlevel 1 exit /b %errorlevel%\r\n"
    )
    return command, shell, batch


def package_esp_idf(project: Path, build_dir: Path, framework_version: str, variant: str, output_dir: Path) -> Path:
    framework_version = safe_single_segment(framework_version, "framework version", version=True)
    variant = safe_single_segment(variant, "variant")
    repo_root = Path.cwd().resolve()
    examples_root = repo_root / "examples" / "esp-idf"
    project = contained_path(examples_root, project, "project")
    if not (project / "CMakeLists.txt").is_file() or not (project / "main").is_dir():
        raise ValueError("project must be a direct ESP-IDF product project under examples/esp-idf")
    if project.parent != examples_root:
        raise ValueError("project must be a direct child of examples/esp-idf")
    build_dir = build_dir.resolve()
    args_path = contained_path(build_dir, build_dir / "flasher_args.json", "flasher arguments")
    if not args_path.is_file():
        raise FileNotFoundError(f"ESP-IDF flasher arguments not found: {args_path}")
    raw_args = json.loads(args_path.read_text(encoding="utf-8"))
    flash_files = raw_args.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise ValueError("flasher_args.json must contain a non-empty flash_files object")
    esptool_args = generated_esptool_args(raw_args.get("extra_esptool_args", {}), framework_version)
    write_args = generated_write_args(raw_args.get("flash_settings", {}))
    records: list[dict[str, object]] = []
    sources: list[tuple[Path, str]] = []
    used_names: set[str] = set()
    ranges: list[tuple[int, int]] = []
    offsets: set[int] = set()
    for raw_offset, raw_path in sorted(flash_files.items(), key=lambda item: parse_offset(item[0])):
        offset = parse_offset(raw_offset)
        if offset in offsets:
            raise ValueError(f"duplicate flash offset: {raw_offset}")
        source = contained_path(build_dir, build_dir / str(raw_path), "ESP-IDF flasher binary")
        if not source.is_file():
            raise FileNotFoundError(f"Referenced ESP-IDF binary not found: {source}")
        size = source.stat().st_size
        if size <= 0 or offset + size > MAX_FLASH_BYTES:
            raise ValueError(f"unsafe flash range for {source.name}")
        ranges.append((offset, offset + size))
        offsets.add(offset)
        name = archive_name(source, used_names)
        records.append({"offset": f"0x{offset:x}", "archive_path": name, "size": size, "sha256": sha256(source)})
        sources.append((source, name))
    for (_, previous_end), (current_start, _) in zip(sorted(ranges), sorted(ranges)[1:]):
        if previous_end > current_start:
            raise ValueError("flasher_args.json contains overlapping flash ranges")
    command, shell, batch = flash_helpers(records, esptool_args, write_args)
    project_path = project.relative_to(repo_root).as_posix()
    git_sha = manifest_git_sha()
    run_id = manifest_run_id()
    manifest = {
        "schema_version": 1,
        "board": BOARD,
        "chip": CHIP,
        "framework": "esp-idf",
        "framework_version": framework_version,
        "variant": variant,
        "target": CHIP,
        "project_path": project_path,
        "source_project": project_path,
        "git_sha": git_sha,
        "run_sha": git_sha,
        "run_id": run_id,
        "flash_size_bytes": MAX_FLASH_BYTES,
        "artifact_name": f"firmware-esp-idf-{slugify(project.name)}-{framework_version}-{slugify(variant)}",
        "workflow": WORKFLOW,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "flash": {"baud": BAUD, "esptool_args": esptool_args, "write_args": write_args, "command": command},
        "files": records,
    }
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = contained_path(
        output_dir,
        output_dir / f"firmware-esp-idf-{slugify(project.name)}-{framework_version}-{slugify(variant)}.zip",
        "package output",
    )
    with zipfile.ZipFile(output_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for source, name in sources:
            archive.write(source, name)
        archive.write(args_path, "metadata/flasher_args.json")
        flash_args = build_dir / "flash_args"
        if flash_args.is_file():
            archive.write(contained_path(build_dir, flash_args, "flash arguments"), "metadata/flash_args")
        archive.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        archive.writestr("flash.sh", shell)
        archive.writestr("flash.bat", batch)
    return output_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--framework-version", required=True)
    parser.add_argument("--variant", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    print(package_esp_idf(args.project, args.build_dir, args.framework_version, args.variant, args.output_dir).as_posix())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
