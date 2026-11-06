const trysteroModule = "https://esm.sh/trystero@0.26.0/es2022/trystero.bundle.mjs";
const roomAlphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
const appId = "maro-arena-multiplayer-v1";
const version = 1;
const joinTimeout = 22000;

function newRoomCode() {
    return Array.from(crypto.getRandomValues(new Uint8Array(8)), byte => roomAlphabet[byte & 31]).join("");
}

function checkedConfig(config) {
    const teams = config?.teams;
    if (!Number.isInteger(teams) || teams < 2 || teams > 4 || !Array.isArray(config.teamSizes)
        || config.teamSizes.length < teams) {
        return null;
    }
    const teamSizes = config.teamSizes.slice(0, teams);
    if (!teamSizes.every(size => Number.isInteger(size) && size >= 1 && size <= 4)) {
        return null;
    }
    return { teams, teamSizes };
}

function playerCount(config) {
    return config.teamSizes.reduce((total, size) => total + size, 0);
}

function emptyInput() {
    return {
        movement: { x: 0, y: 0 },
        aim: { x: 0, y: 0 },
        destination: { x: 0, y: 0 },
        attack: false,
        guard: false,
        parry: false,
        moveTo: false,
        attackPressed: false
    };
}

function numberIn(value, min, max) {
    return Number.isFinite(value) && value >= min && value <= max;
}

function pointIn(point, limit) {
    return point && numberIn(point.x, -limit, limit) && numberIn(point.y, -limit, limit);
}

function checkedInput(input) {
    if (!input || !pointIn(input.movement, 1.01) || !pointIn(input.aim, 4096)
        || !pointIn(input.destination, 4096)) {
        return null;
    }
    return {
        movement: { x: input.movement.x, y: input.movement.y },
        aim: { x: input.aim.x, y: input.aim.y },
        destination: { x: input.destination.x, y: input.destination.y },
        attack: input.attack === true,
        guard: input.guard === true,
        parry: input.parry === true,
        moveTo: input.moveTo === true,
        attackPressed: input.attackPressed === true
    };
}

function withPendingEdges(input, previous) {
    if (previous) {
        input.parry ||= previous.parry;
        input.attackPressed ||= previous.attackPressed;
    }
    return input;
}

function validFrame(frame, config) {
    const count = playerCount(config);
    if (!frame || !Number.isFinite(frame.elapsed) || frame.elapsed < 0
        || !["playing", "victory", "draw"].includes(frame.result)
        || (frame.result === "victory"
            ? !Number.isInteger(frame.winner) || frame.winner < 0 || frame.winner >= config.teams
            : frame.winner !== -1)
        || !Array.isArray(frame.actors) || frame.actors.length !== count
        || !Array.isArray(frame.choices) || frame.choices.length !== count
        || !Array.isArray(frame.levelTime) || frame.levelTime.length !== count
        || !Array.isArray(frame.bullets) || frame.bullets.length > 1024
        || !Array.isArray(frame.effects) || frame.effects.length > 512
        || !Array.isArray(frame.obstacles) || frame.obstacles.length > 16) {
        return false;
    }
    let slot = 0;
    for (let team = 0; team < config.teams; team++) {
        for (let member = 0; member < config.teamSizes[team]; member++, slot++) {
            const actor = frame.actors[slot];
            const options = frame.choices[slot];
            if (!actor || actor.team !== team || !Number.isInteger(actor.level) || actor.level < 1 || actor.level > 6
                || !Number.isInteger(actor.kills) || actor.kills < 0 || actor.kills >= count
                || !pointIn(actor.position, 4096) || !pointIn(actor.facing, 1.01)
                || !numberIn(actor.maxHp, 1, 4096) || !numberIn(actor.hp, 0, actor.maxHp)
                || !numberIn(actor.radius, 1, 128) || !numberIn(actor.parryTime, 0, 15)
                || !numberIn(frame.levelTime[slot], 0, 60)
                || (options !== null && (!Array.isArray(options) || options.length !== 3 || !options.every(option =>
                    typeof option?.name === "string" && option.name.length <= 80
                    && typeof option.description === "string" && option.description.length <= 250)))) {
                return false;
            }
        }
    }
    return frame.bullets.every(bullet => bullet && pointIn(bullet.position, 4096)
        && pointIn(bullet.velocity, 4096) && numberIn(bullet.radius, 0, 128)
        && Number.isInteger(bullet.team) && numberIn(bullet.team, 0, config.teams - 1))
        && frame.effects.every(effect => pointIn(effect, 4096) && numberIn(effect.radius, 0, 4096)
            && numberIn(effect.life, 0, 60) && Number.isInteger(effect.team)
            && numberIn(effect.team, 0, config.teams - 1))
        && frame.obstacles.every(wall => wall && numberIn(wall.left, 0, 4096)
            && numberIn(wall.right, wall.left, 4096) && numberIn(wall.top, 0, 4096)
            && numberIn(wall.bottom, wall.top, 4096));
}

