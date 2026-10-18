"""Serve board.html, stream boarddemo's JSON events to it (Server-Sent Events), and drive the paper-dj engine.
Run on the Pi:  python3 ~/touchdemo/board_server.py   then open  http://qnxpi.local:8081
(port 8080 belongs to paper-dj's dj_engine, whose phone page plays the audio).

DJ bridge (UDP text commands to dj_engine, default 127.0.0.1:9000, set DJ=host:port, DJ=off to disable):
  button N pressed            -> "pad N"
  pinch fader 1/2/3/4 moves   -> "a.vol v" / "a.filter x" / "b.filter x" / "b.vol v"   (filter: fader 0..1 -> -1..1)"""
import json, os, socket, subprocess, sys, threading, time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
HERE = os.path.dirname(os.path.abspath(__file__)); PORT = int(os.environ.get('PORT', '8081'))
DJ = os.environ.get('DJ', '127.0.0.1:9000')
dj_sock = None if DJ == 'off' else socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
dj_addr = None if DJ == 'off' else (DJ.rsplit(':', 1)[0], int(DJ.rsplit(':', 1)[1]))
FADER_CMDS = [('a.vol', False), ('a.filter', True), ('b.filter', True), ('b.vol', False)]
dj_state = {'pressed': 0, 'faders': [None] * 4, 'sent': 0}

def dj_send(cmd):
    if dj_sock is None: return
    try: dj_sock.sendto(cmd.encode(), dj_addr); dj_state['sent'] += 1
    except OSError as e: print('[dj] send failed:', e, flush=True)

def dj_bridge(e):
    """Turn tracker events into engine commands: one 'pad N' per new press, fader values while pinching."""
    p = e.get('pressed', 0)
    if p and p != dj_state['pressed']: dj_send('pad %d' % p); print('[dj] pad', p, flush=True)
    dj_state['pressed'] = p
    if e.get('pinch') and e.get('faders') and 1 <= e.get('fader', 0) <= 4:   # only the fader being held
        i = e['fader'] - 1; v = e['faders'][i]
        if dj_state['faders'][i] is None or abs(v - dj_state['faders'][i]) > 0.005:
            key, bipolar = FADER_CMDS[i]; dj_state['faders'][i] = v
            dj_send('%s %.3f' % (key, v * 2 - 1 if bipolar else v))
clients, lock = [], threading.Lock(); latest = {'status': 'starting camera…'}

def broadcast(line):
    global latest
    latest = json.loads(line)
    if 'status' not in latest: dj_bridge(latest)
    with lock:
        for c in clients[:]:
            try: c.wfile.write(b'data: ' + line.encode() + b'\n\n'); c.wfile.flush()
            except Exception: clients.remove(c)

def run_demo():
    subprocess.run(['slay', '-f', 'camera_example3_viewfinder'], stderr=subprocess.DEVNULL)
    args = [os.path.join(HERE, os.environ.get('TRACKER', 'boarddemo'))] + sys.argv[1:]   # TRACKER=deck for the slider/dial tracker
    while True:
        p = subprocess.Popen(args, cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        def errs():
            for l in p.stderr:
                l = l.strip()
                if l.startswith('[') or l.startswith('>>>') or l.startswith('<<<'):
                    print(l, flush=True)
                    if l.startswith('[board]') or l.startswith('[ready]'): broadcast(json.dumps({'status': l.split('] ', 1)[-1]}))
        threading.Thread(target=errs, daemon=True).start()
        for line in p.stdout:
            if line.startswith('{'): broadcast(line.strip())
        p.wait(); broadcast(json.dumps({'status': 'demo exited (%s), restarting…' % p.returncode})); time.sleep(2)

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path in ('/', '/index.html'):
            body = open(os.path.join(HERE, 'board.html'), 'rb').read()
            self.send_response(200); self.send_header('Content-Type', 'text/html; charset=utf-8'); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
        elif self.path == '/events':
            self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.send_header('Cache-Control', 'no-cache'); self.end_headers()
            self.wfile.write(b'data: ' + json.dumps(latest).encode() + b'\n\n'); self.wfile.flush()
            with lock: clients.append(self)
            while self in clients: time.sleep(1)
        elif self.path.startswith('/rec'):
            # /rec?session=NAME&label=press_6  -> boarddemo saves labeled frames;  /rec (no label) stops
            from urllib.parse import urlparse, parse_qs
            q = parse_qs(urlparse(self.path).query); label = q.get('label', [''])[0]; sess = q.get('session', ['s'])[0]
            safe = lambda t: ''.join(ch for ch in t if ch.isalnum() or ch in '-_')
            with open(os.path.join(HERE, 'rec.label'), 'w') as f:
                if label: f.write(safe(sess) + '/' + safe(label))
            self.send_response(204); self.end_headers()
        elif self.path == '/reset':
            # restart the tracker; run_demo() relaunches it and it re-captures the empty board
            subprocess.run(['slay', '-f', os.environ.get('TRACKER', 'boarddemo')], stderr=subprocess.DEVNULL)
            broadcast(json.dumps({'status': 'resetting: keep hands off the board…'}))
            self.send_response(204); self.end_headers()
        else: self.send_error(404)

def relay(listen_port, target=('127.0.0.1', 8080)):
    """dj_engine only listens on IPv4; this dual-stack TCP pass-through makes its phone page and audio stream
    reachable on networks (phone hotspots) that route LAN traffic over IPv6:  http://qnxpi.local:8082"""
    srv = socket.socket(socket.AF_INET6, socket.SOCK_STREAM); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0); srv.bind(('::', listen_port)); srv.listen(16)
    def pipe(a, b):
        try:
            while True:
                d = a.recv(65536)
                if not d: break
                b.sendall(d)
        except OSError: pass
        finally:
            for x in (a, b):
                try: x.shutdown(socket.SHUT_RDWR)
                except OSError: pass
    while True:
        c, _ = srv.accept()
        try: u = socket.create_connection(target, timeout=3); u.settimeout(None)
        except OSError: c.close(); continue
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1); u.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=pipe, args=(c, u), daemon=True).start(); threading.Thread(target=pipe, args=(u, c), daemon=True).start()

if os.environ.get('DJ_RELAY', '8082') != 'off': threading.Thread(target=relay, args=(int(os.environ.get('DJ_RELAY', '8082')),), daemon=True).start()
threading.Thread(target=run_demo, daemon=True).start()
print('Open http://qnxpi.local:%d in a browser on the same network. Ctrl+C stops.' % PORT, flush=True)
print('DJ bridge:', 'off' if dj_sock is None else 'UDP %s:%d' % dj_addr, flush=True)
class DualStack(ThreadingHTTPServer):
    address_family = socket.AF_INET6          # phone hotspots often route LAN traffic over IPv6
    def server_bind(self):
        self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)   # also accept IPv4
        super().server_bind()
DualStack(('::', PORT), H).serve_forever()
