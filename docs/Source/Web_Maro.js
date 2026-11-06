import { Arena } from "./Arena_Maro.js";
import { PeerSession } from "./Peer_Maro.js";

const width = 1080;
const height = 560;
const levelCap = 6;
const levelInterval = 15;
const teamColors = ["#65e5ca", "#ff867a", "#8daaff", "#ffcb73"];
const preferenceKey = "maro.browser.settings";
const defaultPreferences = {
    controls: "wasd",
    masterVolume: 80,
    effectsVolume: 75,
    screenShake: !window.matchMedia("(prefers-reduced-motion: reduce)").matches,
    showFps: false
};
const bindings = {
    wasd: ["W / A / S / D", "마우스 커서", "좌클릭", "우클릭 유지", "우클릭 + 좌클릭"],
    moba: ["좌클릭", "마우스 커서", "Q", "E 유지", "Shift"]
};
const $ = id => document.getElementById(id);
const settingsDialog = $("settings-dialog");
const canvas = $("arena");
const drawing = canvas.getContext("2d", { alpha: false });
const keys = new Set();
const pointer = { x: width / 2, y: height / 2, inside: false };
const aim = { x: pointer.x, y: pointer.y };
const grid = new Path2D();

for (let x = 54; x < width; x += 54) {
    grid.moveTo(x, 0);
    grid.lineTo(x, height);
}
for (let y = 56; y < height; y += 56) {
    grid.moveTo(0, y);
    grid.lineTo(width, y);
}

const settings = readSettings();
let screen = "setup";
let session = null;
let arena = null;
let frame = null;
let rosterKey = "";
let augmentKey = "";
let chosenLevel = 0;
let choicePending = false;
let previousTick = 0;
let nextPublish = 0;
let nextSend = 0;
let leftMouse = false;
let rightMouse = false;
let attackPressed = false;
let parryPressed = false;
let destination = null;
let fullscreenChanging = false;
let wasFullscreen = Boolean(document.fullscreenElement);
let lastEscape = -Infinity;
let audio = null;
let feedback = null;
let shakeUntil = 0;
let fpsStarted = 0;
let fpsFrames = 0;

function readSettings() {
    let saved;
    try {
        saved = JSON.parse(localStorage.getItem(preferenceKey));
    } catch {
        saved = null;
    }
    return {
        teams: clampInteger(saved?.teams, 2, 4, 2),
        teamSizes: Array.from({ length: 4 }, (_, team) => clampInteger(saved?.teamSizes?.[team], 1, 4, 1)),
        controls: saved?.controls === "moba" ? "moba" : defaultPreferences.controls,
        masterVolume: clampInteger(saved?.masterVolume, 0, 100, defaultPreferences.masterVolume),
        effectsVolume: clampInteger(saved?.effectsVolume, 0, 100, defaultPreferences.effectsVolume),
        screenShake: typeof saved?.screenShake === "boolean" ? saved.screenShake : defaultPreferences.screenShake,
        showFps: saved?.showFps === true
    };
}

function clampInteger(value, min, max, fallback) {
    return Number.isInteger(value) ? Math.min(max, Math.max(min, value)) : fallback;
}

function saveSettings() {
    try {
        localStorage.setItem(preferenceKey, JSON.stringify(settings));
    } catch {}
}

function element(tag, content, className) {
    const node = document.createElement(tag);
    if (content !== undefined) node.textContent = content;
    if (className) node.className = className;
    return node;
}

function setText(node, content) {
    if (node.textContent !== content) node.textContent = content;
}

function setActive(button, active) {
    button.dataset.active = String(active);
    button.setAttribute("aria-pressed", String(active));
}

function option(name, selected, callback) {
    const button = element("button", name);
    button.type = "button";
    setActive(button, selected);
    button.addEventListener("click", callback);
    return button;
}

function options(container, values, describe, selected, choose) {
    container.replaceChildren(...values.map(value => option(describe(value), selected(value), () => {
        choose(value);
        saveSettings();
        renderSettings();
        container.querySelectorAll("button")[values.indexOf(value)]?.focus();
    })));
}

