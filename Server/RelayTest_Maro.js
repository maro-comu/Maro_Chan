import assert from 'node:assert/strict';
import { once } from 'node:events';
import test from 'node:test';
import WebSocket from 'ws';
import { createRelay } from './Relay_Maro.js';

function frame(type, id, content = Buffer.alloc(0)) {
  const result = Buffer.allocUnsafe(5 + content.length);
  result[0] = type;
  result.writeUInt32LE(id, 1);
  content.copy(result, 5);
  return result;
}

async function setup(t, heartbeatMs) {
  const relay = createRelay({ heartbeatMs });
  relay.server.listen(0, '127.0.0.1');
  await once(relay.server, 'listening');
  const url = `ws://127.0.0.1:${relay.server.address().port}`;
  const sockets = [];
  t.after(async () => {
    for (const ws of sockets) ws.terminate();
    await relay.close();
  });
  async function connect(path, options) {
    const ws = new WebSocket(url + path, options);
    sockets.push(ws);
    const messages = [];
    const waiters = [];
    ws.on('message', (data, binary) => {
      assert.equal(binary, true);
      const next = waiters.shift();
      if (next) next(data);
      else messages.push(data);
    });
    await once(ws, 'open');
    return {
      ws,
      next() {
        if (messages.length) return Promise.resolve(messages.shift());
        return new Promise((resolve, reject) => {
          const timer = setTimeout(() => reject(new Error('relay packet timeout')), 3000);
          waiters.push((data) => { clearTimeout(timer); resolve(data); });
        });
      },
    };
  }
  async function room() {
    const host = await connect('/host');
    const created = await host.next();
    assert.equal(created[0], 1);
    const code = created.subarray(1).toString('ascii');
    assert.match(code, /^[A-HJ-NP-Z2-9]{8}$/);
    return { host, code };
  }
  async function guest(host, code) {
    const client = await connect(`/join/${code}`);
    const joined = await client.next();
    assert.equal(joined[0], 2);
    assert.deepEqual(await host.next(), joined);
    return { ...client, id: joined.readUInt32LE(1) };
  }
  return { connect, room, guest, url, sockets };
}

test('방 코드로 연결한 네이티브 클라이언트의 이진 패킷을 양방향으로 보존한다', async (t) => {
  const { room, guest, url } = await setup(t);
  const health = await fetch(url.replace('ws:', 'http:') + '/health');
  assert.equal(health.status, 200);
  assert.deepEqual(await health.json(), { status: 'ok', protocol: 1 });
  const { host, code } = await room();
  const client = await guest(host, code.toLowerCase());
  const payload = Buffer.from([0, 1, 255, 128, 13, 10, 0]);
  const packet = frame(3, client.id, payload);
  host.ws.send(packet);
  assert.deepEqual(await client.next(), packet);
  client.ws.send(packet.subarray(0, 4), { fin: false });
  client.ws.send(packet.subarray(4), { fin: true });
  assert.deepEqual(await host.next(), packet);
  const maxPacket = frame(3, client.id, Buffer.alloc(65531, 123));
  host.ws.send(maxPacket);
  assert.deepEqual(await client.next(), maxPacket);
});

test('서로 다른 방의 패킷과 참가자가 섞이지 않는다', async (t) => {
  const { room, guest } = await setup(t);
  const first = await room();
  const second = await room();
  assert.notEqual(first.code, second.code);
  const firstGuest = await guest(first.host, first.code);
  const secondGuest = await guest(second.host, second.code);
  const firstPacket = frame(3, firstGuest.id, Buffer.from('first'));
  const secondPacket = frame(3, secondGuest.id, Buffer.from('second'));
  firstGuest.ws.send(firstPacket);
  secondGuest.ws.send(secondPacket);
  assert.deepEqual(await first.host.next(), firstPacket);
  assert.deepEqual(await second.host.next(), secondPacket);
});

