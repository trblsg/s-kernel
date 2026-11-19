"""Unit checks for the S9 no-widget staging ablation (never runs on phone)."""
import io
import plistlib
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

import strip_widgets


class StripWidgetsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "sysroot"
        (self.root / "System/Library").mkdir(parents=True)
        self.app = self.root / "Applications" / "Weather.app" / "PlugIns"
        self.app.mkdir(parents=True)
        self.manifest = Path(self.tmp.name) / "system.manifest"
        self.out = Path(self.tmp.name) / "filtered.manifest"

    def add_bundle(self, name, extension_point="", bundle_id=""):
        bundle = self.app / name
        bundle.mkdir()
        info = {"CFBundleIdentifier": bundle_id}
        if extension_point:
            info["NSExtension"] = {"NSExtensionPointIdentifier": extension_point}
        with (bundle / "Info.plist").open("wb") as out:
            plistlib.dump(info, out)
        (bundle / "executable").write_text("test", encoding="utf8")
        return bundle

    def prepare(self, include_aegir=True):
        self.widget = self.add_bundle(
            "CalendarWidgetExtension.appex", "com.apple.widgetkit-extension",
            "com.apple.mobilecal.CalendarWidgetExtension"
        )
        self.legacy = self.add_bundle(
            "StocksToday.appex", "com.apple.widget-extension"
        )
        self.share = self.add_bundle(
            "Share.appex", "com.apple.share-services"
        )
        if include_aegir:
            self.aegir = self.add_bundle(
                "AegirPoster.appex", "com.apple.posterkit-extension",
                "com.apple.NanoUniverse.AegirProxyApp.AegirPoster"
            )
        paths = [self.widget, self.legacy, self.share]
        if include_aegir:
            paths.append(self.aegir)
        with self.manifest.open("wb") as f:
            for path in paths:
                rel = path.relative_to(self.root).as_posix()
                for suffix in ("", "/Info.plist", "/executable"):
                    f.write(f"100644\t0\t0\t{rel}{suffix}\t\n".encode())

    def call(self, dry_run=False):
        argv = ["strip_widgets.py", str(self.root),
                "--manifest-in", str(self.manifest),
                "--manifest-out", str(self.out)]
        if dry_run:
            argv.append("--dry-run")
        with patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()):
            strip_widgets.main()

    def test_precise_bundle_selection_and_manifest(self):
        self.prepare()
        self.call()
        self.assertFalse(self.widget.exists())
        self.assertFalse(self.legacy.exists())
        self.assertFalse(self.aegir.exists())
        self.assertTrue(self.share.exists())
        result = self.out.read_text()
        self.assertIn("Share.appex", result)
        self.assertNotIn("CalendarWidgetExtension.appex", result)
        self.assertNotIn("StocksToday.appex", result)
        self.assertNotIn("AegirPoster.appex", result)
        self.assertIn("widget_bundles=2 aegir_bundles=1",
                      (self.root / ".ioss9-widget-ablation").read_text())

    def test_missing_poster_fails_without_changes(self):
        self.prepare(include_aegir=False)
        with self.assertRaises(SystemExit):
            self.call()
        self.assertTrue(self.widget.exists())
        self.assertTrue(self.share.exists())
        self.assertFalse(self.out.exists())

    def test_dry_run_keeps_all_files(self):
        self.prepare()
        self.call(dry_run=True)
        self.assertTrue(self.widget.exists())
        self.assertTrue(self.aegir.exists())
        self.assertTrue(self.share.exists())
        self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main()
