export const Width = 1080;
export const Height = 560;
export const MaxLevel = 6;
export const LevelInterval = 15;

const Step = 1 / 120;
const BulletLimit = 512;
const obstacles = [
    { left: 348, top: 248, right: 418, bottom: 424 },
    { left: 662, top: 136, right: 732, bottom: 312 }
];
const add = (a, b) => ({ x: a.x + b.x, y: a.y + b.y });
const sub = (a, b) => ({ x: a.x - b.x, y: a.y - b.y });
const scale = (a, n) => ({ x: a.x * n, y: a.y * n });
const dot = (a, b) => a.x * b.x + a.y * b.y;
const length = a => Math.hypot(a.x, a.y);
const zero = () => ({ x: 0, y: 0 });
const clamp = (n, min, max) => Math.max(min, Math.min(max, n));
const unit = a => length(a) > 0.0001 ? scale(a, 1 / length(a)) : zero();
const rotate = (a, angle) => ({
    x: a.x * Math.cos(angle) - a.y * Math.sin(angle),
    y: a.x * Math.sin(angle) + a.y * Math.cos(angle)
});

function patch(player, key, amount) {
    player[key] += amount;
}

const augments = [
    ["체력 증가", "최대 체력과 현재 체력이 25 늘어납니다.", p => { p.maxHp += 25; p.hp += 25; }],
    ["공격 속도 증가", "사격 간격이 14% 줄어듭니다.", p => { p.interval *= 0.86; }],
    ["대미지 증가", "탄 한 발의 피해가 5 증가합니다.", p => patch(p, "damage", 5)],
    ["빠른 발", "이동 속도가 26 증가합니다.", p => patch(p, "speed", 26)],
    ["방어 강화", "막아낸 탄에서 받는 피해가 22% 감소합니다.", p => { p.guardFactor *= 0.78; }],
    ["패링 가속", "패링 재사용 대기시간이 20% 줄어듭니다.", p => { p.parryInterval *= 0.8; }],
    ["고속탄", "탄속이 초당 120만큼 증가합니다.", p => patch(p, "bulletSpeed", 120)],
    ["자가 회복", "4초간 피해를 받지 않으면 초당 체력을 3 회복합니다.", p => patch(p, "regen", 3)],
    ["처치 회복", "적을 처치하면 체력이 20 회복됩니다.", p => patch(p, "killHeal", 20)],
    ["산탄총", "넓게 퍼지는 탄 5발을 쏩니다. 사격 간격이 늘어납니다."],
    ["폭발탄", "탄이 부딪히면 주변 적에게도 피해를 줍니다."],
    ["도탄", "탄이 벽과 엄폐물에 한 번 튕깁니다."],
    ["관통탄", "탄이 첫 적을 관통해 다음 적까지 맞힙니다."],
    ["냉각탄", "맞은 적의 이동 속도를 잠시 낮춥니다."],
    ["흡혈탄", "준 피해의 14%만큼 체력을 회복합니다.", p => patch(p, "leech", 0.14)],
    ["위기 질주", "체력이 35% 미만이면 30% 빨라집니다."],
    ["패링 돌진", "패링을 시도할 때 잠시 이동이 빨라집니다."],
    ["불사조", "한 번 쓰러질 위기에서 체력 26으로 살아납니다."],
    ["과충전", "세 번째 사격마다 피해와 탄속이 증가합니다."],
    ["조준 안정", "멈춰 있을 때 사격 간격이 20% 줄어듭니다."],
    ["복수의 파편", "피해를 받으면 약한 탄이 주변으로 튀어나갑니다."],
    ["연속 패링", "탄을 반사하면 패링을 즉시 다시 쓸 수 있습니다."],
    ["폭발 증폭", "폭발 반경과 범위 피해가 증가합니다. 다른 폭발 효과와 연계됩니다."],
    ["패링 분열", "반사한 탄에서 양옆으로 작은 탄 2발이 갈라집니다."],
    ["방패 반격", "탄을 막으면 다음 사격의 피해가 증가합니다."],
    ["빙결 파쇄", "이미 느려진 적에게 더 큰 피해를 줍니다."],
    ["처치 폭죽", "적을 처치한 자리에 폭발을 일으킵니다."],
    ["마지막 탄창", "체력이 35% 미만이면 사격 간격이 25% 줄어듭니다."],
    ["도탄 과열", "벽에 튕긴 탄의 피해가 45% 증가합니다."],
    ["강철 방패", "방어와 패링이 좌우에서 들어오는 탄까지 막습니다."],
    ["첫발 강화", "1.5초간 사격하지 않으면 다음 사격이 강해집니다."],
    ["반사 회복", "패링에 성공하면 체력이 5 회복됩니다."],
    ["쌍둥이 탄", "원래 탄 옆으로 작은 탄을 추가로 쏩니다."],
    ["행운의 회피", "피해를 입으면 잠시 이동 속도가 증가합니다."],
    ["폭풍 발걸음", "공격하지 않고 이동하는 동안 이동 속도가 16% 증가합니다."]
];

