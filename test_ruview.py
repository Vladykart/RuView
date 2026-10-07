#!/usr/bin/env python3
"""
Quick RuView test script
Demonstrates WiFi-based pose estimation and vital signs monitoring
"""

import requests
import json
import time
import websocket
import threading

# Base URL for RuView API
BASE_URL = "http://localhost:3000"
WS_URL = "ws://localhost:3001/ws/sensing"

def test_health():
    """Test if RuView is running"""
    try:
        response = requests.get(f"{BASE_URL}/health")
        print(f"✅ Health Check: {response.json()}")
        return True
    except Exception as e:
        print(f"❌ Health Check Failed: {e}")
        return False

def test_info():
    """Get system information"""
    try:
        response = requests.get(f"{BASE_URL}/api/v1/info")
        info = response.json()
        print(f"✅ System Info:")
        print(f"   Version: {info.get('version')}")
        print(f"   Backend: {info.get('backend')}")
        print(f"   Features: {', '.join([k for k, v in info.get('features', {}).items() if v])}")
        print(f"   Source: {info.get('source')}")
        return True
    except Exception as e:
        print(f"❌ Info Request Failed: {e}")
        return False

def test_pose_estimation():
    """Test pose estimation endpoint"""
    try:
        response = requests.get(f"{BASE_URL}/api/v1/pose/current")
        pose_data = response.json()
        persons = pose_data.get('persons', [])
        print(f"✅ Pose Estimation:")
        print(f"   Detected persons: {len(persons)}")
        for i, person in enumerate(persons):
            print(f"   Person {i+1}: confidence {person.get('confidence', 0):.2f}")
            keypoints = person.get('keypoints', [])
            print(f"              keypoints: {len(keypoints)}")
        return True
    except Exception as e:
        print(f"❌ Pose Estimation Failed: {e}")
        return False

def test_vital_signs():
    """Test vital signs monitoring"""
    try:
        response = requests.get(f"{BASE_URL}/api/v1/vital-signs")
        vitals = response.json()
        vital_signs = vitals.get('vital_signs', {})
        print(f"✅ Vital Signs:")
        print(f"   Breathing: {vital_signs.get('breathing_rate_bpm', 0):.1f} BPM (confidence: {vital_signs.get('breathing_confidence', 0):.2f})")
        print(f"   Heart Rate: {vital_signs.get('heart_rate_bpm', 0):.1f} BPM (confidence: {vital_signs.get('heartbeat_confidence', 0):.2f})")
        print(f"   Signal Quality: {vital_signs.get('signal_quality', 0):.2f}")
        return True
    except Exception as e:
        print(f"❌ Vital Signs Failed: {e}")
        return False

def test_websocket():
    """Test WebSocket real-time streaming"""
    messages_received = 0
    
    def on_message(ws, message):
        nonlocal messages_received
        try:
            data = json.loads(message)
            messages_received += 1
            if messages_received <= 3:  # Show first 3 messages
                persons = len(data.get('persons', []))
                print(f"   📡 WebSocket Message {messages_received}: {persons} person(s) detected")
        except json.JSONDecodeError:
            pass
    
    def on_error(ws, error):
        print(f"   ❌ WebSocket Error: {error}")
    
    def on_open(ws):
        print(f"✅ WebSocket Connected")
    
    def on_close(ws, close_status_code, close_msg):
        print(f"   📡 WebSocket Closed")
    
    try:
        ws = websocket.WebSocketApp(WS_URL,
                                    on_message=on_message,
                                    on_error=on_error,
                                    on_open=on_open,
                                    on_close=on_close)
        
        # Run WebSocket in background for 5 seconds
        ws_thread = threading.Thread(target=ws.run_forever)
        ws_thread.daemon = True
        ws_thread.start()
        
        time.sleep(5)  # Listen for 5 seconds
        ws.close()
        
        print(f"   📊 Received {messages_received} real-time messages")
        return True
    except Exception as e:
        print(f"❌ WebSocket Test Failed: {e}")
        return False

def main():
    print("🔬 RuView WiFi DensePose Test Suite")
    print("=" * 50)
    
    # Run all tests
    tests = [
        ("Health Check", test_health),
        ("System Info", test_info),
        ("Pose Estimation", test_pose_estimation),
        ("Vital Signs", test_vital_signs),
        ("WebSocket Streaming", test_websocket),
    ]
    
    passed = 0
    for name, test_func in tests:
        print(f"\n🧪 Testing {name}...")
        if test_func():
            passed += 1
        time.sleep(1)
    
    print(f"\n📋 Results: {passed}/{len(tests)} tests passed")
    
    if passed == len(tests):
        print("\n🎉 RuView is fully operational!")
        print(f"🌐 Web Interface: http://localhost:3000")
        print(f"📡 REST API: http://localhost:3000/api/v1/")
        print(f"⚡ WebSocket: ws://localhost:3001/ws/sensing")
        print("\n💡 Note: Currently running in simulation mode.")
        print("   For real WiFi sensing, connect ESP32-S3 hardware or use CSI-capable WiFi.")
    else:
        print("\n⚠️  Some tests failed. Check RuView container logs:")
        print("   docker logs ruview")

if __name__ == "__main__":
    main()