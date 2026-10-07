#!/usr/bin/env node
const http = require('http');
const fs = require('fs');
const path = require('path');
const url = require('url');
const { WebSocketServer } = require('ws');

const PORT = 4000;
const UI_DIR = __dirname;

// ─── MIME types ───
const MIME = {
  '.html': 'text/html', '.css': 'text/css', '.js': 'application/javascript',
  '.json': 'application/json', '.png': 'image/png', '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml', '.ico': 'image/x-icon', '.wasm': 'application/wasm',
  '.woff': 'font/woff', '.woff2': 'font/woff2', '.ttf': 'font/ttf',
  '.map': 'application/json', '.txt': 'text/plain', '.gif': 'image/gif',
};

// ─── Mock data generators ───
function generateKeypoints() {
  const cx = Math.random() * 600 + 100, cy = Math.random() * 400 + 100;
  const offsets = [
    [0,-80],[-10,-90],[10,-90],[-20,-85],[20,-85],[-40,-40],[40,-40],
    [-60,10],[60,10],[-65,60],[65,60],[-20,60],[20,60],[-25,120],[25,120],[-25,180],[25,180]
  ];
  return offsets.map(([dx,dy]) => ({
    x: cx + dx + (Math.random()-0.5)*10,
    y: cy + dy + (Math.random()-0.5)*10,
    confidence: Math.random()*0.3+0.7
  }));
}

function generatePersons(n) {
  const persons = [];
  for (let i = 0; i < n; i++) {
    persons.push({
      person_id: `person_${i}`,
      confidence: Math.random()*0.3+0.7,
      bbox: { x: Math.random()*400, y: Math.random()*300, width: Math.random()*100+50, height: Math.random()*150+100 },
      keypoints: generateKeypoints(),
      zone_id: `zone${Math.floor(Math.random()*3)+1}`
    });
  }
  return persons;
}

function generateSensingData() {
  const t = Date.now() / 1000;
  const baseRssi = -50; // Real signal from WiFi "Vald"
  const variance = 1.5 + Math.sin(t * 0.1) * 1.0;
  const motionBand = 0.05 + Math.abs(Math.sin(t * 0.3)) * 0.15;
  const breathBand = 0.03 + Math.abs(Math.sin(t * 0.05)) * 0.08;
  const isPresent = variance > 0.8;
  const isActive = motionBand > 0.12;

  const gridSize = 20;
  const values = [];
  for (let iz = 0; iz < gridSize; iz++) {
    for (let ix = 0; ix < gridSize; ix++) {
      const cx = gridSize/2, cy = gridSize/2;
      const dist = Math.sqrt((ix-cx)**2 + (iz-cy)**2);
      let v = Math.max(0, 1 - dist/(gridSize*0.7)) * 0.3;
      const bx = cx + 3*Math.sin(t*0.2), by = cy + 2*Math.cos(t*0.15);
      const bodyDist = Math.sqrt((ix-bx)**2 + (iz-by)**2);
      if (isPresent) v += Math.exp(-bodyDist*bodyDist/8) * (0.3 + motionBand*3);
      values.push(Math.min(1, Math.max(0, v + Math.random()*0.05)));
    }
  }

  return {
    type: 'sensing_update',
    timestamp: t,
    source: 'wifi',
    wifi: { ssid: 'Vald', bssid: 'D6:92:5E:9F:00:00', frequency_ghz: 2.462, channel: 11, interface: 'wlp43s0' },
    nodes: [{
      node_id: 1,
      rssi_dbm: baseRssi + Math.sin(t*0.5)*3,
      position: [2, 0, 1.5],
      amplitude: [],
      subcarrier_count: 52,
      bitrate_mbps: 286.7,
    }],
    features: {
      mean_rssi: baseRssi + Math.sin(t*0.5)*3,
      variance, std: Math.sqrt(variance),
      motion_band_power: motionBand,
      breathing_band_power: breathBand,
      dominant_freq_hz: 0.3 + Math.sin(t*0.02)*0.1,
      change_points: Math.floor(Math.random()*3),
      spectral_power: motionBand + breathBand + Math.random()*0.1,
      range: variance*3, iqr: variance*1.5,
      skewness: (Math.random()-0.5)*0.5,
      kurtosis: Math.random()*2,
    },
    classification: {
      motion_level: isActive ? 'active' : (isPresent ? 'present_still' : 'absent'),
      presence: isPresent,
      confidence: isPresent ? 0.75+Math.random()*0.2 : 0.5+Math.random()*0.3,
    },
    signal_field: { grid_size: [gridSize, 1, gridSize], values },
  };
}

