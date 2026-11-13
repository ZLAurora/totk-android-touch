"""Exercise CoverSession's lifecycle decisions on a host JVM through its Host calls."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
SOURCE = TOTK / 'android/java/com/aure/totk/companion/CoverSession.java'
DISPLAY_CLASS = TOTK / 'android/java/com/aure/totk/companion/DisplayClass.java'
PROBE = TOTK / 'tests/displays/CoverProbe.java'


class CoverSessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            raise unittest.SkipTest('javac/java not found')
        cls.java = java
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-cover-')
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', cls.temp.name,
                        str(DISPLAY_CLASS), str(SOURCE), str(PROBE)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def calls(self, scenario):
        result = subprocess.run([self.java, '-cp', self.temp.name, 'com.aure.totk.companion.CoverProbe', scenario],
                                capture_output=True, text=True, timeout=60, check=True)
        calls = []
        for item in result.stdout.split():
            fields = item.split(':')
            calls.append((fields[0], *(int(field) for field in fields[1:])))
        return calls

    def assert_starts(self, calls, displays):
        starts = [call for call in calls if call[0] == 'start']
        self.assertEqual([call[1] for call in starts], displays)
        tokens = [call[2] for call in starts]
        self.assertTrue(all(token > 0 for token in tokens))
        self.assertEqual(len(tokens), len(set(tokens)), 'every host start must receive a unique token')

    def test_disabled_or_non_builtin_display_never_starts(self):
        for scenario in ('off', 'none', 'external', 'not-resumed'):
            with self.subTest(scenario=scenario):
                self.assertEqual(self.calls(scenario), [])

    def test_initial_start_and_repeated_refresh_keep_one_cover(self):
        calls = self.calls('initial-and-keep')
        self.assert_starts(calls, [4])
        self.assertEqual(calls, [('start', 4, calls[0][2])])

    def test_display_change_finishes_old_cover_before_starting_new(self):
        calls = self.calls('display-change')
        self.assert_starts(calls, [4, 7])
        first_token, second_token = calls[0][2], calls[2][2]
        self.assertEqual(calls, [('start', 4, first_token), ('finish', first_token), ('start', 7, second_token)])

    def test_dismiss_owner_stop_and_return_start_fresh_activations(self):
        calls = self.calls('dismiss-owner-stop-return')
        self.assert_starts(calls, [4, 4, 4])
        tokens = [call[2] for call in calls if call[0] == 'start']
        self.assertEqual([call[1] for call in calls if call[0] == 'finish'], tokens[:2])
        self.assertEqual(len(tokens), len(set(tokens)))

    def test_display_removal_or_external_reclassification_finishes_cover(self):
        for scenario in ('display-removed', 'display-became-external'):
            with self.subTest(scenario=scenario):
                calls = self.calls(scenario)
                self.assert_starts(calls, [4])
                self.assertEqual(calls, [('start', 4, calls[0][2]), ('finish', calls[0][2])])

    def test_close_finishes_cover_and_is_terminal(self):
        calls = self.calls('close')
        self.assert_starts(calls, [4])
        self.assertEqual(calls, [('start', 4, calls[0][2]), ('finish', calls[0][2])])

    def test_pause_alone_keeps_cover_without_starting_again(self):
        calls = self.calls('pause-keeps-cover')
        self.assert_starts(calls, [4])
        self.assertEqual(calls, [('start', 4, calls[0][2])])

    def test_stale_host_finish_does_not_affect_current_cover(self):
        calls = self.calls('stale-finish')
        self.assert_starts(calls, [4, 7])
        self.assertEqual(calls, [('start', 4, calls[0][2]), ('finish', calls[0][2]), ('start', 7, calls[2][2])])

    def test_unexpected_finish_restarts_once_without_a_loop(self):
        calls = self.calls('one-restart')
        self.assert_starts(calls, [4, 4])
        self.assertEqual(len(calls), 2)

    def test_refusal_and_host_exception_block_retries_until_new_activation(self):
        refused = self.calls('refused')
        self.assert_starts(refused, [4])
        self.assertEqual(refused, [('start', 4, refused[0][2]), ('finish', refused[0][2])])
        thrown = self.calls('throws')
        self.assert_starts(thrown, [4])
        self.assertEqual(thrown, [('start', 4, thrown[0][2]), ('finish', thrown[0][2])])

    def test_display_change_resets_refused_start_budget(self):
        calls = self.calls('refused-reset-by-display-change')
        self.assert_starts(calls, [4, 7])
        self.assertEqual(calls, [('start', 4, calls[0][2]), ('finish', calls[0][2]), ('start', 7, calls[2][2])])

    def test_accepted_but_refused_launch_is_finished_without_retry(self):
        calls = self.calls('start-refused')
        self.assert_starts(calls, [4])
        self.assertEqual(calls, [('start', 4, calls[0][2]), ('finish', calls[0][2])])

    def test_pause_defers_unexpected_restart_until_owner_resumes(self):
        calls = self.calls('pause-and-resume')
        self.assert_starts(calls, [4, 4])
        self.assertEqual(len(calls), 2)

    def test_dismiss_or_close_during_start_cancels_late_accepted_launch(self):
        for scenario in ('dismiss-during-start', 'close-during-start'):
            with self.subTest(scenario=scenario):
                calls = self.calls(scenario)
                self.assert_starts(calls, [4])
                self.assertEqual([call[1] for call in calls if call[0] == 'finish'],
                                 [calls[0][2], calls[0][2]])

    def test_start_tokens_are_unique_across_sessions(self):
        calls = self.calls('tokens-global')
        self.assert_starts(calls, [4, 4])


if __name__ == '__main__':
    unittest.main()
