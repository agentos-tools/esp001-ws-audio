#!/usr/bin/env python3
"""
ESP001 USB Command Test Script

Test USB serial commands for ESP32-S3 WebSocket Audio device.

Usage:
    python test_usb_cmd.py [command] [args]
    
Commands:
    status              - Query device status
    connect-to <url>    - Set WebSocket server URL
    connect             - Initiate WebSocket connection
    disconnect          - Disconnect WebSocket
    set-volume <0-100>  - Set volume
    show-config         - Show current configuration
    reset               - Factory reset
    test-all            - Run all tests
"""

import serial
import serial.tools.list_ports
import struct
import argparse
import time
import sys

# Protocol constants
SOF = 0xAA
EOF = 0x55

# Command codes
CMD_CONNECT_TO    = 0x01
CMD_CONNECT       = 0x02
CMD_DISCONNECT    = 0x03
CMD_SET_SOUND     = 0x04
CMD_SET_VOLUME    = 0x05
CMD_STATUS        = 0x06
CMD_SHOW_CONFIG   = 0x07
CMD_RESET         = 0x08

# Response codes
CMD_ACK           = 0x81
CMD_NACK          = 0x82
CMD_STATUS_RESP   = 0x83

# Device states
STATES = {
    0: "IDLE",
    1: "CONNECTING",
    2: "CONNECTED",
    3: "DISCONNECTING",
    4: "ERROR",
}


