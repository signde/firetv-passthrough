#!/usr/bin/env python3
"""Exercise the unified installer and supervisor in an isolated shell harness.

Only device paths, firmware probing and Magisk functions are substituted.
Utility patching, payload hashes, migration and supervisor scripts are real.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock', required=True, type=Path)
args = parser.parse_args()
STOCK = args.stock.read_bytes()
FIXED = '690cfd8c33e3c02d68c7e0d1c51415530907bcf41f1cf8784186ba17c897e4f2'
IDS = ('gazelle_ddplus_bypass', 'firetv_dtshd_passthrough')


class Unified(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='unified-audio-')
        self.base = Path(self.temp.name)
        self.module = self.base/'module'
        self.adb = self.base/'adb'
        self.utility = self.base/'aparam'
        self.module.mkdir()
        self.utility.write_bytes(STOCK)
        self.utility.chmod(0o755)
        for name in ('customize.sh', 'service.sh', 'module.prop', 'scripts/legacy.sh',
                     'scripts/patch_karat_aparam.sh'):
            dest = self.module/name
            dest.parent.mkdir(exist_ok=True)
            dest.write_bytes((ROOT/name).read_bytes())
        (self.module/'bin').mkdir()
        (self.module/'bin/frida-inject').write_text('test-runtime')
        (self.module/'verify-firmware.sh').write_text('''
verify_firmware() { FIRMWARE_PROFILE=test-profile; [ "${TEST_VERIFY:-yes}" = yes ]; }
''')
        # Substitute only absolute device references, not $MODPATH/system/bin.
        for name in ('customize.sh', 'service.sh'):
            p = self.module/name
            s = p.read_text().replace('/data/adb', str(self.adb))
            s = s.replace(' /system/bin/aparam', f' "{self.utility}"')
            s = s.replace('/dev/firetv_passthrough.lock', str(self.base/'lock'))
            p.write_text(s)
        for worker, other in (('dolby', 'dts'), ('dts', 'dolby')):
            (self.module/f'scripts/{worker}-service.sh').write_text(f'''
trap 'touch "$MODDIR/{worker}.exit"' EXIT
touch "$MODDIR/{worker}.start"
i=0
while [ ! -f "$MODDIR/{other}.start" ]; do
    i=$((i+1)); [ "$i" -lt 100 ] || exit 19
    sleep 0.01
done
''')
        self.hash_payload()

    def tearDown(self):
        self.temp.cleanup()

    def hash_payload(self):
        manifest = ''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(self.module))+'\n'
                           for p in sorted(self.module.rglob('*')) if p.is_file()
                           and p.name not in ('payload.sha256', 'customize.sh', 'README.md'))
        (self.module/'payload.sha256').write_text(manifest)

    def legacy(self, parent='modules', ident=IDS[0], marker=None):
        p = self.adb/parent/ident
        p.mkdir(parents=True, exist_ok=True)
        (p/'module.prop').write_text('id='+ident+'\n')
        if marker:
            (p/marker).touch()
        return p

    def run_shell(self, command, **env):
        bootstrap = '''
getprop() { case "$1" in ro.product.device) echo "$TEST_DEVICE";; esac; }
abort() { echo "$*" >&2; exit 1; }
ui_print() { echo "$*"; }
set_perm_recursive() { [ "${TEST_PERMISSIONS:-yes}" = yes ]; }
set_perm() { chmod "$4" "$1"; }
flock() { return 0; }
'''
        return subprocess.run(['sh', '-c', bootstrap+command, str(self.module/'service.sh')],
                              env={**os.environ, 'MODPATH': str(self.module), 'BOOTMODE': 'true',
                                   'TEST_DEVICE': 'gazelle', **env},
                              capture_output=True, text=True, timeout=10)

    def install(self, **env):
        return self.run_shell('. "$MODPATH/customize.sh"', **env)

    def service(self, **env):
        return self.run_shell('. "$MODPATH/service.sh"', **env)

    def test_gazelle_fresh_install(self):
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.module/'system').exists())
        self.assertEqual(self.utility.read_bytes(), STOCK)

    def test_karat_fresh_and_already_corrected_overlay(self):
        for _ in range(2):
            result = self.install(TEST_DEVICE='karat')
            self.assertEqual(result.returncode, 0, result.stderr)
            overlay = self.module/'system/bin/aparam'
            self.assertEqual(hashlib.sha256(overlay.read_bytes()).hexdigest(), FIXED)
            self.assertEqual(overlay.stat().st_mode & 0o777, 0o755)
            # A previous module can expose its corrected overlay at install time.
            self.utility.write_bytes(overlay.read_bytes())

    def test_migration_including_pending_updates(self):
        old = [self.legacy(parent, ident) for parent in ('modules', 'modules_update') for ident in IDS]
        unrelated = self.legacy(ident='unrelated_module')
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(all((p/'disable').exists() for p in old))
        self.assertFalse((unrelated/'disable').exists())
        self.assertTrue(all((p/'module.prop').exists() for p in old))
        self.assertEqual(self.install().returncode, 0)  # Repeat install is safe.

    def test_each_legacy_module_alone(self):
        for ident in IDS:
            p = self.legacy(ident=ident)
            self.assertEqual(self.install().returncode, 0)
            self.assertTrue((p/'disable').exists())

    def test_rejected_install_keeps_existing_modules_enabled(self):
        old = [self.legacy(ident=ident) for ident in IDS]
        for env in ({'TEST_VERIFY': 'no'}, {'BOOTMODE': 'false'}, {'TEST_DEVICE': 'raven'}, {'TEST_PERMISSIONS': 'no'}):
            self.assertNotEqual(self.install(**env).returncode, 0)
            self.assertTrue(all(not (p/'disable').exists() for p in old))
        self.utility.write_bytes(b'unknown utility')
        self.assertNotEqual(self.install(TEST_DEVICE='karat').returncode, 0)
        self.assertTrue(all(not (p/'disable').exists() for p in old))
        (self.module/'bin/frida-inject').write_text('corrupt')
        self.assertNotEqual(self.install().returncode, 0)
        self.assertTrue(all(not (p/'disable').exists() for p in old))

    def test_migration_rollback_preserves_prior_state(self):
        existing = self.legacy(ident=IDS[0], marker='disable')
        first = self.legacy(ident=IDS[1])
        failing = self.legacy(parent='modules_update', ident=IDS[0])
        result = self.run_shell('''
. "$MODPATH/scripts/legacy.sh"
touch() { [ "$1" != "$FAIL_MARKER" ] || return 1; command touch "$@"; }
disable_legacy_modules "$ADB_ROOT"
''', ADB_ROOT=str(self.adb), FAIL_MARKER=str(failing/'disable'))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((existing/'disable').exists())
        self.assertFalse((first/'disable').exists())
        self.assertFalse((failing/'disable').exists())

    def test_removed_legacy_not_reenabled(self):
        old = self.legacy(marker='remove')
        self.assertEqual(self.install().returncode, 0)
        self.assertTrue((old/'remove').exists())
        self.assertFalse((old/'disable').exists())

    def test_supervisor_starts_both_workers_and_isolates_traps(self):
        result = self.service()
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ('dolby.start', 'dolby.exit', 'dts.start', 'dts.exit'):
            self.assertTrue((self.module/name).exists(), name)

    def test_supervisor_rejects_active_legacy(self):
        self.legacy(parent='modules_update', ident=IDS[1])
        self.assertNotEqual(self.service().returncode, 0)
        self.assertIn('LEGACY_MODULE_ENABLED', (self.module/'status.txt').read_text())
        self.assertFalse((self.module/'dts.start').exists())
        self.assertFalse((self.module/'dolby.start').exists())

    def test_supervisor_accepts_disabled_legacy_after_install(self):
        self.legacy()
        self.assertEqual(self.install().returncode, 0)
        self.assertEqual(self.service().returncode, 0)
        self.assertTrue((self.module/'dts.start').exists())

    def test_supervisor_checks_overlay_before_starting(self):
        self.assertNotEqual(self.service(TEST_DEVICE='karat').returncode, 0)
        self.assertIn('KARAT_OVERLAY_UNAVAILABLE', (self.module/'status.txt').read_text())
        self.assertFalse((self.module/'dolby.start').exists())
        self.assertEqual(self.install(TEST_DEVICE='karat').returncode, 0)
        shutil.copyfile(self.module/'system/bin/aparam', self.utility)
        self.assertEqual(self.service(TEST_DEVICE='karat').returncode, 0)

    def test_supervisor_disabled_or_corrupt(self):
        (self.module/'disable').touch()
        self.assertEqual(self.service().returncode, 0)
        self.assertFalse((self.module/'dts.start').exists())
        (self.module/'disable').unlink()
        (self.module/'scripts/dts-service.sh').write_text('corruption')
        self.assertNotEqual(self.service().returncode, 0)
        self.assertIn('CORRUPT_PAYLOAD', (self.module/'status.txt').read_text())
        self.assertFalse((self.module/'dolby.start').exists())


unittest.main(argv=[__file__], verbosity=2)
