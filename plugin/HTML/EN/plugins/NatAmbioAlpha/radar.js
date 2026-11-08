"use strict";

const controls = {
    front: ["FRONT", -6, 6, .5, " dB"], ambience: ["AMBIENCE", -6, 6, .5, " dB"],
    window: ["WINDOW BLOCKS", 1, 20, 1, ""], ild: ["ILD GAIN", 3, 20, .5, " dB"],
    azimuth: ["AZIMUTH", 10, 30, 1, "°"], itd: ["ITD", 80, 300, 5, " µs"],
    alpha: ["ILD ALPHA", 1, 2.5, .1, ""], slope: ["BASS ROLLOFF", 60, 500, 1, " Hz"],
    trim: ["OUTPUT TRIM", -12, 6, .5, " dB"], response: ["RESPONSE", 0, 600, 10, " ms"],
    preamp: ["PREAMP", -18, 0, .5, " dB"],
};
const inputs = new Map();
const player = document.querySelector("#player");
const status = document.querySelector("#status");
const canvas = document.querySelector("#radar");
const ctx = canvas.getContext("2d");
const pending = new Map();
let settings = {};
let telemetry = {};
let busy = false;
let writeQueue = Promise.resolve();

function showValue(input) {
    const key = input.dataset.setting;
    const spec = controls[key];
    if (!spec) return;
    const [, min, max, step, unit] = spec;
    const value = Number(input.value);
    const digits = key === "alpha" ? 2 : step < 1 ? 1 : 0;
    input.previousElementSibling.value = key === "response" && !value ? "RAW" :
        (key === "ild" ? -value : value).toFixed(digits) + unit;
    const fill = key === "ild" ? 1 - (value - min) / (max - min) : (value - min) / (max - min);
    input.style.setProperty("--position", `${fill * 100}%`);
}

for (const node of document.querySelectorAll("[data-control]")) {
    const key = node.dataset.control;
    const [title, min, max, step] = controls[key];
    const label = document.createElement("label");
    const value = document.createElement("output");
    const range = document.createElement("input");
    Object.assign(range, {type: "range", min, max, step});
    range.setAttribute("aria-label", title);
    range.dataset.setting = key;
    label.append(document.createTextNode(title), value, range);
    node.append(label);
}
for (const input of document.querySelectorAll("[data-setting]")) {
    const key = input.dataset.setting;
    inputs.set(key, input);
    input.disabled = true;
    input.addEventListener("input", () => {
        showValue(input);
        settings[key] = input.type === "checkbox" ? Number(input.checked) : Number(input.value);
        const id = player.value;
        const old = pending.get(key);
        if (old) clearTimeout(old.timer);
        const value = settings[key];
        const timer = setTimeout(() => {
            writeQueue = writeQueue.then(async () => {
                try {
                    await rpc(id, ["natambioalpha", "set", key, String(value)]);
                } catch {
                    status.textContent = "Lyrion could not save the control.";
                } finally {
                    if (pending.get(key)?.timer === timer) pending.delete(key);
                }
            });
        }, 120);
        pending.set(key, {timer, id});
    });
    showValue(input);
}

async function rpc(id, command) {
    const response = await fetch("/jsonrpc.js", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({id: 1, method: "slim.request", params: [id, command]}),
        signal: AbortSignal.timeout(3000),
    });
    if (!response.ok) throw Error(response.statusText);
    const data = await response.json();
    if (data.error || !data.result) throw Error(data.error?.message || "No Lyrion result");
    return data.result;
}

async function refresh() {
    if (busy || !player.value || document.hidden) return;
    busy = true;
    const id = player.value;
    try {
        const result = await rpc(id, ["natambioalpha", "state", "?"]);
        if (player.value !== id) return;
        for (const [key, input] of inputs) {
            input.disabled = !result.available;
            if (pending.get(key)?.id === id) continue;
            const value = result.settings[key];
            settings[key] = value;
            if (input.type === "checkbox") input.checked = Boolean(Number(value));
            else input.value = value;
            showValue(input);
        }
        telemetry = result.telemetry || {};
        status.textContent = !result.available ? "DSP helper or SoX unavailable" :
            !settings.enabled ? "Alpha disabled" : "Alpha enabled · takes effect on the next stream";
        updateReadouts();
    } catch {
        if (player.value === id) status.textContent = "Lyrion connection unavailable";
    } finally {
        busy = false;
    }
}

function updateReadouts() {
    const current = telemetry.age <= 5;
    const display = {
        cpu: current ? (100 * telemetry.cpu).toFixed(1) + "%" : "idle",
        rate: current ? (telemetry.rate / 1000).toFixed(1) + " kHz" : "—",
        block: current ? telemetry.block + " smp" : "—",
        xrun: current ? telemetry.xrun : 0,
        peak: current ? (20 * Math.log10(Math.max(telemetry.peak, .000032))).toFixed(1) + " dB" : "—",
        level: current ? Math.min(1, Math.max(telemetry.left, telemetry.right)) : 0,
    };
    for (const [key, value] of Object.entries(display)) document.getElementById(key).value = value;
}