function newLink(id) {
    return { id, slot: -1, control: [], live: null, sending: false, rejected: false, timer: null };
}

export class PeerSession {
    #room;
    #action;
    #upstream;
    #clients = new Map();
    #inputs = [];
    #choiceLevels = [];
    #choices = [];
    #departures = [];
    #timer;
    #relayCheck;
    #closed = false;

    constructor(hosting, code, config) {
        this.hosting = hosting;
        this.roomCode = code;
        this.config = config;
        this.localSlot = hosting ? 0 : -1;
        this.connected = false;
        this.started = false;
        this.error = "";
        this.frame = null;
        this.occupiedSlots = config ? Array.from({ length: playerCount(config) }, (_, slot) => slot === 0) : [];
        this.#inputs = this.occupiedSlots.map(emptyInput);
        this.#choiceLevels = this.occupiedSlots.map(() => 0);
    }

    static host(config) {
        const checked = checkedConfig(config);
        const session = new PeerSession(true, newRoomCode(), checked);
        if (checked) {
            session.#initialize();
        } else {
            session.#fail("팀 구성과 참가 인원을 확인해 주세요.");
        }
        return session;
    }

    static join(code) {
        const normalized = String(code ?? "").toUpperCase().replace(/[\s-]/g, "");
        const session = new PeerSession(false, normalized, null);
        if (normalized.length === 8 && [...normalized].every(character => roomAlphabet.includes(character))) {
            session.#initialize();
        } else {
            session.#fail("방장이 공유한 8자리 방 코드를 입력해 주세요.");
        }
        return session;
    }

    get connectedPlayers() {
        return this.occupiedSlots.filter(Boolean).length;
    }

