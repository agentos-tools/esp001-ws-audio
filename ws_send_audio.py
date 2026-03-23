#!/usr/bin/env python3
"""
Send audio to ESP32 via WebSocket and monitor the echo response.
Usage: python3 ws_send_audio.py <audio_file.wav> [esp32_ws_url]
"""
import asyncio
import websockets
import wave
import sys
import time

DEFAULT_URL = "ws://192.168.8.191:8080"


async def send_audio_and_monitor(audio_file, url=None):
    """Send audio to ESP32 and monitor the echo."""
    
    # Read WAV file
    with wave.open(audio_file, 'rb') as wf:
        channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        framerate = wf.getframerate()
        nframes = wf.getnframes()
        audio_data = wf.readframes(nframes)
    
    print(f"Audio: {framerate}Hz, {channels}ch, {sampwidth}B/sample, {nframes} frames ({len(audio_data)} bytes, {nframes/framerate:.1f}s)")
    
    uri = url or DEFAULT_URL
    print(f"Connecting to {uri}...")
    
    try:
        async with websockets.connect(uri, ping_interval=None) as ws:
            print("Connected! Sending audio...")
            start = time.time()
            
            # Send audio in chunks (matching ESP32's 2048-byte frames)
            chunk_size = 2048
            chunks_sent = 0
            total_sent = 0
            
            for i in range(0, len(audio_data), chunk_size):
                chunk = audio_data[i:i+chunk_size]
                await ws.send(chunk)
                chunks_sent += 1
                total_sent += len(chunk)
                
                if chunks_sent % 10 == 0:
                    elapsed = time.time() - start
                    print(f"  Sent {chunks_sent} chunks, {total_sent} bytes ({elapsed:.1f}s)")
                
                # Small delay to simulate real-time streaming
                await asyncio.sleep(0.01)
            
            elapsed = time.time() - start
            print(f"Sent all {chunks_sent} chunks, {total_sent} bytes in {elapsed:.1f}s")
            
            # Wait for echo response
            print("Waiting for echo response...")
            echo_count = 0
            echo_bytes = 0
            echo_start = time.time()
            
            try:
                while echo_count < chunks_sent:
                    data = await asyncio.wait_for(ws.recv(), timeout=10.0)
                    echo_bytes += len(data)
                    echo_count += 1
                    if echo_count % 20 == 0:
                        print(f"  Echo {echo_count}/{chunks_sent} ({echo_bytes} bytes)")
            except asyncio.TimeoutError:
                print(f"Timeout: received {echo_count}/{chunks_sent} echo packets")
            
            echo_duration = time.time() - echo_start
            print(f"Echo: {echo_count} packets, {echo_bytes} bytes in {echo_duration:.1f}s")
            
    except Exception as e:
        print(f"Error: {e}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 ws_send_audio.py <wav_file> [ws_url]")
        print("  wav_file: Path to WAV file (16kHz, 16-bit mono recommended)")
        print("  ws_url: WebSocket URL (default: ws://192.168.8.191:8080)")
        sys.exit(1)
    
    audio_file = sys.argv[1]
    url = sys.argv[2] if len(sys.argv) > 2 else None
    
    asyncio.run(send_audio_and_monitor(audio_file, url))
