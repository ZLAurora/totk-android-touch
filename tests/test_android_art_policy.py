"""Production second-screen art decisions on the host JVM, without Android stubs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
SOURCE = TOTK / 'android/java/com/aure/totk/setup/ArtPolicy.java'
PROBE = TOTK / 'tests/art-policy/ArtPolicyProbe.java'


class ArtPolicyTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'javac/java not found')
    def test_all_display_stage_decline_combinations(self):
        with tempfile.TemporaryDirectory(prefix='totk-art-policy-') as work:
            subprocess.run(['javac', '--release', '17', '-encoding', 'UTF-8', '-d', work,
                            str(SOURCE), str(PROBE)], check=True, capture_output=True, text=True)
            result = subprocess.run(['java', '-cp', work, 'com.aure.totk.setup.ArtPolicyProbe'],
                                    check=True, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.stdout.strip(), '20 combinations; asset-state precedence passed')


if __name__ == '__main__':
    unittest.main()
