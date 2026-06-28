#!/usr/bin/env python3
"""
HTTP Long-Poll Relay Server — works through any HTTP proxy (OnRender).
No WebSocket needed. Uses simple POST/GET for message passing.

POST /register   — register as client/server, returns session ID
POST /send?id=X  — send binary message to relay
GET  /poll?id=X  — long-poll for pending messages (30s timeout)
GET  /            — status
GET  /health      — health check
"""
import os
import sys
import time
import uuid
import asyncio
from aiohttp import web

# ── Protocol constants ─────────────────────────────────────────────
MSG_REGISTER    = 1
MSG_CLIENT_LIST = 2
MSG_COMMAND     = 3
MSG_RESPONSE    = 4
ROLE_CLIENT     = 1
ROLE_SERVER     = 2

# ── Session ────────────────────────────────────────────────────────
next_id = 1

class Session:
    def __init__(self, sid, role, name):
        self.id = sid          # integer ID
        self.sid_str = ""      # string session token (for HTTP)
        self.role = role       # 1=client, 2=server
        self.name = name
        self.messages = asyncio.Queue()
        self.alive = True

sessions = {}          # sid_str -> Session
client_sessions = {}  # sid_str -> Session (only clients)
server_sessions = {}   # sid_str -> Session (only servers)

def build_client_list():
    import struct
    payload = struct.pack('<I', len(client_sessions))
    for sid_str, s in client_sessions.items():
        name = s.name.encode('utf-8')
        payload += struct.pack('<I', s.id)
        payload += struct.pack('<I', len(name)) + name
    return payload

def build_msg(msg_type, cmd_type, payload=b''):
    import struct
    return struct.pack('<IBB', len(payload), msg_type, cmd_type) + payload

async def broadcast_client_list():
    if not client_sessions:
        return
    payload = build_client_list()
    msg = build_msg(MSG_CLIENT_LIST, 0, payload)
    for sid_str, s in server_sessions.items():
        if s.alive:
            await s.messages.put(msg)

async def handle_register(request):
    body = await request.read()
    if len(body) < 1:
        return web.Response(text='Bad body', status=400)

    role = body[0]
    name = "unknown"
    if len(body) >= 5:
        import struct
        name_len = struct.unpack('<I', body[1:5])[0]
        name = body[5:5+name_len].decode('utf-8', errors='replace')

    global next_id
    sid_int = next_id
    next_id += 1
    sid_str = str(sid_int)
    s = Session(sid_int, role, name)
    s.sid_str = sid_str
    sessions[sid_str] = s

    if role == ROLE_CLIENT:
        client_sessions[sid_str] = s
        import struct
        sid_hash = struct.pack('<I', sid_int)
        await s.messages.put(build_msg(MSG_REGISTER, 0, sid_hash))
        await broadcast_client_list()
        print(f"[+] Client '{name}' id={sid_int}", flush=True)
    elif role == ROLE_SERVER:
        server_sessions[sid_str] = s
        payload = build_client_list()
        await s.messages.put(build_msg(MSG_CLIENT_LIST, 0, payload))
        print(f"[+] Server id={sid_int}", flush=True)
    else:
        del sessions[sid_str]
        return web.Response(text='Bad role', status=400)

    return web.Response(text=sid_str)

async def handle_send(request):
    """POST /send?id=XXX — body = binary message"""
    sid = request.query.get('id', '')
    s = sessions.get(sid)
    if not s or not s.alive:
        return web.Response(text='Bad session', status=403)

    body = await request.read()
    if not body:
        return web.Response(text='Empty', status=400)

    import struct
    if len(body) < 6:
        return web.Response(text='Too short', status=400)

    payload_size, msg_type, cmd_type = struct.unpack('<IBB', body[:6])
    payload = body[6:6+payload_size]

    if s.role == ROLE_SERVER:
        # Server → Client: MSG_COMMAND with target_id prefix
        if msg_type == MSG_COMMAND and len(payload) >= 4:
            target_id = struct.unpack('<I', payload[:4])[0]
            # Find client by integer ID
            for sid_str, cs in client_sessions.items():
                if cs.id == target_id:
                    fwd = build_msg(MSG_COMMAND, cmd_type, payload[4:])
                    await cs.messages.put(fwd)
                    break
    elif s.role == ROLE_CLIENT:
        # Client → Server: MSG_RESPONSE with source_id prefix
        if msg_type == MSG_RESPONSE:
            fwd = build_msg(MSG_RESPONSE, cmd_type, struct.pack('<I', s.id) + payload)
            for sid_str, ss in server_sessions.items():
                if ss.alive:
                    await ss.messages.put(fwd)

    return web.Response(text='OK')

async def handle_poll(request):
    """GET /poll?id=XXX — long-poll for messages (30s timeout)"""
    sid = request.query.get('id', '')
    s = sessions.get(sid)
    if not s or not s.alive:
        return web.Response(text='Bad session', status=403)

    try:
        # Wait up to 25 seconds for a message
        msg = await asyncio.wait_for(s.messages.get(), timeout=25.0)
        return web.Response(body=msg, content_type='application/octet-stream')
    except asyncio.TimeoutError:
        return web.Response(status=204)  # No content = no messages

async def handle_disconnect(request):
    """POST /disconnect?id=XXX"""
    sid = request.query.get('id', '')
    s = sessions.get(sid)
    if s:
        s.alive = False
        if sid in client_sessions:
            del client_sessions[sid]
            await broadcast_client_list()
            print(f"[-] Client sid={sid} disconnected", flush=True)
        if sid in server_sessions:
            del server_sessions[sid]
            print(f"[-] Server sid={sid} disconnected", flush=True)
        del sessions[sid]
    return web.Response(text='OK')

async def root_handler(request):
    n_c = len(client_sessions)
    n_s = len(server_sessions)
    return web.Response(text=f'Relay Server Running\nClients: {n_c}\nServers: {n_s}')

async def health_check(request):
    return web.Response(text='OK')

async def on_startup(app):
    print("[*] HTTP Relay Server starting...", flush=True)

def main():
    port = int(os.environ.get('PORT', '10000'))
    if len(sys.argv) > 1:
        port = int(sys.argv[1])

    app = web.Application()
    app.on_startup.append(on_startup)
    app.router.add_get('/', root_handler)
    app.router.add_get('/health', health_check)
    app.router.add_post('/register', handle_register)
    app.router.add_post('/send', handle_send)
    app.router.add_get('/poll', handle_poll)
    app.router.add_post('/disconnect', handle_disconnect)

    print("========================================", flush=True)
    print(f"  HTTP Relay Server on port {port}", flush=True)
    print(f"  POST /register   - register", flush=True)
    print(f"  POST /send?id=X  - send message", flush=True)
    print(f"  GET  /poll?id=X  - long-poll messages", flush=True)
    print(f"  GET  /health     - status", flush=True)
    print("========================================", flush=True)

    web.run_app(app, host='0.0.0.0', port=port, print=None)

if __name__ == '__main__':
    main()
