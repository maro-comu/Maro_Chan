import { createServer } from 'node:http';
import { randomInt } from 'node:crypto';
import { pathToFileURL } from 'node:url';
import WebSocket, { WebSocketServer } from 'ws';

const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const maxGuests = 15;
const maxPayload = 65536;
const maxBuffered = 2 * 1024 * 1024;
const maxRooms = 256;
const maxConnections = maxRooms * (maxGuests + 1);

function peerFrame(type, id) {
  const frame = Buffer.allocUnsafe(5);
  frame[0] = type;
  frame.writeUInt32LE(id, 1);
  return frame;
}

export function createRelay({ heartbeatMs = 20000 } = {}) {
  const rooms = new Map();
  const clients = new Set();
  let stopping = false;
  const server = createServer((request, response) => {
    if (request.method === 'GET' && request.url === '/health') {
      response.writeHead(stopping ? 503 : 200, {
        'Content-Type': 'application/json; charset=utf-8',
        'Cache-Control': 'no-store',
      });
      response.end(JSON.stringify({ status: stopping ? 'stopping' : 'ok', protocol: 1 }));
      return;
    }
    response.writeHead(404, { 'Content-Length': 0 });
    response.end();
  });
  server.requestTimeout = 10000;
  server.headersTimeout = 10000;
  server.maxHeadersCount = 32;
  const wss = new WebSocketServer({
    noServer: true,
    maxPayload,
    perMessageDeflate: false,
    clientTracking: false,
  });

  function remove(client) {
    if (client.removed) return;
    client.removed = true;
    clients.delete(client);
    const room = client.room;
    if (!room) return;
    if (client.host) {
      rooms.delete(room.code);
      for (const guest of [...room.guests.values()]) {
        send(guest, peerFrame(4, guest.id));
        disconnect(guest, 1001, '방장이 방을 닫았습니다.');
      }
      room.guests.clear();
    } else if (room.guests.delete(client.id) && !room.host.removed) {
      send(room.host, peerFrame(4, client.id));
    }
  }

  function disconnect(client, code = 1000, reason = '') {
    remove(client);
    if (client.ws.readyState === WebSocket.OPEN) {
      client.ws.close(code, reason);
      const timer = setTimeout(() => client.ws.terminate(), 2000);
      timer.unref();
      client.ws.once('close', () => clearTimeout(timer));
    } else if (client.ws.readyState !== WebSocket.CLOSED) {
      client.ws.terminate();
    }
  }

  function send(client, data) {
    if (client.removed || client.ws.readyState !== WebSocket.OPEN) return false;
    if (client.ws.bufferedAmount + data.length > maxBuffered) {
      disconnect(client, 1013, '연결이 지연되고 있습니다. 다시 참여해 주세요.');
      return false;
    }
    client.ws.send(data, { binary: true, compress: false }, (error) => {
      if (error) {
        remove(client);
        client.ws.terminate();
      }
    });
    return true;
  }

  function fail(client, reason, code = 1008) {
    send(client, Buffer.concat([Buffer.from([5]), Buffer.from(reason)]));
    disconnect(client, code, reason);
  }

  function route(client, data, binary) {
    if (client.removed) return;
    if (!binary || data.length < 5) {
      fail(client, '올바르지 않은 게임 패킷입니다.');
      return;
    }
    const now = performance.now();
    const elapsed = Math.max(0, now - client.budgetAt);
    client.budget = Math.min(client.byteRate * 2, client.budget + elapsed * client.byteRate / 1000);
    client.budgetAt = now;
    client.budget -= data.length;
    if (client.budget < 0) {
      fail(client, '전송량이 너무 많습니다. 잠시 후 다시 참여해 주세요.', 1013);
      return;
    }
    const type = data[0];
    const id = data.readUInt32LE(1);
    if (!client.host && id !== client.id) {
      fail(client, '참가자 정보가 올바르지 않습니다.');
      return;
    }
    const peer = client.host ? client.room.guests.get(id) : client.room.host;
    if (type === 3 && data.length > 5) {
      if (peer) send(peer, data);
    } else if (type === 4 && data.length === 5) {
      const guest = client.host ? peer : client;
      if (guest) {
        send(guest, peerFrame(4, guest.id));
        disconnect(guest);
      }
    } else {
      fail(client, '지원하지 않는 게임 패킷입니다.');
    }
  }

  wss.on('connection', (ws, request, routePath) => {
    const host = routePath === '/host';
    const byteRate = (host ? 16 : 2) * 1024 * 1024;
    const client = {
      ws,
      host,
      room: null,
      id: 0,
      removed: false,
      alive: true,
      byteRate,
      budget: byteRate * 2,
      budgetAt: performance.now(),
    };
    clients.add(client);
    ws.on('pong', () => { client.alive = true; });
    ws.on('error', () => remove(client));
    ws.on('close', () => remove(client));
    ws.on('message', (data, binary) => route(client, data, binary));
    if (host) {
      if (rooms.size >= maxRooms) {
        fail(client, '서버가 혼잡합니다. 잠시 후 다시 만들어 주세요.', 1013);
        return;
      }
      let code;
      do {
        code = Array.from({ length: 8 }, () => alphabet[randomInt(alphabet.length)]).join('');
      } while (rooms.has(code));
      const room = { code, host: client, guests: new Map() };
      client.room = room;
      rooms.set(code, room);
      send(client, Buffer.concat([Buffer.from([1]), Buffer.from(code, 'ascii')]));
      return;
    }
    const code = routePath.slice(6).toUpperCase();
    const room = rooms.get(code);
    if (!room || room.host.removed) {
      fail(client, '방을 찾을 수 없습니다. 방 코드를 확인해 주세요.');
      return;
    }
    if (room.guests.size >= maxGuests) {
      fail(client, '방이 가득 찼습니다.');
      return;
    }
    let id;
    do { id = randomInt(1, 0x100000000); } while (room.guests.has(id));
    client.id = id;
    client.room = room;
    room.guests.set(id, client);
    const joined = peerFrame(2, id);
    if (send(client, joined)) send(room.host, joined);
  });

  server.on('upgrade', (request, socket, head) => {
    socket.on('error', () => {});
    const path = request.url || '';
    const validPath = path === '/host' || /^\/join\/[A-Z2-9]{8}$/i.test(path);
    if (stopping || clients.size >= maxConnections) {
      socket.end('HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n');
      return;
    }
    if (!validPath || request.headers.origin) {
      socket.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n');
      return;
    }
    wss.handleUpgrade(request, socket, head, (ws) => wss.emit('connection', ws, request, path));
  });

  const heartbeat = setInterval(() => {
    for (const client of clients) {
      if (!client.alive) {
        remove(client);
        client.ws.terminate();
        continue;
      }
      client.alive = false;
      client.ws.ping();
    }
  }, heartbeatMs);
  heartbeat.unref();

  return {
    server,
    close() {
      if (stopping) return Promise.resolve();
      stopping = true;
      clearInterval(heartbeat);
      for (const client of [...clients]) {
        remove(client);
        client.ws.terminate();
      }
      wss.close();
      return new Promise((resolve, reject) => {
        server.close((error) => error ? reject(error) : resolve());
        server.closeIdleConnections();
      });
    },
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const relay = createRelay();
  const port = Number(process.env.PORT || 10000);
  relay.server.on('error', (error) => { console.error(error.message); process.exit(1); });
  relay.server.listen(port, '0.0.0.0', () => console.log(`Maro relay listening on ${port}`));
  const stop = () => {
    relay.close().then(() => process.exit(0), (error) => {
      console.error(error.message);
      process.exit(1);
    });
  };
  process.once('SIGTERM', stop);
  process.once('SIGINT', stop);
}