function configSizes(config) {
    return config.teamSizes.slice(0, config.teams);
}

function matchFormat(config) {
    return configSizes(config).join("v");
}

function totalPlayers(config) {
    return configSizes(config).reduce((total, size) => total + size, 0);
}

function renderSettings() {
    options($("presets"), [1, 2, 3, 4], size => `${size}v${size}`,
        size => settings.teams === 2 && settings.teamSizes[0] === size && settings.teamSizes[1] === size,
        size => {
            settings.teams = 2;
            settings.teamSizes[0] = size;
            settings.teamSizes[1] = size;
        });
    options($("team-count"), [2, 3, 4], value => String(value), value => value === settings.teams,
        value => { settings.teams = value; });

    const rows = configSizes(settings).map((size, team) => {
        const row = element("div", undefined, "team-size-row");
        row.style.setProperty("--team", teamColors[team]);
        const choices = element("div", undefined, "options");
        choices.setAttribute("aria-label", `${team + 1}팀 인원`);
        for (let players = 1; players <= 4; players++) {
            choices.append(option(String(players), size === players, () => {
                settings.teamSizes[team] = players;
                saveSettings();
                renderSettings();
                $("team-sizes").children[team].querySelectorAll("button")[players - 1]?.focus();
            }));
        }
        row.append(element("strong", `${team + 1}팀`), choices);
        return row;
    });
    $("team-sizes").replaceChildren(...rows);
    setText($("format"), `${matchFormat(settings)}  /  ${totalPlayers(settings)}명`);
}

function renderBindings() {
    for (const button of $("controls").querySelectorAll("button")) {
        setActive(button, button.dataset.controls === settings.controls);
    }
    const actions = ["이동", "조준", "공격", "방어", "패링"];
    $("bindings").replaceChildren(...actions.map((action, index) => {
        const row = element("div");
        row.append(element("dt", action), element("dd", bindings[settings.controls][index]));
        return row;
    }));
}

function renderPreferences() {
    renderBindings();
    for (const [volume, value] of [["master", settings.masterVolume], ["effects", settings.effectsVolume]]) {
        setText($(`volume-${volume}`), `${value}%`);
        for (const button of settingsDialog.querySelectorAll(`[data-volume="${volume}"]`)) {
            button.disabled = Number(button.dataset.delta) < 0 ? value === 0 : value === 100;
        }
    }
    for (const [id, enabled] of [
        ["screen-shake", settings.screenShake], ["show-fps", settings.showFps],
        ["fullscreen", Boolean(document.fullscreenElement)]
    ]) {
        setActive($(id), enabled);
        setText($(id), enabled ? "켜짐" : "꺼짐");
    }
    $("fullscreen").disabled = !document.fullscreenEnabled;
    $("fps-counter").hidden = !settings.showFps;
    $("game-leave").hidden = screen !== "game";
    $("game-main").hidden = screen !== "game" || !canResume();
}

function applyPreferences() {
    saveSettings();
    renderPreferences();
}

function openSettings() {
    if (settingsDialog.open || screen === "exited") return;
    clearInput();
    settingsStatus("");
    renderPreferences();
    settingsDialog.showModal();
    $("settings-close").focus();
}

function settingsStatus(message) {
    const status = $("settings-status");
    setText(status, message);
    status.hidden = !message;
}

function toggleSettings() {
    if (settingsDialog.open) settingsDialog.close();
    else openSettings();
}

async function toggleFullscreen() {
    fullscreenChanging = true;
    try {
        if (document.fullscreenElement) await document.exitFullscreen();
        else await document.documentElement.requestFullscreen({ navigationUI: "hide" });
        settingsStatus("");
    } catch {
        settingsStatus("이 브라우저에서는 전체 화면을 사용할 수 없습니다.");
    } finally {
        fullscreenChanging = false;
        renderPreferences();
    }
}

function prepareAudio() {
    if (!window.AudioContext) return;
    try {
        audio ??= new AudioContext();
    } catch {
        return;
    }
    if (audio.state === "suspended") audio.resume().catch(() => {});
}

