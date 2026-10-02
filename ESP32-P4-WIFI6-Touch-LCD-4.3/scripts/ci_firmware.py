#!/usr/bin/env python3
"""Safely select, inspect, and flash one SHA-bound ESP-IDF CI artifact.

This module intentionally uses only the Python standard library.  It is shared
by the Windows and POSIX launchers; no platform wrapper makes policy decisions.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Callable

BOARD = "ESP32-P4-WIFI6-Touch-LCD-4.3"
CHIP = "esp32p4"
BAUD = 921600
MAX_FLASH_BYTES = 32 * 1024 * 1024
MAX_ARCHIVE_BYTES = 128 * 1024 * 1024
MAX_ZIP_ENTRIES = 128
MAX_ZIP_ENTRY_BYTES = 64 * 1024 * 1024
MAX_ZIP_TOTAL_BYTES = 256 * 1024 * 1024
WORKFLOW = "esp-idf-examples.yml"
SHA = re.compile(r"^[0-9a-f]{40}$")
OFFSET = re.compile(r"^0x[0-9a-fA-F]+$")
PROTECTED = {"--port", "-p", "--chip", "-c", "--baud", "-b", "write_flash", "write-flash", "erase_flash", "erase-flash"}
OWNER = re.compile(r"^[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})$")
REPOSITORY = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
PATH_SEGMENT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


class SafetyError(RuntimeError):
    """A required provenance or package-safety condition was not met."""


@dataclass(frozen=True)
class Lane:
    project: str
    version: str
    variant: str
    artifact: str

    @property
    def job_name(self) -> str:
        return f"Build {self.project} ({self.version}, {self.variant})"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def run(command: list[str], cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as error:
        raise SafetyError(f"Required executable is unavailable: {command[0]}") from error
    if completed.returncode:
        raise SafetyError(f"Command failed ({command[0]}): {completed.stderr.strip() or completed.stdout.strip()}")
    return completed.stdout


def resolve_executable(name: str, which: Callable[[str], str | None] = shutil.which) -> str:
    """Resolve a tool without relying on checkout- or user-specific paths."""
    found = which(name)
    if found:
        return found
    if os.name == "nt":
        candidates: list[Path] = []
        for variable in ("ProgramFiles", "ProgramW6432", "LOCALAPPDATA"):
            root = os.environ.get(variable)
            if root:
                candidates.extend((Path(root) / "Git" / "cmd" / f"{name}.exe", Path(root) / "GitHub CLI" / f"{name}.exe"))
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, rf"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{name}.exe") as key:
                candidates.insert(0, Path(winreg.QueryValueEx(key, None)[0]))
        except (ImportError, OSError):
            pass
        for candidate in candidates:
            if candidate.is_file(): return str(candidate)
    raise SafetyError(f"Required executable is unavailable: {name}")


def parse_github_origin(origin: str) -> str:
    """Return owner/repository from supported GitHub origin forms only."""
    value = origin.strip()
    patterns = (
        r"^https://github\.com/([^/\s]+)/([^/\s]+?)(?:\.git)?/?$",
        r"^ssh://git@github\.com/([^/\s]+)/([^/\s]+?)(?:\.git)?/?$",
        r"^git@github\.com:([^/\s]+)/([^/\s]+?)(?:\.git)?$",
    )
    for pattern in patterns:
        match = re.fullmatch(pattern, value)
        if match:
            owner, name = match.groups()
            if OWNER.fullmatch(owner) and REPOSITORY.fullmatch(name):
                return f"{owner}/{name}"
    raise SafetyError("origin must be a GitHub HTTPS, ssh://git@github.com, or git@github.com remote")


def local_identity(root: Path) -> tuple[str, str, str]:
    git = resolve_executable("git")
    origin = run([git, "remote", "get-url", "origin"], root).strip()
    branch = run([git, "symbolic-ref", "--quiet", "--short", "HEAD"], root).strip()
    head = run([git, "rev-parse", "HEAD"], root).strip().lower()
    if not branch or not SHA.fullmatch(head):
        raise SafetyError("a clean, non-detached branch with a full local HEAD SHA is required")
    if run([git, "status", "--porcelain=v1", "--untracked-files=all"], root).strip():
        raise SafetyError("the working tree must be clean before selecting CI firmware")
    return parse_github_origin(origin), branch, head


def load_catalog(root: Path | None = None) -> list[Lane]:
    """Derive the CI artifact catalog from the authoritative discovery module."""
    root = root or repo_root()
    path = root / ".github" / "scripts" / "discover_esp_idf_examples.py"
    spec = importlib.util.spec_from_file_location("ci_discovery", path)
    if not spec or not spec.loader:
        raise SafetyError("unable to load the repository CI discovery module")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    matrix = module.build_matrix(module.list_examples(root))["include"]
    lanes = [Lane(item["example"], item["idf_version"], item["variant"], item["artifact_name"]) for item in matrix]
    if not lanes or len({lane.artifact for lane in lanes}) != len(lanes):
        raise SafetyError("the active discovery/workflow matrix must derive a non-empty set of unique artifacts")
    return lanes


def auth_mode(which: Callable[[str], str | None] = shutil.which, environ: dict[str, str] | None = None,
              gh_authenticated: Callable[[], bool] | None = None) -> tuple[str, str | None]:
    environ = environ if environ is not None else os.environ
    try: gh_available = bool(which("gh") or (which is shutil.which and resolve_executable("gh")))
    except SafetyError: gh_available = False
    if gh_available and (gh_authenticated() if gh_authenticated else _gh_authenticated()):
        return "gh", None
    token = environ.get("GH_TOKEN") or environ.get("GITHUB_TOKEN")
    if token:
        return "token", token
    raise SafetyError("authenticated gh or GH_TOKEN/GITHUB_TOKEN is required; anonymous access is not allowed")


def _gh_authenticated() -> bool:
    try:
        return subprocess.run([resolve_executable("gh"), "auth", "status", "--hostname", "github.com"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    except SafetyError: return False


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    """Permit HTTPS redirects but never carry bearer credentials across hosts."""
    def redirect_request(self, request: urllib.request.Request, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> urllib.request.Request:
        destination = urllib.parse.urlsplit(newurl)
        if destination.scheme != "https" or not destination.hostname:
            raise SafetyError("artifact redirect must use HTTPS")
        redirected = super().redirect_request(request, fp, code, msg, headers, newurl)
        if redirected and urllib.parse.urlsplit(request.full_url).hostname != destination.hostname:
            for attribute in ("headers", "unredirected_hdrs"):
                values = getattr(redirected, attribute, None)
                if isinstance(values, dict):
                    values.pop("Authorization", None); values.pop("authorization", None)
        return redirected


def safe_urlopen(request: urllib.request.Request, timeout: int):
    return urllib.request.build_opener(SafeRedirect()).open(request, timeout=timeout)


class GitHub:
    def __init__(self, repo: str, mode: str, token: str | None):
        self.repo, self.mode, self.token = repo, mode, token

    def json(self, endpoint: str) -> dict[str, Any]:
        if self.mode == "gh":
            text = run([resolve_executable("gh"), "api", endpoint])
            return json.loads(text)
        request = urllib.request.Request("https://api.github.com/" + endpoint.lstrip("/"), headers={
            "Accept": "application/vnd.github+json", "Authorization": f"Bearer {self.token}", "X-GitHub-Api-Version": "2022-11-28"})
        try:
            with safe_urlopen(request, timeout=30) as response:
                return json.loads(response.read().decode("utf-8"))
        except urllib.error.URLError as error:
            raise SafetyError(f"GitHub API request failed: {error}") from error

    def bytes(self, endpoint: str) -> bytes:
        if self.mode == "gh":
            completed = subprocess.run([resolve_executable("gh"), "api", endpoint], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if completed.returncode:
                raise SafetyError("authenticated artifact download failed")
            return completed.stdout
        request = urllib.request.Request("https://api.github.com/" + endpoint.lstrip("/"), headers={
            "Accept": "application/vnd.github+json", "Authorization": f"Bearer {self.token}", "X-GitHub-Api-Version": "2022-11-28"})
        try:
            with safe_urlopen(request, timeout=60) as response:
                return response.read()
        except urllib.error.URLError as error:
            raise SafetyError(f"GitHub artifact download failed: {error}") from error


def get_all(api: GitHub, endpoint: str, key: str) -> list[dict[str, Any]]:
    values: list[dict[str, Any]] = []
    page = 1
    while True:
        joiner = "&" if "?" in endpoint else "?"
        payload = api.json(f"{endpoint}{joiner}per_page=100&page={page}")
        page_values = payload.get(key)
        if not isinstance(page_values, list):
            raise SafetyError(f"GitHub response lacks {key}")
        values.extend(page_values)
        if len(page_values) < 100:
            return values
        page += 1


def select_run(api: GitHub, branch: str, head: str, run_id: int | None) -> dict[str, Any]:
    if run_id is not None:
        candidate = api.json(f"repos/{api.repo}/actions/runs/{run_id}")
    else:
        runs = get_all(api, f"repos/{api.repo}/actions/workflows/{WORKFLOW}/runs?branch={urllib.parse.quote(branch, safe='')}", "workflow_runs")
        branch_runs = [r for r in runs if r.get("head_branch") == branch]
        if not branch_runs:
            raise SafetyError("no workflow run exists for the current branch")
        candidate = max(branch_runs, key=lambda value: value.get("created_at", ""))
    path = str(candidate.get("path", ""))
    if (candidate.get("head_branch") != branch or str(candidate.get("head_sha", "")).lower() != head or
            candidate.get("status") != "completed" or candidate.get("conclusion") != "success" or
            not (path.endswith("/" + WORKFLOW) or path == WORKFLOW)):
        raise SafetyError("newest/selected workflow run is not this branch's completed successful current-HEAD run")
    return candidate


def verify_run_coverage(api: GitHub, run_id: int, lanes: list[Lane], head: str | None = None) -> list[dict[str, Any]]:
    jobs = get_all(api, f"repos/{api.repo}/actions/runs/{run_id}/jobs", "jobs")
    preflight = [job for job in jobs if job.get("name") == "Preflight and route changes"]
    expected_jobs = {lane.job_name for lane in lanes}
    builds = [job for job in jobs if str(job.get("name", "")).startswith("Build ")]
    lane_count = len(lanes)
    if (len(jobs) != lane_count + 1 or len(preflight) != 1 or preflight[0].get("status") != "completed" or preflight[0].get("conclusion") != "success" or
            len(builds) != lane_count or {job.get("name") for job in builds} != expected_jobs or
            any(job.get("status") != "completed" or job.get("conclusion") != "success" for job in builds)):
        raise SafetyError(f"run does not have exactly one successful preflight and all {lane_count} expected successful build jobs")
    artifacts = get_all(api, f"repos/{api.repo}/actions/runs/{run_id}/artifacts", "artifacts")
    expected_artifacts = {lane.artifact for lane in lanes}
    if (len(artifacts) != lane_count or {item.get("name") for item in artifacts} != expected_artifacts or
            any(item.get("expired") or not isinstance(item.get("size_in_bytes"), int) or item["size_in_bytes"] <= 0 or
                ("workflow_run" in item and (not isinstance(item["workflow_run"], dict) or item["workflow_run"].get("id") != run_id or
                 (isinstance(item["workflow_run"].get("head_sha"), str) and bool(item["workflow_run"]["head_sha"]) and
                  head is not None and item["workflow_run"]["head_sha"] != head))) for item in artifacts)):
        raise SafetyError(f"run does not have exactly {lane_count} unique, non-expired, non-empty expected artifacts")
    return artifacts


def safe_archive_path(value: str) -> str:
    if (not isinstance(value, str) or not value or "\\" in value or value.startswith("/") or
            re.match(r"^[A-Za-z]:", value) or any(ord(character) < 32 or ord(character) == 127 for character in value)):
        raise SafetyError("unsafe ZIP path")
    path = PurePosixPath(value)
    if any(part in ("", ".", "..") or not PATH_SEGMENT.fullmatch(part) for part in path.parts) or path.is_absolute():
        raise SafetyError("unsafe ZIP path")
    return path.as_posix()


def checked_zip_infos(source: zipfile.ZipFile) -> list[zipfile.ZipInfo]:
    seen: set[str] = set()
    manifest_names: list[str] = []
    if len(source.infolist()) > MAX_ZIP_ENTRIES:
        raise SafetyError("ZIP contains too many entries")
    total_size = 0
    for info in source.infolist():
        name = safe_archive_path(info.filename)
        mode = info.external_attr >> 16
        kind = stat.S_IFMT(mode)
        canonical = unicodedata.normalize("NFKC", name).casefold()
        total_size += info.file_size
        if (info.flag_bits & 0x1 or info.file_size < 0 or info.file_size > MAX_ZIP_ENTRY_BYTES or total_size > MAX_ZIP_TOTAL_BYTES or
                info.is_dir() or kind not in (0, stat.S_IFREG) or stat.S_ISLNK(mode) or canonical in seen):
            raise SafetyError("ZIP contains encrypted, symlink, special, oversized, or duplicate canonical entries")
        seen.add(canonical)
        if PurePosixPath(name).name == "manifest.json": manifest_names.append(name)
    return source.infolist()


def extract_zip_safely(archive: Path, destination: Path) -> Path:
    try:
        with zipfile.ZipFile(archive) as source:
            infos = checked_zip_infos(source)
            manifest_names = [safe_archive_path(info.filename) for info in infos if PurePosixPath(safe_archive_path(info.filename)).name == "manifest.json"]
            if manifest_names != ["manifest.json"]:
                raise SafetyError("ZIP must contain exactly one root manifest.json")
            destination.mkdir(parents=True, exist_ok=False)
            for info in infos:
                name = safe_archive_path(info.filename)
                target = destination / name
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(info) as input_file, target.open("xb") as output_file:
                    shutil.copyfileobj(input_file, output_file)
    except (OSError, zipfile.BadZipFile) as error:
        raise SafetyError("unsafe or malformed ZIP") from error
    return destination


def unwrap_artifact_zip(payload: bytes, artifact_name: str) -> bytes:
    """Accept only the Actions wrapper containing the one expected package ZIP."""
    try:
        with zipfile.ZipFile(__import__("io").BytesIO(payload)) as outer:
            infos = checked_zip_infos(outer)
            if len(infos) != 1 or safe_archive_path(infos[0].filename) != f"{artifact_name}.zip":
                raise SafetyError("artifact wrapper must contain exactly the expected package ZIP")
            inner = outer.read(infos[0])
            if not inner or len(inner) > MAX_ARCHIVE_BYTES:
                raise SafetyError("inner package ZIP is empty or exceeds the archive cap")
            return inner
    except (OSError, zipfile.BadZipFile) as error:
        raise SafetyError("unsafe or malformed artifact wrapper") from error


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def validate_args(values: Any, field: str) -> list[str]:
    if not isinstance(values, list) or not all(isinstance(item, str) and item for item in values):
        raise SafetyError(f"manifest {field} must be an array of non-empty strings")
    allowed_pairs = {
        "--before": {"default_reset", "no_reset", "default-reset", "no-reset"},
        "--after": {"hard_reset", "no_reset", "hard-reset", "no-reset"},
    } if field == "flash.esptool_args" else {
        "--flash_mode": {"qio", "qout", "dio", "dout", "keep"}, "--flash_freq": {"keep", "20m", "26m", "40m", "80m"},
        "--flash_size": {"keep", "detect", "2MB", "4MB", "8MB", "16MB", "32MB"}}
    result: list[str] = []
    index = 0
    while index < len(values):
        flag = values[index]
        if field == "flash.esptool_args" and flag == "--stub":
            result.append(flag); index += 1; continue
        if field == "flash.write_args" and flag == "--compress":
            result.append(flag); index += 1; continue
        if flag not in allowed_pairs or index + 1 >= len(values) or values[index + 1] not in allowed_pairs[flag]:
            raise SafetyError(f"manifest {field} has unsupported or positional esptool arguments")
        result.extend((flag, values[index + 1])); index += 2
    return result


def validate_manifest(package: Path, lane: Lane, head: str, run_id: int) -> tuple[list[tuple[int, Path]], list[str], list[str]]:
    try:
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SafetyError("invalid manifest.json") from error
    required = {"schema_version", "board", "chip", "framework", "framework_version", "variant", "target", "project_path", "source_project", "git_sha", "run_sha", "run_id", "flash_size_bytes", "artifact_name", "workflow", "flash", "files"}
    if not isinstance(manifest, dict) or not required.issubset(manifest) or any(manifest[key] != value for key, value in {
        "schema_version": 1, "board": BOARD, "chip": CHIP, "framework": "esp-idf", "framework_version": lane.version,
        "variant": lane.variant, "target": CHIP, "project_path": lane.project, "source_project": lane.project, "git_sha": head, "run_sha": head,
        "run_id": run_id, "flash_size_bytes": MAX_FLASH_BYTES, "artifact_name": lane.artifact, "workflow": WORKFLOW}.items()):
        raise SafetyError("manifest identity does not match the selected current-HEAD CI lane")
    flash = manifest["flash"]
    if not isinstance(flash, dict) or flash.get("baud") != BAUD:
        raise SafetyError("manifest flash settings are invalid")
    esptool_args = validate_args(flash.get("esptool_args"), "flash.esptool_args")
    write_args = validate_args(flash.get("write_args"), "flash.write_args")
    files = manifest["files"]
    if not isinstance(files, list) or not files:
        raise SafetyError("manifest files must be a non-empty array")
    plan: list[tuple[int, Path]] = []
    paths: set[str] = set()
    offsets: set[int] = set()
    ranges: list[tuple[int, int]] = []
    previous_offset = -1
    for item in files:
        if not isinstance(item, dict):
            raise SafetyError("manifest file entry is invalid")
        name, sha, size, raw_offset = item.get("archive_path"), item.get("sha256"), item.get("size"), item.get("offset")
        if (not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha) or type(size) is not int or size <= 0 or
                not isinstance(raw_offset, str) or not OFFSET.fullmatch(raw_offset)):
            raise SafetyError("manifest file hash, size, or offset is invalid")
        name = safe_archive_path(name)
        if name in paths:
            raise SafetyError("manifest contains duplicate canonical file paths")
        paths.add(name)
        path = package / name
        try:
            path.resolve().relative_to(package.resolve())
        except ValueError as error:
            raise SafetyError("manifest file escapes the extracted package") from error
        if not path.is_file() or path.stat().st_size != size or digest(path) != sha:
            raise SafetyError("manifest file does not match its declared hash or size")
        offset = int(raw_offset, 16)
        if offset <= previous_offset or offset in offsets or offset + size > MAX_FLASH_BYTES:
            raise SafetyError("manifest flash offset is duplicate or outside 32 MiB")
        previous_offset = offset; offsets.add(offset); ranges.append((offset, offset + size)); plan.append((offset, path))
    ranges.sort()
    if any(previous[1] > current[0] for previous, current in zip(ranges, ranges[1:])):
        raise SafetyError("manifest flash ranges overlap")
    return sorted(plan), esptool_args, write_args


def enumerate_ports() -> list[str]:
    try:
        import serial.tools.list_ports  # type: ignore[import-not-found]
        return sorted(port.device for port in serial.tools.list_ports.comports())
    except ImportError:
        if os.name == "nt":
            try:
                import winreg
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
                    return sorted({str(winreg.EnumValue(key, index)[1]) for index in range(winreg.QueryInfoKey(key)[1])})
            except (ImportError, OSError):
                return []
        return sorted({str(path) for pattern in ("ttyUSB*", "ttyACM*", "serial/by-id/*") for path in Path("/dev").glob(pattern)})


def choose(prompt: str, values: list[str], explicit: str | None = None) -> str:
    if not values:
        raise SafetyError("no serial ports were discovered; pass --port explicitly")
    if explicit:
        if explicit not in values:
            raise SafetyError(f"unknown selection: {explicit}")
        return explicit
    for index, value in enumerate(values, 1):
        print(f"{index}: {value}")
    raw = input(prompt).strip()
    if not raw.isdigit() or not 1 <= int(raw) <= len(values):
        raise SafetyError("a numbered selection is required")
    return values[int(raw) - 1]


def select_serial_port(explicit: str | None, ports: list[str]) -> str:
    return explicit if explicit else choose("Select serial port number: ", ports)


def require_flash_confirmation(artifact: str, port: str, reader: Callable[[str], str] = input) -> None:
    """Require an exact human confirmation for the one selected artifact."""
    confirmation = reader(f"Type FLASH {artifact} to flash exactly this one item on {port}: ").strip()
    if confirmation != f"FLASH {artifact}":
        raise SafetyError("explicit confirmation was not provided; no flash was attempted")


def download_and_validate(api: GitHub, artifact: dict[str, Any], lane: Lane, head: str, run_id: int, output: Path) -> tuple[Path, list[tuple[int, Path]], list[str], list[str]]:
    artifact_id = artifact.get("id")
    if not isinstance(artifact_id, int):
        raise SafetyError("artifact has no valid ID")
    expected_size = artifact.get("size_in_bytes")
    if type(expected_size) is not int or not 0 < expected_size <= MAX_ARCHIVE_BYTES:
        raise SafetyError("artifact size is invalid or exceeds the archive cap")
    try:
        output.mkdir(parents=True, exist_ok=True)
        invocation = Path(tempfile.mkdtemp(prefix="validate-", dir=output))
        archive = invocation / f"{lane.artifact}.zip"
        payload = api.bytes(f"repos/{api.repo}/actions/artifacts/{artifact_id}/zip")
        if len(payload) != expected_size:
            raise SafetyError("downloaded artifact byte count does not match GitHub metadata")
        archive.write_bytes(unwrap_artifact_zip(payload, lane.artifact))
        package = extract_zip_safely(archive, invocation / "package")
        plan, esptool_args, write_args = validate_manifest(package, lane, head, run_id)
    except (OSError, zipfile.BadZipFile) as error:
        raise SafetyError("artifact download or extraction failed safely") from error
    return archive, plan, esptool_args, write_args


def self_test(root: Path, catalog_only: bool = False) -> None:
    lanes = load_catalog(root)
    if catalog_only:
        for number, lane in enumerate(lanes, 1):
            print(f"{number}: artifact={lane.artifact} project={lane.project} version={lane.version} variant={lane.variant}")
        return
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        archive = directory / "safe.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("manifest.json", "{}")
        extract_zip_safely(archive, directory / "safe")
        for name in ("../bad", "bin\\bad"):
            bad = directory / "bad.zip"
            with zipfile.ZipFile(bad, "w") as output:
                output.writestr(name, "x")
            try:
                extract_zip_safely(bad, directory / ("x" + str(len(name))))
            except SafetyError:
                continue
            raise SafetyError("self-test did not reject unsafe ZIP path")
    print(f"SELF_TEST_OK lanes={len(lanes)} manual-confirmation=true one-item=true")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", choices=("self-test", "list", "preflight", "flash"), default="flash")
    parser.add_argument("--artifact")
    parser.add_argument("--port")
    parser.add_argument("--run-id", type=int)
    parser.add_argument("--catalog-only", action="store_true")
    args = parser.parse_args(argv)
    root = repo_root()
    try:
        if args.command == "self-test":
            self_test(root, args.catalog_only)
            return 0
        repo, branch, head = local_identity(root)
        mode, token = auth_mode()
        api = GitHub(repo, mode, token)
        lanes = load_catalog(root)
        selected_run = select_run(api, branch, head, args.run_id)
        run_id = selected_run.get("id")
        if not isinstance(run_id, int):
            raise SafetyError("workflow run has no ID")
        artifacts = verify_run_coverage(api, run_id, lanes, head)
        print(f"Repository: {repo}\nBranch: {branch}\nHEAD: {head}\nRun: {run_id} {selected_run.get('html_url', '')}")
        by_name = {str(item["name"]): item for item in artifacts}
        if args.command == "list":
            for number, lane in enumerate(lanes, 1):
                print(f"{number}: {lane.artifact} ({lane.project}, {lane.version}, {lane.variant})")
            return 0
        selected_name = choose("Select firmware number: ", [lane.artifact for lane in lanes], args.artifact)
        lane = next(value for value in lanes if value.artifact == selected_name)
        output = root / "ci-firmware" / "downloads" / f"run-{run_id}" / lane.artifact
        archive, plan, esptool_args, write_args = download_and_validate(api, by_name[lane.artifact], lane, head, run_id, output)
        print(f"Validated: {archive.relative_to(root)}")
        if args.command == "preflight":
            print("PREFLIGHT_OK: no serial port or esptool was used")
            return 0
        ports = enumerate_ports()
        port = select_serial_port(args.port, ports)
        require_flash_confirmation(lane.artifact, port)
        result = subprocess.run([sys.executable, "-m", "esptool", "--port", port, "--chip", CHIP, "--baud", str(BAUD), *esptool_args, "write_flash", *write_args, *[part for offset, path in plan for part in (f"0x{offset:x}", str(path))]], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end="")
        if result.returncode or "Hash of data verified" not in result.stdout:
            raise SafetyError("esptool did not complete with 'Hash of data verified'")
        print("Flash completed for one selected item. This does not prove display, touch, audio, networking, or HIL behavior.")
        return 0
    except SafetyError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
