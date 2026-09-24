// SC01 v04 geometry check (NOT an After Effects render).
// Re-computes the builder's camera / rig / timing maths at the review timestamps and reports
//   P1  effects drawn IN FRONT of the baby window that reach into the face zone
//   P2  gap between the copy and the circle (ring), and between the copy and the orbit ellipses
//   +   clearance between the path of light (and its stars) and the ring
// Brightness (P3) cannot be judged without a real render; it is not checked here.
// Constants mirror src/BUILD_SC01_v04.src.jsx; keep them in sync when the builder changes.
// usage: node geometry_check.cjs [t1 t2 ...]   (default: 1.9 3.2 4.9 6.8 9.4 11.2)

const W = 1920, H = 1080, ZOOM = 2666.7, CX = W / 2, CY = H / 2, R = 380;
const C = { point: 1.2, bloom: 2.4, layout: 4.0, text: 4.8, path: 8.2, hx: 1330, hy: 500, pull: 700, push: 90 };
// z on the hero rig (window = 0; negative = in front of the window, toward the camera)
const Z = { halo: 30, ring: -2, point: 20, flare: 19, sparks: -10, orbitA: 40, orbitB: 60 };
// world-space z of the free layers
const ZW = { path: 10, star: 8, bokeh: -700, textBase: -120 };
const PATH = { pts: [[-120, 1015], [760, 975], [2060, 950]], pIn: [[0, 0], [-360, 11], [-460, 6]], pOut: [[360, -18], [460, -14], [0, 0]] };
const STAR_SAMPLES = [0.18, 0.34, 0.5, 0.66, 0.8, 0.92];
// assumption: the face sits inside the central 80% of the window radius (real plate not known)
const FACE_R = 0.8 * R;

const clamp = (v, a = 0, b = 1) => Math.max(a, Math.min(b, v));
const lin = (t, t0, t1, v0, v1) => v0 + (v1 - v0) * clamp((t - t0) / (t1 - t0));
const e3 = t => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);
const out3 = t => 1 - Math.pow(1 - t, 3);

const camZ = t => -ZOOM + C.pull * (1 - e3(clamp(t / 5.6))) + C.push * e3(clamp((t - 5.6) / 6.4));
const k = (z, t) => ZOOM / (z - camZ(t));
const proj = (x, y, z, t) => { const f = k(z, t); return [CX + (x - CX) * f, CY + (y - CY) * f]; };
const rig = t => { const u = e3(clamp((t - C.layout) / 1.7)); return [CX + (C.hx - CX) * u, CY + (C.hy - CY) * u]; };
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);

function bez(p0, c0, c1, p1, t) {
    const u = 1 - t;
    return [0, 1].map(i => u * u * u * p0[i] + 3 * u * u * t * c0[i] + 3 * u * t * t * c1[i] + t * t * t * p1[i]);
}
function pathPoints(n) {
    const out = [];
    for (let s = 0; s < 2; s++) {
        const p0 = PATH.pts[s], p1 = PATH.pts[s + 1];
        const c0 = [p0[0] + PATH.pOut[s][0], p0[1] + PATH.pOut[s][1]], c1 = [p1[0] + PATH.pIn[s + 1][0], p1[1] + PATH.pIn[s + 1][1]];
        for (let j = 0; j < n; j++) { out.push(bez(p0, c0, c1, p1, j / (n - 1))); }
    }
    return out;
}
function starPoint(frac) {
    const seg = frac < 0.4 ? 0 : 1, tt = seg === 0 ? frac / 0.4 : (frac - 0.4) / 0.6;
    const p0 = PATH.pts[seg], p1 = PATH.pts[seg + 1];
    return bez(p0, [p0[0] + PATH.pOut[seg][0], p0[1] + PATH.pOut[seg][1]], [p1[0] + PATH.pIn[seg + 1][0], p1[1] + PATH.pIn[seg + 1][1]], p1, tt);
}