const sounds = {
    shot: ["triangle", 210, 85, 0.07, 0.23],
    hit: ["sine", 110, 46, 0.13, 0.32],
    parry: ["triangle", 790, 1350, 0.13, 0.18],
    blast: ["triangle", 83, 30, 0.19, 0.23]
};

function playSound(name) {
    const volume = settings.masterVolume * settings.effectsVolume / 10000;
    if (audio?.state !== "running" || volume === 0) return;
    const [wave, start, end, duration, strength] = sounds[name];
    const now = audio.currentTime;
    const oscillator = audio.createOscillator();
    const gain = audio.createGain();
    oscillator.type = wave;
    oscillator.frequency.setValueAtTime(start, now);
    oscillator.frequency.exponentialRampToValueAtTime(end, now + duration);
    gain.gain.setValueAtTime(strength * volume, now);
    gain.gain.exponentialRampToValueAtTime(0.0001, now + duration);
    oscillator.connect(gain).connect(audio.destination);
    oscillator.start();
    oscillator.stop(now + duration);
}

function updateFeedback(now) {
    const actor = localActor();
    if (!actor) return;
    const blastTime = frame.effects.reduce((latest, effect) =>
        Math.hypot(effect.x - actor.position.x, effect.y - actor.position.y) < 220
            ? Math.max(latest, frame.elapsed + effect.life) : latest, 0);
    if (feedback) {
        if (actor.hp < feedback.hp) {
            playSound("hit");
            shakeUntil = now + 160;
        }
        if (actor.shots > feedback.shots) playSound("shot");
        if (actor.parryTime > feedback.parry + 0.04) playSound("parry");
        if (blastTime > feedback.blastTime + 0.02) playSound("blast");
    }
    feedback = { hp: actor.hp, shots: actor.shots ?? 0, parry: actor.parryTime, blastTime };
}

function show(next) {
    screen = next;
    for (const id of ["setup", "lobby", "game", "exited"]) $(id).hidden = id !== next;
    if (screen === "setup") renderMain();
    if (settingsDialog.open) renderPreferences();
}

function canResume() {
    return Boolean(session?.started && session.connected && !session.error && (!frame || frame.result === "playing"));
}

function renderMain() {
    const resumable = canResume();
    $("continue").hidden = !resumable;
    $("suspended-leave").hidden = !resumable;
    $("host").disabled = resumable;
    for (const control of $("join-form").elements) control.disabled = resumable;
}

function setupError(message) {
    const error = $("setup-error");
    setText(error, message);
    error.hidden = !message;
}

function openRoom(create) {
    if (canResume()) return;
    if (session) leaveRoom();
    prepareAudio();
    setupError("");
    try {
        session = create();
    } catch {
        setupError("방에 연결할 수 없습니다.");
        return;
    }
    if (!session) {
        setupError("방에 연결할 수 없습니다.");
        return;
    }
    rosterKey = "";
    $("copy-room").textContent = "코드 복사";
    show("lobby");
    renderLobby();
}

function leaveRoom() {
    if (settingsDialog.open) settingsDialog.close();
    session?.close();
    session = null;
    arena = null;
    frame = null;
    clearInput();
    setupError("");
    show("setup");
}

function exitGame() {
    if (session) leaveRoom();
    if (document.fullscreenElement) {
        fullscreenChanging = true;
        document.exitFullscreen().catch(() => {}).finally(() => { fullscreenChanging = false; });
    }
    show("exited");
    $("reopen").focus();
    if (window.opener || history.length === 1) window.close();
}

function returnToMain() {
    if (!canResume()) {
        leaveRoom();
        return;
    }
    settingsDialog.close();
    clearInput();
    pointer.inside = false;
    show("setup");
    $("continue").focus();
}

function resumeGame() {
    if (!canResume()) {
        renderMain();
        return;
    }
    prepareAudio();
    feedback = null;
    shakeUntil = 0;
    fpsStarted = fpsFrames = 0;
    show("game");
    resizeCanvas();
    renderHud();
}