function bounds(point, radius) {
    return { x: clamp(point.x, radius, Width - radius), y: clamp(point.y, radius, Height - radius) };
}

function inside(point, wall, margin) {
    return point.x >= wall.left - margin && point.x <= wall.right + margin
        && point.y >= wall.top - margin && point.y <= wall.bottom + margin;
}

function wallHit(point, move, wall, margin) {
    let entry = 0;
    let exit = 1;
    let axis = "x";
    for (const [key, min, max] of [
        ["x", wall.left - margin, wall.right + margin],
        ["y", wall.top - margin, wall.bottom + margin]
    ]) {
        const velocity = move[key];
        if (Math.abs(velocity) < 0.00001) {
            if (point[key] < min || point[key] > max) return null;
            continue;
        }
        const a = (min - point[key]) / velocity;
        const b = (max - point[key]) / velocity;
        if (Math.min(a, b) >= entry) {
            entry = Math.min(a, b);
            axis = key;
        }
        exit = Math.min(exit, Math.max(a, b));
        if (entry > exit) return null;
    }
    return { time: entry, axis };
}

function clearPath(from, to, margin) {
    const move = sub(to, from);
    return obstacles.every(wall => !wallHit(from, move, wall, margin));
}

function projectedGoal(goal, radius) {
    let point = bounds(goal, radius + 1);
    for (const wall of obstacles) {
        if (!inside(point, wall, radius + 1)) continue;
        const left = wall.left - radius - 3;
        const right = wall.right + radius + 3;
        const top = wall.top - radius - 3;
        const bottom = wall.bottom + radius + 3;
        point = [
            { x: left, y: point.y }, { x: right, y: point.y },
            { x: point.x, y: top }, { x: point.x, y: bottom }
        ].reduce((a, b) => length(sub(a, point)) < length(sub(b, point)) ? a : b);
    }
    return point;
}

function moveDirection(actor, goal) {
    const from = actor.position;
    const radius = actor.radius;
    const target = projectedGoal(goal, radius);
    if (length(sub(target, from)) < 4) return zero();
    if (clearPath(from, target, radius)) return unit(sub(target, from));
    const points = [from, target];
    for (const wall of obstacles) {
        for (const x of [wall.left - radius - 3, wall.right + radius + 3]) {
            for (const y of [wall.top - radius - 3, wall.bottom + radius + 3]) points.push({ x, y });
        }
    }
    const costs = points.map((_, i) => i === 0 ? 0 : Infinity);
    const first = [];
    const settled = new Set();
    while (settled.size < points.length) {
        let current = -1;
        for (let i = 0; i < points.length; i++) {
            if (!settled.has(i) && costs[i] < (current < 0 ? Infinity : costs[current])) current = i;
        }
        if (current < 0) return zero();
        if (current === 1) return unit(sub(points[first[1]], from));
        settled.add(current);
        for (let i = 1; i < points.length; i++) {
            if (settled.has(i) || !clearPath(points[current], points[i], radius)) continue;
            const cost = costs[current] + length(sub(points[current], points[i]));
            if (cost < costs[i]) {
                costs[i] = cost;
                first[i] = current === 0 ? i : first[current];
            }
        }
    }
    return zero();
}

function actorHit(point, move, actor, radius) {
    const offset = sub(point, actor.position);
    const outside = dot(offset, offset) - (actor.radius + radius) ** 2;
    if (outside <= 0) return 0;
    const speed = dot(move, move);
    const approach = dot(offset, move);
    const discriminant = approach ** 2 - speed * outside;
    if (speed === 0 || approach >= 0 || discriminant < 0) return Infinity;
    const hit = (-approach - Math.sqrt(discriminant)) / speed;
    return hit <= 1 ? hit : Infinity;
}

