#!/usr/bin/env node

const http = require('http');
const url = require('url');

class MockWiFiDensePoseAPI {
  constructor() {
    this.setupEndpoints();
  }

  setupEndpoints() {
    this.endpoints = new Map();
    
    // Health endpoints
    this.endpoints.set('GET /health/health', () => ({
      status: 'healthy',
      timestamp: new Date().toISOString(),
      components: {
        pose: { status: 'healthy', message: 'Pose detection service running' },
        hardware: { status: 'healthy', message: 'Hardware connected' },
        stream: { status: 'healthy', message: 'Streaming service active' }
      },
      system_metrics: {
        cpu: { percent: Math.random() * 30 + 10 },
        memory: { percent: Math.random() * 40 + 20 },
        disk: { percent: Math.random() * 20 + 5 }
      }
    }));

    this.endpoints.set('GET /api/v1/info', () => ({
      name: 'WiFi-DensePose API',
      version: '1.0.0',
      environment: 'development',
      zones: ['zone1', 'zone2', 'living-room'],
      routers: ['router-001', 'router-002'],
      features: {
        pose_estimation: true,
        streaming: true,
        multi_zone: true,
        real_time: true
      }
    }));

    this.endpoints.set('GET /api/v1/status', () => ({
      services: {
        api: 'running',
        hardware: 'connected',
        inference: 'ready',
        streaming: Math.random() > 0.5 ? 'active' : 'idle'
      }
    }));

    this.endpoints.set('GET /api/v1/pose/current', () => {
      const personCount = Math.floor(Math.random() * 3);
      return {
        timestamp: new Date().toISOString(),
        persons: this.generateMockPersons(personCount),
        processing_time: Math.random() * 20 + 5,
        zone_id: 'living-room',
        total_detections: Math.floor(Math.random() * 10000)
      };
    });

    this.endpoints.set('GET /api/v1/pose/zones/summary', () => ({
      zones: {
        'zone_1': Math.floor(Math.random() * 2),
        'zone_2': Math.floor(Math.random() * 2),
        'zone_3': Math.floor(Math.random() * 2),
        'zone_4': Math.floor(Math.random() * 2)
      }
    }));

    this.endpoints.set('GET /api/v1/pose/stats', () => ({
      total_detections: Math.floor(Math.random() * 10000),
      average_confidence: Math.random() * 0.4 + 0.6,
      peak_persons: Math.floor(Math.random() * 5) + 1,
      hours_analyzed: 24
    }));
  }

  generateMockPersons(count) {
    const persons = [];
    for (let i = 0; i < count; i++) {
      persons.push({
        person_id: `person_${i}`,
        confidence: Math.random() * 0.3 + 0.7,
        bbox: {
          x: Math.random() * 400,
          y: Math.random() * 300,
          width: Math.random() * 100 + 50,
          height: Math.random() * 150 + 100
        },
        keypoints: this.generateMockKeypoints(),
        zone_id: `zone${Math.floor(Math.random() * 3) + 1}`
      });
    }
    return persons;
  }

  generateMockKeypoints() {
    const keypoints = [];
    const centerX = Math.random() * 600 + 100;
    const centerY = Math.random() * 400 + 100;
    
    const offsets = [
      [0, -80],     // nose
      [-10, -90],   // left_eye
      [10, -90],    // right_eye
      [-20, -85],   // left_ear
      [20, -85],    // right_ear
      [-40, -40],   // left_shoulder
      [40, -40],    // right_shoulder
      [-60, 10],    // left_elbow
      [60, 10],     // right_elbow
      [-65, 60],    // left_wrist
      [65, 60],     // right_wrist
      [-20, 60],    // left_hip
      [20, 60],     // right_hip
      [-25, 120],   // left_knee
      [25, 120],    // right_knee
      [-25, 180],   // left_ankle
      [25, 180]     // right_ankle
    ];
    
    for (let i = 0; i < 17; i++) {
      keypoints.push({
        x: centerX + offsets[i][0] + (Math.random() - 0.5) * 10,
        y: centerY + offsets[i][1] + (Math.random() - 0.5) * 10,
        confidence: Math.random() * 0.3 + 0.7
      });
    }
    
    return keypoints;
  }

  handleRequest(req, res) {
    const parsedUrl = url.parse(req.url, true);
    const method = req.method;
    const pathname = parsedUrl.pathname;
    const key = `${method} ${pathname}`;

    // Enable CORS
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');

    if (method === 'OPTIONS') {
      res.writeHead(200);
      res.end();
      return;
    }

    if (this.endpoints.has(key)) {
      try {
        const data = this.endpoints.get(key)();
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(data, null, 2));
      } catch (error) {
        res.writeHead(500, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'Internal server error' }));
      }
    } else {
      res.writeHead(404, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'Not found' }));
    }
  }
}

const api = new MockWiFiDensePoseAPI();
const server = http.createServer((req, res) => api.handleRequest(req, res));

const PORT = 8000;
server.listen(PORT, () => {
  console.log(`🚀 WiFi DensePose Mock API Server running on http://localhost:${PORT}`);
  console.log(`📋 Available endpoints:`);
  console.log(`   - Health: http://localhost:${PORT}/health/health`);
  console.log(`   - API Info: http://localhost:${PORT}/api/v1/info`);
  console.log(`   - Status: http://localhost:${PORT}/api/v1/status`);
  console.log(`   - Current Pose: http://localhost:${PORT}/api/v1/pose/current`);
  console.log(`   - Zone Summary: http://localhost:${PORT}/api/v1/pose/zones/summary`);
  console.log(`   - Stats: http://localhost:${PORT}/api/v1/pose/stats`);
  console.log(``);
  console.log(`💡 This mock server provides realistic WiFi DensePose data for testing.`);
  console.log(`   Press Ctrl+C to stop`);
});

server.on('error', (err) => {
  if (err.code === 'EADDRINUSE') {
    console.log(`❌ Port ${PORT} is already in use. Please stop the existing server or use a different port.`);
  } else {
    console.log(`❌ Server error:`, err);
  }
});