function renderLobby() {
    const { config, roomCode, connected, hosting, occupiedSlots, localSlot, error } = session;
    const occupied = occupiedSlots.filter(Boolean).length;
    const needed = config ? totalPlayers(config) : 0;
    const full = needed > 0 && needed === occupied;
    setText($("room-code"), roomCode || "—");
    $("copy-room").hidden = !roomCode;
    $("start").hidden = !hosting;
    $("start").disabled = Boolean(error) || !connected || !full;
    setText($("room-count"), error ? "연결 오류" : connected ? `참가 인원 ${occupied} / ${needed}` : "연결 중");
    const status = $("room-status");
    status.classList.toggle("error", Boolean(error));
    setText(status, error || (!connected ? "방에 연결하는 중입니다." : hosting
        ? full ? "모든 참가자가 연결되었습니다." : "모든 자리가 차면 경기를 시작할 수 있습니다."
        : "방장의 경기 시작을 기다리는 중입니다."));

    const key = connected && config ? `${matchFormat(config)}:${occupiedSlots.map(Number).join("")}:${localSlot}` : "";
    if (key === rosterKey && (key || !$("lobby-teams").hasChildNodes())) return;
    rosterKey = key;
    let slot = 0;
    const teams = key ? configSizes(config).map((size, team) => {
        const card = element("section", undefined, "surface lobby-team");
        card.style.setProperty("--team", teamColors[team]);
        const list = element("ul");
        for (let seat = 0; seat < size; seat++, slot++) {
            const row = element("li");
            const state = slot === localSlot ? "나" : occupiedSlots[slot] ? "연결됨" : "대기 중";
            row.append(element("span", `${seat + 1}번`), element("span", state, occupiedSlots[slot] ? "accent" : "muted"));
            list.append(row);
        }
        card.append(element("h2", `${team + 1}팀`), list);
        return card;
    }) : [];
    $("lobby-teams").replaceChildren(...teams);
}

async function copyRoomCode() {
    const code = session?.roomCode;
    if (!code) return;
    try {
        await navigator.clipboard.writeText(code);
        if (session?.roomCode === code) setText($("copy-room"), "복사됨");
    } catch {
        const range = document.createRange();
        range.selectNodeContents($("room-code"));
        const selection = window.getSelection();
        selection.removeAllRanges();
        selection.addRange(range);
    }
}

function beginMatch(now) {
    arena = session.hosting ? new Arena(session.config) : null;
    frame = arena?.snapshot() ?? session.frame;
    feedback = null;
    shakeUntil = 0;
    fpsStarted = 0;
    fpsFrames = 0;
    previousTick = now;
    nextPublish = 0;
    nextSend = 0;
    chosenLevel = 0;
    choicePending = false;
    augmentKey = "";
    pointer.inside = false;
    clearInput();
    setText($("match-format"), matchFormat(session.config));
    $("game-teams").replaceChildren(...configSizes(session.config).map((size, team) => {
        const chip = element("span", `${team + 1}팀  ${size}`, "surface team-chip");
        chip.style.setProperty("--team", teamColors[team]);
        return chip;
    }));
    show("game");
    resizeCanvas();
    renderHud();
}

function clearInput() {
    keys.clear();
    leftMouse = rightMouse = attackPressed = parryPressed = false;
    destination = null;
}

function localActor() {
    return frame?.actors[session?.localSlot];
}

function canPlay() {
    return screen === "game" && !settingsDialog.open && $("battle-overlay").hidden && (localActor()?.hp ?? 0) > 0;
}

function inputState() {
    const moba = settings.controls === "moba";
    const actor = localActor();
    if (destination && actor && Math.hypot(actor.position.x - destination.x, actor.position.y - destination.y) < 5) {
        destination = null;
    }
    return {
        movement: {
            x: moba ? 0 : Number(keys.has("KeyD")) - Number(keys.has("KeyA")),
            y: moba ? 0 : Number(keys.has("KeyS")) - Number(keys.has("KeyW"))
        },
        aim: { ...aim },
        attack: moba ? keys.has("KeyQ") : leftMouse && !rightMouse,
        guard: moba ? keys.has("KeyE") : rightMouse,
        parry: parryPressed,
        attackPressed,
        moveTo: destination !== null,
        destination: destination ?? { x: 0, y: 0 }
    };
}

