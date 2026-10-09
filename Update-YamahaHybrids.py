#!/usr/bin/env python3
"""Native Linux installer for the existing RSA-signed Yamaha update protocol."""
import argparse
import ast
import base64
from collections import deque
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

UPDATER_VERSION = (1, 2, 0)
METADATA_LIMIT = 1024 * 1024
PACKAGE_LIMIT = 100 * 1024 * 1024
EXTRACTED_LIMIT = 150 * 1024 * 1024
ENTRY_LIMIT = 40
PRODUCT_FILES = {"README.html", "TESTER-NOTES.md", "Mu2026 Hybrid.ini",
                 "inst_mu2026_hybrid.ini", "Mu2026 Hybrid.reabank"}
UPDATER_FILES = {"Update Yamaha Hybrids.cmd", "Update-YamahaHybrids.ps1",
                 "Update-YamahaHybrids.sh", "Update-YamahaHybrids.py",
                 "yamaha-update-products.json"}


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r"[vV]?\d+\.\d+\.\d+", value):
        raise ValueError("Invalid release version")
    return tuple(int(part) for part in value.lstrip("vV").split("."))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_json(data):
    def unique_pairs(pairs):
        result = {}
        for key, val in pairs:
            if key in result:
                raise ValueError("Duplicate JSON property")
            result[key] = val
        return result
    return json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique_pairs)


def load_products(directory):
    data = (directory / "yamaha-update-products.json").read_bytes()
    if len(data) > METADATA_LIMIT:
        raise ValueError("Product definitions are unexpectedly large")
    products = read_json(data)
    if not isinstance(products, list) or len(products) != 3:
        raise ValueError("Invalid updater product definitions")
    return products


def https_url(url):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError("Update addresses must use HTTPS without embedded credentials")
    return parsed


class HttpsRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        https_url(newurl)
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        # Never forward an optional API credential to a redirected asset host.
        if new is not None:
            new.remove_header("Authorization")
        return new


def download(url, limit):
    parsed = https_url(url)
    headers = {"User-Agent": "OnjResearch-YamahaHybridUpdater/1.2.0", "Cache-Control": "no-cache"}
    token = os.environ.get("GH_TOKEN", "").strip()
    if parsed.hostname == "api.github.com" and token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(url, headers=headers)
    opener = urllib.request.build_opener(HttpsRedirect())
    with opener.open(request, timeout=120) as response:
        https_url(response.url)
        size = response.headers.get("Content-Length")
        if size is not None and int(size) > limit:
            raise ValueError("Update download exceeds its size limit")
        data = bytearray()
        deadline = time.monotonic() + 360
        while True:
            if time.monotonic() > deadline:
                raise ValueError("Update download exceeded its time limit")
            chunk = response.read(min(65536, limit - len(data) + 1))
            if not chunk:
                return bytes(data)
            data.extend(chunk)
            if len(data) > limit:
                raise ValueError("Update download exceeds its size limit")


def select_release(releases):
    if not isinstance(releases, list):
        raise ValueError("Invalid GitHub release list")
    candidates = []
    for release in releases:
        if not isinstance(release, dict) or release.get("draft", True) or release.get("prerelease", True):
            continue
        try:
            candidates.append((version(release.get("tag_name")), release))
        except ValueError:
            continue
    return max(candidates, key=lambda item: item[0])[1] if candidates else None


def release_for(product):
    url = "https://api.github.com/repos/" + product["Repository"] + "/releases?per_page=20"
    return select_release(read_json(download(url, METADATA_LIMIT)))


def asset_url(release, name):
    matches = [asset for asset in release.get("assets", []) if asset.get("name") == name]
    if len(matches) != 1:
        raise ValueError("Missing or duplicate release asset: " + name)
    url = matches[0]["browser_download_url"]
    https_url(url)
    return url


