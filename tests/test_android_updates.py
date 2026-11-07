"""Production update URL/version/checksum/trust policy and cross-process private-file persistence on a host JVM."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/android/java/com/aure/totk/UpdateRules.java'
PROBE = r'''
import com.aure.totk.UpdateRules;
import java.net.URI;
import java.io.*;
import java.nio.file.*;
public class UpdateProbe {
    static URI github = URI.create("https://github.com");
    static String tag(String version) { return "https://github.com/AurelioB/totk-android/releases/tag/v" + version; }
    static void equal(Object a, Object b) { if (!java.util.Objects.equals(a, b)) throw new AssertionError(a + " != " + b); }
    public static void main(String[] args) throws Exception {
        switch (args[0]) {
        case "location":
            equal(UpdateRules.location(tag("1.0.2"), github, false), "1.0.2");
            for (String bad : new String[]{"", "/AurelioB/totk-android/releases/tag/v1.0.2", tag("1.0.2-rc1"), tag("1.0.2+build"),
                tag("01.0.2"), tag("1.2"), tag("1.0.2/"), tag("1.0.2?x=1"), tag("1.0.2#x"), tag("%31.0.2"),
                tag("1.0.2").replace("github.com", "github.com.evil.test"), tag("1.0.2").replace("https:", "http:"),
                tag("1.0.2").replace("github.com", "user@github.com"), tag("1.0.2").replace("github.com", "github.com:443"),
                tag("1.0.2").replace("AurelioB", "Other")}) equal(UpdateRules.location(bad, github, false), null);
            URI fake = URI.create("http://192.168.8.2:8042");
            equal(UpdateRules.location("http://192.168.8.2:8042/AurelioB/totk-android/releases/tag/v1.0.2", fake, true), "1.0.2");
            equal(UpdateRules.location("http://192.168.8.3:8042/AurelioB/totk-android/releases/tag/v1.0.2", fake, true), null);
            break;
        case "semver":
            equal(Integer.signum(UpdateRules.compare("1.10.0", "1.9.999")), 1);
            equal(Integer.signum(UpdateRules.compare("2.0.0", "1.999.999")), 1);
            equal(Integer.signum(UpdateRules.compare("1.0.0", "1.0.1")), -1);
            equal(UpdateRules.compare("1.0.1", "1.0.1"), 0);
            equal(Integer.signum(UpdateRules.compare("999999999999999999999999.0.0", "2.0.0")), 1);
            for (String bad : new String[]{"1.0.1-dev", "v1.0.1", "1.0.01", "1.0", "1.0.1\n"}) equal(UpdateRules.version(bad), false);
            break;
        case "checksum":
            String hash = "aB".repeat(32), asset = UpdateRules.asset("1.0.2");
            equal(UpdateRules.sha256(hash + "  " + asset + "\n", asset), hash.toLowerCase(java.util.Locale.ROOT));
            equal(UpdateRules.sha256(hash + " *" + asset + "\r\n", asset), hash.toLowerCase(java.util.Locale.ROOT));
            for (String bad : new String[]{hash, hash + "  other.apk", hash + "  ../" + asset, hash + "  " + asset + "\nextra", "g".repeat(64) + "  " + asset}) {
                try { UpdateRules.sha256(bad, asset); throw new AssertionError("accepted " + bad); } catch (IOException expected) { }
            }
            break;
        case "decision":
            int[] calls = {0};
            UpdateRules.Head head = () -> { calls[0]++; return tag("1.0.2"); };
            equal(UpdateRules.check(true, true, "1.0.1", "com.aure.totk", false, github, head), "1.0.2");
            equal(calls[0], 1);
            equal(UpdateRules.check(false, true, "1.0.1", "com.aure.totk", false, github, head), null);
            equal(UpdateRules.check(true, false, "1.0.1", "com.aure.totk", false, github, head), null);
            equal(UpdateRules.check(true, true, "1.0.1-dev", "com.aure.totk", false, github, head), null);
            equal(UpdateRules.check(true, true, "1.0.1", "com.aure.totk.test", false, github, head), null);
            equal(calls[0], 1);
            equal(UpdateRules.check(true, true, "1.0.2", "com.aure.totk", false, github, head), null);
            equal(UpdateRules.check(true, true, "1.0.3", "com.aure.totk", false, github, head), null);
            equal(UpdateRules.check(true, true, "1.0.1", "com.aure.totk", false, github, () -> { throw new java.net.SocketTimeoutException(); }), null);
            equal(UpdateRules.check(true, true, "1.0.1", "com.aure.totk", false, github, () -> "malformed"), null);
            equal(UpdateRules.check(true, true, "1.0.1", "com.aure.totk.test", true, github, head), "1.0.2");
            break;
        case "trust":
            byte[][] cert = {new byte[]{1, 2, 3}}, wrong = {new byte[]{4, 5, 6}};
            equal(UpdateRules.verify("com.aure.totk", 10001, cert, "com.aure.totk", 10002, cert), null);
            for (String result : new String[]{
                UpdateRules.verify("com.aure.totk", 10001, cert, "other", 10002, cert),
                UpdateRules.verify("com.aure.totk", 10001, cert, "com.aure.totk", 10001, cert),
                UpdateRules.verify("com.aure.totk", 10001, cert, "com.aure.totk", 10000, cert),
                UpdateRules.verify("com.aure.totk", 10001, cert, "com.aure.totk", 10002, wrong),
                UpdateRules.verify("com.aure.totk", 10001, null, "com.aure.totk", 10002, cert),
                UpdateRules.verify("com.aure.totk", 10001, cert, "com.aure.totk", 10002, null)}) if (result == null) throw new AssertionError("accepted untrusted APK");
            equal(UpdateRules.allowedDownload(URI.create("https://release-assets.githubusercontent.com/a?token=b"), github, false), true);
            for (String bad : new String[]{"http://github.com/a", "https://github.com.evil/a", "https://evilgithubusercontent.com/a", "https://user@github.com/a", "https://github.com:8080/a"})
                equal(UpdateRules.allowedDownload(URI.create(bad), github, false), false);
            break;
        case "persistence":
            File files = new File(args[1]);
            equal(UpdateRules.enabled(files), true);
            UpdateRules.enabled(files, false);
            equal(UpdateRules.enabled(new File(args[1])), false);
            UpdateRules.setPending(files, "1.0.2");
            equal(UpdateRules.pending(new File(args[1]), "1.0.1"), "1.0.2");
            equal(UpdateRules.pending(files, "1.0.2"), null);
            equal(new File(files, "pending-update.txt").exists(), false);
            UpdateRules.enabled(files, true);
            equal(UpdateRules.enabled(files), true);
            Files.writeString(new File(files, "pending-update.txt").toPath(), "1.0.2-rc1");
            equal(UpdateRules.pending(files, "1.0.1"), null);
            equal(UpdateRules.checkAndStore(files, true, true, "1.0.1", "com.aure.totk", false, github, () -> tag("1.0.2")), "1.0.2");
            equal(UpdateRules.pending(files, "1.0.1"), "1.0.2");
            File blocked = new File(files, "not-a-directory");
            Files.write(blocked.toPath(), new byte[]{1});
            int[] attempted = {0};
            equal(UpdateRules.checkAndStore(blocked, true, true, "1.0.1", "com.aure.totk", false, github,
                    () -> { attempted[0]++; return tag("1.0.2"); }), null);
            equal(attempted[0], 1);
            break;
        default: throw new AssertionError(args[0]);
        }
    }
}
'''


class AndroidUpdateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('javac') or not shutil.which('java'):
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-updates-')
        cls.work = Path(cls.temp.name)
        probe = cls.work / 'UpdateProbe.java'
        probe.write_text(PROBE)
        subprocess.run(['javac', '--release', '17', '-d', str(cls.work), str(SOURCE), str(probe)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def probe(self, name):
        with tempfile.TemporaryDirectory(prefix='totk-update-state-') as state:
            subprocess.run(['java', '-cp', str(self.work), 'UpdateProbe', name, state], check=True, capture_output=True, text=True, timeout=10)

    def test_location(self): self.probe('location')
    def test_semver(self): self.probe('semver')
    def test_checksum(self): self.probe('checksum')
    def test_decision(self): self.probe('decision')
    def test_signature_package_version(self): self.probe('trust')
    def test_pending_persistence_and_default_on(self): self.probe('persistence')