function tickSession() {
    if (!session) return;
    const now = performance.now();
    if (screen === "lobby") {
        if (session.started && session.connected && !session.error) {
            beginMatch(now);
        } else {
            renderLobby();
        }
        return;
    }
    if (screen !== "game" && (screen !== "setup" || !session.started)) return;

    if (arena && !session.error) {
        for (const slot of session.takeDepartures()) arena.removePlayer(slot);
        for (const [slot, index] of session.takeChoices()) arena.chooseAugment(slot, index);
        const inputs = session.takeInputs();
        inputs[session.localSlot] = inputState();
        arena.update(Math.min((now - previousTick) / 1000, 0.05), inputs);
        attackPressed = parryPressed = false;
        frame = arena.snapshot();
        if (now >= nextPublish) {
            session.publish(frame);
            nextPublish = now + 1000 / 30;
        }
    } else if (!arena) {
        frame = session.frame ?? frame;
        if (now >= nextSend || attackPressed || parryPressed) {
            session.sendInput(inputState());
            attackPressed = parryPressed = false;
            nextSend = now + 1000 / 30;
        }
    }
    previousTick = now;
    if (screen === "game") {
        $("game-main").hidden = !canResume();
        updateFeedback(now);
        renderHud();
    } else {
        renderMain();
    }
}

function clockText(elapsed) {
    const seconds = Math.max(0, Math.floor(elapsed));
    return `${String(Math.floor(seconds / 60)).padStart(2, "0")}:${String(seconds % 60).padStart(2, "0")}`;
}

function showMessage(title, body, leave = false) {
    if ($("battle-overlay").hidden) clearInput();
    $("battle-overlay").hidden = false;
    $("augment-dialog").hidden = true;
    $("message-dialog").hidden = false;
    setText($("message-title"), title);
    setText($("message-body"), body);
    $("message-leave").hidden = !leave;
}

function chooseAugment(index) {
    const actor = localActor();
    const choices = frame?.choices[session.localSlot];
    if (!actor || !choices?.[index] || choicePending) return;
    choicePending = true;
    chosenLevel = actor.level;
    if (arena) {
        arena.chooseAugment(session.localSlot, index);
    } else {
        session.sendChoice(index);
    }
    for (const button of $("augment-options").querySelectorAll("button")) button.disabled = true;
}

function showAugments(actor, choices) {
    if ($("battle-overlay").hidden) clearInput();
    $("battle-overlay").hidden = false;
    $("message-dialog").hidden = true;
    $("augment-dialog").hidden = false;
    setText($("augment-level"), `레벨 ${actor.level} / ${levelCap}`);
    const key = `${actor.level}:${choices.map(choice => choice.name).join("|")}`;
    if (augmentKey === key) return;
    augmentKey = key;
    $("augment-options").replaceChildren(...choices.map(({ name, description }, index) => {
        const card = element("article", undefined, "augment");
        const button = element("button", `선택  ${index + 1}`);
        button.type = "button";
        button.disabled = choicePending;
        button.addEventListener("click", () => chooseAugment(index));
        card.append(element("h3", name), element("p", description, "muted"), button);
        return card;
    }));
}