let cloud = {angle: 0, spread: 1, opacity: 0, left: 0, right: 0};
let lastPaint = 0;
function paint(now) {
    requestAnimationFrame(paint);
    if (canvas.closest("[hidden]") || now - lastPaint < 15) return;
    const dt = now - lastPaint;
    lastPaint = now;
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    if (!width || !height) return;
    const ratio = window.devicePixelRatio || 1;
    if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
        canvas.width = Math.round(width * ratio);
        canvas.height = Math.round(height * ratio);
    }
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, width, height);
    const x = width / 2, y = height * .85;
    const radius = Math.min(width * .45, height * .72);
    const theta = (settings.azimuth || 30) * Math.PI / 180;
    ctx.strokeStyle = "#d2cabd";
    for (let ring = 1; ring <= 5; ++ring) {
        ctx.beginPath(); ctx.arc(x, y, radius * ring / 5, Math.PI, 2 * Math.PI); ctx.stroke();
    }
    for (let spoke = -3; spoke <= 3; ++spoke) {
        const a = spoke * Math.PI / 8;
        ctx.beginPath(); ctx.moveTo(x, y);
        ctx.lineTo(x + radius * Math.sin(a), y - radius * Math.cos(a)); ctx.stroke();
    }

    const current = telemetry.age <= 5;
    const valid = current && telemetry.valid && telemetry.major > 0;
    const target = {
        angle: valid ? Math.atan2(telemetry.side * Math.sign(telemetry.mid || 1), Math.abs(telemetry.mid)) : 0,
        spread: valid ? Math.sqrt(Math.max(0, telemetry.minor / telemetry.major)) : 1,
        opacity: Number(valid), left: current ? telemetry.left : 0, right: current ? telemetry.right : 0,
    };
    const decay = settings.response ? 1 - Math.exp(-dt / settings.response) : 1;
    for (const key of Object.keys(cloud)) cloud[key] += (target[key] - cloud[key]) * decay;
    if (cloud.opacity > .01) {
        ctx.save();
        ctx.translate(x, y); ctx.rotate(cloud.angle);
        ctx.scale(.18 + cloud.spread * .7, 1);
        const gradient = ctx.createRadialGradient(0, -.52 * radius, 0, 0, -.52 * radius, .56 * radius);
        gradient.addColorStop(0, `rgba(195,58,34,${.66 * cloud.opacity})`);
        gradient.addColorStop(1, "rgba(195,58,34,0)");
        ctx.fillStyle = gradient;
        ctx.fillRect(-radius, -1.15 * radius, radius * 2, radius * 1.2);
        ctx.restore();
    }
    for (const sign of [-1, 1]) {
        const sx = x + sign * radius * Math.sin(theta), sy = y - radius * Math.cos(theta);
        const rms = sign < 0 ? cloud.left : cloud.right;
        const glow = ctx.createRadialGradient(sx, sy, 0, sx, sy, .18 * radius);
        glow.addColorStop(0, `rgba(196,75,44,${Math.min(.6, rms * 2.8)})`);
        glow.addColorStop(1, "rgba(196,75,44,0)");
        ctx.fillStyle = glow;
        ctx.fillRect(sx - .18 * radius, sy - .18 * radius, .36 * radius, .36 * radius);
        ctx.strokeStyle = "#a49c90";
        ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(sx, sy); ctx.stroke();
        const bend = .14 * radius * (settings.alpha || 1.8);
        const cx = (x + sx) / 2 - sign * bend, cy = (y + sy) / 2;
        ctx.strokeStyle = `rgba(185,66,39,${.28 + (settings.ild || 14) / 60})`;
        ctx.beginPath(); ctx.moveTo(x, y); ctx.quadraticCurveTo(cx, cy, sx, sy); ctx.stroke();
        ctx.fillStyle = ctx.strokeStyle;
        for (let dot = 1; dot <= 4; ++dot) {
            const t = dot / 5 * (1 + ((settings.itd || 260) - 260) / 1600);
            const q = 1 - t;
            ctx.beginPath();
            ctx.arc(q*q*x + 2*q*t*cx + t*t*sx, q*q*y + 2*q*t*cy + t*t*sy, 2, 0, 2 * Math.PI);
            ctx.fill();
        }
        ctx.save(); ctx.translate(sx, sy); ctx.rotate(sign * theta);
        ctx.fillStyle = "#333431"; ctx.fillRect(-8, -13, 16, 26);
        ctx.fillStyle = "#171715"; ctx.beginPath(); ctx.arc(0, 8, 4, 0, 2 * Math.PI); ctx.fill();
        ctx.restore();
    }
    ctx.fillStyle = "#e7e1d6"; ctx.strokeStyle = "#66625b";
    ctx.beginPath(); ctx.arc(x, y, 9, 0, 2 * Math.PI); ctx.fill(); ctx.stroke();
    ctx.fillStyle = "#393934";
    ctx.beginPath(); ctx.arc(x, y, 4, 0, 2 * Math.PI); ctx.fill();
}

const expand = document.querySelector("#expand");
expand.addEventListener("click", () => {
    const open = expand.getAttribute("aria-expanded") !== "true";
    expand.setAttribute("aria-expanded", open);
    expand.setAttribute("aria-label", open ? "Hide technical view" : "Show technical view");
    expand.textContent = open ? "‹" : "›";
    document.querySelector("#field").hidden = !open;
    document.querySelector("#technical").hidden = !open;
    document.querySelector("#instrument").classList.toggle("expanded", open);
});
player.addEventListener("change", () => {
    telemetry = {};
    for (const input of inputs.values()) input.disabled = true;
    refresh();
});
(async () => {
    try {
        const data = await rpc("", ["players", "0", "100"]);
        const selected = new URLSearchParams(location.search).get("player");
        for (const item of data.players_loop || []) {
            player.add(new Option(item.name, item.playerid, false, selected === item.playerid));
        }
        if (player.options.length === 0) status.textContent = "Connect a Lyrion player to get started.";
        await refresh();
    } catch {
        status.textContent = "Could not load Lyrion players.";
    }
})();
setInterval(refresh, 200);
requestAnimationFrame(paint);