def der(tag, payload):
    length = len(payload)
    if length < 128:
        prefix = bytes([length])
    else:
        encoded = length.to_bytes((length.bit_length() + 7) // 8, "big")
        prefix = bytes([128 + len(encoded)]) + encoded
    return bytes([tag]) + prefix + payload


def public_pem(product):
    if "PublicKeyPem" in product:
        return product["PublicKeyPem"]
    xml = ET.fromstring(product["PublicKeyXml"])
    integers = []
    for name in ("Modulus", "Exponent"):
        val = base64.b64decode(xml.findtext(name), validate=True).lstrip(b"\0")
        if not val:
            raise ValueError("Invalid RSA verification key")
        if val[0] & 128:
            val = b"\0" + val
        integers.append(der(2, val))
    payload = base64.b64encode(der(48, b"".join(integers))).decode("ascii")
    return "-----BEGIN RSA PUBLIC KEY-----\n" + "\n".join(
        payload[i:i + 64] for i in range(0, len(payload), 64)) + "\n-----END RSA PUBLIC KEY-----\n"


def verify_manifest(raw, signature, expected, product, workspace):
    if len(raw) > METADATA_LIMIT or len(signature) > 65536:
        raise ValueError("Oversized signed metadata")
    try:
        decoded = base64.b64decode(signature.strip(), validate=True)
    except ValueError as exc:
        raise ValueError("Invalid update manifest signature") from exc
    if not decoded or len(decoded) > 1024:
        raise ValueError("Invalid update manifest signature")
    if not shutil.which("openssl"):
        raise ValueError("OpenSSL is required for signature verification; nothing was installed")
    with tempfile.TemporaryDirectory(prefix="verify-", dir=workspace) as tmp:
        root = Path(tmp)
        (root / "manifest").write_bytes(raw)
        (root / "signature").write_bytes(decoded)
        (root / "public.pem").write_text(public_pem(product), encoding="ascii")
        checked = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(root / "public.pem"),
                                  "-signature", str(root / "signature"), str(root / "manifest")],
                                 capture_output=True, timeout=30)
        if checked.returncode != 0:
            raise ValueError("Invalid update manifest signature; nothing was installed")
    manifest = read_json(raw)
    if manifest.get("schemaVersion") != 1 or manifest.get("product") != product["Id"]:
        raise ValueError("Signed manifest belongs to another product or schema")
    if version(manifest.get("version")) != expected:
        raise ValueError("Signed manifest version does not match the release")
    version(manifest.get("updaterVersion"))
    package = manifest["package"]
    https_url(package["url"])
    if not re.fullmatch(r"[a-fA-F0-9]{64}", package.get("sha256", "")):
        raise ValueError("Invalid signed package hash")
    files = manifest.get("files")
    if not isinstance(files, list) or not 1 <= len(files) <= ENTRY_LIMIT:
        raise ValueError("Invalid signed file count")
    allowed = {"target": set(product["AllowedTargetFiles"]),
               "product": PRODUCT_FILES, "updater": UPDATER_FILES}
    seen = set()
    for file in files:
        scope, name = file.get("scope"), file.get("name")
        if not isinstance(name, str) or not name or any(char in name for char in '/\\:\0') or name in (".", ".."):
            raise ValueError("Signed manifest contains an unsafe filename")
        if scope not in allowed or name not in allowed[scope]:
            raise ValueError("Signed manifest contains an unexpected file: " + str(scope) + "/" + name)
        if not re.fullmatch(r"[a-fA-F0-9]{64}", file.get("sha256", "")):
            raise ValueError("Invalid signed file hash")
        identity = (scope + "/" + name).lower()
        if identity in seen:
            raise ValueError("Signed manifest contains a duplicate file")
        seen.add(identity)
    for name in (product["AnchorFile"], product["VersionFile"]):
        if "target/" + name.lower() not in seen:
            raise ValueError("Signed manifest omits required synth files")
    return manifest


def safe_path(path):
    path = Path(os.path.abspath(path))
    for item in (path, *path.parents):
        if item.is_symlink():
            raise ValueError("Refusing a symbolic-link path: " + str(item))
    return path


