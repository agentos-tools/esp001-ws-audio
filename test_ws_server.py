#!/usr/bin/env python3
"""
ESP001 WebSocket Test Server
Simple WebSocket server for testing ESP32-S3 WebSocket client
"""

import asyncio
import websockets
import json
from datetime import datetime

async def test_server(websocket):
    """Handle WebSocket connections"""
    client_ip = websocket.remote_address[0] if websocket.remote_address else "unknown"
    print(f"[{datetime.now().strftime('%H:%M:%S')}] Client connected from {client_ip}")
    
    try:
        async for message in websocket:
            if isinstance(message, str):
                # Text message
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Received text: {message[:100]}...")
                
                # Echo back
                response = json.dumps({
                    "type": "echo",
                    "data": message,
                    "timestamp": datetime.now().isoformat()
                })
                await websocket.send(response)
                
            elif isinstance(message, bytes):
                # Binary message (audio data)
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Received binary: {len(message)} bytes")
                
                # Echo back the same data
                await websocket.send(message)
                
    except websockets.exceptions.ConnectionClosed as e:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Client disconnected: {e}")
    except Exception as e:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Error: {e}")

async def main():
    """Start WebSocket server"""
    host = "0.0.0.0"
    port = 8080
    
    print(f"ESP001 WebSocket Test Server")
    print(f"=" * 40)
    print(f"Starting server on {host}:{port}")
    print(f"WebSocket URL: ws://{host}:{port}")
    print(f"")
    print(f"Waiting for connections...")
    
    async with websockets.serve(test_server, host, port):
        await asyncio.Future()  # Run forever

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServer stopped")