function renderHud() {
    const actor = localActor();
    if (!frame || !actor) {
        showMessage(session.error ? "연결이 종료되었습니다" : "경기 연결 중", session.error || "경기 정보를 불러오는 중입니다.", Boolean(session.error));
        return;
    }
    const slot = session.localSlot;
    const time = clockText(frame.elapsed);
    const remaining = frame.levelTime[slot] ?? 0;
    const counts = teamColors.map((_, team) => frame.actors.filter(fighter => fighter.team === team && fighter.hp > 0).length);
    const choices = frame.choices[slot];
    if (!choices || actor.level !== chosenLevel) choicePending = false;
    setText($("clock"), time);
    setText($("my-team"), `${actor.team + 1}팀`);
    for (let team = 0; team < $("game-teams").children.length; team++) {
        setText($("game-teams").children[team], `${team + 1}팀  ${counts[team]}`);
    }
    $("health").max = actor.maxHp;
    $("health").value = actor.hp;
    setText($("health-text"), `${Math.ceil(actor.hp)} / ${Math.ceil(actor.maxHp)}`);
    setText($("level"), `레벨 ${actor.level} / ${levelCap}`);
    $("level-progress").value = actor.level >= levelCap ? 1 : Math.max(0, 1 - remaining / levelInterval);
    setText($("level-text"), actor.hp <= 0 ? "관전 중" : actor.level >= levelCap ? "최고 레벨" : `처치 또는 ${Math.ceil(remaining)}초 후`);
    setText($("kills"), `처치 ${actor.kills}`);

    if (frame.result !== "playing") {
        const title = frame.winner === actor.team ? "승리" : frame.winner >= 0 ? "패배" : "무승부";
        showMessage(title, `${time}  /  처치 ${actor.kills}  /  레벨 ${actor.level}`, true);
    } else if (session.error || !session.connected) {
        showMessage("연결이 종료되었습니다", session.error || "방과의 연결이 끊겼습니다.", true);
    } else if (choices?.length) {
        showAugments(actor, choices);
    } else if (frame.choices.some(entry => entry?.length)) {
        showMessage("증강 선택 대기 중", "다른 참가자가 증강을 선택하고 있습니다.");
    } else {
        $("battle-overlay").hidden = true;
    }
    canvas.classList.toggle("playing", canPlay());
}

function resizeCanvas() {
    const pixelRatio = Math.min(window.devicePixelRatio || 1, 2);
    const box = canvas.getBoundingClientRect();
    if (!box.width || !box.height) return;
    const nextWidth = Math.round(box.width * pixelRatio);
    const nextHeight = Math.round(box.height * pixelRatio);
    if (canvas.width !== nextWidth || canvas.height !== nextHeight) {
        canvas.width = nextWidth;
        canvas.height = nextHeight;
    }
}

function strokeLine(x, y, toX, toY, color, size) {
    drawing.beginPath();
    drawing.moveTo(x, y);
    drawing.lineTo(toX, toY);
    drawing.strokeStyle = color;
    drawing.lineWidth = size;
    drawing.stroke();
}

function circle(x, y, radius, fill, stroke, weight = 1) {
    drawing.beginPath();
    drawing.arc(x, y, radius, 0, Math.PI * 2);
    if (fill) {
        drawing.fillStyle = fill;
        drawing.fill();
    }
    if (stroke) {
        drawing.lineWidth = weight;
        drawing.strokeStyle = stroke;
        drawing.stroke();
    }
}

function drawFighter(actor, index) {
    if (actor.hp <= 0) return;
    const { x, y } = actor.position;
    const color = teamColors[actor.team];
    const angle = Math.atan2(actor.facing.y, actor.facing.x);
    const radius = actor.radius;
    drawing.fillStyle = "#34445a";
    drawing.fillRect(x - 15, y - radius - 9, 30, 4);
    drawing.fillStyle = color;
    drawing.fillRect(x - 15, y - radius - 9, 30 * actor.hp / actor.maxHp, 4);
    circle(x, y, radius, "#182130", color, 2);
    strokeLine(x + 3 * Math.cos(angle), y + 3 * Math.sin(angle), x + (radius + 5) * Math.cos(angle),
        y + (radius + 5) * Math.sin(angle), color, 6);
    if (actor.guarding || actor.parryTime > 0) {
        drawing.beginPath();
        drawing.arc(x, y, radius + 8, angle - 1.27, angle + 1.27);
        drawing.lineWidth = actor.parryTime > 0 ? 4 : 2;
        drawing.strokeStyle = actor.parryTime > 0 ? "#f2f7fc" : color;
        drawing.stroke();
    }
    if (index === session.localSlot) {
        drawing.font = '13px "Malgun Gothic", system-ui, sans-serif';
        drawing.textAlign = "center";
        drawing.fillStyle = color;
        drawing.fillText("나", x, y + radius + 16);
    }
}