def find_targets(root, products):
    root = safe_path(root)
    if not root.is_dir():
        raise ValueError("Search folder not found: " + str(root))
    queue, targets, visited = deque([(root, 0)]), [], 0
    while queue:
        directory, depth = queue.popleft()
        visited += 1
        if visited > 250:
            raise ValueError("Search exceeds 250 directories; use a narrower folder")
        for product in products:
            anchor = directory / product["AnchorFile"]
            if anchor.is_file():
                safe_path(anchor)
                targets.append({"product": product, "runtime": directory,
                                "product_dir": directory.parent if directory.name.lower() == "vst" else directory})
        if depth < 2:
            for child in sorted(directory.iterdir()):
                if child.is_dir() and not child.is_symlink():
                    queue.append((child, depth + 1))
    return targets


def installed_version(target):
    path = safe_path(target["runtime"] / target["product"]["VersionFile"])
    if not path.exists():
        return (0, 0, 0)
    if path.stat().st_size > METADATA_LIMIT:
        raise ValueError("Installed version metadata is oversized")
    marker = read_json(path.read_bytes())
    if marker.get("product") != target["product"]["Id"]:
        raise ValueError("Installed version metadata belongs to another product")
    return version(marker.get("version"))


def destinations(target, scripts, files):
    roots = {"target": target["runtime"], "product": target["product_dir"], "updater": scripts}
    result, seen = [], set()
    for file in files:
        path = safe_path(roots[file["scope"]] / file["name"])
        if path in seen or (path.exists() and not path.is_file()):
            raise ValueError("Duplicate or non-file update destination: " + str(path))
        seen.add(path)
        result.append((file, path))
    return result


def package_contents(package, manifest):
    if len(package) > PACKAGE_LIMIT or digest(package) != manifest["package"]["sha256"].lower():
        raise ValueError("Update package failed its signed hash or size check")
    expected = {file["scope"] + "/" + file["name"]: file for file in manifest["files"]}
    result = {}
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        entries = archive.infolist()
        if len(entries) != len(expected) or sum(e.file_size for e in entries) > EXTRACTED_LIMIT:
            raise ValueError("Update archive does not match its signed file list or size limit")
        for entry in entries:
            mode = entry.external_attr >> 16
            if (entry.filename not in expected or entry.filename in result or entry.is_dir()
                    or (stat.S_IFMT(mode) not in (0, stat.S_IFREG)) or entry.flag_bits & 1):
                raise ValueError("Update archive contains an unexpected path or non-regular entry")
            with archive.open(entry) as stream:
                data = stream.read(entry.file_size + 1)
            if len(data) != entry.file_size or digest(data) != expected[entry.filename]["sha256"].lower():
                raise ValueError("Update file hash verification failed: " + entry.filename)
            if entry.filename.endswith(".py"):
                ast.parse(data.decode("utf-8"))
            elif entry.filename.endswith(".json"):
                read_json(data)
            result[entry.filename] = data
    return result


