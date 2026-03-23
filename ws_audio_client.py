#!/usr/bin/env python3
"""
WebSocket audio client - connects to ESP32 and plays audio.
Usage: python3 ws_audio_client.py [esp32_ip]
"""
import asyncio
import websockets
import wave
import struct
import sys
import numpy as np

ESP32_IP = "192.168.8.191"  # ESP32's IP
WS_PORT = 8080
AUDIO_FILE = "/tmp/esp32_audio.wav"


async def audio_client(esp32_ip=None):
    uri = f"ws://{esp32_ip or ESP32_IP}:{WS_PORT}"
    print(f"Connecting to {uri}...")
    
    # Create WAV file to save received audio
    wav_file = open(AUDIO_FILE, 'wb')
    wav_writer = wave.open(wav_file, 'wb')
    wav_writer.setnchannels(1)
    wav_writer.setsampwidth(2)  # 16-bit
    wav_writer.setframerate(16000)
    
    total_bytes = 0
    packet_count = 0
    
    try:
        async with websockets.connect(uri) as ws:
            print(f"Connected! Waiting for audio data...")
            
            while True:
                try:
                    data = await asyncio.wait_for(ws.recv(), timeout=5.0)
                    packet_count += 1
                    total_bytes += len(data)
                    
                    # Write to WAV
                    wav_writer.writeframes(data)
                    
                    # Print stats every 50 packets
                    if packet_count % 50 == 0:
                        duration = total_bytes / (16000 * 2)  # 16kHz, 16-bit mono
                        print(f"  [{packet_count}] {len(data)} bytes | total: {total_bytes} ({duration:.1f}s) | {packet_count/duration:.1f} pkt/s")
                    
                except asyncio.TimeoutError:
                    print(f"Timeout waiting for audio (got {packet_count} packets, {total_bytes} bytes)")
                    break
                    
    except Exception as e:
        print(f"Error: {e}")
    finally:
        wav_writer.close()
        wav_file.close()
        duration = total_bytes / (16000 * 2)
        print(f"Total: {packet_count} packets, {total_bytes} bytes ({duration:.1f}s)")
        print(f"Saved to: {AUDIO_FILE}")


if __name__ == "__main__":
    ip = sys.argv[1] if len(sys.argv) > 1 else None
    asyncio.run(audio_client(ip))
