#!/usr/bin/env python3
"""
HTTP Long-Poll Relay Server — works through any HTTP proxy (Railway/OnRender).
"""
import os
import sys
import struct
import asyncio
from aiohttp import web

MSG_REGISTER    = 1
MSG_CLIENT_LIST = 2
MSG_COMMAND     = 3
MSG_RESPONSE    = 4
ROLE_CLIENT     = 1
ROLE_SERVER     = 2

next_id = 1

class Session:
    def __init__(self, sid_int, role, name):
        self.id = sid_int
        self.role = role
        self.name = name
        self.messages = asyncio.Queue()
        self.alive = True

sessions = {}
client_sessions = {}
server_sessions = {}

def build_msg(msg_type, cmd_type, payload=b''):
    return struct.pack('<IBB', len(payload), msg_type, cmd_type) + payload

def build_client_list():
    payload = struct.pack('<I', len(client_sessions))
    for sid_str, s in client_sessions.items():
        name = s.name.encode('utf-8')
        payload += struct.pack('<I', s.id)
        payload += struct.pack('<I', len(name)) + name
    return payload

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
        name_len = struct.unpack('<I', body[1:5])[0]
        name = body[5:5+name_len].decode('utf-8', errors='replace')

    global next_id
    sid_int = next_id
    next_id += 1
    sid_str = str(sid_int)
    s = Session(sid_int, role, name)
    sessions[sid_str] = s

    if role == ROLE_CLIENT:
        client_sessions[sid_str] = s
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
    sid = request.query.get('id', '')
    s = sessions.get(sid)
    if not s or not s.alive:
        return web.Response(text='Bad session', status=403)

    body = await request.read()
    if not body or len(body) < 6:
        return web.Response(text='Bad message', status=400)

    payload_size, msg_type, cmd_type = struct.unpack('<IBB', body[:6])
    payload = body[6:6+payload_size]

    if s.role == ROLE_SERVER:
        if msg_type == MSG_COMMAND and len(payload) >= 4:
            target_id = struct.unpack('<I', payload[:4])[0]
            for sid_str, cs in client_sessions.items():
                if cs.id == target_id:
                    fwd = build_msg(MSG_COMMAND, cmd_type, payload[4:])
                    await cs.messages.put(fwd)
                    break
    elif s.role == ROLE_CLIENT:
        if msg_type == MSG_RESPONSE:
            fwd = build_msg(MSG_RESPONSE, cmd_type, struct.pack('<I', s.id) + payload)
            for sid_str, ss in server_sessions.items():
                if ss.alive:
                    await ss.messages.put(fwd)

    return web.Response(text='OK')

async def handle_poll(request):
    sid = request.query.get('id', '')
    s = sessions.get(sid)
    if not s or not s.alive:
        return web.Response(text='Bad session', status=403)

    try:
        msg = await asyncio.wait_for(s.messages.get(), timeout=25.0)
        return web.Response(body=msg, content_type='application/octet-stream')
    except asyncio.TimeoutError:
        return web.Response(status=204)

async def handle_disconnect(request):
    sid = request.query.get('id', '')
    s = sessions.get(sid)
    if s:
        s.alive = False
        if sid in client_sessions:
            del client_sessions[sid]
            await broadcast_client_list()
            print(f"[-] Client id={s.id} disconnected", flush=True)
        if sid in server_sessions:
            del server_sessions[sid]
            print(f"[-] Server id={s.id} disconnected", flush=True)
        del sessions[sid]
    return web.Response(text='OK')

async def root_handler(request):
    return web.Response(text=f'Relay Server Running\nClients: {len(client_sessions)}\nServers: {len(server_sessions)}')

async def health_check(request):
    return web.Response(text='OK')

def main():
    port = int(os.environ.get('PORT', '10000'))
    if len(sys.argv) > 1:
        port = int(sys.argv[1])

    app = web.Application()
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