def atomic_write(path, data, mode):
    safe_path(path)
    fd, name = tempfile.mkstemp(prefix="." + path.name + "-", suffix=".new", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(name, mode)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def trim_state(directory, keep):
    for path in sorted(directory.glob("*.zip"), key=lambda p: p.stat().st_mtime, reverse=True):
        if path != keep:
            safe_path(path).unlink()


def install(target, manifest, package, scripts, state, writer=atomic_write):
    contents = package_contents(package, manifest)
    product = target["product"]
    if not safe_path(target["runtime"] / product["AnchorFile"]).is_file():
        raise ValueError("Synth is no longer installed")
    previous = installed_version(target)
    files = [file for file in manifest["files"] if file["scope"] != "updater"
             or version(manifest["updaterVersion"]) > UPDATER_VERSION]
    plan = destinations(target, scripts, files)
    marker = read_json(contents["target/" + product["VersionFile"]])
    if marker.get("product") != product["Id"] or version(marker.get("version")) != version(manifest["version"]):
        raise ValueError("Package version metadata does not match its signed manifest")
    safe_path(state).mkdir(parents=True, exist_ok=True, mode=0o700)
    backups = safe_path(state / "backups")
    backups.mkdir(exist_ok=True, mode=0o700)
    saved = []
    for file, path in plan:
        if not path.parent.is_dir():
            raise ValueError("Update destination directory is missing: " + str(path.parent))
        existed = path.exists()
        mode = stat.S_IMODE(path.stat().st_mode) if existed else (0o755 if path.suffix == ".sh" else 0o644)
        saved.append((file, path, path.read_bytes() if existed else None, mode))
    if sum(len(item[2]) for item in saved if item[2] is not None) > EXTRACTED_LIMIT:
        raise ValueError("Existing managed files exceed the rollback size limit")
    stamp = time.strftime("%Y%m%d-%H%M%S") + "-" + str(time.time_ns())
    backup = backups / (product["Slug"] + "-" + ".".join(map(str, previous)) + "-" + stamp + ".zip")
    with zipfile.ZipFile(backup, "x", zipfile.ZIP_DEFLATED) as archive:
        records = []
        for file, path, data, mode in saved:
            records.append({"scope": file["scope"], "name": file["name"], "path": str(path),
                            "existed": data is not None, "mode": mode})
            if data is not None:
                archive.writestr(file["scope"] + "/" + file["name"], data)
        archive.writestr("rollback.json", json.dumps({"product": product["Id"], "files": records}))
    os.chmod(backup, 0o600)
    with zipfile.ZipFile(backup) as archive:
        if archive.testzip() is not None:
            raise ValueError("Rollback ZIP failed validation; nothing was installed")
    # Commit the marker last, so an interrupted install never advertises success early.
    ordered = sorted(saved, key=lambda item: item[0]["name"] == product["VersionFile"])
    changed = []
    try:
        for file, path, old, mode in ordered:
            data = contents[file["scope"] + "/" + file["name"]]
            if old == data:
                continue
            changed.append((path, old, mode))
            writer(path, data, mode)
        for file, path in plan:
            if digest(path.read_bytes()) != file["sha256"].lower():
                raise ValueError("Installed file hash mismatch")
        if installed_version(target) != version(manifest["version"]):
            raise ValueError("Installed version verification failed")
    except BaseException:
        try:
            for path, old, mode in reversed(changed):
                if old is None:
                    if path.exists():
                        safe_path(path).unlink()
                else:
                    atomic_write(path, old, mode)
        except BaseException as exc:
            raise RuntimeError("Update and automatic rollback failed. Preserve and restore " + str(backup)) from exc
        raise
    trim_state(backups, backup)
    return backup


def check_running(targets, proc_root=Path("/proc")):
    managed = {str(target["runtime"] / name) for target in targets
               for name in target["product"]["AllowedTargetFiles"]}
    workers = {name for target in targets for name in target["product"]["AllowedTargetFiles"]
               if name.endswith("-worker.exe")}
    for proc in proc_root.iterdir():
        if not proc.name.isdigit() or int(proc.name) == os.getpid():
            continue
        try:
            if proc.stat().st_uid != os.getuid():
                continue
            cmdline = (proc / "cmdline").read_bytes().decode("utf-8", "replace")
            names = [arg.replace("\\", "/").rsplit("/", 1)[-1].strip('"').lower()
                     for arg in cmdline.split("\0") if arg]
            if any(name.lower() in names for name in workers):
                raise ValueError("A synth worker is still running (PID " + proc.name + "); close the audio host")
            # Protected maps on unrelated native programs do not indicate an audio host.
            # Wine hosts normally expose a Windows executable in argv or a wine loader.
            if not any(name.endswith(".exe") or name.startswith("wine") for name in names[:2]):
                continue
            with (proc / "maps").open(errors="replace") as stream:
                for line in stream:
                    fields = line.split(None, 5)
                    if len(fields) == 6:
                        path = fields[5].strip().removesuffix(" (deleted)")
                        path = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), path)
                        if path in managed:
                            raise ValueError("A synth file is loaded (PID " + proc.name + "); close the audio host")
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError as exc:
            raise ValueError("Cannot check your running processes; nothing was installed") from exc


def state_root():
    value = os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local" / "state"))
    root = safe_path(value)
    if not Path(value).is_absolute():
        raise ValueError("XDG_STATE_HOME must be an absolute path")
    return root / "onj-research" / "yamaha-hybrid-updater"


def clean_text(text):
    return re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f-\x9f]", "", text)[:6000]