function makeActor(position, team, now) {
    return {
        position, facing: unit(sub({ x: Width / 2, y: Height / 2 }, position)),
        team, level: 1, kills: 0, hp: 100, maxHp: 100, radius: 16,
        parryTime: 0, guarding: false, speed: 210, damage: 21, interval: 0.32,
        bulletSpeed: 600, guardFactor: 0.4, parryInterval: 0.9, shotDelay: 0,
        parryDelay: 0, lastShot: -10, lastHit: -10, nextLevel: now + LevelInterval,
        regen: 0, killHeal: 0, leech: 0, slowed: 0, invulnerable: 0, sprint: 0,
        revengeDelay: 0, shots: 0, queuedLevels: 0, boost: 0, pending: null,
        pressedAttack: false, pressedParry: false, perks: new Set()
    };
}

export class Arena {
    constructor(config) {
        this.elapsed = 0;
        this.result = "playing";
        this.winner = -1;
        this.accumulator = 0;
        this.actors = [];
        this.bullets = [];
        this.effects = [];
        const spawns = [{ x: 106, y: 280 }, { x: 974, y: 280 }, { x: 540, y: 84 }, { x: 540, y: 476 }];
        const sizes = config.teamSizes.slice(0, config.teams ?? config.teamSizes.length);
        if (sizes.length === 3) {
            spawns[1] = { x: 930, y: 102 };
            spawns[2] = { x: 930, y: 458 };
        }
        sizes.forEach((size, team) => {
            const spawn = spawns[team];
            const facing = unit(sub({ x: Width / 2, y: Height / 2 }, spawn));
            const row = { x: -facing.y, y: facing.x };
            for (let slot = 0; slot < size; slot++) {
                const actor = makeActor(add(spawn, scale(row, (slot - (size - 1) / 2) * 44)), team, this.elapsed);
                this.offer(actor);
                this.actors.push(actor);
            }
        });
    }

    offer(actor) {
        const pool = augments.map((_, id) => id).filter(id => {
            if (id === 22 && !actor.perks.has(10) && !actor.perks.has(26)) return false;
            if (id === 25 && !actor.perks.has(13)) return false;
            if (id === 28 && !actor.perks.has(11)) return false;
            return augments[id][2] || !actor.perks.has(id);
        });
        const pick = available => available[Math.floor(Math.random() * available.length)];
        const choices = actor.level === 1 ? [pick([9, 10, 11, 12])] : [];
        while (choices.length < 3) choices.push(pick(pool.filter(id => !choices.includes(id))));
        if (actor.level === 1) choices.push(choices.shift());
        actor.pending = choices;
    }

    chooseAugment(slot, index) {
        const actor = this.actors[slot];
        const id = actor?.pending?.[index];
        if (this.result !== "playing" || id === undefined) return false;
        const apply = augments[id][2];
        if (apply) apply(actor);
        else actor.perks.add(id);
        actor.pending = null;
        if (actor.queuedLevels > 0 && actor.level < MaxLevel) this.award(actor);
        return true;
    }

    removePlayer(slot) {
        const actor = this.actors[slot];
        if (this.result !== "playing" || !actor) return;
        actor.hp = 0;
        actor.pending = null;
        this.bullets = this.bullets.filter(bullet => bullet.owner !== slot);
        this.settle();
    }

    directionTo(slot, target) {
        const actor = this.actors[slot];
        return actor && actor.hp > 0 ? moveDirection(actor, target) : zero();
    }

    update(dt, inputs) {
        if (this.result !== "playing" || this.actors.some(actor => actor.pending)) return;
        this.actors.forEach((actor, slot) => {
            actor.pressedAttack ||= !!inputs[slot]?.attackPressed;
            actor.pressedParry ||= !!inputs[slot]?.parry;
        });
        this.accumulator += clamp(dt, 0, 0.25);
        while (this.accumulator >= Step) {
            this.accumulator -= Step;
            this.tick(inputs);
            if (this.result !== "playing" || this.actors.some(actor => actor.pending)) {
                this.accumulator = 0;
                this.actors.forEach(actor => { actor.pressedAttack = actor.pressedParry = false; });
                break;
            }
        }
    }

