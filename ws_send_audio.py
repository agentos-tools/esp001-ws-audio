#!/usr/bin/env python3
"""
Send audio file to ESP32 via WebSocket.
Usage: python3 ws_send_audio.py <wav_file> [esp32_ip]
"""
import asyncio
import sys
import wave
import struct

WS_HOST = "192.168.8.234"  # PC's IP (where this runs)
WS_PORT = 8080


async def send_audio(filename, target_ip=None):
    # Read WAV file
    with wave.open(filename, 'rb') as wf:
        channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        framerate = wf.getframerate()
        nframes = wf.getnframes()
        audio_data = wf.readframes(nframes)
    
    print(f"Audio: {framerate}Hz, {channels}ch, {sampwidth} bytes/sample, {nframes} frames ({nframes/framerate:.1f}s)")
    
    # For ESP32, we need to send raw PCM (16-bit mono at 16kHz)
    # If the WAV is stereo or different rate, we'd need to convert
    
    uri = f"ws://{target_ip or WS_HOST}:{WS_PORT}"
    print(f"Connecting to {uri}...")
    
    try:
        async with asyncio.timeout(10):
            async with asyncio.get_event_loop().run_in_executor(
                None, __import__('websockets').connect, uri
            ) as ws:
                print(f"Connected! Sending {len(audio_data)} bytes...")
                
                # Send as binary WebSocket frame
                await ws.send(audio_data)
                print(f"Sent {len(audio_data)} bytes of audio data")
                
                # Wait for response (echo)
                try:
                    response = await asyncio.wait_for(ws.recv(), timeout=5)
                    print(f"Received echo: {len(response)} bytes")
                except asyncio.TimeoutError:
                    print("No echo received (timeout)")
                    
    except Exception as e:
        print(f"Error: {e}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 ws_send_audio.py <wav_file> [target_ip]")
        print("  wav_file: Path to WAV file (16kHz, 16-bit mono recommended)")
        print("  target_ip: Target IP (default: 192.168.8.234)")
        sys.exit(1)
    
    filename = sys.argv[1]
    target_ip = sys.argv[2] if len(sys.argv) > 2 else None
    
    asyncio.run(send_audio(filename, target_ip))
