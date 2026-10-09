#!/usr/bin/env python3
"""Exercise recovery with delayed resets, unavailable HAL and disabled module."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class RecoveryTests(unittest.TestCase):
    def run_shell(self, script):
        with tempfile.TemporaryDirectory() as d:
            prelude = '''
. "$RECOVERY"
MODDIR=$PWD
printf 5 > mode
echo 0 > clock
: > writes
: > dolby.log
log() { echo "$*" >> dolby.log; }
enabled() { [ ! -e disabled ]; }
read_mode() { [ -e unavailable ] || cat mode; }
timeout() { cat clock >> writes; echo 6 > mode; }
sleep() {
    now=$(cat clock); now=$((now + $1)); echo "$now" > clock
    # Model Fire OS overwriting bypass at delayed startup milestones.
    case "$now" in 5|20|60) echo 5 > mode ;; esac
    if [ -e disable_at_5 ] && [ "$now" -ge 5 ]; then touch disabled; fi
}
'''
            return subprocess.run(['sh', '-c', prelude + script], cwd=d,
                env={'RECOVERY': str(ROOT/'scripts/recovery.sh'), 'PATH': '/usr/bin:/bin'},
                capture_output=True, text=True, check=True).stdout.strip()

    def test_real_event_shapes_and_irrelevant_events(self):
        result = self.run_shell('''
for event in 'I/power_screen_state( 8495): [1,0,0,0]' \
 'I/boot_progress_enable_screen( 8495): 263743' \
 'E/AudioService( 8495): Audioserver started.' \
 'I/power_screen_state( 8495): [0,0,0,0]' \
 'E/AudioService( 8495): Audioserver died.' \
 'I/unrelated( 8495): [1,0]' \
 'E/OtherAudioService( 8495): Audioserver started.'; do
 event_reason "$event" || echo Ignored
done
''')
        self.assertEqual(result.splitlines(), ['Resume','FrameworkRestart','AudioRestart'] + ['Ignored']*4)

    def test_restart_catches_late_reset(self):
        for reason in ['FrameworkRestart', 'AudioRestart']:
            self.assertEqual(self.run_shell(f'reconcile {reason}; cat writes; cat mode').splitlines(),
                             ['0','5','20','60','6'])

    def test_resume_keeps_existing_window(self):
        self.assertEqual(self.run_shell('reconcile Resume; cat writes; cat clock').splitlines(),
                         ['0','5','20','20'])

    def test_unavailable_hal_is_bounded(self):
        self.assertEqual(self.run_shell('touch unavailable; reconcile AudioRestart; wc -l < writes; cat clock').split(), ['0','60'])

    def test_disable_cancels_followups(self):
        self.assertEqual(self.run_shell('touch disable_at_5; reconcile AudioRestart || echo Cancelled; cat writes').splitlines(), ['Cancelled','0'])

    def test_already_correct_mode_has_no_write(self):
        self.assertEqual(self.run_shell('sleep() { :; }; echo 6 > mode; reconcile AudioRestart; wc -l < writes'), '0')

if __name__ == '__main__':
    unittest.main()