def crc16(data: bytes) -> int:
    """Calculate CRC16-CCITT"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            crc &= 0xFFFF
    return crc


def build_frame(cmd: int, data: bytes = b'', seq: int = 0) -> bytes:
    """Build a command frame"""
    length = len(data)
    
    # Build frame without CRC and EOF
    frame = struct.pack('<BHBB', SOF, length, seq, cmd)
    frame += data
    
    # Calculate CRC (from LEN to DATA)
    crc = crc16(frame[1:])  # Skip SOF
    frame += struct.pack('<H', crc)
    frame += bytes([EOF])
    
    return frame


def find_frame(data: bytes) -> bytes:
    """Find a valid frame in the data (skip log output)"""
    for i in range(len(data) - 8):  # Minimum frame is 8 bytes (LEN=0)
        if data[i] == SOF:
            # Check if this looks like a valid frame
            length = struct.unpack('<H', data[i+1:i+3])[0]
            # Total frame length = 1(SOF) + 2(LEN) + 1(SEQ) + 1(CMD) + LEN(DATA) + 2(CRC) + 1(EOF) = 8 + LEN
            frame_len = 8 + length
            if i + frame_len <= len(data):
                # Check EOF at position 7 + length (0-indexed)
                if data[i + 7 + length] == EOF:
                    return data[i:i + frame_len]
    return None


def parse_frame(data: bytes) -> dict:
    """Parse a response frame"""
    # Find the actual frame in the data
    frame_data = find_frame(data)
    if not frame_data:
        return None
    
    data = frame_data
    
    if len(data) < 8:
        return None
    
    if data[0] != SOF or data[-1] != EOF:
        return None
    
    length = struct.unpack('<H', data[1:3])[0]
    seq = data[3]
    cmd = data[4]
    
    if len(data) < 7 + length:
        return None
    
    payload = data[5:5+length]
    crc_recv = struct.unpack('<H', data[5+length:7+length])[0]
    
    # Verify CRC
    crc_calc = crc16(data[1:5+length])
    if crc_calc != crc_recv:
        return {'error': 'CRC mismatch', 'cmd': cmd}
    
    return {
        'cmd': cmd,
        'seq': seq,
        'length': length,
        'data': payload,
    }


class ESP001Tester:
    def __init__(self, port: str = None, baudrate: int = 115200):
        self.port = port or self.find_port()
        self.baudrate = baudrate
        self.serial = None
        self.seq = 0
    
    @staticmethod
    def find_port():
        """Find ESP32 serial port"""
        ports = serial.tools.list_ports.comports()
        for port in ports:
            # ESP32-S3 usually shows as USB Serial
            if 'USB' in port.description or 'ACM' in port.device:
                return port.device
        return '/dev/ttyACM0'
    
    def connect(self):
        """Connect to serial port"""
        print(f"Connecting to {self.port}...")
        self.serial = serial.Serial(self.port, self.baudrate, timeout=2)
        time.sleep(0.5)  # Wait for connection
        self.serial.reset_input_buffer()
        print(f"Connected to {self.port}")
    
    def disconnect(self):
        """Disconnect from serial port"""
        if self.serial:
            self.serial.close()
            self.serial = None
    
    def send_command(self, cmd: int, data: bytes = b'') -> dict:
        """Send command and wait for response"""
        frame = build_frame(cmd, data, self.seq)
        self.seq = (self.seq + 1) & 0xFF
        
        print(f"TX: {frame.hex(' ')}")
        self.serial.write(frame)
        
        # Wait for response
        response = self.serial.read(512)
        if response:
            print(f"RX: {response.hex(' ')}")
            return parse_frame(response)
        return None
    
    def test_status(self):
        """Test STATUS command"""
        print("\n=== Testing STATUS command ===")
        resp = self.send_command(CMD_STATUS)
        
        if resp and resp['cmd'] == CMD_STATUS_RESP:
            if len(resp['data']) >= 4:
                seq = resp['data'][0]
                state = resp['data'][1]
                configured = resp['data'][2]
                volume = resp['data'][3]
                
                print(f"✓ Status Response:")
                print(f"  Sequence: {seq}")
                print(f"  State: {STATES.get(state, 'UNKNOWN')} ({state})")
                print(f"  Configured: {'Yes' if configured else 'No'}")
                print(f"  Volume: {volume}%")
                return True
        elif resp and resp['cmd'] == CMD_ACK:
            print(f"✓ ACK received")
            return True
        elif resp and resp['cmd'] == CMD_NACK:
            print(f"✗ NACK received")
            return False
        
        print("✗ No valid response")
        return False
    
    def test_connect_to(self, url: str = "ws://192.168.1.100:8080"):
        """Test CONNECT_TO command"""
        print(f"\n=== Testing CONNECT_TO command ===")
        print(f"URL: {url}")
        
        resp = self.send_command(CMD_CONNECT_TO, url.encode())
        
        if resp and resp['cmd'] == CMD_ACK:
            print(f"✓ ACK received - URL set successfully")
            return True
        elif resp and resp['cmd'] == CMD_NACK:
            err = resp['data'][1] if len(resp['data']) > 1 else 0
            print(f"✗ NACK received - Error: {err}")
            return False
        
        print("✗ No valid response")
        return False
    
    def test_connect(self):
        """Test CONNECT command"""
        print("\n=== Testing CONNECT command ===")
        resp = self.send_command(CMD_CONNECT)
        
        if resp and resp['cmd'] == CMD_ACK:
            print(f"✓ ACK received - Connecting...")
            return True
        elif resp and resp['cmd'] == CMD_NACK:
            err = resp['data'][1] if len(resp['data']) > 1 else 0
            print(f"✗ NACK received - Error: {err}")
            return False
        
        print("✗ No valid response")
        return False
    
    def test_disconnect(self):
        """Test DISCONNECT command"""
        print("\n=== Testing DISCONNECT command ===")
        resp = self.send_command(CMD_DISCONNECT)
        
        if resp and resp['cmd'] == CMD_ACK:
            print(f"✓ ACK received - Disconnecting...")
            return True
        
        print("✗ No valid response")
        return False
    
    def test_set_volume(self, volume: int = 80):
        """Test SET_VOLUME command"""
        print(f"\n=== Testing SET_VOLUME command ===")
        print(f"Volume: {volume}%")
        
        resp = self.send_command(CMD_SET_VOLUME, bytes([volume]))
        
        if resp and resp['cmd'] == CMD_ACK:
            print(f"✓ ACK received - Volume set to {volume}%")
            return True
        elif resp and resp['cmd'] == CMD_NACK:
            print(f"✗ NACK received")
            return False
        
        print("✗ No valid response")
        return False
    
    def test_show_config(self):
        """Test SHOW_CONFIG command"""
        print("\n=== Testing SHOW_CONFIG command ===")
        resp = self.send_command(CMD_SHOW_CONFIG)
        
        if resp and resp['cmd'] == CMD_ACK:
            print(f"✓ ACK received - Check device logs for config")
            return True
        
        print("✗ No valid response")
        return False
    
    def test_reset(self):
        """Test RESET command"""
        print("\n=== Testing RESET command ===")
        resp = self.send_command(CMD_RESET)
        
        if resp and resp['cmd'] == CMD_ACK:
            print(f"✓ ACK received - Factory reset")
            return True
        
        print("✗ No valid response")
        return False
    
    def test_all(self):
        """Run all tests"""
        print("=" * 50)
        print("ESP001 USB Command Test Suite")
        print("=" * 50)
        
        results = []
        
        try:
            self.connect()
            
            # Run tests
            results.append(("STATUS", self.test_status()))
            results.append(("SET_VOLUME", self.test_set_volume(80)))
            results.append(("STATUS", self.test_status()))
            results.append(("CONNECT_TO", self.test_connect_to()))
            results.append(("SHOW_CONFIG", self.test_show_config()))
            
            # Don't reset for now
            # results.append(("RESET", self.test_reset()))
            
        finally:
            self.disconnect()
        
        # Print summary
        print("\n" + "=" * 50)
        print("Test Summary")
        print("=" * 50)
        passed = sum(1 for _, r in results if r)
        total = len(results)
        
        for name, result in results:
            status = "✓ PASS" if result else "✗ FAIL"
            print(f"  {name}: {status}")
        
        print(f"\nTotal: {passed}/{total} passed")
        return passed == total


def main():
    parser = argparse.ArgumentParser(description="ESP001 USB Command Test")
    parser.add_argument('command', nargs='?', default='test-all',
                        choices=['status', 'connect-to', 'connect', 'disconnect',
                                'set-volume', 'show-config', 'reset', 'test-all'],
                        help='Command to execute')
    parser.add_argument('args', nargs='*', help='Command arguments')
    parser.add_argument('-p', '--port', help='Serial port')
    
    args = parser.parse_args()
    
    tester = ESP001Tester(port=args.port)
    
    try:
        if args.command == 'test-all':
            success = tester.test_all()
            sys.exit(0 if success else 1)
        
        tester.connect()
        
        if args.command == 'status':
            tester.test_status()
        elif args.command == 'connect-to':
            url = args.args[0] if args.args else "ws://192.168.1.100:8080"
            tester.test_connect_to(url)
        elif args.command == 'connect':
            tester.test_connect()
        elif args.command == 'disconnect':
            tester.test_disconnect()
        elif args.command == 'set-volume':
            vol = int(args.args[0]) if args.args else 80
            tester.test_set_volume(vol)
        elif args.command == 'show-config':
            tester.test_show_config()
        elif args.command == 'reset':
            tester.test_reset()
            
    finally:
        tester.disconnect()


if __name__ == "__main__":
    main()