#!/usr/bin/env python3
"""
Relay server for Server-Client remote control.
Routes messages between server and client applications.
Responds to HTTP health checks (OnRender requirement).

Usage:  python relay.py [port]
OnRender sets the PORT env var automatically.
"""
import socket
import struct
import threading
import os
import sys

# ── Protocol constants ─────────────────────────────────────────────
MSG_REGISTER    = 1
MSG_CLIENT_LIST = 2
MSG_COMMAND     = 3
MSG_RESPONSE    = 4
ROLE_CLIENT     = 1
ROLE_SERVER     = 2

# ── Shared state ───────────────────────────────────────────────────
clients = {}          # id -> socket
client_names = {}     # socket -> name
servers = []
next_id = 1
lock = threading.Lock()

def build_msg(msg_type, cmd_type, payload=b''):
    return struct.pack('<IBB', len(payload), msg_type, cmd_type) + payload

def recv_exact(sock, n):
    data = b''
    while len(data) < n:
        try:
            chunk = sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        except Exception:
            return None
    return data

def recv_message(sock):
    header = recv_exact(sock, 6)
    if not header:
        return None, None, None
    payload_size, msg_type, cmd_type = struct.unpack('<IBB', header)
    if payload_size > 64 * 1024 * 1024:
        return None, None, None
    payload = b''
    if payload_size > 0:
        payload = recv_exact(sock, payload_size)
        if payload is None:
            return None, None, None
    return msg_type, cmd_type, payload

def build_client_list():
    with lock:
        payload = struct.pack('<I', len(clients))
        for cid, csock in list(clients.items()):
            name = client_names.get(csock, 'unknown').encode('utf-8')
            payload += struct.pack('<I', cid)
            payload += struct.pack('<I', len(name)) + name
    return payload

def broadcast_client_list():
    payload = build_client_list()
    msg = build_msg(MSG_CLIENT_LIST, 0, payload)
    with lock:
        srvs = list(servers)
    for s in srvs:
        try:
            s.sendall(msg)
        except Exception:
            pass

def send_to_all_servers(msg):
    with lock:
        srvs = list(servers)
    for s in srvs:
        try:
            s.sendall(msg)
        except Exception:
            pass

def is_http_request(data):
    return data[:3] in (b'GET', b'POS', b'HEA', b'PUT', b'DEL', b'OPT')

def handle_http(sock):
    sock.settimeout(1.0)
    try:
        while True:
            data = sock.recv(4096)
            if not data:
                break
    except Exception:
        pass
    try:
        sock.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK')
    except Exception:
        pass
    sock.close()

def handle_connection(sock, addr):
    global next_id

    header = recv_exact(sock, 6)
    if not header:
        sock.close()
        return

    if is_http_request(header):
        handle_http(sock)
        return

    payload_size, msg_type, cmd_type = struct.unpack('<IBB', header)
    if payload_size > 64 * 1024 * 1024:
        sock.close()
        return

    payload = b''
    if payload_size > 0:
        payload = recv_exact(sock, payload_size)
        if payload is None:
            sock.close()
            return

    if msg_type != MSG_REGISTER or not payload:
        sock.close()
        return

    role = payload[0]

    if role == ROLE_CLIENT:
        name = "client"
        if len(payload) > 4:
            name_len = struct.unpack('<I', payload[1:5])[0]
            name = payload[5:5+name_len].decode('utf-8', errors='replace')

        with lock:
            cid = next_id
            next_id += 1
            clients[cid] = sock
            client_names[sock] = name

        ack = build_msg(MSG_REGISTER, 0, struct.pack('<I', cid))
        try:
            sock.sendall(ack)
        except Exception:
            pass

        broadcast_client_list()
        print(f"[+] Client '{name}' registered as id={cid}")

        while True:
            mt, ct, pl = recv_message(sock)
            if mt is None:
                break
            if mt == MSG_RESPONSE:
                fwd = build_msg(MSG_RESPONSE, ct, struct.pack('<I', cid) + pl)
                send_to_all_servers(fwd)

        with lock:
            if cid in clients:
                del clients[cid]
            if sock in client_names:
                del client_names[sock]
        sock.close()
        broadcast_client_list()
        print(f"[-] Client id={cid} disconnected")

    elif role == ROLE_SERVER:
        with lock:
            servers.append(sock)

        payload = build_client_list()
        msg = build_msg(MSG_CLIENT_LIST, 0, payload)
        try:
            sock.sendall(msg)
        except Exception:
            pass
        print("[+] Server connected")

        while True:
            mt, ct, pl = recv_message(sock)
            if mt is None:
                break
            if mt == MSG_COMMAND and len(pl) >= 4:
                target_id = struct.unpack('<I', pl[:4])[0]
                fwd = build_msg(MSG_COMMAND, ct, pl[4:])
                with lock:
                    csock = clients.get(target_id)
                if csock:
                    try:
                        csock.sendall(fwd)
                    except Exception:
                        pass

        with lock:
            if sock in servers:
                servers.remove(sock)
        sock.close()
        print("[-] Server disconnected")

    else:
        sock.close()

def main():
    port = int(os.environ.get('PORT', '10000'))
    if len(sys.argv) > 1:
        port = int(sys.argv[1])

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', port))
    srv.listen(8)

    print("========================================")
    print(f"  Relay server (Python) on port {port}")
    print("========================================")

    while True:
        client, addr = srv.accept()
        t = threading.Thread(target=handle_connection, args=(client, addr), daemon=True)
        t.start()

if __name__ == '__main__':
    main()