    tick(inputs) {
        this.elapsed += Step;
        for (const effect of this.effects) effect.life -= Step;
        this.effects = this.effects.filter(effect => effect.life > 0);
        this.actors.forEach((actor, slot) => {
            if (actor.hp <= 0) return;
            this.drive(actor, slot, inputs[slot] ?? {});
        });
        this.separateActors();
        this.advanceBullets();
        this.settle();
        if (this.result === "playing") {
            this.actors.forEach(actor => {
                if (actor.hp <= 0 || actor.level >= MaxLevel) return;
                if (this.elapsed >= actor.nextLevel && actor.queuedLevels === 0) actor.queuedLevels = 1;
                if (actor.queuedLevels > 0) this.award(actor);
            });
        }
    }

    drive(actor, slot, input) {
        if (input.aim) {
            const aim = unit(sub(input.aim, actor.position));
            if (length(aim) > 0) actor.facing = aim;
        }
        for (const timer of ["shotDelay", "parryDelay", "parryTime", "slowed", "invulnerable", "sprint", "revengeDelay"]) {
            actor[timer] = Math.max(0, actor[timer] - Step);
        }
        const perk = id => actor.perks.has(id);
        if (actor.pressedParry && actor.parryDelay === 0) {
            actor.parryTime = 0.22;
            actor.parryDelay = actor.parryInterval;
            if (perk(16)) actor.sprint = Math.max(actor.sprint, 0.3);
        }
        actor.guarding = !!input.guard;
        const defending = actor.guarding || actor.parryTime > 0;
        const movement = input.moveTo && input.destination ? moveDirection(actor, input.destination)
            : input.movement ? unit(input.movement) : zero();
        const moving = length(movement) > 0;
        const lowHp = actor.hp < actor.maxHp * 0.35;
        const firing = input.attack || actor.pressedAttack;
        let speed = actor.speed * (defending ? 0.62 : 1) * (actor.slowed ? 0.74 : 1);
        if (lowHp && perk(15)) speed *= 1.3;
        if (actor.sprint) speed *= perk(16) ? 1.65 : 1.35;
        if (!firing && moving && perk(34)) speed *= 1.16;
        this.move(actor, scale(movement, speed * Step));
        if (firing && !defending && actor.shotDelay === 0) {
            this.shoot(actor, slot);
            actor.shotDelay = actor.interval * (perk(9) ? 1.4 : 1)
                * (!moving && perk(19) ? 0.8 : 1) * (lowHp && perk(27) ? 0.75 : 1);
        }
        actor.pressedAttack = actor.pressedParry = false;
        if (this.elapsed - actor.lastHit >= 4) this.heal(actor, actor.regen * Step);
    }

    move(actor, delta) {
        for (const axis of ["x", "y"]) {
            const next = bounds({ ...actor.position, [axis]: actor.position[axis] + delta[axis] }, actor.radius);
            if (obstacles.every(wall => !inside(next, wall, actor.radius))) actor.position = next;
        }
    }

    separateActors() {
        for (let i = 0; i < this.actors.length; i++) {
            const actor = this.actors[i];
            if (actor.hp <= 0) continue;
            for (let j = i + 1; j < this.actors.length; j++) {
                const other = this.actors[j];
                if (other.hp <= 0) continue;
                const delta = sub(actor.position, other.position);
                const distance = length(delta);
                const overlap = actor.radius + other.radius - distance;
                if (overlap <= 0) continue;
                const repel = scale(distance > 0.001 ? unit(delta) : { x: 1, y: 0 }, overlap / 2);
                this.move(actor, repel);
                this.move(other, scale(repel, -1));
            }
        }
    }

    shoot(actor, slot) {
        const perk = id => actor.perks.has(id);
        actor.shots++;
        const charged = perk(18) && actor.shots % 3 === 0;
        const first = perk(30) && this.elapsed - actor.lastShot >= 1.5;
        const damage = actor.damage * (charged ? 1.55 : 1) * (first ? 1.45 : 1) * (1 + actor.boost);
        actor.boost = 0;
        actor.lastShot = this.elapsed;
        const rounds = perk(9) ? [-0.24, -0.12, 0, 0.12, 0.24].map(angle => [angle, 0.36]) : [[0, 1]];
        if (perk(32)) rounds.push([0.32, 0.35]);
        for (const [angle, fraction] of rounds) {
            const aim = rotate(actor.facing, angle);
            this.addBullet(actor, slot, actor.position, aim, damage * fraction,
                actor.bulletSpeed * (charged ? 1.2 : 1), {
                    explosive: perk(10), bounces: perk(11) ? 1 : 0,
                    pierces: perk(12) ? 1 : 0, slow: perk(13)
                });
        }
    }

