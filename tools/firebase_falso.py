#!/usr/bin/env python3
"""Imitação mínima do Firebase Realtime Database (REST + streaming) para testar o multiplayer sem internet."""
import json, threading, time, itertools, sys
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

tree = {}
lock = threading.Lock()
listeners = []  # (path_parts, queue_list, cond)
counter = itertools.count()


def parts(p):
    p = urlparse(p).path
    if p.endswith('.json'):
        p = p[:-5]
    return [x for x in p.split('/') if x]


def sv(v):
    if isinstance(v, dict):
        if v == {'.sv': 'timestamp'}:
            return int(time.time() * 1000)
        return {k: sv(x) for k, x in v.items()}
    return v


def get(ps):
    n = tree
    for p in ps:
        if not isinstance(n, dict) or p not in n:
            return None
        n = n[p]
    return n


def setv(ps, v):
    global tree
    if not ps:
        tree = v if isinstance(v, dict) else {}
        return
    n = tree
    for p in ps[:-1]:
        if not isinstance(n.get(p), dict):
            n[p] = {}
        n = n[p]
    if v is None:
        n.pop(ps[-1], None)
    else:
        n[ps[-1]] = v


def notify(ps, event, data):
    for lp, q, cond in list(listeners):
        if ps[:len(lp)] == lp:
            rel = '/' + '/'.join(ps[len(lp):])
            with cond:
                q.append((event, rel, data))
                cond.notify()
        elif lp[:len(ps)] == ps:  # escreveu acima do caminho ouvido
            with cond:
                q.append(('put', '/', get(lp)))
                cond.notify()


class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *a):
        pass

    def body(self):
        n = int(self.headers.get('Content-Length') or 0)
        return json.loads(self.rfile.read(n).decode()) if n else None

    def reply(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        ps = parts(self.path)
        if 'text/event-stream' in (self.headers.get('Accept') or ''):
            q, cond = [], threading.Condition()
            with lock:
                q.append(('put', '/', get(ps)))
                listeners.append((ps, q, cond))
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            try:
                while True:
                    with cond:
                        while not q:
                            if not cond.wait(5):
                                break
                        items, q[:] = list(q), []
                    if not items:
                        self.wfile.write(b'event: keep-alive\ndata: null\n\n'); self.wfile.flush(); continue
                    for ev, rel, data in items:
                        self.wfile.write(f'event: {ev}\ndata: {json.dumps({"path": rel, "data": data})}\n\n'.encode())
                    self.wfile.flush()
            except Exception:
                pass
            finally:
                with lock:
                    listeners[:] = [l for l in listeners if l[1] is not q]
            return
        with lock:
            self.reply(get(ps))

    def write(self, method):
        ps = parts(self.path)
        m = self.headers.get('X-HTTP-Method-Override') or method
        data = sv(self.body())
        with lock:
            if m == 'PUT':
                setv(ps, data); notify(ps, 'put', data); out = data
            elif m == 'PATCH':
                for k, v in (data or {}).items():
                    setv(ps + k.split('/'), v)
                notify(ps, 'patch', data); out = data
            elif m == 'POST':
                key = '-K%012d' % next(counter)
                setv(ps + [key], data); notify(ps + [key], 'put', data); out = {'name': key}
            elif m == 'DELETE':
                setv(ps, None); notify(ps, 'put', None); out = None
        self.reply(out)

    def do_PUT(self): self.write('PUT')
    def do_POST(self): self.write('POST')
    def do_DELETE(self): self.write('DELETE')


ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1]) if len(sys.argv) > 1 else 8765), H).serve_forever()
