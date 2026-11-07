"""Exercise production update transport and exported-intent handlers, without an Android runtime."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'games/totk/android/java/com/aure/totk'
TRANSFER = r'''
package com.aure.totk;
import java.io.*;
import java.net.URI;
import java.nio.file.*;
import java.security.MessageDigest;
public class TransferProbe {
    static void require(boolean value, String why) { if (!value) throw new AssertionError(why); }
    public static void main(String[] args) throws Exception {
        if (args[0].equals("copy")) {
            byte[] payload = "verified session bytes".getBytes(java.nio.charset.StandardCharsets.UTF_8);
            String hash = java.util.HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(payload));
            try (UpdateTransfer transfer = new UpdateTransfer()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                transfer.copyVerified(new ByteArrayInputStream(payload), out, hash);
                require(java.util.Arrays.equals(out.toByteArray(), payload), "session bytes differ");
                payload[0] ^= 1;
                try { transfer.copyVerified(new ByteArrayInputStream(payload), new ByteArrayOutputStream(), hash); throw new AssertionError("tamper accepted"); }
                catch (IOException expected) { require(expected.getMessage().contains("changed before installation"), expected.toString()); }
            }
            return;
        }
        URI source = URI.create(args[1]);
        File target = new File(args[2], "partial.apk"), checksum = new File(args[2], "checksum.tmp");
        long overall = args[0].equals("combined") ? 500 : args[0].equals("chain-deadline") ? 300 : 2000;
        long window = args[0].equals("drip") || args[0].equals("blocked") || args[0].equals("headers") ? 250 : 5000;
        String path = args[0].equals("combined") ? "/first" : "/" + args[0];
        long started = System.nanoTime();
        try (UpdateTransfer transfer = new UpdateTransfer(overall, window, 64 * 1024)) {
            try {
                transfer.download(source, true, source.resolve(path), target, 2 * 1024 * 1024, (bytes, total) -> {});
                if (args[0].equals("combined")) {
                    Thread.sleep(150);
                    transfer.download(source, true, source.resolve("/second"), checksum, 4096, (bytes, total) -> {});
                }
                require(args[0].equals("redirect-ok"), "unbounded or unsafe download succeeded");
                require(target.length() == 65536, "wrong redirected content");
            } catch (IOException expected) {
                require(!args[0].equals("redirect-ok"), expected.toString());
                String message = expected.getMessage();
                if (args[0].equals("drip") || args[0].equals("blocked") || args[0].equals("headers"))
                    require(message.contains("too slow"), message);
                if (args[0].equals("combined") || args[0].equals("chain-deadline")) require(message.contains("time limit"), message);
                if (args[0].equals("redirect-limit")) require(message.contains("Too many"), message);
                if (args[0].equals("redirect-unsafe")) require(message.contains("Unsafe"), message);
                require(!checksum.exists(), "partial checksum kept");
                if (!args[0].equals("combined")) require(!target.exists(), "partial APK kept");
                require(System.nanoTime() - started < 3_000_000_000L, "watchdog did not interrupt the blocked read");
                System.out.println(message);
            }
        } finally { Files.deleteIfExists(target.toPath()); Files.deleteIfExists(checksum.toPath()); }
    }
}
'''

AUTH_FRAMEWORK = r'''
package com.aure.totk.setup;
import java.util.*;
class Intent {
    static final String ACTION_MAIN="main";
    Map<String,Object> extras=new HashMap<>();
    Intent putExtra(String key,Object value){extras.put(key,value);return this;}
    boolean getBooleanExtra(String key,boolean fallback){return (boolean)extras.getOrDefault(key,fallback);}
    String getStringExtra(String key){return (String)extras.get(key);}
    void removeExtra(String key){extras.remove(key);}
    String getAction(){return "view";}
}
class Activity { protected void onNewIntent(Intent intent){} protected void onStart(){} }
class SetupService { static class Status {boolean running;} static final Status status=new Status(); }
class InstallState { static InstallState read(Object context){return new InstallState();} boolean installed(){return true;} }
class SetupActivity extends Activity {
    static final String EXTRA_UPDATE="update", EXTRA_UPDATE_AUTH="authorization", EXTRA_COLD_SETTINGS="cold", EXTRA_SETTINGS="settings", EXTRA_GAME_ERROR="error";
    enum Screen {HOME,INSTALL,WELCOME,ABOUT}
    boolean ui=true, started, pendingAsked, closingGame, runningGame;
    Object updater; Runnable blockedAction; String gameError;
    Intent intent; Screen screen=Screen.ABOUT;
    int prompts, downloads, closes;
    Intent getIntent(){return intent;} void setIntent(Intent value){intent=value;}
    String pendingUpdate(){return "1.0.2";}
    void replace(Screen value){screen=value;}
    void offerPendingUpdate(){prompts++; pendingAsked=true;}
    void startAppUpdate(){if(runningGame)closes++; downloads++;}
    boolean moveSettingsToMain(Intent intent){return false;}
    boolean returnToRunningGame(Intent intent){return false;}
    void showUi(){ui=true; screen=Screen.HOME;}
    void render(){} void startUi(){}
'''
AUTH_TEST = r'''
    static void require(boolean condition,String why){if(!condition)throw new AssertionError(why);}
    public static void main(String[] args) {
        for(boolean running:new boolean[]{false,true}) for(boolean initial:new boolean[]{false,true}) {
            SetupActivity a=new SetupActivity();a.runningGame=running;
            Intent external=new Intent().putExtra(EXTRA_UPDATE,true).putExtra(EXTRA_UPDATE_AUTH,"forged");
            if(initial){a.intent=external;a.onStart();}else{a.started=true;a.onNewIntent(external);}
            require(a.prompts==1 && a.closes==0 && a.downloads==0,"external initial="+initial+" running="+running+" acted without choice");
            a.startAppUpdate();
            require(a.downloads==1 && a.closes==(running?1:0),"user choice stopped working");
        }
        String grant=UpdateAuthorization.issue();
        require(!UpdateAuthorization.consume("forged"),"guessed grant accepted");
        for(boolean initial:new boolean[]{false,true}) {
            SetupActivity a=new SetupActivity();a.runningGame=true;
            String token=initial?UpdateAuthorization.issue():grant;
            Intent internal=new Intent().putExtra(EXTRA_UPDATE,true).putExtra(EXTRA_UPDATE_AUTH,token);
            if(initial){a.intent=internal;a.onStart();}else{a.started=true;a.onNewIntent(internal);}
            require(a.prompts==0 && a.downloads==1 && a.closes==1,"authorized choice lost");
            a.onNewIntent(new Intent().putExtra(EXTRA_UPDATE,true).putExtra(EXTRA_UPDATE_AUTH,token));
            require(a.prompts==1 && a.downloads==1,"replayed authorization accepted");
        }
    }
}
'''


class Handler(BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *_args):
        pass

    def do_GET(self):
        self.requests.append(self.path)
        try:
            if self.path.startswith('/chain-deadline'):
                time.sleep(.15)
                return self.redirect(self.path + 'x')
            if self.path.startswith('/redirect-limit'):
                return self.redirect(self.path + 'x')
            if self.path == '/redirect-unsafe':
                return self.redirect('http://example.invalid/not-allowed')
            if self.path == '/redirect-ok':
                return self.redirect('/redirect-middle')
            if self.path == '/redirect-middle':
                return self.redirect('/content')
            if self.path == '/headers':
                self.connection.sendall(b'HTTP/1.1 200 OK\r\nX-Slow: ')
                for _ in range(100):
                    self.connection.sendall(b'x')
                    time.sleep(.03)
                return
            if self.path == '/first':
                time.sleep(.2)
            self.send_response(200)
            self.send_header('Content-Length', '1024' if self.path == '/second' else '65536')
            self.end_headers()
            if self.path == '/blocked':
                time.sleep(4)
                return
            if self.path == '/second':
                time.sleep(.3)
            if self.path == '/drip':
                for _ in range(100):
                    self.wfile.write(b'x')
                    self.wfile.flush()
                    time.sleep(.03)
            else:
                self.wfile.write(b'x' * (1024 if self.path == '/second' else 65536))
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
    def redirect(self, location):
        self.send_response(302)
        self.send_header('Location', location)
        self.send_header('Content-Length', '0')
        self.end_headers()


class UpdateSecurityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('javac') or not shutil.which('java'):
            raise unittest.SkipTest('host JDK required')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-update-security-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        transfer = cls.work / 'TransferProbe.java'
        transfer.write_text(TRANSFER)
        activity = (JAVA / 'setup/SetupActivity.java').read_text()
        requested = activity[activity.index('    private void requestedUpdate()'):activity.index('    private void startAppUpdate()')]
        new_intent = activity[activity.index('    @Override protected void onNewIntent(Intent intent)'):activity.index('    @Override protected void onStart()')]
        start = activity[activity.index('    @Override protected void onStart()'):activity.index('    /** showUi can finish')]
        authorization = cls.work / 'SetupActivity.java'
        authorization.write_text(AUTH_FRAMEWORK + requested + new_intent + start + AUTH_TEST)
        subprocess.run(['javac', '--release', '17', '-d', str(cls.work), str(JAVA / 'UpdateRules.java'),
                        str(JAVA / 'UpdateTransfer.java'), str(JAVA / 'setup/UpdateAuthorization.java'), str(transfer), str(authorization)],
                       check=True, capture_output=True, text=True)
        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.origin = f'http://127.0.0.1:{cls.server.server_port}'
        cls.addClassCleanup(cls.server.server_close)
        cls.addClassCleanup(cls.server.shutdown)

    def probe(self, mode):
        Handler.requests.clear()
        result = subprocess.run(['java', '-cp', str(self.work), 'com.aure.totk.TransferProbe', mode, self.origin, str(self.work)],
                                capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_slow_drip_aborts_and_deletes_partial(self): self.probe('drip')
    def test_blocked_body_is_disconnected(self): self.probe('blocked')
    def test_slow_headers_are_disconnected(self): self.probe('headers')
    def test_shared_apk_checksum_deadline(self): self.probe('combined')
    def test_redirects_cannot_reset_deadline(self): self.probe('chain-deadline')
    def test_redirect_limit(self):
        self.probe('redirect-limit')
        self.assertEqual(len(Handler.requests), 9)
    def test_disallowed_redirect_is_not_fetched(self):
        self.probe('redirect-unsafe')
        self.assertEqual(Handler.requests, ['/redirect-unsafe'])
    def test_allowed_redirects_download(self): self.probe('redirect-ok')
    def test_exact_session_stream_hash(self): self.probe('copy')
    def test_external_initial_and_new_intents_cannot_authorize(self):
        result = subprocess.run(['java', '-cp', str(self.work), 'com.aure.totk.setup.SetupActivity'], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