    addBullet(actor, owner, position, facing, damage, speed, traits = {}) {
        if (this.bullets.length >= BulletLimit) return;
        this.bullets.push({
            position, velocity: scale(facing, speed), team: actor.team, owner, radius: 3,
            life: 2, damage, explosive: false, bounces: 0, pierces: 0, slow: false,
            hitMask: 0, fragment: false, ...traits
        });
    }

    advanceBullets() {
        const count = this.bullets.length;
        for (let i = 0; i < count; i++) {
            const bullet = this.bullets[i];
            bullet.life -= Step;
            if (bullet.life <= 0) continue;
            const move = scale(bullet.velocity, Step);
            let impact = Infinity;
            let axis = "";
            let victim = -1;
            for (const wall of obstacles) {
                const hit = wallHit(bullet.position, move, wall, bullet.radius);
                if (hit && hit.time < impact) {
                    impact = hit.time;
                    axis = hit.axis;
                }
            }
            const edges = [["x", Width], ["y", Height]];
            for (const [key, max] of edges) {
                if (move[key] === 0) continue;
                const edge = move[key] > 0 ? max - bullet.radius : bullet.radius;
                const hit = (edge - bullet.position[key]) / move[key];
                if (hit >= 0 && hit <= 1 && hit < impact) {
                    impact = hit;
                    axis = key;
                }
            }
            this.actors.forEach((actor, slot) => {
                if (actor.hp <= 0 || actor.team === bullet.team || (bullet.hitMask & (1 << slot))) return;
                const time = actorHit(bullet.position, move, actor, bullet.radius);
                if (time < impact) {
                    impact = time;
                    victim = slot;
                }
            });
            bullet.position = add(bullet.position, scale(move, Math.min(impact, 1)));
            if (victim >= 0) this.hitBullet(bullet, victim);
            else if (axis) {
                if (bullet.bounces > 0) {
                    bullet.bounces--;
                    bullet.velocity[axis] *= -1;
                    bullet.position = add(bullet.position, scale(unit(bullet.velocity), 0.1));
                    if (this.actors[bullet.owner].perks.has(28)) bullet.damage *= 1.45;
                } else {
                    if (bullet.explosive) this.blast(bullet.owner, bullet.position, bullet.damage * 0.45, 70);
                    bullet.life = 0;
                }
            }
        }
        this.bullets = this.bullets.filter(bullet => bullet.life > 0);
    }

    hitBullet(bullet, slot) {
        const target = this.actors[slot];
        const incoming = scale(unit(bullet.velocity), -1);
        const angle = dot(target.facing, incoming);
        if (target.parryTime > 0 && angle > 0.30) {
            this.reflect(bullet, target, slot, incoming);
            return;
        }
        const blocked = target.guarding && angle > (target.perks.has(29) ? -0.12 : 0.30);
        const shooter = this.actors[bullet.owner];
        const combo = shooter.perks.has(25) && target.slowed > 0 ? 1.35 : 1;
        this.hit(slot, bullet.owner, bullet.damage * combo * (blocked ? target.guardFactor : 1), bullet.fragment);
        if (blocked && target.perks.has(24)) target.boost = Math.min(0.75, target.boost + 0.25);
        if (bullet.slow && target.hp > 0) target.slowed = Math.max(target.slowed, 0.85);
        if (bullet.pierces > 0) {
            bullet.pierces--;
            bullet.damage *= 0.65;
            bullet.hitMask |= 1 << slot;
        } else {
            if (bullet.explosive) this.blast(bullet.owner, bullet.position, bullet.damage * 0.45, 70, slot);
            bullet.life = 0;
        }
    }

