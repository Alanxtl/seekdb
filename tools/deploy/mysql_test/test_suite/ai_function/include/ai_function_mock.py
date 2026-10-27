#!/usr/bin/env python3
# Copyright (c) 2025 OceanBase.
# Licensed under the Apache License, Version 2.0.
"""Deterministic AI protocols, request assertions and controlled failure barriers."""
import argparse
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        self.config = {}
        self.release = threading.Event()
        self.release.set()


STATE = State()


def embedding(text, dimension=4):
    digest = hashlib.sha256(text.encode('utf-8')).digest()
    return [round((digest[i % len(digest)] - 128) / 128, 6) for i in range(dimension)]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def send_json(self, value, status=200):
        if hasattr(self, 'request_record'):
            with STATE.lock:
                self.request_record['done'] = True
        body = json.dumps(value, ensure_ascii=False, allow_nan=False).encode('utf-8')
        try:
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        if self.path == '/requests':
            with STATE.lock:
                result = list(STATE.requests)
            self.send_json(result)
        elif self.path == '/health':
            self.send_json({'ready': True})
        else:
            self.send_json({'error': 'not found'}, 404)

    def do_POST(self):
        try:
            length = int(self.headers.get('Content-Length', 0))
            if not 0 < length <= 8 * 1024 * 1024:
                return self.send_json({'error': 'invalid length'}, 400)
            data = json.loads(self.rfile.read(length))
        except (ValueError, UnicodeError):
            return self.send_json({'error': 'invalid JSON'}, 400)
        if self.path == '/control':
            with STATE.lock:
                STATE.config = dict(data)
                if data.get('clear', True):
                    STATE.requests.clear()
                if data.get('block'):
                    STATE.release = threading.Event()
                else:
                    STATE.release.set()
            return self.send_json({'ok': True})
        with STATE.lock:
            config = dict(STATE.config)
            barrier = STATE.release
            self.request_record = {'path': self.path, 'body': data, 'done': False}
            STATE.requests.append(self.request_record)
            if len(STATE.requests) > 10000:
                del STATE.requests[:1000]
            failures = STATE.config.get('failures', 0)
            if failures:
                STATE.config['failures'] = failures - 1
        request_text = data.get('prompt', data.get('messages', [{}])[-1].get('content', ''))
        if config.get('block') and (not config.get('block_text') or config['block_text'] == request_text) and not barrier.wait(30):
            return self.send_json({'error': 'barrier timeout'}, 503)
        if failures:
            return self.send_json({'error': 'injected failure'}, config.get('status', 429))
        mode = config.get('mode', '')
        if mode == 'disconnect':
            self.close_connection = True
            return
        if mode == 'root_array':
            return self.send_json([])
        if 'documents' in data:
            results = [{'index': i, 'relevance_score': (100 if text == data.get('query') else 0) + sum(text.encode('utf-8')) % 5}
                       for i, text in enumerate(data['documents'])]
            if mode == 'duplicate' and len(results) > 1:
                results[-1]['index'] = 0
            if mode == 'missing':
                results = results[:-1]
            if mode == 'out_of_range':
                results[-1]['index'] = len(results) + 1
            return self.send_json({'results': list(reversed(results))})
        if 'input' in data:
            texts = data['input'] if isinstance(data['input'], list) else [data['input']]
            dimension = data.get('dimensions', 768 if data.get('model') in ('text-embedding-v1', 'nomic-embed-text') else 4)
            vectors = [embedding(text, dimension) for text in texts]
            if mode == 'dimension':
                vectors[-1].append(0)
            if mode == 'element':
                vectors[-1][-1] = 'invalid'
            if self.path.startswith('/api/'):
                return self.send_json({'embeddings': vectors})
            results = [{'index': i, 'embedding': vector} for i, vector in enumerate(vectors)]
            if mode == 'duplicate' and len(results) > 1:
                results[-1]['index'] = 0
            if mode == 'missing':
                results = results[:-1]
            if mode == 'out_of_range':
                results[-1]['index'] = len(results) + 1
            if mode == 'wrong_type':
                return self.send_json({'data': {}})
            return self.send_json({'data': list(reversed(results))})
        prompt = data.get('prompt')
        if prompt is None:
            prompt = data.get('messages', [{}])[-1].get('content', '')
        content = json.dumps({'text': prompt}, ensure_ascii=False)
        if data.get('model') == 'column-mock':
            system = ' '.join(m.get('content', '') for m in data.get('messages', []) if m.get('role') == 'system')
            content = '7' if 'base-10 integer' in system else json.dumps({'input': prompt}) if 'valid JSON value' in system else 'mock:' + prompt
            if mode == 'invalid_json':
                content = 'invalid JSON value'
            if mode == 'invalid_integer':
                content = 'seven'
        if mode == 'empty':
            content = ''
        if mode == 'content_object':
            content = {'invalid': True}
        if self.path.startswith('/api/'):
            if data.get('stream') is not False:
                return self.send_json({'error': 'stream must be false'}, 400)
            return self.send_json({'response': content})
        return self.send_json({'choices': [{'message': {'content': content}}]})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8000)
    args = parser.parse_args()
    server = ThreadingHTTPServer(('0.0.0.0', args.port), Handler)
    server.daemon_threads = True
    server.serve_forever()


if __name__ == '__main__':
    main()
