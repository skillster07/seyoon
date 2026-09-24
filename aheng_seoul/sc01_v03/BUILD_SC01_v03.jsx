/*  AHENG SEOUL | SC01 v03 | After Effects skeleton builder
    ------------------------------------------------------------------
    What this script builds (structure only, look is tuned by eye in AE):
      - 16bpc project (linear blending OFF since v03.1)
      - nothing passes over the baby face (stencil / silhouette cutters)
      - 3D stage: nebula (far) / stars (mid) / baby window (hero) /
        Saber ring + orbit arcs / typography / foreground bokeh (near)
      - one-node camera with slow push-in, micro drift and depth of field
      - CTRL null with sliders that drive camera, baby plate, reveal timing
      - SRC_BABY_PLATE: replaceable slot for the real footage
    What it does NOT do:
      - it does not guarantee the final look; every value marked MANUAL
        in the log must be checked by eye in AE.
    Run: File > Scripts > Run Script File... in a NEW, EMPTY project.
    Requires: AE 2022+ recommended. Saber (Video Copilot, free) optional.
*/
(function () {
    var VERSION = "SC01 v03.1";
    var W = 1920, H = 1080, FPS = 30, DUR = 12;
    var ZOOM = 2666.7;                 // 50mm-equivalent zoom for a 1920 comp
    var HERO = [1340, 470];            // circle centre in MAIN (from storyboard cut 01)
    var R = 420;                       // baby window radius
    var MAIN_NAME = "SC01_v03_MAIN";

    // decode Korean stored as hex code points (keeps this file pure ASCII)
    function K(h) { var a = h.split(","), s = "", i; for (i = 0; i < a.length; i++) { s += String.fromCharCode(parseInt(a[i], 16)); } return s; }

    var log = [];
    function L(s) { log.push(s); }

    // ---------- helpers ----------
    function col(r, g, b) { return [r, g, b, 1]; }

    function ellipseShape(cx, cy, rx, ry) {
        var kx = 0.5523 * rx, ky = 0.5523 * ry;
        var s = new Shape();
        s.vertices = [[cx, cy - ry], [cx + rx, cy], [cx, cy + ry], [cx - rx, cy]];
        s.inTangents = [[-kx, 0], [0, -ky], [kx, 0], [0, ky]];
        s.outTangents = [[kx, 0], [0, ky], [-kx, 0], [0, -ky]];
        s.closed = true;
        return s;
    }

    function addMask(layer, shape, feather, mode) {
        var m = layer.property("ADBE Mask Parade").addProperty("ADBE Mask Atom");
        m.property("ADBE Mask Shape").setValue(shape);
        if (feather) { m.property("ADBE Mask Feather").setValue([feather, feather]); }
        if (mode !== undefined) { m.maskMode = mode; }
        return m;
    }

    function findP(group, names) {
        var i, j, q, r;
        for (i = 1; i <= group.numProperties; i++) {
            q = group.property(i);
            for (j = 0; j < names.length; j++) {
                if (q.name === names[j] || q.matchName === names[j]) { return q; }
            }
            if (q.propertyType !== PropertyType.PROPERTY) {
                r = findP(q, names);
                if (r) { return r; }
            }
        }
        return null;
    }

    // set a value (or expression) on an effect parameter found by name, or by index as fallback
    function setP(eff, names, idx, value, label, isExpr) {
        try {
            var q = findP(eff, names);
            if (!q && idx) { q = eff.property(idx); }
            if (!q) { L("MANUAL  " + label + " (parameter not found)"); return null; }
            if (isExpr) { q.expression = value; } else { q.setValue(value); }
            return q;
        } catch (e) {
            L("MANUAL  " + label + " -> " + e.toString());
            return null;
        }
    }

    function addFx(layer, matchName, name) {
        var parade = layer.property("ADBE Effect Parade");
        if (!parade.canAddProperty(matchName)) { L("MISSING effect " + matchName + " on " + layer.name); return null; }
        var e = parade.addProperty(matchName);
        if (name) { e.name = name; }
        return e;
    }

    function findEffectMatchName(displayPart) {
        try {
            var list = app.effects, i;
            for (i = 0; i < list.length; i++) {
                if (list[i].displayName.toLowerCase().indexOf(displayPart) >= 0) { return list[i].matchName; }
            }
        } catch (e) {}
        return null;
    }

    function pickFont(candidates) {
        var i;
        try {
            if (app.fonts && app.fonts.getFontsByPostScriptName) {
                for (i = 0; i < candidates.length; i++) {
                    if (app.fonts.getFontsByPostScriptName(candidates[i]).length > 0) { return candidates[i]; }
                }
            }
        } catch (e) {}
        L("MANUAL  font: none of [" + candidates.join(", ") + "] confirmed; AE may substitute");
        return candidates[candidates.length - 1];
    }

    // effect-free solid whose mask cuts the layers below it in the same comp
    // (SILHOUETTE_ALPHA = punch a hole, STENCIL_ALPHA = keep only inside)
    function alphaCutter(comp, name, cx, cy, rx, ry, feather, mode) {
        var c = comp.layers.addSolid([1, 1, 1], name, comp.width, comp.height, 1, comp.duration);
        addMask(c, ellipseShape(cx, cy, rx, ry), feather);
        c.blendingMode = mode;
        return c;
    }

    function slider(ctrl, name, v) {
        var e = addFx(ctrl, "ADBE Slider Control", name);
        e.property(1).setValue(v);
        return e;
    }

    function solid(comp, name, c, w, h) {
        return comp.layers.addSolid(c, name, w || W, h || H, 1, comp.duration);
    }

    // CTRL reference used by expressions inside precomps
    var CT = 'comp("' + MAIN_NAME + '").layer("CTRL").effect';

    app.beginUndoGroup("AHENG " + VERSION);
    var proj = app.project;
    if (!proj) { proj = app.newProject(); }

    // ---------- project settings ----------
    try { proj.bitsPerChannel = 16; L("OK      project 16bpc"); } catch (e1) { L("MANUAL  set 16bpc: " + e1); }
    // v03.1: linear blending OFF. With many ADD layers it pushed the whole frame to white.
    try { proj.linearBlending = false; L("OK      linear blending off (predictable ADD/Glow)"); } catch (e2) { L("MANUAL  linear blending off: " + e2); }

    // ---------- folders ----------
    var fRoot = proj.items.addFolder("AHENG_SC01_v03");
    var fSrc = proj.items.addFolder("01_REPLACE_SOURCE"); fSrc.parentFolder = fRoot;
    var fPre = proj.items.addFolder("02_PRECOMPS"); fPre.parentFolder = fRoot;

    // ---------- MAIN comp first (expressions in precomps refer to it by name) ----------
    var main = proj.items.addComp(MAIN_NAME, W, H, 1, DUR, FPS);
    main.parentFolder = fRoot;
    main.motionBlur = true;
    try { main.shutterAngle = 180; main.shutterPhase = -90; } catch (e3) {}
    main.bgColor = [0, 0, 0];

    // CTRL
    var ctrl = main.layers.addNull(DUR);
    ctrl.name = "CTRL";
    ctrl.enabled = false;
    slider(ctrl, "Camera Push (px)", 220);
    slider(ctrl, "Camera Drift (px)", 6);
    slider(ctrl, "DOF Aperture", 30);
    slider(ctrl, "Reveal Start (s)", 1.2);
    slider(ctrl, "Baby Speed %", 100);
    slider(ctrl, "Baby Start (s)", 0);
    slider(ctrl, "Baby Scale %", 100);
    slider(ctrl, "Baby X Offset", 0);
    slider(ctrl, "Baby Y Offset", 0);
    slider(ctrl, "Nebula Opacity", 100);
    slider(ctrl, "Warm Core Opacity", 18);
    // v03.1 look masters: start low, raise by eye
    slider(ctrl, "Ring Glow %", 100);
    slider(ctrl, "Bokeh Opacity", 10);
    slider(ctrl, "Bloom Mix %", 35);
    slider(ctrl, "Text Start (s)", 4.6);          // narration line 2
    slider(ctrl, "Streak Start (s)", 8.2);        // narration line 3
    L("OK      CTRL sliders");

    // ---------- SRC_BABY_PLATE (replace slot) ----------
    var src = proj.items.addComp(("SRC_BABY_PLATE  <- " + K("c2e4,c81c") + " " + K("cd2c,c601,bcf8,c73c,b85c") + " " + K("ad50,ccb4")), W, H, 1, 14, FPS);
    src.parentFolder = fSrc;
    var ph = solid(src, "PLACEHOLDER_BG", [0.2, 0.16, 0.14]);
    var phRamp = addFx(ph, "ADBE Ramp");
    if (phRamp) {
        phRamp.property(1).setValue([W / 2, H / 2]);
        phRamp.property(2).setValue(col(0.42, 0.34, 0.30));
        phRamp.property(3).setValue([W / 2 + 520, H / 2]);
        phRamp.property(4).setValue(col(0.07, 0.06, 0.06));
        phRamp.property(5).setValue(2);
    }
    // slow drift so the placeholder is never a still frame
    ph.transform.scale.expression = 'var s=linear(time,0,thisComp.duration,100,108);[s,s];';
    var phT = src.layers.addText(("REPLACE: " + K("c544,ae30") + " " + K("d074,b85c,c988,c5c5") + " " + K("c2e4,c0ac") + " | 4K " + K("ad8c,c7a5") + " | 14" + K("cd08") + " " + K("c774,c0c1") + " | " + K("c5bc,ad74,c744") + " " + K("d654,ba74") + " " + K("c911,c559,c5d0")));
    var phDoc = phT.property("ADBE Text Properties").property("ADBE Text Document").value;
    phDoc.fontSize = 34; phDoc.fillColor = [1, 1, 1]; phDoc.applyFill = true; phDoc.applyStroke = false;
    phDoc.justification = ParagraphJustification.CENTER_JUSTIFY;
    phT.property("ADBE Text Properties").property("ADBE Text Document").setValue(phDoc);
    phT.transform.position.setValue([W / 2, H / 2]);

    // ---------- PRE_BABY_WINDOW ----------
    var win = proj.items.addComp("PRE_BABY_WINDOW", W, H, 1, DUR, FPS);
    win.parentFolder = fPre;
    var baby = win.layers.add(src);
    baby.name = "BABY_PLATE";
    try {
        baby.timeRemapEnabled = true;
        baby.property("ADBE Time Remapping").expression =
            'var sp=' + CT + '("Baby Speed %")(1)/100;\n' +
            'var st=' + CT + '("Baby Start (s)")(1);\n' +
            'clamp(st+time*sp,0,source.duration-thisComp.frameDuration);';
        L("OK      baby time remap driven by CTRL");
    } catch (e4) { L("MANUAL  baby time remap: " + e4); }
    baby.transform.position.expression =
        '[' + HERO[0] + '+' + CT + '("Baby X Offset")(1),' + HERO[1] + '+' + CT + '("Baby Y Offset")(1)];';
    baby.transform.scale.expression =
        'var s=' + CT + '("Baby Scale %")(1)*linear(time,0,thisComp.duration,1,1.035);[s,s];';
    baby.motionBlur = true;
    var bm = addMask(baby, ellipseShape(HERO[0], HERO[1], R, R), 90);
    // mask shape is in layer space; follow the layer so the circle stays fixed on screen
    bm.property("ADBE Mask Shape").expression =
        'var c=fromComp([' + HERO[0] + ',' + HERO[1] + ']);var r=' + R + '/(transform.scale[0]/100);var k=0.5523*r;\n' +
        'createPath([[c[0],c[1]-r],[c[0]+r,c[1]],[c[0],c[1]+r],[c[0]-r,c[1]]],[[-k,0],[0,-k],[k,0],[0,k]],[[k,0],[0,k],[-k,0],[0,-k]],true);';
    bm.property("ADBE Mask Offset").expression =
        'var st=' + CT + '("Reveal Start (s)")(1);\n' +
        'var t=(time-(st+0.9))/2.2;t=clamp(t,0,1);t=1-Math.pow(1-t,3);\n' +
        'linear(t,0,1,-' + (R + 100) + ',0);';

    var core = solid(win, "WARM_CORE", [0, 0, 0]);
    core.blendingMode = BlendingMode.ADD;
    var coreRamp = addFx(core, "ADBE Ramp");
    if (coreRamp) {
        coreRamp.property(1).setValue([HERO[0] - 60, HERO[1] - 90]);
        coreRamp.property(2).setValue(col(1.0, 0.78, 0.55));
        coreRamp.property(3).setValue([HERO[0] + R, HERO[1] + R]);
        coreRamp.property(4).setValue(col(0, 0, 0));
        coreRamp.property(5).setValue(2);
    }
    alphaCutter(win, "STENCIL_WINDOW", HERO[0], HERO[1], R + 30, R + 30, 110, BlendingMode.STENCIL_ALPHA);
    core.transform.opacity.expression = CT + '("Warm Core Opacity")(1)*linear(time,' + CT + '("Reveal Start (s)")(1)+1.5,' + CT + '("Reveal Start (s)")(1)+3.5,0,1);';
    L("OK      PRE_BABY_WINDOW");

    // ---------- PRE_BG_NEBULA ----------
    var neb = proj.items.addComp("PRE_BG_NEBULA", W, H, 1, DUR, FPS);
    neb.parentFolder = fPre;
    var nbase = solid(neb, "BASE_GRADIENT", [0, 0, 0]);
    var nr = addFx(nbase, "ADBE Ramp");
    if (nr) {
        nr.property(1).setValue(HERO);
        nr.property(2).setValue(col(0.07, 0.13, 0.30));
        nr.property(3).setValue([HERO[0] + 1500, HERO[1] + 300]);
        nr.property(4).setValue(col(0.008, 0.015, 0.045));
        nr.property(5).setValue(2);
    }

    function nebulaLayer(comp, name, tintWhite, opacity, mode, scale, evo, contrast, brightness) {
        var l = solid(comp, name, [0, 0, 0]);
        var fn = addFx(l, "ADBE Fractal Noise");
        if (fn) {
            setP(fn, ["Contrast", (K("b300,be44"))], 4, contrast, name + " Fractal Contrast=" + contrast);
            setP(fn, ["Brightness", (K("bc1d,ae30"))], 5, brightness, name + " Fractal Brightness=" + brightness);
            setP(fn, ["Complexity", (K("bcf5,c7a1,b3c4"))], 8, 7, name + " Fractal Complexity=7");
            setP(fn, ["Uniform Scaling", (K("ade0,c77c") + " " + K("be44,c728"))], null, 1, name + " Uniform Scaling on");
            setP(fn, ["Scale", (K("be44,c728"))], null, scale, name + " Fractal Scale=" + scale);
            setP(fn, ["Evolution", (K("c9c4,d654"))], 10, "time*" + evo, name + " Evolution=time*" + evo, true);
        }
        var tn = addFx(l, "ADBE Tint");
        if (tn) {
            tn.property(1).setValue(col(0, 0, 0));
            tn.property(2).setValue(tintWhite);
        }
        l.blendingMode = mode;
        l.transform.opacity.expression = opacity + '*' + CT + '("Nebula Opacity")(1)/100;';
        return l;
    }
    // v03.1: darker, sparser nebula (v03.0 values blew out to white)
    nebulaLayer(neb, "NEBULA_COOL", col(0.10, 0.20, 0.48), 32, BlendingMode.SCREEN, 900, 10, 115, -48);
    // warm spill lives in its own comp so a stencil can limit it to the area around the baby
    var warmC = proj.items.addComp("PRE_WARM_SPILL", W, H, 1, DUR, FPS);
    warmC.parentFolder = fPre;
    nebulaLayer(warmC, "NEBULA_WARM", col(0.9, 0.62, 0.38), 100, BlendingMode.NORMAL, 500, 16, 120, -55);
    alphaCutter(warmC, "STENCIL_AROUND_BABY", HERO[0], HERO[1], 900, 650, 500, BlendingMode.STENCIL_ALPHA);
    var warmL = neb.layers.add(warmC);
    warmL.blendingMode = BlendingMode.ADD;
    warmL.transform.opacity.expression = '9*' + CT + '("Nebula Opacity")(1)/100;';
    L("OK      PRE_BG_NEBULA (Fractal values need eye check)");

    // ---------- PRE_BG_STARS ----------
    var stars = proj.items.addComp("PRE_BG_STARS", W, H, 1, DUR, FPS);
    stars.parentFolder = fPre;
    function starLayer(name, c, grid, size, speed, opacity) {
        var l = solid(stars, name, c);
        var sb = addFx(l, "CC Star Burst");
        if (sb) {
            setP(sb, ["Scatter"], 1, 300, name + " StarBurst Scatter=300");
            setP(sb, ["Speed"], 2, speed, name + " StarBurst Speed=" + speed);
            setP(sb, ["Grid Spacing"], 4, grid, name + " StarBurst Grid=" + grid);
            setP(sb, ["Size"], 5, size, name + " StarBurst Size=" + size);
        }
        l.blendingMode = BlendingMode.ADD;
        l.transform.opacity.setValue(opacity);
        // twinkle
        l.transform.opacity.expression = 'value*(0.8+0.2*noise(time*1.5));';
        return l;
    }
    starLayer("STARS_FINE", [0.78, 0.86, 1.0], 9, 22, 0.02, 70);
    starLayer("STARS_BRIGHT", [1.0, 0.95, 0.86], 34, 55, 0.03, 90);
    L("OK      PRE_BG_STARS (CC Star Burst)");

    // ---------- PRE_FG_BOKEH ----------
    var bok = proj.items.addComp("PRE_FG_BOKEH", W, H, 1, DUR, FPS);
    bok.parentFolder = fPre;
    var bl = solid(bok, "BOKEH_BLOBS", [0, 0, 0]);
    var bfn = addFx(bl, "ADBE Fractal Noise");
    if (bfn) {
        setP(bfn, ["Contrast", (K("b300,be44"))], 4, 420, "BOKEH Fractal Contrast=420");
        setP(bfn, ["Brightness", (K("bc1d,ae30"))], 5, -165, "BOKEH Fractal Brightness=-165");
        setP(bfn, ["Complexity", (K("bcf5,c7a1,b3c4"))], 8, 1, "BOKEH Fractal Complexity=1");
        setP(bfn, ["Uniform Scaling", (K("ade0,c77c") + " " + K("be44,c728"))], null, 1, "BOKEH Uniform Scaling on");
        setP(bfn, ["Scale", (K("be44,c728"))], null, 520, "BOKEH Fractal Scale=520");
        setP(bfn, ["Evolution", (K("c9c4,d654"))], 10, "time*8", "BOKEH Evolution", true);
    }
    var bbl = addFx(bl, "ADBE Box Blur2");
    if (bbl) { bbl.property(1).setValue(28); bbl.property(2).setValue(3); }
    var btn = addFx(bl, "ADBE Tint");
    if (btn) { btn.property(1).setValue(col(0, 0, 0)); btn.property(2).setValue(col(0.85, 0.8, 1.0)); }
    bl.blendingMode = BlendingMode.ADD;
    bl.transform.opacity.expression = 'comp("' + MAIN_NAME + '").layer("CTRL").effect("Bokeh Opacity")(1);';
    // keep the baby face clear: bokeh never passes over the window (approx. screen position through the z -700 scale)
    alphaCutter(bok, "HOLE_OVER_BABY", 1410, 455, 620, 620, 320, BlendingMode.SILHOUETTE_ALPHA);
    L("OK      PRE_FG_BOKEH");

    // ---------- MAIN: 3D stage ----------
    function stage(item, name, z, mode, extraScale) {
        var l = main.layers.add(item);
        l.name = name;
        l.threeDLayer = true;
        l.transform.position.setValue([W / 2, H / 2, z]);
        var s = (ZOOM + z) / ZOOM * 100 * (extraScale || 1.15);
        l.transform.scale.setValue([s, s, 100]);
        if (mode !== undefined) { l.blendingMode = mode; }
        l.motionBlur = true;
        return l;
    }
    stage(neb, "BG_NEBULA", 3000);
    stage(stars, "BG_STARS", 1400, BlendingMode.ADD);
    var babyL = stage(win, "BABY_WINDOW", 0, undefined, 1.0);
    var bokL = stage(bok, "FG_BOKEH", -700, BlendingMode.ADD);

    // Saber ring + orbits
    var saberMN = findEffectMatchName("saber");
    if (!saberMN) { L("MISSING Saber not found: ring layers are created with masks only, apply Saber manually"); }

    function saberLayer(name, radius, z, rotX, rotZ, glowCol, intensity, coreSize, endOffsetExpr, startOffsetExpr, spinDegPerSec) {
        var S = Math.ceil((radius + 400) * 2);
        var l = main.layers.addSolid([0, 0, 0], name, S, S, 1, DUR);
        l.threeDLayer = true;
        l.blendingMode = BlendingMode.ADD;
        l.motionBlur = true;
        l.transform.position.setValue([HERO[0], HERO[1], z]);
        var tr = l.property("ADBE Transform Group");
        tr.property("ADBE Rotate X").setValue(rotX);
        tr.property("ADBE Rotate Z").setValue(rotZ);
        if (spinDegPerSec) { tr.property("ADBE Rotate Z").expression = 'value+time*' + spinDegPerSec + ';'; }
        addMask(l, ellipseShape(S / 2, S / 2, radius, radius), 0, MaskMode.NONE);
        if (saberMN) {
            var sb = addFx(l, saberMN, "Saber");
            if (sb) {
                setP(sb, ["Glow Color"], null, glowCol, name + " Saber Glow Color");
                setP(sb, ["Glow Intensity"], null, intensity, name + " Saber Glow Intensity=" + intensity);
                setP(sb, ["Glow Intensity"], null, 'value*thisComp.layer("CTRL").effect("Ring Glow %")(1)/100;', name + " Saber Glow Intensity -> CTRL Ring Glow %", true);
                setP(sb, ["Core Size"], null, coreSize, name + " Saber Core Size=" + coreSize);
                setP(sb, ["Core Type"], null, 2, name + " Saber Core Type=Layer Masks");
                L("VERIFY  " + name + ": Saber > Customize Core > Core Type = Layer Masks");
                if (endOffsetExpr) { setP(sb, ["End Offset"], null, endOffsetExpr, name + " Saber End Offset (draw-on)", true); }
                if (startOffsetExpr) { setP(sb, ["Start Offset"], null, startOffsetExpr, name + " Saber Start Offset", true); }
                L("VERIFY  " + name + ": Saber > Render Settings > Composite Settings = Transparent");
            }
        }
        return l;
    }
    var rs = CT.replace('comp("' + MAIN_NAME + '").', 'thisComp.');
    var drawOn = 'var st=' + rs + '("Reveal Start (s)")(1);var t=clamp((time-st)/2.4,0,1);100*(1-Math.pow(1-t,3));';
    saberLayer("LIGHT_RING_HERO", R + 6, 0, 0, 0, col(1.0, 0.82, 0.6), 9, 1.0, drawOn, null, 0);
    saberLayer("ORBIT_A", R + 230, 40, 72, -14, col(0.45, 0.65, 1.0), 3, 0.5,
        'var st=' + rs + '("Reveal Start (s)")(1);linear(time,st+0.8,st+3.6,0,62);', null, 7);
    saberLayer("ORBIT_B", R + 420, -60, 78, 10, col(0.55, 0.72, 1.0), 2, 0.4,
        'var st=' + rs + '("Reveal Start (s)")(1);linear(time,st+1.4,st+4.4,0,38);', null, -4);
    // v03.1: bottom light streak kept from the approved v02 (foreshadows the path to the star in SC14)
    var streak = main.layers.addSolid([0, 0, 0], "LIGHT_STREAK_BOTTOM", W, H, 1, DUR);
    streak.threeDLayer = true;
    streak.blendingMode = BlendingMode.ADD;
    streak.motionBlur = true;
    streak.transform.position.setValue([W / 2, H / 2, 20]);
    var sp = new Shape();
    sp.vertices = [[-120, 1010], [880, 925], [2040, 975]];
    sp.inTangents = [[0, 0], [-420, 18], [-380, -20]];
    sp.outTangents = [[380, -30], [420, -18], [0, 0]];
    sp.closed = false;
    addMask(streak, sp, 0, MaskMode.NONE);
    if (saberMN) {
        var ssb = addFx(streak, saberMN, "Saber");
        if (ssb) {
            setP(ssb, ["Glow Color"], null, col(1.0, 0.8, 0.58), "STREAK Saber Glow Color");
            setP(ssb, ["Glow Intensity"], null, 3, "STREAK Saber Glow Intensity=3");
            setP(ssb, ["Glow Intensity"], null, 'value*thisComp.layer("CTRL").effect("Ring Glow %")(1)/100;', "STREAK Glow -> CTRL", true);
            setP(ssb, ["Core Size"], null, 0.4, "STREAK Saber Core Size=0.4");
            setP(ssb, ["Core Type"], null, 2, "STREAK Saber Core Type=Layer Masks");
            setP(ssb, ["End Offset"], null, 'var st=' + rs + '("Streak Start (s)")(1);var t=clamp((time-st)/2.8,0,1);100*(1-Math.pow(1-t,3));', "STREAK draw-on", true);
            L("VERIFY  LIGHT_STREAK_BOTTOM: Core Type = Layer Masks, thin warm line under the text");
        }
    }
    L("OK      Saber ring + 2 orbits + bottom streak (verify Core Type / Composite in Saber UI)");

    // ---------- typography ----------
    var fontKR = pickFont(["NotoSerifKR-Light", "NanumMyeongjo", "AppleMyungjo"]);
    var fontKRB = pickFont(["NotoSerifKR-SemiBold", "NanumMyeongjoBold", "AppleMyungjo"]);
    var fontEN = pickFont(["Montserrat-Light", "HelveticaNeue-Light"]);

    function textLayer(str, font, size, tracking, pos, z, fill, inOffset, revealDur) {
        var t = main.layers.addText(str);
        var tp = t.property("ADBE Text Properties").property("ADBE Text Document");
        var d = tp.value;
        d.font = font; d.fontSize = size; d.tracking = tracking;
        d.fillColor = fill; d.applyFill = true; d.applyStroke = false;
        d.justification = ParagraphJustification.LEFT_JUSTIFY;
        tp.setValue(d);
        t.threeDLayer = true;
        t.motionBlur = true;
        t.transform.position.setValue([pos[0], pos[1], z]);
        // slow forward drift: nothing on screen is ever fully static
        t.transform.position.expression = 'value+[0,0,-linear(time,inPoint,thisComp.duration,0,40)];';

        var anims = t.property("ADBE Text Properties").property("ADBE Text Animators");
        // A1: per-character reveal (opacity + blur + rise)
        var a1 = anims.addProperty("ADBE Text Animator");
        a1.name = "REVEAL";
        var ap = a1.property("ADBE Text Animator Properties");
        ap.addProperty("ADBE Text Opacity").setValue(0);
        ap = a1.property("ADBE Text Animator Properties");
        ap.addProperty("ADBE Text Blur").setValue([16, 16]);
        ap = a1.property("ADBE Text Animator Properties");
        ap.addProperty("ADBE Text Position 3D").setValue([0, 26, 0]);
        var sel = a1.property("ADBE Text Selectors").addProperty("ADBE Text Selector");
        try { sel.property("ADBE Text Range Advanced").property("ADBE Text Range Shape").setValue(2); } catch (e5) { L("MANUAL  " + str + " range shape=Ramp Up"); }
        L("VERIFY  text '" + str + "': invisible before Text Start, reveals left to right");
        // Ramp Up + Offset -100 -> 100: characters start fully hidden and reveal left to right
        sel.property("ADBE Text Percent Offset").expression =
            'var st=thisComp.layer("CTRL").effect("Text Start (s)")(1)+' + inOffset + ';\n' +
            'var t=clamp((time-st)/' + revealDur + ',0,1);t=1-Math.pow(1-t,3);\n' +
            'linear(t,0,1,-100,100);';
        // A2: whole-line tracking settle (long, slow)
        var a2 = anims.addProperty("ADBE Text Animator");
        a2.name = "TRACK_SETTLE";
        a2.property("ADBE Text Animator Properties").addProperty("ADBE Text Tracking Amount").expression =
            'var st=thisComp.layer("CTRL").effect("Text Start (s)")(1)+' + inOffset + ';\n' +
            'ease(time,st,st+4.5,' + Math.round(size * 0.4) + ',0);';
        // exit
        t.transform.opacity.expression = 'linear(time,10.9,11.7,100,0);';
        return t;
    }
    var ivory = [0.96, 0.94, 0.9];
    var t1 = textLayer((K("c791,c740") + " " + K("c2dc,c791,c774")), fontKR, 66, 20, [190, 505], -160, ivory, 0, 1.4);
    var t2 = textLayer((K("c138,c0c1,c744") + " " + K("bc14,afc9,b2c8,b2e4")), fontKRB, 104, 0, [184, 640], -160, ivory, 0.55, 1.6);
    var t3 = textLayer("A SMALL BEGINNING, A BRIGHTER TOMORROW", fontEN, 19, 420, [194, 712], -160, [0.72, 0.8, 0.95], 1.8, 1.4);
    t3.transform.opacity.expression = 'linear(time,10.9,11.7,70,0);';

    // light sweep across the hero line
    var ls = addFx(t2, "CC Light Sweep");
    if (ls) {
        setP(ls, ["Center"], 1,
            'var r=sourceRectAtTime(time,false);var t=ease(time,6.6,7.8,0,1);\n' +
            '[r.left+linear(t,0,1,-300,r.width+300),r.top+r.height/2];', "Light Sweep Center", true);
        setP(ls, ["Width"], 4, 70, "Light Sweep Width=70");
        setP(ls, ["Sweep Intensity"], 5, 60, "Light Sweep Intensity=60");
        setP(ls, ["Light Color"], 8, col(1.0, 0.86, 0.66), "Light Sweep Color");
    }
    L("OK      typography (serif KR + wide-tracked EN caption)");

    // ---------- finishing: bloom / vignette / grain (2D, on top) ----------
    var bloom = solid(main, "FX_BLOOM", [0, 0, 0]);
    bloom.adjustmentLayer = true;
    function glow(name, threshold, radius, intensity) {
        var g = addFx(bloom, "ADBE Glo2", name);
        if (g) {
            setP(g, ["Glow Threshold"], 2, threshold, name + " threshold=" + threshold + "%");
            L("VERIFY  FX_BLOOM " + name + " threshold reads " + threshold + "% in the Effect Controls");
            setP(g, ["Glow Radius"], 3, radius, name + " radius=" + radius);
            setP(g, ["Glow Intensity"], 4, intensity, name + " intensity=" + intensity);
        }
    }
    // v03.1: bloom only catches real highlights; overall amount via CTRL "Bloom Mix %"
    glow("GLOW_TIGHT", 88, 10, 0.35);
    glow("GLOW_MID", 84, 50, 0.22);
    glow("GLOW_WIDE", 80, 200, 0.12);
    bloom.transform.opacity.expression = 'thisComp.layer("CTRL").effect("Bloom Mix %")(1);';

    var vig = solid(main, "FX_VIGNETTE", [0, 0, 0]);
    var vm = addMask(vig, ellipseShape(W / 2 + 120, H / 2, W * 0.62, H * 0.66), 520);
    vm.inverted = true;
    vig.transform.opacity.setValue(55);

    var grain = solid(main, "FX_GRAIN", [0, 0, 0]);
    grain.adjustmentLayer = true;
    var nz = addFx(grain, "ADBE Noise");
    if (nz) {
        setP(nz, ["Amount of Noise"], 1, 2.5, "Grain amount=2.5%");
        setP(nz, ["Use Color Noise"], 2, 0, "Grain color noise off");
    }

    // ---------- camera ----------
    var cam = main.layers.addCamera("CAM", [W / 2, H / 2]);
    cam.autoOrient = AutoOrientType.NO_AUTO_ORIENT;
    var co = cam.property("ADBE Camera Options Group");
    co.property("ADBE Camera Zoom").setValue(ZOOM);
    try {
        co.property("ADBE Camera Depth of Field").setValue(1);
        co.property("ADBE Camera Blur Level").setValue(100);
        co.property("ADBE Camera Aperture").expression = 'thisComp.layer("CTRL").effect("DOF Aperture")(1);';
        co.property("ADBE Camera Focus Distance").expression =
            'thisComp.layer("BABY_WINDOW").transform.position[2]-transform.position[2];';
    } catch (e6) { L("MANUAL  camera DOF: " + e6); }
    cam.transform.position.setValue([W / 2, H / 2, -ZOOM]);
    cam.transform.position.expression =
        'var push=thisComp.layer("CTRL").effect("Camera Push (px)")(1);\n' +
        'var d=thisComp.layer("CTRL").effect("Camera Drift (px)")(1);\n' +
        'var t=time/thisComp.duration;t=t<0.5?4*t*t*t:1-Math.pow(-2*t+2,3)/2;\n' +
        'var w=wiggle(0.25,d);\n' +
        '[w[0],w[1],value[2]+push*t];';
    L("OK      camera: push-in + drift + DOF focused on BABY_WINDOW");

    // layer order: FX on top, CTRL at the very top
    grain.moveToBeginning();
    bloom.moveAfter(grain);
    vig.moveAfter(bloom);
    ctrl.moveToBeginning();
    cam.moveAfter(ctrl);

    app.endUndoGroup();

    // ---------- save + log ----------
    var here = File($.fileName).parent;
    try {
        if (!proj.file) {
            var out = new File(here.fsName + "/AHENG_SC01_v03_EDITABLE.aep");
            if (out.exists) { out = new File(here.fsName + "/AHENG_SC01_v03_EDITABLE_" + new Date().getTime() + ".aep"); }
            proj.save(out);
            L("SAVED   " + out.fsName);
        } else {
            L("INFO    project already had a file; not saved automatically");
        }
    } catch (e7) { L("MANUAL  save project: " + e7); }

    var manual = 0, i;
    for (i = 0; i < log.length; i++) { if (log[i].indexOf("MANUAL") === 0 || log[i].indexOf("MISSING") === 0) { manual++; } }
    var header = VERSION + "\nAfter Effects " + app.version + "\n" + (manual ? manual + " item(s) need a manual check\n" : "no manual items\n") + "----\n";
    try {
        var lf = new File(here.fsName + "/BUILD_LOG_SC01_v03_" + new Date().getTime() + ".txt");
        lf.encoding = "UTF-8";
        lf.open("w"); lf.write(header + log.join("\n")); lf.close();
    } catch (e8) {}
    main.openInViewer();
    alert(header + (manual ? "Open BUILD_LOG for the MANUAL list." : "Done."));
})();
