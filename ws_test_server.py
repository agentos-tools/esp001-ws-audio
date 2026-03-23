#!/usr/bin/env python3
"""
ESP001 WebSocket Test Server
- Echoes received audio data back to client
- Logs connection events and audio stats
"""

import asyncio
import websockets
import signal
import sys
import struct
import time

PORT = 8080

connected_clients = set()
start_time = time.time()

async def echo_handler(websocket):
    client_addr = websocket.remote_address
    print(f"[+] Client connected: {client_addr}")
    connected_clients.add(websocket)
    
    # Notify all existing clients
    try:
        await websocket.send(f"Server: connected ({len(connected_clients)} clients)".encode())
    except:
        pass
    
    total_bytes = 0
    packet_count = 0
    
    try:
        async for message in websocket:
            packet_count += 1
            data_len = len(message)
            total_bytes += data_len
            
            # Print stats every 50 packets
            if packet_count % 50 == 0:
                elapsed = time.time() - start_time
                print(f"    [{packet_count}] {data_len} bytes | total: {total_bytes} | {packet_count/elapsed:.1f} pkt/s")
            
            # Echo back to sender only (simple loopback)
            try:
                await websocket.send(message)
            except Exception as e:
                print(f"    Echo error: {e}")
                
    except websockets.exceptions.ConnectionClosedOK:
        print(f"[-] Client disconnected (normal): {client_addr}")
    except websockets.exceptions.ConnectionClosedError as e:
        print(f"[-] Client disconnected (error): {client_addr} -> {e}")
    finally:
        connected_clients.discard(websocket)
        print(f"    Clients remaining: {len(connected_clients)}")

async def main():
    print(f"Starting ESP001 WebSocket Test Server on port {PORT}...")
    print(f"Will echo all received data back to sender")
    
    stop = asyncio.Future()
    
    # Handle SIGINT
    def signal_handler(sig, frame):
        print("\nShutting down...")
        stop.set_result(None)
    
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    try:
        async with websockets.serve(echo_handler, "0.0.0.0", PORT):
            print(f"Server ready. Waiting for connections...")
            await stop
    except OSError as e:
        if e.errno == 98:  # Address already in use
            print(f"Error: Port {PORT} is already in use")
            sys.exit(1)
        raise

if __name__ == "__main__":
    # Check if port is available
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("0.0.0.0", PORT))
        s.close()
    except OSError:
        print(f"Error: Port {PORT} is already in use")
        sys.exit(1)
    
    asyncio.run(main())