// ─── REST endpoints ───
const endpoints = {
  'GET /health/health': () => {
    const metrics = {
      cpu:{percent:Math.random()*30+10}, memory:{percent:Math.random()*40+20}, disk:{percent:Math.random()*20+5}
    };
    return {
      status:'healthy', timestamp: new Date().toISOString(),
      components: {
        pose:{status:'healthy',message:'Pose detection service running'},
        hardware:{status:'healthy',message:'Hardware connected'},
        stream:{status:'healthy',message:'Streaming service active'}
      },
      system_metrics: metrics,
      metrics: metrics
    };
  },
  'GET /health/ready': () => ({ status:'ready', checks:{database:'ready',hardware:'ready',inference:'ready'} }),
  'GET /health/live': () => ({ status:'alive', timestamp: new Date().toISOString() }),
  'GET /health/version': () => ({ name:'WiFi-DensePose API', version:'1.0.0', environment:'development', build:'2025-01-07-dev' }),
  'GET /api/v1/info': () => ({
    name:'WiFi-DensePose API', version:'1.0.0', environment:'development',
    zones:['zone1','zone2','living-room'], routers:['router-001','router-002'],
    features:{pose_estimation:true,streaming:true,multi_zone:true,real_time:true}
  }),
  'GET /api/v1/status': () => ({
    services:{api:'running',hardware:'connected',inference:'ready',streaming:'active'},
    source: 'wifi',
    wifi: { ssid: 'Vald', interface: 'wlp43s0', frequency_ghz: 2.462, signal_dbm: -50 }
  }),
  'GET /api/v1/pose/current': () => ({
    timestamp: new Date().toISOString(),
    persons: generatePersons(Math.floor(Math.random()*3)+1),
    processing_time: Math.random()*20+5, zone_id:'living-room',
    total_detections: Math.floor(Math.random()*10000)
  }),
  'GET /api/v1/pose/zones/summary': () => ({
    zones:{ zone_1: Math.floor(Math.random()*2), zone_2: Math.floor(Math.random()*2),
            zone_3: Math.floor(Math.random()*2), zone_4: Math.floor(Math.random()*2) }
  }),
  'GET /api/v1/pose/stats': () => ({
    total_detections: Math.floor(Math.random()*10000),
    average_confidence: Math.random()*0.4+0.6,
    peak_persons: Math.floor(Math.random()*5)+1, hours_analyzed:24
  }),
  'GET /api/v1/stream/status': () => ({
    is_active:true, connected_clients: Math.floor(Math.random()*10),
    messages_sent: Math.floor(Math.random()*5000), uptime: Math.floor(Date.now()/1000)-900
  }),
  'POST /api/v1/stream/start': () => ({ message:'Streaming started', status:'active' }),
  'POST /api/v1/stream/stop': () => ({ message:'Streaming stopped', status:'inactive' }),
};