function drawWorld() {
    drawing.setTransform(canvas.width / width, 0, 0, canvas.height / height, 0, 0);
    drawing.fillStyle = "#0e141b";
    drawing.fillRect(0, 0, width, height);
    drawing.strokeStyle = "#1e2b3a";
    drawing.lineWidth = 1;
    drawing.stroke(grid);
    if (!frame) return;
    const shake = settings.screenShake ? Math.max(0, shakeUntil - performance.now()) / 160 : 0;
    drawing.save();
    drawing.translate(Math.sin(shake * 32) * 5 * shake, Math.cos(shake * 25) * 3 * shake);
    for (const cover of frame.obstacles) {
        const top = Math.min(cover.top, cover.bottom);
        const span = Math.abs(cover.top - cover.bottom);
        drawing.fillStyle = "#182130";
        drawing.fillRect(cover.left, top, cover.right - cover.left, span);
        drawing.strokeStyle = "#34445a";
        drawing.strokeRect(cover.left, top, cover.right - cover.left, span);
    }
    if (destination && canPlay()) circle(destination.x, destination.y, 8, null, teamColors[localActor().team]);
    for (const bullet of frame.bullets) {
        const { x, y } = bullet.position;
        const speed = Math.hypot(bullet.velocity.x, bullet.velocity.y) || 1;
        strokeLine(x, y, x - bullet.velocity.x * 14 / speed, y - bullet.velocity.y * 14 / speed,
            teamColors[bullet.team], bullet.radius);
        if (bullet.explosive) circle(x, y, bullet.radius, "#ffcb73");
    }
    for (const effect of frame.effects ?? []) {
        drawing.globalAlpha = Math.min(1, Math.max(0, effect.life * 4));
        circle(effect.x, effect.y, effect.radius, null, teamColors[effect.team], 2.5);
        drawing.globalAlpha = 1;
    }
    frame.actors.forEach(drawFighter);
    drawing.restore();
    if (pointer.inside && canPlay()) {
        strokeLine(pointer.x - 8, pointer.y, pointer.x - 3, pointer.y, "#f2f7fc", 1.4);
        strokeLine(pointer.x + 3, pointer.y, pointer.x + 8, pointer.y, "#f2f7fc", 1.4);
        strokeLine(pointer.x, pointer.y - 8, pointer.x, pointer.y - 3, "#f2f7fc", 1.4);
        strokeLine(pointer.x, pointer.y + 3, pointer.x, pointer.y + 8, "#f2f7fc", 1.4);
    }
}

function render(now) {
    if (screen === "game") {
        drawWorld();
        if (settings.showFps) {
            fpsStarted ||= now;
            fpsFrames++;
            if (now - fpsStarted >= 500) {
                setText($("fps-counter"), `${Math.round(fpsFrames * 1000 / (now - fpsStarted))} FPS`);
                fpsStarted = now;
                fpsFrames = 0;
            }
        } else {
            fpsStarted = 0;
            fpsFrames = 0;
        }
    }
    requestAnimationFrame(render);
}

function movePointer(event) {
    const box = canvas.getBoundingClientRect();
    pointer.x = Math.min(width, Math.max(0, (event.clientX - box.left) * width / box.width));
    pointer.y = Math.min(height, Math.max(0, (event.clientY - box.top) * height / box.height));
    pointer.inside = true;
    aim.x = pointer.x;
    aim.y = pointer.y;
}