test('다른 참가자의 ID를 위조하면 한국어 오류를 보내고 연결을 끊는다', async (t) => {
  const { room, guest } = await setup(t);
  const { host, code } = await room();
  const client = await guest(host, code);
  const closed = once(client.ws, 'close');
  client.ws.send(frame(3, (client.id ^ 1) >>> 0, Buffer.from('forged')));
  const error = await client.next();
  assert.equal(error[0], 5);
  assert.match(error.subarray(1).toString(), /참가자/);
  assert.equal((await closed)[0], 1008);
  assert.deepEqual(await host.next(), frame(4, client.id));
});

test('최대 16명까지 허용하고 나간 자리를 다시 사용할 수 있다', async (t) => {
  const { room, guest, connect } = await setup(t);
  const { host, code } = await room();
  const guests = [];
  for (let i = 0; i < 15; i++) guests.push(await guest(host, code));
  assert.equal(new Set(guests.map((client) => client.id)).size, 15);
  const overflow = await connect(`/join/${code}`);
  const full = await overflow.next();
  assert.equal(full[0], 5);
  assert.match(full.subarray(1).toString(), /가득/);
  const kicked = guests[0];
  host.ws.send(frame(4, kicked.id));
  assert.deepEqual(await kicked.next(), frame(4, kicked.id));
  assert.deepEqual(await host.next(), frame(4, kicked.id));
  await guest(host, code);
});

test('방장이 종료하면 모든 참가자를 종료하고 방 코드를 폐기한다', async (t) => {
  const { room, guest, connect } = await setup(t);
  const { host, code } = await room();
  const client = await guest(host, code);
  const clientClosed = once(client.ws, 'close');
  host.ws.close();
  assert.deepEqual(await client.next(), frame(4, client.id));
  await clientClosed;
  const orphan = await connect(`/join/${code}`);
  assert.equal((await orphan.next())[0], 5);
});

test('명시적으로 방을 나가거나 닫으면 상대에게 바로 알린다', async (t) => {
  const { room, guest, connect } = await setup(t);
  const { host, code } = await room();
  const leaving = await guest(host, code);
  leaving.ws.send(frame(4, leaving.id));
  assert.deepEqual(await host.next(), frame(4, leaving.id));
  const remaining = await guest(host, code);
  host.ws.send(frame(4, 0));
  assert.deepEqual(await remaining.next(), frame(4, remaining.id));
  const closed = await connect(`/join/${code}`);
  assert.equal((await closed.next())[0], 5);
});

test('존재하지 않는 방과 잘못된 패킷에 오류를 반환한다', async (t) => {
  const { room, connect } = await setup(t);
  const missing = await connect('/join/AAAAAAAA');
  assert.equal((await missing.next())[0], 5);
  const { host } = await room();
  host.ws.send('text packet');
  assert.equal((await host.next())[0], 5);
});

test('64 KiB 초과 패킷을 차단하고 방장에게 퇴장을 알린다', async (t) => {
  const { room, guest } = await setup(t);
  const { host, code } = await room();
  const client = await guest(host, code);
  const closed = once(client.ws, 'close');
  client.ws.send(frame(3, client.id, Buffer.alloc(65532)));
  assert.equal((await closed)[0], 1009);
  assert.deepEqual(await host.next(), frame(4, client.id));
});

test('브라우저 Origin이 포함된 연결을 차단한다', async (t) => {
  const { url, sockets } = await setup(t);
  const ws = new WebSocket(url + '/host', { origin: 'https://example.com' });
  sockets.push(ws);
  const [error] = await once(ws, 'error');
  assert.match(error.message, /403/);
});

test('응답하지 않는 호스트를 감지하여 방을 정리한다', async (t) => {
  const { connect } = await setup(t, 25);
  const host = await connect('/host', { autoPong: false });
  assert.equal((await host.next())[0], 1);
  const closed = once(host.ws, 'close');
  const timeout = new Promise((_, reject) => {
    const timer = setTimeout(() => reject(new Error('heartbeat timeout')), 3000);
    timer.unref();
  });
  await Promise.race([closed, timeout]);
});