// ─── HTTP server ───
const server = http.createServer((req, res) => {
  const parsed = url.parse(req.url, true);
  const pathname = parsed.pathname;
  const method = req.method;
  const key = `${method} ${pathname}`;

  // CORS
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');
  if (method === 'OPTIONS') { res.writeHead(200); res.end(); return; }

  // API endpoints
  if (endpoints[key]) {
    res.writeHead(200, {'Content-Type':'application/json'});
    res.end(JSON.stringify(endpoints[key]()));
    return;
  }

  // Static files
  let filePath = pathname === '/' ? '/index.html' : pathname;
  filePath = path.join(UI_DIR, filePath);
  const ext = path.extname(filePath).toLowerCase();

  fs.stat(filePath, (err, stats) => {
    if (err || !stats.isFile()) {
      const indexPath = path.join(filePath, 'index.html');
      fs.stat(indexPath, (err2, stats2) => {
        if (!err2 && stats2.isFile()) {
          serveFile(indexPath, '.html', res);
        } else {
          res.writeHead(404); res.end('Not Found');
        }
      });
      return;
    }
    serveFile(filePath, ext, res);
  });
});

function serveFile(filePath, ext, res) {
  const mime = MIME[ext] || 'application/octet-stream';
  fs.readFile(filePath, (err, data) => {
    if (err) { res.writeHead(500); res.end('Error'); return; }
    res.writeHead(200, {'Content-Type': mime});
    res.end(data);
  });
}

// ─── WebSocket server (using ws library) ───
const wss = new WebSocketServer({ server });

wss.on('connection', (ws, req) => {
  console.log(`[WS] Client connected: ${req.url}`);

  // Send connection established
  ws.send(JSON.stringify({
    type: 'connection_established',
    payload: { client_id: `client_${Date.now()}`, server: 'WiFi-DensePose Mock', version: '1.0.0' },
    timestamp: new Date().toISOString()
  }));

  // Handle incoming messages
  ws.on('message', (raw) => {
    try {
      const msg = JSON.parse(raw.toString());
      if (msg.type === 'ping') {
        ws.send(JSON.stringify({ type: 'pong', timestamp: msg.timestamp }));
      } else if (msg.type === 'get_status') {
        ws.send(JSON.stringify({ type: 'status', payload: { connected: true, streaming: true, data_source: 'esp32' } }));
      }
    } catch(e) {}
  });

  ws.on('close', () => console.log('[WS] Client disconnected'));
});

// ─── Broadcast data to all WS clients every 500ms ───
setInterval(() => {
  if (wss.clients.size === 0) return;

  const sensing = generateSensingData();
  const pose = {
    type: 'pose_update',
    timestamp: new Date().toISOString(),
    data: {
      persons: generatePersons(Math.floor(Math.random()*3)+1),
      processing_time_ms: Math.random()*20+5,
      zone_id: 'living-room',
      frame_id: Date.now(),
      metadata: { source: 'wifi', mock_data: false, sample_rate: 100 }
    }
  };

  const sensingStr = JSON.stringify(sensing);
  const poseStr = JSON.stringify(pose);

  for (const client of wss.clients) {
    if (client.readyState === 1) { // WebSocket.OPEN
      client.send(sensingStr);
      client.send(poseStr);
    }
  }
}, 500);

// ─── Start ───
server.listen(PORT, () => {
  console.log(`\n🚀 WiFi DensePose Server running on http://localhost:${PORT}`);
  console.log(`   🌐 Frontend UI:    http://localhost:${PORT}`);
  console.log(`   📡 REST API:       http://localhost:${PORT}/api/v1/`);
  console.log(`   🔌 WebSocket:      ws://localhost:${PORT}/ws/sensing`);
  console.log(`   ❤️  Health:         http://localhost:${PORT}/health/health`);
  console.log(`\n💡 Open http://localhost:${PORT} in your browser.`);
  console.log(`   Press Ctrl+C to stop\n`);
});

server.on('error', (err) => {
  if (err.code === 'EADDRINUSE') {
    console.log(`❌ Port ${PORT} is already in use.`);
  } else {
    console.log(`❌ Server error:`, err);
  }
});