    reflect(bullet, actor, slot, facing) {
        bullet.team = actor.team;
        bullet.owner = slot;
        bullet.hitMask = 0;
        bullet.velocity = scale(bullet.velocity, -1);
        bullet.position = add(bullet.position, scale(facing, 0.5));
        if (actor.perks.has(21)) actor.parryDelay = 0;
        if (actor.perks.has(31)) this.heal(actor, 5);
        if (actor.perks.has(23) && !bullet.fragment) {
            for (const angle of [-0.25, 0.25]) {
                this.addBullet(actor, slot, bullet.position, rotate(facing, angle),
                    bullet.damage * 0.42, length(bullet.velocity), {
                        explosive: bullet.explosive || actor.perks.has(10), fragment: true
                    });
            }
        }
    }

    blast(owner, position, damage, radius, excluded = -1) {
        const attacker = this.actors[owner];
        if (attacker.perks.has(22)) {
            radius *= 1.45;
            damage *= 1.3;
        }
        if (this.effects.length >= 64) this.effects.shift();
        this.effects.push({ ...position, radius, team: attacker.team, life: 0.32 });
        this.actors.forEach((target, slot) => {
            if (slot === excluded || target.hp <= 0 || target.team === attacker.team) return;
            const offset = sub(position, target.position);
            if (length(offset) > radius || !clearPath(position, target.position, -0.25)) return;
            const blocked = target.guarding && dot(target.facing, unit(offset)) > 0.30;
            this.hit(slot, owner, damage * (blocked ? target.guardFactor : 1), true);
        });
    }

    hit(slot, owner, damage, fragment) {
        const victim = this.actors[slot];
        const attacker = this.actors[owner];
        if (victim.hp <= 0 || victim.invulnerable > 0) return;
        const dealt = Math.min(victim.hp, damage);
        victim.hp -= dealt;
        victim.lastHit = this.elapsed;
        this.heal(attacker, dealt * attacker.leech);
        if (victim.hp <= 0 && victim.perks.has(17) && !victim.phoenixUsed) {
            victim.phoenixUsed = true;
            victim.hp = 26;
            victim.invulnerable = 0.6;
        }
        if (victim.hp > 0) {
            if (victim.perks.has(33)) victim.sprint = Math.max(victim.sprint, 0.6);
            if (!fragment && victim.revengeDelay === 0 && victim.perks.has(20)) {
                victim.revengeDelay = 2.5;
                for (let i = 0; i < 8; i++) {
                    this.addBullet(victim, slot, victim.position, rotate({ x: 1, y: 0 }, i * Math.PI / 4),
                        victim.damage * 0.24, victim.bulletSpeed * 0.8, { fragment: true });
                }
            }
            return;
        }
        attacker.kills++;
        this.heal(attacker, attacker.killHeal);
        if (attacker.hp > 0) attacker.queuedLevels = Math.min(MaxLevel - attacker.level, attacker.queuedLevels + 1);
        if (attacker.perks.has(26)) this.blast(owner, victim.position, attacker.damage * 0.7, 85, slot);
    }

    heal(actor, amount) {
        if (actor.hp > 0 && amount > 0) actor.hp = Math.min(actor.maxHp, actor.hp + amount);
    }

    award(actor) {
        actor.queuedLevels--;
        actor.level++;
        actor.nextLevel = this.elapsed + LevelInterval;
        this.offer(actor);
    }

    settle() {
        const teams = new Set(this.actors.filter(actor => actor.hp > 0).map(actor => actor.team));
        if (teams.size <= 1) {
            this.winner = teams.values().next().value ?? -1;
            this.result = this.winner >= 0 ? "victory" : "draw";
            this.actors.forEach(actor => { actor.pending = null; });
        }
    }

    snapshot() {
        const actors = this.actors.map(({ position, facing, team, level, kills, hp, maxHp, radius, parryTime, guarding, shots }) =>
            ({ position, facing, team, level, kills, hp, maxHp, radius, parryTime, guarding, shots }));
        const bullets = this.bullets.map(({ position, velocity, team, radius, explosive }) =>
            ({ position, velocity, team, radius, explosive }));
        const choices = this.actors.map(actor => actor.pending?.map(id => {
            const [name, description] = augments[id];
            return { name, description };
        }) ?? null);
        const levelTime = this.actors.map(actor =>
            actor.hp > 0 && actor.level < MaxLevel ? Math.max(0, actor.nextLevel - this.elapsed) : 0);
        return { elapsed: this.elapsed, result: this.result, winner: this.winner,
            actors, bullets, obstacles, choices, levelTime, effects: this.effects };
    }
}