$("host").addEventListener("click", () => openRoom(() => PeerSession.host({
    teams: settings.teams,
    teamSizes: configSizes(settings)
})));
$("join-form").addEventListener("submit", event => {
    event.preventDefault();
    const code = $("join-code").value.trim();
    if (code) openRoom(() => PeerSession.join(code));
});
$("copy-room").addEventListener("click", copyRoomCode);
$("start").addEventListener("click", () => session?.start());
for (const id of ["lobby-leave", "game-leave", "message-leave", "suspended-leave"]) {
    $(id).addEventListener("click", leaveRoom);
}
$("continue").addEventListener("click", resumeGame);
$("game-main").addEventListener("click", returnToMain);
$("exit").addEventListener("click", exitGame);
$("reopen").addEventListener("click", () => {
    show("setup");
    $("host").focus();
});
for (const button of document.querySelectorAll(".settings-open")) button.addEventListener("click", openSettings);
$("settings-close").addEventListener("click", () => settingsDialog.close());
settingsDialog.addEventListener("close", clearInput);
$("settings-reset").addEventListener("click", () => {
    Object.assign(settings, defaultPreferences);
    clearInput();
    applyPreferences();
    playSound("parry");
});
for (const button of settingsDialog.querySelectorAll("[data-volume]")) {
    button.addEventListener("click", () => {
        prepareAudio();
        const key = button.dataset.volume === "master" ? "masterVolume" : "effectsVolume";
        settings[key] = clampInteger(settings[key] + Number(button.dataset.delta), 0, 100, 0);
        applyPreferences();
        playSound("parry");
    });
}
$("screen-shake").addEventListener("click", () => {
    settings.screenShake = !settings.screenShake;
    applyPreferences();
});
$("show-fps").addEventListener("click", () => {
    settings.showFps = !settings.showFps;
    applyPreferences();
});
$("fullscreen").addEventListener("click", toggleFullscreen);
document.addEventListener("fullscreenchange", () => {
    const fullscreen = Boolean(document.fullscreenElement);
    if (wasFullscreen && !fullscreen && !fullscreenChanging && performance.now() - lastEscape > 250) {
        openSettings();
    }
    wasFullscreen = fullscreen;
    renderPreferences();
});
for (const button of $("controls").querySelectorAll("button")) {
    button.addEventListener("click", () => {
        settings.controls = button.dataset.controls;
        clearInput();
        applyPreferences();
    });
}

canvas.addEventListener("contextmenu", event => event.preventDefault());
canvas.addEventListener("pointermove", movePointer);
canvas.addEventListener("pointerleave", () => { pointer.inside = false; });
canvas.addEventListener("mousedown", event => {
    if (!canPlay()) return;
    movePointer(event);
    if (settings.controls === "moba") {
        if (event.button === 0) destination = { x: pointer.x, y: pointer.y };
    } else if (event.button === 0) {
        leftMouse = true;
        attackPressed ||= !rightMouse;
        parryPressed ||= rightMouse;
    } else if (event.button === 2) {
        rightMouse = true;
        if (leftMouse) {
            attackPressed = false;
            parryPressed = true;
        }
    }
});
window.addEventListener("mouseup", event => {
    if (event.button === 0) leftMouse = false;
    if (event.button === 2) rightMouse = false;
});
window.addEventListener("keydown", event => {
    if (event.key === "Escape" && screen !== "exited") {
        event.preventDefault();
        if (!event.repeat) {
            lastEscape = performance.now();
            toggleSettings();
        }
        return;
    }
    if (settingsDialog.open) return;
    if (screen !== "game") return;
    const digit = /^(?:Digit|Numpad)([1-3])$/.exec(event.code);
    if (digit && !$("augment-dialog").hidden && !event.repeat) {
        chooseAugment(Number(digit[1]) - 1);
        event.preventDefault();
        return;
    }
    if (!canPlay()) return;
    keys.add(event.code);
    if (!event.repeat && settings.controls === "moba") {
        if (event.code === "ShiftLeft" || event.code === "ShiftRight") parryPressed = true;
        if (event.code === "KeyQ") attackPressed = true;
    }
});
window.addEventListener("keyup", event => keys.delete(event.code));
window.addEventListener("blur", clearInput);
window.addEventListener("pagehide", () => {
    if (session) leaveRoom();
});
new ResizeObserver(resizeCanvas).observe(canvas);
renderSettings();
renderMain();
renderPreferences();
setInterval(tickSession, 1000 / 60);
requestAnimationFrame(render);
