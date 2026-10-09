"""ROM-free tests for the native Linux updater. Run with Python 3.9+."""
import base64
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

SOURCE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("yamaha_updater", SOURCE / "Update-YamahaHybrids.py")
updater = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(updater)


class UpdaterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.keydir = tempfile.TemporaryDirectory()
        cls.key = Path(cls.keydir.name) / "test.key"
        cls.pub = Path(cls.keydir.name) / "test.pub"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
                        "rsa_keygen_bits:2048", "-out", str(cls.key)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["openssl", "pkey", "-in", str(cls.key), "-pubout", "-out",
                        str(cls.pub)], check=True, stdout=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.keydir.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.runtime = self.root / "product" / "VST"
        self.runtime.mkdir(parents=True)
        self.product = dict(updater.load_products(SOURCE)[0])
        self.product["PublicKeyPem"] = self.pub.read_text()
        self.anchor = self.runtime / self.product["AnchorFile"]
        self.anchor.write_bytes(b"old synth")
        self.marker = self.runtime / self.product["VersionFile"]
        self.marker.write_text(json.dumps({"product": self.product["Id"], "version": "0.0.1"}))
        self.scripts = self.root / "scripts"
        self.scripts.mkdir()
        self.target = {"product": self.product, "runtime": self.runtime,
                       "product_dir": self.runtime.parent}
        self.state = self.root / "state"
        self.contents = {
            "target/" + self.product["AnchorFile"]: b"new synth",
            "target/" + self.product["VersionFile"]: json.dumps(
                {"product": self.product["Id"], "version": "0.0.2"}).encode(),
            "product/README.html": b"new manual",
        }

    def tearDown(self):
        self.tmp.cleanup()

    def signed(self, change=None, contents=None):
        contents = self.contents if contents is None else contents
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in contents.items():
                archive.writestr(name, data)
        package = buf.getvalue()
        manifest = {"schemaVersion": 1, "product": self.product["Id"], "version": "0.0.2",
                    "updaterVersion": "1.2.0", "package": {
                        "url": "https://example.org/update.zip", "sha256": updater.digest(package)},
                    "files": [{"scope": name.split("/")[0], "name": name.split("/")[1],
                               "sha256": updater.digest(data)} for name, data in contents.items()]}
        if change:
            change(manifest)
        raw = json.dumps(manifest).encode()
        path = self.root / "manifest"
        path.write_bytes(raw)
        signature = subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(self.key),
                                    str(path)], check=True, capture_output=True).stdout
        return raw, base64.b64encode(signature), package

    def verify(self, raw, sig):
        return updater.verify_manifest(raw, sig, (0, 0, 2), self.product, self.root)

    def test_signature_and_install(self):
        raw, sig, package = self.signed()
        manifest = self.verify(raw, sig)
        backup = updater.install(self.target, manifest, package, self.scripts, self.state)
        self.assertEqual(self.anchor.read_bytes(), b"new synth")
        self.assertEqual(updater.installed_version(self.target), (0, 0, 2))
        self.assertTrue(backup.is_file())
        self.assertEqual(list((self.state / "backups").glob("*.zip")), [backup])
        self.assertFalse(list(self.state.glob("work-*")))

    def test_tampered_manifest_rejected(self):
        raw, sig, _ = self.signed()
        with self.assertRaisesRegex(ValueError, "signature"):
            self.verify(raw + b" ", sig)

    def test_missing_signature_rejected(self):
        raw, _, _ = self.signed()
        with self.assertRaisesRegex(ValueError, "signature"):
            self.verify(raw, b"")

    def test_cross_product_rejected(self):
        raw, sig, _ = self.signed(lambda m: m.update(product="OnjResearch.Mu2026Hybrid"))
        with self.assertRaisesRegex(ValueError, "product"):
            self.verify(raw, sig)

    def test_release_version_rejected(self):
        raw, sig, _ = self.signed(lambda m: m.update(version="0.0.3"))
        with self.assertRaisesRegex(ValueError, "version"):
            self.verify(raw, sig)

    def test_cross_product_file_rejected(self):
        raw, sig, _ = self.signed(lambda m: m["files"][0].update(name="mu2026-hybrid.dll"))
        with self.assertRaisesRegex(ValueError, "unexpected"):
            self.verify(raw, sig)

    def test_traversal_rejected(self):
        raw, sig, _ = self.signed(lambda m: m["files"][0].update(name="../evil.dll"))
        with self.assertRaisesRegex(ValueError, "unsafe"):
            self.verify(raw, sig)

    def test_duplicate_file_rejected(self):
        raw, sig, _ = self.signed(lambda m: m["files"].append(m["files"][0]))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.verify(raw, sig)

    def test_http_package_rejected(self):
        raw, sig, _ = self.signed(lambda m: m["package"].update(url="http://example.org/u.zip"))
        with self.assertRaisesRegex(ValueError, "HTTPS"):
            self.verify(raw, sig)

    def test_package_hash_rejected(self):
        raw, sig, package = self.signed()
        with self.assertRaisesRegex(ValueError, "package"):
            updater.install(self.target, self.verify(raw, sig), package + b"changed",
                            self.scripts, self.state)
        self.assertEqual(self.anchor.read_bytes(), b"old synth")

    def test_file_hash_rejected(self):
        raw, sig, package = self.signed(lambda m: m["files"][0].update(sha256="0" * 64))
        with self.assertRaisesRegex(ValueError, "file hash"):
            updater.install(self.target, self.verify(raw, sig), package, self.scripts, self.state)
        self.assertEqual(self.anchor.read_bytes(), b"old synth")

    def test_unexpected_zip_entry_rejected(self):
        raw, sig, package = self.signed()
        buf = io.BytesIO(package)
        with zipfile.ZipFile(buf, "a") as archive:
            archive.writestr("target/evil.dll", b"bad")
        manifest = self.verify(raw, sig)
        manifest["package"]["sha256"] = updater.digest(buf.getvalue())
        with self.assertRaisesRegex(ValueError, "archive"):
            updater.install(self.target, manifest, buf.getvalue(), self.scripts, self.state)

    def test_symlink_destination_rejected(self):
        if not hasattr(os, "symlink"):
            self.skipTest("OS lacks symbolic links")
        external = self.root / "external"
        external.write_text("preserve")
        self.anchor.unlink()
        try:
            self.anchor.symlink_to(external)
        except OSError:
            self.skipTest("OS does not permit test symbolic links")
        raw, sig, package = self.signed()
        with self.assertRaisesRegex(ValueError, "symbolic"):
            updater.install(self.target, self.verify(raw, sig), package, self.scripts, self.state)
        self.assertEqual(external.read_text(), "preserve")

    def test_failed_replacement_restores_old_and_absent_files(self):
        raw, sig, package = self.signed()
        count = 0
        real_replace = updater.atomic_write

        def fail_second(path, data, mode):
            nonlocal count
            count += 1
            if count == 2:
                raise OSError("simulated replacement failure")
            real_replace(path, data, mode)

        with self.assertRaisesRegex(OSError, "simulated"):
            updater.install(self.target, self.verify(raw, sig), package, self.scripts,
                            self.state, writer=fail_second)
        self.assertEqual(self.anchor.read_bytes(), b"old synth")
        self.assertEqual(updater.installed_version(self.target), (0, 0, 1))
        self.assertFalse((self.runtime.parent / "README.html").exists())
        self.assertFalse(list(self.state.glob("work-*")))

    def test_unrelated_files_and_permissions_preserved(self):
        config = self.runtime / "custom.ini"
        config.write_text("my settings")
        self.anchor.chmod(0o640)
        raw, sig, package = self.signed()
        updater.install(self.target, self.verify(raw, sig), package, self.scripts, self.state)
        self.assertEqual(config.read_text(), "my settings")
        self.assertEqual(self.anchor.stat().st_mode & 0o777, 0o640)

    def test_discovery_depth_and_spaces(self):
        found = updater.find_targets(self.root, [self.product])
        self.assertEqual(len(found), 1)
        deep = self.root / "one" / "two" / "three"
        deep.mkdir(parents=True)
        (deep / self.product["AnchorFile"]).write_text("not detected")
        self.assertEqual(len(updater.find_targets(self.root, [self.product])), 1)

    def test_stable_family_selection(self):
        releases = [{"tag_name": "updater-v1.2.0", "draft": False, "prerelease": False},
                    {"tag_name": "v9.0.0", "draft": False, "prerelease": True},
                    {"tag_name": "v0.0.2", "draft": False, "prerelease": False},
                    {"tag_name": "v0.0.3", "draft": True, "prerelease": False}]
        self.assertEqual(updater.select_release(releases)["tag_name"], "v0.0.2")

    def test_symlink_archive_rejected(self):
        raw, sig, package = self.signed()
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as archive:
            for name, data in self.contents.items():
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = (0o120777 if name.endswith(".dll") else 0o100644) << 16
                archive.writestr(info, data)
        manifest = self.verify(raw, sig)
        manifest["package"]["sha256"] = updater.digest(buf.getvalue())
        with self.assertRaisesRegex(ValueError, "archive"):
            updater.install(self.target, manifest, buf.getvalue(), self.scripts, self.state)

    def test_wrong_installed_metadata_rejected(self):
        self.marker.write_text(json.dumps({"product": "wrong", "version": "0.0.1"}))
        with self.assertRaisesRegex(ValueError, "product"):
            updater.installed_version(self.target)

    def test_missing_openssl_fails_closed(self):
        raw, sig, _ = self.signed()
        with mock.patch.object(updater.shutil, "which", return_value=None):
            with self.assertRaisesRegex(ValueError, "OpenSSL"):
                self.verify(raw, sig)

    def test_duplicate_json_rejected(self):
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            updater.read_json(b'{"version":"0.0.1","version":"0.0.2"}')

    def test_archive_size_limit(self):
        raw, sig, package = self.signed()
        with mock.patch.object(updater, "EXTRACTED_LIMIT", 1):
            with self.assertRaisesRegex(ValueError, "archive"):
                updater.package_contents(package, self.verify(raw, sig))

    def test_archive_duplicate_entry(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as archive:
            archive.writestr("target/" + self.product["AnchorFile"], b"new synth")
            archive.writestr("target/" + self.product["AnchorFile"], b"new synth")
            archive.writestr("product/README.html", b"new manual")
        raw, sig, _ = self.signed()
        manifest = self.verify(raw, sig)
        manifest["package"]["sha256"] = updater.digest(buf.getvalue())
        with self.assertRaisesRegex(ValueError, "archive"):
            updater.package_contents(buf.getvalue(), manifest)

    def test_bad_package_version_leaves_install_untouched(self):
        self.contents["target/" + self.product["VersionFile"]] = json.dumps(
            {"product": self.product["Id"], "version": "0.0.3"}).encode()
        raw, sig, package = self.signed()
        with self.assertRaisesRegex(ValueError, "version metadata"):
            updater.install(self.target, self.verify(raw, sig), package, self.scripts, self.state)
        self.assertEqual(self.anchor.read_bytes(), b"old synth")

    def test_repeated_installs_keep_one_compressed_backup(self):
        raw, sig, package = self.signed()
        manifest = self.verify(raw, sig)
        first = updater.install(self.target, manifest, package, self.scripts, self.state)
        second = updater.install(self.target, manifest, package, self.scripts, self.state)
        self.assertFalse(first.exists())
        self.assertTrue(second.exists())
        self.assertEqual(len(list((self.state / "backups").iterdir())), 1)

    def test_installing_native_updater_sets_shell_executable(self):
        self.contents["updater/Update-YamahaHybrids.sh"] = b"#!/bin/sh\nexit 0\n"
        raw, sig, package = self.signed(lambda m: m.update(updaterVersion="1.3.0"))
        updater.install(self.target, self.verify(raw, sig), package, self.scripts, self.state)
        self.assertEqual((self.scripts / "Update-YamahaHybrids.sh").stat().st_mode & 0o777, 0o755)

    def test_updater_not_downgraded(self):
        self.contents["updater/Update-YamahaHybrids.py"] = b"print('old')\n"
        path = self.scripts / "Update-YamahaHybrids.py"
        path.write_text("print('current')\n")
        raw, sig, package = self.signed(lambda m: m.update(updaterVersion="1.1.0"))
        updater.install(self.target, self.verify(raw, sig), package, self.scripts, self.state)
        self.assertEqual(path.read_text(), "print('current')\n")

    @unittest.skipUnless(os.name == "posix", "Linux process filesystem")
    def test_active_mapped_synth_blocks_update(self):
        proc = self.root / "proc" / "9999999"
        proc.mkdir(parents=True)
        (proc / "cmdline").write_bytes(b"wine\0host.exe\0")
        (proc / "maps").write_text("1000-2000 r-xp 00000000 08:01 1 " + str(self.anchor) + "\n")
        with self.assertRaisesRegex(ValueError, "loaded"):
            updater.check_running([self.target], proc.parent)

    @unittest.skipUnless(os.name == "posix", "Linux process filesystem")
    def test_active_worker_blocks_update(self):
        proc = self.root / "proc" / "9999999"
        proc.mkdir(parents=True)
        (proc / "cmdline").write_bytes(b"wine\0syxg100-vl-worker.exe\0")
        with self.assertRaisesRegex(ValueError, "worker"):
            updater.check_running([self.target], proc.parent)

    def test_symlink_directory_not_discovered(self):
        link = self.root / "link"
        try:
            link.symlink_to(self.runtime, target_is_directory=True)
        except OSError:
            self.skipTest("Symbolic links unavailable")
        self.assertEqual(len(updater.find_targets(self.root, [self.product])), 1)

    @unittest.skipUnless(os.name == "posix", "Linux CLI")
    def test_noninteractive_never_prompts_or_installs(self):
        with mock.patch.object(updater, "find_targets", return_value=[self.target]), \
                mock.patch.object(updater, "release_for", return_value={
                    "tag_name": "v0.0.2", "body": "test notes"}), \
                mock.patch.object(updater.sys.stdin, "isatty", return_value=False), \
                mock.patch("builtins.input", side_effect=AssertionError("Unexpected prompt")), \
                mock.patch.object(updater, "install", side_effect=AssertionError("Unexpected install")):
            self.assertEqual(updater.main(["--non-interactive"]), 10)
            self.assertEqual(updater.main(["--check-only"]), 10)
            self.assertEqual(updater.main(["--install", "--non-interactive"]), 2)

    @unittest.skipUnless(os.name == "posix", "Linux lock")
    def test_concurrent_updater_is_rejected(self):
        import fcntl
        self.state.mkdir()
        lock = (self.state / "update.lock").open("a")
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with mock.patch.object(updater, "find_targets", return_value=[self.target]), \
                    mock.patch.object(updater, "release_for", return_value={
                        "tag_name": "v0.0.2", "body": "test notes"}), \
                    mock.patch.object(updater, "check_running"), \
                    mock.patch.object(updater, "state_root", return_value=self.state), \
                    mock.patch.object(updater, "download", side_effect=AssertionError("Unexpected download")):
                self.assertEqual(updater.main(["--install", "--non-interactive", "--hosts-closed"]), 2)
        finally:
            lock.close()

    @unittest.skipUnless(os.name == "posix", "Linux CLI")
    def test_state_inside_plugin_folder_is_rejected(self):
        with mock.patch.object(updater, "find_targets", return_value=[self.target]), \
                mock.patch.object(updater, "release_for", return_value={"tag_name": "v0.0.2"}), \
                mock.patch.object(updater, "check_running"), \
                mock.patch.object(updater, "state_root", return_value=self.runtime / "state"), \
                mock.patch.object(updater, "download", side_effect=AssertionError("Unexpected download")):
            self.assertEqual(updater.main(["--install", "--non-interactive", "--hosts-closed"]), 2)
            self.assertFalse((self.runtime / "state").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
