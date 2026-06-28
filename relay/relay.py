#!/usr/bin/env python3
"""
WebSocket Relay Server for Server-Client Remote Control.
Uses aiohttp for WebSocket + HTTP health check on same port.
Deploy on OnRender — reads PORT from environment.

Usage:  python relay.py [port]
"""
import asyncio
import struct
import os
import sys
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
    """Parse one message from raw bytes. Returns (msg_type, cmd_type, payload) or None."""
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
    """Handle a WebSocket connection (client or server)."""
    global next_id

    # Wait for REGISTER message
    try:
        msg = await asyncio.wait_for(ws.receive(), timeout=10.0)
    except asyncio.TimeoutError:
        return

    if msg.type != WSMsgType.BINARY:
        return

    data = msg.data
    result = recv_message(data)
    if result is None:
        return

    msg_type, cmd_type, payload, consumed = result
    if msg_type != MSG_REGISTER or not payload:
        return

    role = payload[0]

    if role == ROLE_CLIENT:
        # ── Client registration ─────────────────────────────────
        name = "client"
        if len(payload) > 4:
            name_len = struct.unpack('<I', payload[1:5])[0]
            name = payload[5:5 + name_len].decode('utf-8', errors='replace')

        cid = next_id
        next_id += 1
        clients[cid] = ws
        client_names[cid] = name

        # Send assigned ID back
        ack = build_msg(MSG_REGISTER, 0, struct.pack('<I', cid))
        await ws.send_bytes(ack)

        await broadcast_client_list()
        print(f"[+] Client '{name}' registered as id={cid}")

        # ── Client message loop ──────────────────────────────────
        try:
            async for msg in ws:
                if msg.type != WSMsgType.BINARY:
                    continue
                result = recv_message(msg.data)
                if result is None:
                    continue
                mt, ct, pl, _ = result
                if mt == MSG_RESPONSE:
                    # Prepend source client ID, forward to all servers
                    fwd = build_msg(MSG_RESPONSE, ct, struct.pack('<I', cid) + pl)
                    await send_to_all_servers(fwd)
        except Exception:
            pass

        # Cleanup
        if cid in clients:
            del clients[cid]
        if cid in client_names:
            del client_names[cid]
        await broadcast_client_list()
        print(f"[-] Client id={cid} disconnected")

    elif role == ROLE_SERVER:
        # ── Server registration ──────────────────────────────────
        servers.append(ws)

        # Send current client list
        pl = build_client_list()
        msg = build_msg(MSG_CLIENT_LIST, 0, pl)
        await ws.send_bytes(msg)
        print("[+] Server connected")

        # ── Server message loop ──────────────────────────────────
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
        except Exception:
            pass

        # Cleanup
        if ws in servers:
            servers.remove(ws)
        print("[-] Server disconnected")

async def websocket_handler(request):
    ws = web.WebSocketResponse(max_msg_size=0)  # no size limit
    await ws.prepare(request)
    await handle_websocket(ws)
    return ws

async def health_check(request):
    """HTTP health check endpoint for OnRender."""
    return web.Response(text='OK')

async def root_handler(request):
    """Root path — return simple status."""
    return web.Response(text='Relay Server Running')

def main():
    port = int(os.environ.get('PORT', '10000'))
    if len(sys.argv) > 1:
        port = int(sys.argv[1])

    app = web.Application()
    app.router.add_get('/', root_handler)
    app.router.add_get('/health', health_check)
    app.router.add_get('/ws', websocket_handler)

    print(f"========================================")
    print(f"  WebSocket Relay Server on port {port}")
    print(f"  Endpoints:")
    print(f"    /ws      — WebSocket (client/server)")
    print(f"    /health  — HTTP health check")
    print(f"========================================")

    web.run_app(app, port=port, print=None)

if __name__ == '__main__':
    main()