def main(argv=None):
    parser = argparse.ArgumentParser(description="Update installed Yamaha hybrids using signed stable releases.")
    parser.add_argument("--check-only", action="store_true", help="Check without installing; exit 10 if updates exist")
    parser.add_argument("--install", action="store_true", help="Install without the numbered choice")
    parser.add_argument("--non-interactive", action="store_true", help="Never prompt")
    parser.add_argument("--hosts-closed", action="store_true", help="Confirm all audio hosts using these synths are closed")
    parser.add_argument("--search-directory", type=Path, help="Search this folder and two child levels")
    args = parser.parse_args(argv)
    if sys.platform != "linux":
        parser.error("This updater supports Linux/Wine installs. Use the CMD updater on Windows; macOS is unverified.")
    if sys.version_info < (3, 9):
        parser.error("Python 3.9 or later is required")
    scripts = Path(__file__).resolve().parent
    log = None
    try:
        targets = find_targets(args.search_directory or scripts, load_products(scripts))
        if not targets:
            print("No installed hybrid DLL found within two folder levels.")
            return 2
        pending, releases = [], {}
        for target in targets:
            product = target["product"]
            slug = product["Slug"]
            if slug not in releases:
                releases[slug] = release_for(product)
            release = releases[slug]
            current = installed_version(target)
            print(product["DisplayName"] + ": " + str(target["runtime"]))
            if release is None:
                print("No stable update is published. Research previews are not automatic updates.")
            elif version(release["tag_name"]) <= current:
                print("Up to date: " + ".".join(map(str, current)))
            else:
                print("Available: " + release["tag_name"])
                print(clean_text(release.get("body") or ""))
                pending.append((target, release))
        if not pending:
            return 0
        if args.check_only:
            return 10
        interactive = sys.stdin.isatty() and not args.non_interactive
        if not args.install:
            if not interactive:
                print("Updates available. Rerun interactively, or use --install --hosts-closed.")
                return 10
            print("1. Install all detected updates\n2. Cancel")
            if input("Choose 1 or 2: ").strip() != "1":
                return 0
        if not args.hosts_closed:
            if not interactive:
                raise ValueError("Close all audio hosts, then use --hosts-closed to confirm")
            if input("Close every audio host using these synths. Enter 1 when closed, or 2 to cancel: ").strip() != "1":
                return 0
        check_running(targets)
        root = state_root()
        for folder in (target["product_dir"] for target in targets):
            if root == folder or folder in root.parents:
                raise ValueError("Updater state and rollback ZIPs must be outside the plug-in folders")
        safe_path(root).mkdir(parents=True, exist_ok=True, mode=0o700)
        import fcntl
        lock_path = safe_path(root / "update.lock")
        with lock_path.open("a") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise ValueError("Another Yamaha updater is running") from exc
            logs = safe_path(root / "logs")
            logs.mkdir(exist_ok=True, mode=0o700)
            log_path = logs / ("update-" + str(time.time_ns()) + ".log")
            log = log_path.open("x", encoding="utf-8")
            os.chmod(log_path, 0o600)
            for old in sorted(logs.glob("update-*.log"), reverse=True)[2:]:
                safe_path(old).unlink()
            for target, release in pending:
                product = target["product"]
                expected = version(release["tag_name"])
                tag = ".".join(map(str, expected))
                prefix = product["Slug"] + "-" + tag
                raw = download(asset_url(release, prefix + ".update.json"), METADATA_LIMIT)
                sig = download(asset_url(release, prefix + ".update.sig"), 65536)
                manifest = verify_manifest(raw, sig, expected, product, root)
                package = download(manifest["package"]["url"], PACKAGE_LIMIT)
                check_running(targets)
                identity = digest(str(target["runtime"]).encode("utf-8"))[:16]
                backup = install(target, manifest, package, scripts, root / product["Slug"] / identity)
                message = product["DisplayName"] + " " + tag + " installed. Rollback ZIP: " + str(backup)
                print(message)
                log.write(message + "\n")
                log.flush()
        return 0
    except (Exception, KeyboardInterrupt) as exc:
        message = "Update failed: " + clean_text(str(exc) or "Interrupted")
        print(message, file=sys.stderr)
        if log:
            log.write(message + "\n")
        return 2
    finally:
        if log:
            log.close()


if __name__ == "__main__":
    sys.exit(main())