// copy boxes in world space (upper-bound widths: Hangul 1.0 em, space 0.33 em, Latin caps 0.72 em)
function textBoxes(t) {
    const zt = ZW.textBase - lin(t, 0, 12, 0, 30);
    const lines = [
        { s: "작은 시작이", size: 56, x: 176, y: 486, delay: 0 },
        { s: "세상을 바꿉니다", size: 90, x: 170, y: 596, delay: 0.5 },
        { s: "A SMALL BEGINNING, A BRIGHTER TOMORROW", size: 16, x: 178, y: 664, delay: 1.5, trackEm: 0.36 }
    ];
    return lines.filter(l => t >= C.text + l.delay && t < 11.8).map(l => {
        let w = 0;
        for (const ch of l.s) { w += (/[가-힣]/.test(ch) ? 1.0 : ch === " " ? 0.33 : ch === "," ? 0.3 : 0.72) * l.size; }
        w += (l.trackEm || 0) * l.size * l.s.length;
        const a = proj(l.x, l.y - 0.88 * l.size, zt, t), b = proj(l.x + w, l.y + 0.12 * l.size, zt, t);
        return { s: l.s, x0: a[0], y0: a[1], x1: b[0], y1: b[1] };
    });
}
function boxToPoint(b, p) {
    const dx = Math.max(b.x0 - p[0], 0, p[0] - b.x1), dy = Math.max(b.y0 - p[1], 0, p[1] - b.y1);
    return Math.hypot(dx, dy);
}
function ellipsePts(cx, cy, z, rx, ry, rotDeg, t, n = 360) {
    const r = rotDeg * Math.PI / 180, out = [];
    for (let i = 0; i < n; i++) {
        const a = 2 * Math.PI * i / n, x = rx * Math.cos(a), y = ry * Math.sin(a);
        out.push(proj(cx + x * Math.cos(r) - y * Math.sin(r), cy + x * Math.sin(r) + y * Math.cos(r), z, t));
    }
    return out;
}

