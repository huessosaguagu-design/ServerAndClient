#!/usr/bin/env python3
"""
WebSocket Relay Server for Server-Client Remote Control.
Uses aiohttp for WebSocket + HTTP health check on same port.
Deploy on OnRender — reads PORT from environment.
"""
import asyncio
import struct
import os
import sys
import traceback
from aiohttp import web, WSMsgType

# ── Protocol constants ─────────────────────────────────────────────
MSG_REGISTER    = 1
MSG_CLIENT_LIST = 2
MSG_COMMAND     = 3
MSG_RESPONSE    = 4
ROLE_CLIENT     = 1
ROLE_SERVER     = 2

# ── Shared state ───────────────────────────────────────────────────
clients = {}          # id -> websocket
client_names = {}     # id -> name
servers = []          # list of websockets
next_id = 1

def build_msg(msg_type, cmd_type, payload=b''):
    return struct.pack('<IBB', len(payload), msg_type, cmd_type) + payload

def recv_message(data: bytes):
    if len(data) < 6:
        return None
    payload_size, msg_type, cmd_type = struct.unpack('<IBB', data[:6])
    if len(data) < 6 + payload_size:
        return None
    payload = data[6:6 + payload_size]
    return msg_type, cmd_type, payload, 6 + payload_size

def build_client_list():
    payload = struct.pack('<I', len(clients))
    for cid, ws in list(clients.items()):
        name = client_names.get(cid, 'unknown').encode('utf-8')
        payload += struct.pack('<I', cid)
        payload += struct.pack('<I', len(name)) + name
    return payload

async def broadcast_client_list():
    payload = build_client_list()
    msg = build_msg(MSG_CLIENT_LIST, 0, payload)
    for ws in list(servers):
        try:
            await ws.send_bytes(msg)
        except Exception:
            pass

async def send_to_all_servers(msg: bytes):
    for ws in list(servers):
        try:
            await ws.send_bytes(msg)
        except Exception:
            pass

async def handle_websocket(ws):
    global next_id

    try:
        msg = await asyncio.wait_for(ws.receive(), timeout=15.0)
    except asyncio.TimeoutError:
        print("[!] WS: Register timeout")
        return

    if msg.type != WSMsgType.BINARY:
        print(f"[!] WS: Expected binary, got {msg.type}")
        return

    data = msg.data
    result = recv_message(data)
    if result is None:
        print("[!] WS: Bad message format")
        return

    msg_type, cmd_type, payload, consumed = result
    if msg_type != MSG_REGISTER or not payload:
        print(f"[!] WS: Not REGISTER (type={msg_type})")
        return

    role = payload[0]

    if role == ROLE_CLIENT:
        name = "client"
        if len(payload) > 4:
            name_len = struct.unpack('<I', payload[1:5])[0]
            name = payload[5:5 + name_len].decode('utf-8', errors='replace')

        cid = next_id
        next_id += 1
        clients[cid] = ws
        client_names[cid] = name

        ack = build_msg(MSG_REGISTER, 0, struct.pack('<I', cid))
        await ws.send_bytes(ack)

        await broadcast_client_list()
        print(f"[+] Client '{name}' registered as id={cid}", flush=True)

        try:
            async for msg in ws:
                if msg.type != WSMsgType.BINARY:
                    continue
                result = recv_message(msg.data)
                if result is None:
                    continue
                mt, ct, pl, _ = result
                if mt == MSG_RESPONSE:
                    fwd = build_msg(MSG_RESPONSE, ct, struct.pack('<I', cid) + pl)
                    await send_to_all_servers(fwd)
        except Exception as e:
            print(f"[!] Client {cid} error: {e}")

        if cid in clients:
            del clients[cid]
        if cid in client_names:
            del client_names[cid]
        await broadcast_client_list()
        print(f"[-] Client id={cid} disconnected", flush=True)

    elif role == ROLE_SERVER:
        servers.append(ws)

        pl = build_client_list()
        msg = build_msg(MSG_CLIENT_LIST, 0, pl)
        await ws.send_bytes(msg)
        print("[+] Server connected", flush=True)

        try:
            async for msg in ws:
                if msg.type != WSMsgType.BINARY:
                    continue
                result = recv_message(msg.data)
                if result is None:
                    continue
                mt, ct, pl, _ = result
                if mt == MSG_COMMAND and len(pl) >= 4:
                    target_id = struct.unpack('<I', pl[:4])[0]
                    fwd = build_msg(MSG_COMMAND, ct, pl[4:])
                    target_ws = clients.get(target_id)
                    if target_ws:
                        try:
                            await target_ws.send_bytes(fwd)
                        except Exception:
                            pass
        except Exception as e:
            print(f"[!] Server error: {e}")

        if ws in servers:
            servers.remove(ws)
        print("[-] Server disconnected", flush=True)

async def websocket_handler(request):
    # Check if this is actually a WebSocket upgrade request
    if not request.headers.get('Upgrade', '').lower() == 'websocket':
        # Regular HTTP request to /ws — return info page
        return web.Response(text='WebSocket endpoint. Use a WebSocket client to connect.', status=200)

    print(f"[WS] Connection from {request.remote}", flush=True)
    ws = web.WebSocketResponse(max_msg_size=0)
    try:
        await ws.prepare(request)
    except Exception as e:
        print(f"[!] WS prepare failed: {e}", flush=True)
        return ws

    print("[WS] Upgrade OK", flush=True)
    await handle_websocket(ws)
    return ws

async def health_check(request):
    n_clients = len(clients)
    n_servers = len(servers)
    return web.Response(text=f'OK\nClients: {n_clients}\nServers: {n_servers}')

async def root_handler(request):
    return web.Response(text='Relay Server Running\n\nEndpoints:\n  /ws - WebSocket\n  /health - Status')

async def on_startup(app):
    print("[*] Server starting...", flush=True)

async def on_cleanup(app):
    print("[*] Server shutting down...", flush=True)

def main():
    port = int(os.environ.get('PORT', '10000'))
    if len(sys.argv) > 1:
        port = int(sys.argv[1])

    app = web.Application()
    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    app.router.add_get('/', root_handler)
    app.router.add_get('/health', health_check)
    app.router.add_get('/ws', websocket_handler)

    print("========================================", flush=True)
    print(f"  WebSocket Relay Server on port {port}", flush=True)
    print(f"  Endpoints:", flush=True)
    print(f"    /ws      - WebSocket", flush=True)
    print(f"    /health  - HTTP health check", flush=True)
    print("========================================", flush=True)

    try:
        web.run_app(app, host='0.0.0.0', port=port, print=None)
    except Exception as e:
        print(f"[!] FATAL: {e}", flush=True)
        traceback.print_exc()

if __name__ == '__main__':
    main()