    async #initialize() {
        this.#timer = setTimeout(() => this.#fail(this.hosting
            ? "연결 서비스에 접속할 수 없습니다. 인터넷 연결을 확인해 주세요."
            : "방에 연결할 시간이 초과됐습니다. 방 코드와 인터넷 연결을 확인해 주세요."), joinTimeout);
        try {
            const { joinRoom, getRelaySockets } = await import(trysteroModule);
            if (this.#closed) {
                return;
            }
            const config = { appId, password: this.roomCode };
            this.#room = joinRoom(config, this.roomCode, {
                onPeerHandshake: async (_, send, receive, isInitiator) => {
                    const ours = { version, hosting: this.hosting };
                    const verify = remote => {
                        if (remote?.version !== version || remote.hosting !== !this.hosting) {
                            throw new Error("Maro handshake");
                        }
                    };
                    if (isInitiator) {
                        await send(ours);
                        verify((await receive()).data);
                    } else {
                        verify((await receive()).data);
                        await send(ours);
                    }
                }
            });
            this.#action = this.#room.makeAction("maro-game");
            this.#action.onMessage = (message, { peerId }) => this.#receive(peerId, message);
            this.#room.onPeerJoin = id => this.#peerJoined(id);
            this.#room.onPeerLeave = id => this.#peerLeft(id);
            if (this.hosting) {
                const ready = () => {
                    if (Object.values(getRelaySockets()).some(socket => socket?.readyState === WebSocket.OPEN)) {
                        this.connected = true;
                        clearTimeout(this.#timer);
                        clearInterval(this.#relayCheck);
                    }
                };
                this.#relayCheck = setInterval(ready, 100);
                ready();
            }
        } catch {
            this.#fail("온라인 연결을 시작할 수 없습니다. 인터넷 연결을 확인해 주세요.");
        }
    }

    #peerJoined(id) {
        if (this.#closed) {
            return;
        }
        if (this.hosting && this.#clients.size < this.occupiedSlots.length + 4) {
            const record = newLink(id);
            this.#clients.set(id, record);
            record.timer = setTimeout(() => this.#drop(record), joinTimeout);
        } else if (!this.hosting && !this.#upstream) {
            this.#upstream = newLink(id);
            this.#send(this.#upstream, { type: "hello", version });
        } else {
            this.#room.getPeers()[id]?.close();
        }
    }

    #peerLeft(id) {
        if (this.hosting) {
            const record = this.#clients.get(id);
            if (record) {
                this.#drop(record, false);
            }
        } else if (this.#upstream?.id === id) {
            this.#fail("호스트와 연결이 끊겼습니다.");
        }
    }

    #receive(id, message) {
        if (this.#closed) {
            return;
        }
        if (this.hosting) {
            const record = this.#clients.get(id);
            if (record && !this.#fromClient(record, message)) {
                this.#drop(record);
            }
        } else if (this.#upstream?.id === id && !this.#fromHost(message)) {
            this.#fail("호스트에서 잘못된 경기 데이터를 받았습니다.");
        }
    }

    #fromClient(record, message) {
        if (!message || typeof message !== "object") {
            return false;
        }
        if (record.rejected) {
            return true;
        }
        if (record.slot < 0) {
            if (message.type !== "hello") {
                return false;
            }
            const slot = this.occupiedSlots.indexOf(false);
            clearTimeout(record.timer);
            if (message.version !== version || this.started || slot < 0) {
                const reason = message.version !== version ? "게임 버전이 다릅니다."
                    : this.started ? "이미 시작된 경기입니다." : "방이 가득 찼습니다.";
                this.#send(record, { type: "reject", reason });
                record.rejected = true;
                record.timer = setTimeout(() => this.#drop(record), 500);
            } else {
                record.slot = slot;
                this.occupiedSlots[slot] = true;
                this.#sendLobby();
            }
            return true;
        }
        if (!this.started) {
            return false;
        }
        const { slot } = record;
        if (message.type === "input") {
            const input = checkedInput(message.input);
            if (!input) {
                return false;
            }
            this.#inputs[slot] = withPendingEdges(input, this.#inputs[slot]);
            return true;
        }
        if (message.type === "choice") {
            const { level, index } = message;
            if (!Number.isInteger(index) || index < 0 || index > 2 || !Number.isInteger(level)
                || level < 1 || level > 6) {
                return false;
            }
            if (this.frame?.actors[slot]?.level === level && this.frame.choices[slot]
                && this.#choiceLevels[slot] !== level) {
                this.#choices.push([slot, index]);
                this.#choiceLevels[slot] = level;
            }
            return true;
        }
        return false;
    }

    #fromHost(message) {
        if (!message || typeof message !== "object") {
            return false;
        }
        if (message.type === "reject" && !this.connected) {
            this.#fail(typeof message.reason === "string" && message.reason.length <= 100
                ? message.reason : "방에 참가할 수 없습니다.");
            return true;
        }
        if (message.type === "lobby" && !this.started) {
            const config = checkedConfig(message.config);
            const { slot, occupied } = message;
            if (!config || !Array.isArray(occupied) || occupied.length !== playerCount(config)
                || !occupied.every(value => typeof value === "boolean") || occupied[0] !== true
                || !Number.isInteger(slot) || slot < 1 || slot >= occupied.length || !occupied[slot]
                || (this.connected && (slot !== this.localSlot || config.teams !== this.config.teams
                    || config.teamSizes.some((size, team) => size !== this.config.teamSizes[team])))) {
                return false;
            }
            this.config = config;
            this.localSlot = slot;
            this.occupiedSlots = occupied;
            this.connected = true;
            clearTimeout(this.#timer);
            return true;
        }
        if (message.type === "start" && this.connected && !this.started) {
            this.started = true;
            return true;
        }
        if (message.type === "frame" && this.started && validFrame(message.frame, this.config)) {
            this.frame = message.frame;
            return true;
        }
        return false;
    }

    #sendLobby() {
        for (const record of this.#clients.values()) {
            if (record.slot >= 0) {
                this.#send(record, { type: "lobby", slot: record.slot, config: this.config,
                    occupied: [...this.occupiedSlots] });
            }
        }
    }

    #send(record, message) {
        record.control.push(message);
        this.#flush(record);
    }

    async #flush(record) {
        if (record.sending) {
            return;
        }
        record.sending = true;
        const linked = () => !this.#closed && (this.hosting ? this.#clients.get(record.id) === record
            : this.#upstream === record);
        try {
            while (linked()) {
                const message = record.control.shift() ?? record.live;
                if (!message) {
                    break;
                }
                if (message === record.live) {
                    record.live = null;
                }
                await this.#action.send(message, { target: record.id });
            }
        } catch {
            if (linked()) {
                this.#peerLeft(record.id);
            }
        } finally {
            record.sending = false;
        }
    }

    #drop(record, closePeer = true) {
        if (!this.#clients.delete(record.id)) {
            return;
        }
        clearTimeout(record.timer);
        if (closePeer) {
            this.#room.getPeers()[record.id]?.close();
        }
        if (record.slot >= 0) {
            this.occupiedSlots[record.slot] = false;
            this.#inputs[record.slot] = emptyInput();
            if (this.started) {
                this.#departures.push(record.slot);
            } else {
                this.#sendLobby();
            }
        }
    }

    #fail(error) {
        if (!this.#closed) {
            this.error = error;
            this.close();
        }
    }

    start() {
        if (!this.hosting || !this.connected || this.started || !this.occupiedSlots.every(Boolean)) {
            return false;
        }
        this.started = true;
        for (const record of this.#clients.values()) {
            if (record.slot >= 0) {
                this.#send(record, { type: "start" });
            }
        }
        return true;
    }

    sendInput(input) {
        if (this.hosting || !this.started || !this.connected) {
            return;
        }
        const checked = checkedInput(input);
        if (checked) {
            const record = this.#upstream;
            record.live = { type: "input", input: withPendingEdges(checked, record.live?.input) };
            this.#flush(record);
        }
    }

    sendChoice(index) {
        if (this.hosting || !this.started || !this.connected || !Number.isInteger(index)
            || index < 0 || index > 2 || !this.frame?.choices[this.localSlot]) {
            return;
        }
        const { level } = this.frame.actors[this.localSlot];
        this.#send(this.#upstream, { type: "choice", index, level });
    }

    takeInputs() {
        return this.#inputs.map(input => {
            const current = { ...input };
            input.parry = false;
            input.attackPressed = false;
            return current;
        });
    }

    takeChoices() {
        const choices = this.#choices;
        this.#choices = [];
        return choices;
    }

    takeDepartures() {
        const departures = this.#departures;
        this.#departures = [];
        return departures;
    }

    publish(frame) {
        if (!this.hosting || !this.connected || !this.started) {
            return;
        }
        this.frame = frame;
        for (const record of this.#clients.values()) {
            if (record.slot >= 0) {
                record.live = { type: "frame", frame };
                this.#flush(record);
            }
        }
    }

    close() {
        if (this.#closed) {
            return;
        }
        this.#closed = true;
        this.connected = false;
        clearTimeout(this.#timer);
        clearInterval(this.#relayCheck);
        for (const record of this.#clients.values()) {
            clearTimeout(record.timer);
        }
        this.#clients.clear();
        this.#room?.leave().catch(() => {});
    }
}