function check(t) {
    const rows = [], fails = [], warns = [];
    const [rx, ry] = rig(t), k0 = k(0, t), wc = proj(rx, ry, 0, t);
    const tau = clamp((t - C.bloom) / 1.6), maskR = R - (R + 70) * (1 - out3(tau));
    const openR = Math.max(0, maskR) * k0, faceR = Math.min(FACE_R, Math.max(0, maskR)) * k0;
    const ringR = (R + 4) * k0;
    rows.push(`window  centre (${wc[0].toFixed(0)}, ${wc[1].toFixed(0)})  open radius ${openR.toFixed(0)}px  ring ${ringR.toFixed(0)}px  face zone ${faceR.toFixed(0)}px`);

    // ---- P1: effects in front of the window ----
    const front = [];
    const st = C.point, bs = C.bloom;
    const tp = t - st, beat = c => Math.exp(-Math.pow((tp - c) / 0.18, 2));
    let ps = 100 * clamp(tp / 0.5) * (1 + 0.35 * beat(0.55) + 0.25 * beat(0.8) + 0.35 * beat(1.35) + 0.25 * beat(1.6));
    ps *= lin(t, bs, bs + 0.9, 1, 6);
    const pOp = tp > 0 ? lin(t, bs + 0.4, bs + 1.2, 100, 0) : 0;
    if (pOp > 0) { front.push({ name: "POINT_LIGHT (+glow blur 130)", z: Z.point, op: pOp, reach: 7 * ps / 100 + 130 }); }
    const fOp = 45 * lin(t, st, st + 0.6, 0, 1) * lin(t, bs + 0.2, bs + 1.0, 1, 0);
    if (fOp > 0) { front.push({ name: "POINT_FLARE", z: Z.flare, op: fOp, reach: 260 * lin(t, bs, bs + 1.0, 100, 260) / 100 }); }
    const sOp = 70 * lin(t, C.layout - 0.2, C.layout + 0.4, 0, 1) * lin(t, C.layout + 2.2, C.layout + 3.6, 1, 0);
    if (sOp > 0) { front.push({ name: "SPARKS (hole r=360 full cut)", z: Z.sparks, op: sOp, innerEdge: 360 * k(Z.sparks, t) }); }
    // bokeh hole: full cut to 540-140=400 in its comp, mapped to screen through the -700 plane
    const bf = ((ZOOM + ZW.bokeh) / ZOOM * 1.15) * k(ZW.bokeh, t);
    const bc = [CX + (rx - CX) / 1.15 * bf, CY + (ry - CY) / 1.15 * bf];
    front.push({ name: "FG_BOKEH (hole r=400 full cut)", z: ZW.bokeh, op: 18, innerEdge: 400 * bf - dist(bc, wc) });
    front.push({ name: "RING glow (blur 70, inward)", z: Z.ring, op: 35, innerEdge: ringR - 70 });

    for (const f of front) {
        const inFront = f.z < 0;
        let status, detail;
        if (!inFront) { status = "behind window"; detail = "occluded by the plate"; }
        else if (faceR <= 0) { status = "ok"; detail = "window not open yet"; }
        else if (f.innerEdge !== undefined) {
            status = f.innerEdge >= faceR ? "ok" : "FAIL";
            detail = `nearest reach ${f.innerEdge.toFixed(0)}px from centre (face zone ${faceR.toFixed(0)}px)`;
        } else {
            status = "FAIL"; detail = `centred on the window, reach ${f.reach.toFixed(0)}px, opacity ${f.op.toFixed(0)}%`;
        }
        rows.push(`P1  ${f.name.padEnd(32)} z=${String(f.z).padStart(4)}  ${status.padEnd(13)} ${detail}`);
        if (status === "FAIL") { fails.push(`P1 ${f.name}`); }
    }

    // ---- P2: copy vs circle / orbits ----
    const boxes = textBoxes(t);
    const ring = ellipsePts(rx, ry, Z.ring, R + 4, R + 4, 0, t);
    const orbA = ellipsePts(rx + 90, ry, Z.orbitA, R + 120, (R + 120) * 0.30, -12, t);
    const orbB = ellipsePts(rx + 90, ry, Z.orbitB, R + 230, (R + 230) * 0.22, 8, t);
    const visible = pts => pts.filter(p => dist(p, wc) > openR);   // orbit parts inside the window are hidden
    for (const b of boxes) {
        const g = Math.min(...ring.map(p => boxToPoint(b, p)));
        const ga = Math.min(...visible(orbA).map(p => boxToPoint(b, p)));
        const gb = Math.min(...visible(orbB).map(p => boxToPoint(b, p)));
        const st2 = g < 0.5 ? "FAIL" : g < 100 ? "WARN" : "ok";
        rows.push(`P2  "${b.s.slice(0, 18)}" box x ${b.x0.toFixed(0)}-${b.x1.toFixed(0)}  gap to ring ${g.toFixed(0)}px [${st2}]  orbitA ${ga.toFixed(0)}px  orbitB ${gb.toFixed(0)}px`);
        if (st2 === "FAIL") { fails.push(`P2 text/ring "${b.s}"`); }
        if (st2 === "WARN") { warns.push(`P2 text/ring "${b.s}" ${g.toFixed(0)}px`); }
        if (Math.min(ga, gb) < 60) { warns.push(`P2 orbit near "${b.s}" ${Math.min(ga, gb).toFixed(0)}px`); }
    }

    // ---- path of light / stars vs ring ----
    const pp = out3(clamp((t - C.path) / 2.8));
    if (pp > 0) {
        const pts = pathPoints(200).map(p => proj(p[0], p[1], ZW.path, t));
        const shown = pts.slice(0, Math.max(2, Math.round(pts.length * pp)));
        const cl = Math.min(...shown.map(p => dist(p, wc) - ringR));
        let starCl = Infinity;
        STAR_SAMPLES.forEach(fr => {
            const hit = C.path + 2.8 * (1 - Math.pow(1 - fr, 1 / 3));
            if (t >= hit) { const s = starPoint(fr); starCl = Math.min(starCl, dist(proj(s[0], s[1], ZW.star, t), wc) - ringR); }
        });
        const stt = Math.min(cl, starCl) < 40 ? "WARN" : "ok";
        rows.push(`PATH  drawn ${(pp * 100).toFixed(0)}%  clearance to ring: path ${cl.toFixed(0)}px, stars ${isFinite(starCl) ? starCl.toFixed(0) + "px" : "-"}  [${stt}]  (glow blur 24/30 px)`);
        if (stt === "WARN") { warns.push(`PATH/ring clearance ${Math.min(cl, starCl).toFixed(0)}px`); }
    }
    return { rows, fails, warns };
}

const times = process.argv.slice(2).map(Number);
let failN = 0, warnN = 0;
for (const t of (times.length ? times : [1.9, 3.2, 4.9, 6.8, 9.4, 11.2])) {
    const r = check(t);
    console.log(`\n=== t=${t.toFixed(1)}s   camera z ${camZ(t).toFixed(0)}   ${r.fails.length ? "FAIL" : r.warns.length ? "WARN" : "ok"}`);
    r.rows.forEach(x => console.log("  " + x));
    r.warns.forEach(x => console.log("  WARN " + x));
    failN += r.fails.length; warnN += r.warns.length;
}
console.log(`\nP1/P2 fails: ${failN}   warnings: ${warnN}   (P3 brightness: needs an AE render)`);
process.exitCode = failN ? 1 : 0;
