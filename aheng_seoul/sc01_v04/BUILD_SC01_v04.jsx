/*  AHENG SEOUL | SC01 v04 | After Effects builder
    ------------------------------------------------------------------
    Direction: "one small light becomes the world"
      0-3s   a single warm point of light breathes (heartbeat) in the dark, centre frame
      2.4-4s the point blooms into the baby window, a warm halo and ring form around it
      4-6s   the window glides to the right third, sparks spread outward, copy enters left
      8-11s  a path of light is drawn along the bottom, small stars light up along it
      whole  camera starts close and pulls back (the world widens), then eases in slowly
    Lights are native AE shape layers with stacked-blur glows (no Saber parameter guessing).
    Nothing ever passes over the baby face (stencil / silhouette cutters).
    Run: File > Scripts > Run Script File... in a NEW, EMPTY project.
*/
(function () {
    var VERSION = "SC01 v04.0";
    var W = 1920, H = 1080, FPS = 30, DUR = 12;
    var ZOOM = 2666.7;                 // 50mm-equivalent zoom for a 1920 comp
    var CX = W / 2, CY = H / 2;
    var R = 380;                       // baby window radius
    var MAIN_NAME = "SC01_v04_MAIN";

    // decode Korean stored as hex code points (keeps this file pure ASCII)
    function K(h) { var a = h.split(","), s = "", i; for (i = 0; i < a.length; i++) { s += String.fromCharCode(parseInt(a[i], 16)); } return s; }

    var log = [];
    function L(s) { log.push(s); }

    // ---------- helpers ----------
    function col(r, g, b) { return [r, g, b, 1]; }
    var WARM = col(1.0, 0.80, 0.56), WARM_WHITE = col(1.0, 0.93, 0.82), COOL = col(0.55, 0.72, 1.0);

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

    // the AE scripting enum is spelled SILHOUETE_ALPHA (one T); accept both spellings
    var BM_SILHOUETTE = (BlendingMode.SILHOUETE_ALPHA !== undefined) ? BlendingMode.SILHOUETE_ALPHA : BlendingMode.SILHOUETTE_ALPHA;
    var BM_STENCIL = BlendingMode.STENCIL_ALPHA;

    // effect-free solid whose mask cuts the layers below it in the same comp
    function alphaCutter(comp, name, cx, cy, rx, ry, feather, mode) {
        var c = comp.layers.addSolid([1, 1, 1], name, comp.width, comp.height, 1, comp.duration);
        addMask(c, ellipseShape(cx, cy, rx, ry), feather);
        try {
            if (mode === undefined) { throw new Error("blend mode undefined"); }
            c.blendingMode = mode;
        } catch (e) {
            c.enabled = false;
            L("MANUAL  " + name + ": set Mode to Stencil/Silhouette Alpha by hand, then turn the layer on (" + e + ")");
        }
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

    // --- native shape-layer lights ---
    // one shape layer = one group holding a path (ellipse or free path) + stroke or fill (+ optional trim)
    function shapeLight(comp, name, opt) {
        var s = comp.layers.addShape();
        s.name = name;
        var grp = s.property("ADBE Root Vectors Group").addProperty("ADBE Vector Group");
        grp.name = "LIGHT";
        var v = grp.property("ADBE Vectors Group");
        if (opt.path) {
            var pg = v.addProperty("ADBE Vector Shape - Group");
            pg.property("ADBE Vector Shape").setValue(opt.path);
        } else {
            var el = v.addProperty("ADBE Vector Shape - Ellipse");
            el.property("ADBE Vector Ellipse Size").setValue(opt.size);
        }
        if (opt.stroke) {
            var st = v.addProperty("ADBE Vector Graphic - Stroke");
            st.property("ADBE Vector Stroke Color").setValue(opt.color);
            st.property("ADBE Vector Stroke Width").setValue(opt.stroke);
            try { st.property("ADBE Vector Stroke Line Cap").setValue(2); } catch (e0) {}
        } else {
            var fl = v.addProperty("ADBE Vector Graphic - Fill");
            fl.property("ADBE Vector Fill Color").setValue(opt.color);
        }
        if (opt.trimEnd || opt.trimStart || opt.trimOffset) {
            var tr = v.addProperty("ADBE Vector Filter - Trim");
            if (opt.trimStart) { tr.property("ADBE Vector Trim Start").expression = opt.trimStart; }
            if (opt.trimEnd) { tr.property("ADBE Vector Trim End").expression = opt.trimEnd; }
            if (opt.trimOffset) { tr.property("ADBE Vector Trim Offset").expression = opt.trimOffset; }
        }
        s.blendingMode = BlendingMode.ADD;
        s.motionBlur = true;
        return s;
    }

    // stacked-blur glow: duplicates of the core layer with Gaussian Blur, added on top.
    // levels = [[blur, opacity%], ...]; opacity is scaled by CTRL "Light %".
    function glowStack(core, levels, ctrlRef) {
        var i, d, gb, out = [core];
        for (i = 0; i < levels.length; i++) {
            d = core.duplicate();
            d.name = core.name + "_GLOW" + (i + 1);
            gb = addFx(d, "ADBE Gaussian Blur 2");
            if (gb) { gb.property(1).setValue(levels[i][0]); }
            // follow the core's opacity (value or expression), scaled per level and by CTRL "Light %"
            d.transform.opacity.expression =
                'thisComp.layer("' + core.name + '").transform.opacity*' +
                levels[i][1] + '/100*' + ctrlRef + '("Light %")(1)/100;';
            d.moveBefore(core);
            out.push(d);
        }
        return out;
    }

    function ease3(t) { return 't<0.5?4*' + t + '*' + t + '*' + t + ':1-Math.pow(-2*' + t + '+2,3)/2'; }

    // CTRL reference used by expressions inside precomps / main
    var CT = 'comp("' + MAIN_NAME + '").layer("CTRL").effect';
    var TC = 'thisComp.layer("CTRL").effect';

    app.beginUndoGroup("AHENG " + VERSION);
    var proj = app.project;
    if (!proj) { proj = app.newProject(); }

    try { proj.bitsPerChannel = 16; L("OK      project 16bpc"); } catch (e1) { L("MANUAL  set 16bpc: " + e1); }
    try { proj.linearBlending = false; L("OK      linear blending off"); } catch (e2) { L("MANUAL  linear blending off: " + e2); }

    var fRoot = proj.items.addFolder("AHENG_SC01_v04");
    var fSrc = proj.items.addFolder("01_REPLACE_SOURCE"); fSrc.parentFolder = fRoot;
    var fPre = proj.items.addFolder("02_PRECOMPS"); fPre.parentFolder = fRoot;

    var main = proj.items.addComp(MAIN_NAME, W, H, 1, DUR, FPS);
    main.parentFolder = fRoot;
    main.motionBlur = true;
    try { main.shutterAngle = 180; main.shutterPhase = -90; } catch (e3) {}
    main.bgColor = [0, 0, 0];

    // ---------- CTRL ----------
    var ctrl = main.layers.addNull(DUR);
    ctrl.name = "CTRL";
    ctrl.enabled = false;
    // timing (narration: line1 ~1.3s, line2 ~4.6s, line3 ~8.2s; move these after the guide VO)
    slider(ctrl, "Point Start (s)", 1.2);       // line 1
    slider(ctrl, "Bloom Start (s)", 2.4);       // point -> window
    slider(ctrl, "Layout Start (s)", 4.0);      // window glides right
    slider(ctrl, "Text Start (s)", 4.8);        // line 2
    slider(ctrl, "Path Start (s)", 8.2);        // line 3
    // layout
    slider(ctrl, "Hero X End", 1330);
    slider(ctrl, "Hero Y End", 500);
    // camera
    slider(ctrl, "Camera Pull (px)", 700);
    slider(ctrl, "Camera Push End (px)", 90);
    slider(ctrl, "Camera Drift (px)", 5);
    slider(ctrl, "DOF Aperture", 25);
    // baby plate
    slider(ctrl, "Baby Speed %", 100);
    slider(ctrl, "Baby Start (s)", 0);
    slider(ctrl, "Baby Scale %", 100);
    slider(ctrl, "Baby X Offset", 0);
    slider(ctrl, "Baby Y Offset", 0);
    // look masters
    slider(ctrl, "Light %", 100);
    slider(ctrl, "Halo %", 70);
    slider(ctrl, "Sparks %", 70);
    slider(ctrl, "Nebula %", 100);
    slider(ctrl, "Bokeh %", 18);
    slider(ctrl, "Bloom %", 40);
    L("OK      CTRL sliders");

    // ---------- SRC_BABY_PLATE ----------
    var src = proj.items.addComp(("SRC_BABY_PLATE  <- " + K("c2e4,c81c") + " " + K("cd2c,c601,bcf8,c73c,b85c") + " " + K("ad50,ccb4")), W, H, 1, 14, FPS);
    src.parentFolder = fSrc;
    var ph = solid(src, "PLACEHOLDER_BG", [0.2, 0.18, 0.17]);
    var phRamp = addFx(ph, "ADBE Ramp");
    if (phRamp) {
        phRamp.property(1).setValue([CX - 60, CY - 80]);
        phRamp.property(2).setValue(col(0.46, 0.40, 0.37));
        phRamp.property(3).setValue([CX + 480, CY + 300]);
        phRamp.property(4).setValue(col(0.08, 0.07, 0.07));
        phRamp.property(5).setValue(2);
    }
    ph.transform.scale.expression = 'var s=linear(time,0,thisComp.duration,100,108);[s,s];';
    var phT = src.layers.addText(("REPLACE\r" + K("c544,ae30") + " " + K("d074,b85c,c988,c5c5") + " " + K("c2e4,c0ac") + " " + K("b7") + " 4K " + K("b7") + " 14" + K("cd08") + " " + K("c774,c0c1") + "\r" + K("c5bc,ad74,c744") + " " + K("d654,ba74") + " " + K("c911,c559,c5d0")));
    var phDoc = phT.property("ADBE Text Properties").property("ADBE Text Document").value;
    phDoc.fontSize = 26; phDoc.fillColor = [1, 1, 1]; phDoc.applyFill = true; phDoc.applyStroke = false;
    phDoc.justification = ParagraphJustification.CENTER_JUSTIFY;
    phT.property("ADBE Text Properties").property("ADBE Text Document").setValue(phDoc);
    phT.transform.position.setValue([CX, CY - 20]);
    phT.transform.opacity.setValue(60);

    // ---------- PRE_BABY_WINDOW (circle at comp centre) ----------
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
    } catch (e4) { L("MANUAL  baby time remap: " + e4); }
    baby.transform.position.expression =
        '[' + CX + '+' + CT + '("Baby X Offset")(1),' + CY + '+' + CT + '("Baby Y Offset")(1)];';
    baby.transform.scale.expression =
        'var s=' + CT + '("Baby Scale %")(1)*linear(time,0,thisComp.duration,1,1.04);[s,s];';
    // subtle warm grade lift in the centre (inside the circle only, see stencil)
    var core = solid(win, "WARM_CORE", [0, 0, 0]);
    core.blendingMode = BlendingMode.ADD;
    var coreRamp = addFx(core, "ADBE Ramp");
    if (coreRamp) {
        coreRamp.property(1).setValue([CX - 40, CY - 70]);
        coreRamp.property(2).setValue(col(0.55, 0.40, 0.26));
        coreRamp.property(3).setValue([CX + R, CY + R]);
        coreRamp.property(4).setValue(col(0, 0, 0));
        coreRamp.property(5).setValue(2);
    }
    core.transform.opacity.setValue(22);
    // iris: the window opens out of the point of light
    var iris = alphaCutter(win, "IRIS_STENCIL", CX, CY, R, R, 70, BM_STENCIL);
    iris.property("ADBE Mask Parade").property(1).property("ADBE Mask Offset").expression =
        'var st=' + CT + '("Bloom Start (s)")(1);\n' +
        'var t=clamp((time-st)/1.6,0,1);t=1-Math.pow(1-t,3);\n' +
        'linear(t,0,1,-' + (R + 70) + ',0);';
    L("OK      PRE_BABY_WINDOW (iris opens from the centre)");

    // ---------- PRE_BG_NEBULA ----------
    var neb = proj.items.addComp("PRE_BG_NEBULA", W, H, 1, DUR, FPS);
    neb.parentFolder = fPre;
    var nbase = solid(neb, "BASE_GRADIENT", [0, 0, 0]);
    var nr = addFx(nbase, "ADBE Ramp");
    if (nr) {
        nr.property(1).setValue([1150, 520]);
        nr.property(2).setValue(col(0.08, 0.14, 0.32));
        nr.property(3).setValue([1150 + 1500, 820]);
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
        if (tn) { tn.property(1).setValue(col(0, 0, 0)); tn.property(2).setValue(tintWhite); }
        l.blendingMode = mode;
        l.transform.opacity.expression = opacity + '*' + CT + '("Nebula %")(1)/100;';
        return l;
    }
    nebulaLayer(neb, "NEBULA_COOL", col(0.12, 0.24, 0.55), 45, BlendingMode.SCREEN, 900, 10, 115, -45);
    var warmC = proj.items.addComp("PRE_WARM_SPILL", W, H, 1, DUR, FPS);
    warmC.parentFolder = fPre;
    nebulaLayer(warmC, "NEBULA_WARM", col(0.9, 0.62, 0.38), 100, BlendingMode.NORMAL, 500, 16, 120, -50);
    alphaCutter(warmC, "STENCIL_AROUND_HERO", 1250, 520, 950, 650, 500, BM_STENCIL);
    var warmL = neb.layers.add(warmC);
    warmL.blendingMode = BlendingMode.ADD;
    warmL.transform.opacity.expression = '12*' + CT + '("Nebula %")(1)/100;';
    L("OK      PRE_BG_NEBULA");

    // ---------- PRE_BG_STARS ----------
    var stars = proj.items.addComp("PRE_BG_STARS", W, H, 1, DUR, FPS);
    stars.parentFolder = fPre;
    function starLayer(comp, name, c, grid, size, speedExpr, opacity) {
        var l = solid(comp, name, c);
        var sb = addFx(l, "CC Star Burst");
        if (sb) {
            setP(sb, ["Scatter"], 1, 300, name + " StarBurst Scatter=300");
            setP(sb, ["Speed"], 2, speedExpr, name + " StarBurst Speed", true);
            setP(sb, ["Grid Spacing"], 4, grid, name + " StarBurst Grid=" + grid);
            setP(sb, ["Size"], 5, size, name + " StarBurst Size=" + size);
        }
        l.blendingMode = BlendingMode.ADD;
        l.transform.opacity.setValue(opacity);
        return l;
    }
    starLayer(stars, "STARS_FINE", [0.78, 0.86, 1.0], 9, 22, "0.02", 70).transform.opacity.expression = 'value*(0.8+0.2*noise(time*1.5));';
    starLayer(stars, "STARS_BRIGHT", [1.0, 0.95, 0.86], 34, 55, "0.03", 90).transform.opacity.expression = 'value*(0.8+0.2*noise(time*1.5+7));';
    L("OK      PRE_BG_STARS");

    // ---------- PRE_SPARKS (light spreading out of the window: (K("c138,c0c1,c744") + " " + K("bc14,afc9,b2c8,b2e4"))) ----------
    var spk = proj.items.addComp("PRE_SPARKS", W, H, 1, DUR, FPS);
    spk.parentFolder = fPre;
    var sparks = starLayer(spk, "SPARKS", [1.0, 0.86, 0.66], 26, 40,
        'var st=' + CT + '("Layout Start (s)")(1);\n' +
        'var a=clamp((time-st+0.2)/0.5,0,1), b=clamp((time-st-0.6)/2.2,0,1);\n' +
        '0.02+0.7*a*(1-b)*(1-b);', 100);
    alphaCutter(spk, "HOLE_OVER_BABY", CX, CY, R + 20, R + 20, 80, BM_SILHOUETTE);
    L("OK      PRE_SPARKS (burst from the window, face kept clear)");

    // ---------- PRE_FG_BOKEH ----------
    var bok = proj.items.addComp("PRE_FG_BOKEH", W, H, 1, DUR, FPS);
    bok.parentFolder = fPre;
    var bl = solid(bok, "BOKEH_BLOBS", [0, 0, 0]);
    var bfn = addFx(bl, "ADBE Fractal Noise");
    if (bfn) {
        setP(bfn, ["Contrast", (K("b300,be44"))], 4, 420, "BOKEH Fractal Contrast=420");
        setP(bfn, ["Brightness", (K("bc1d,ae30"))], 5, -160, "BOKEH Fractal Brightness=-160");
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
    bl.transform.opacity.expression = CT + '("Bokeh %")(1);';
    // the hole follows the hero (approx. mapping through the z -700 plane scale of 1.15)
    var bh = alphaCutter(bok, "HOLE_OVER_BABY", CX, CY, R + 160, R + 160, 280, BM_SILHOUETTE);
    bh.transform.position.expression =
        'var p=comp("' + MAIN_NAME + '").layer("HERO_RIG").transform.position;\n' +
        '[' + CX + '+(p[0]-' + CX + ')/1.15,' + CY + '+(p[1]-' + CY + ')/1.15];';
    L("OK      PRE_FG_BOKEH");

    // ---------- MAIN: rig + 3D stage ----------
    var rig = main.layers.addNull(DUR);
    rig.name = "HERO_RIG";
    rig.threeDLayer = true;
    rig.enabled = false;
    rig.transform.position.setValue([CX, CY, 0]);
    rig.transform.position.expression =
        'var st=' + TC + '("Layout Start (s)")(1);\n' +
        'var t=clamp((time-st)/1.7,0,1);t=' + ease3('t') + ';\n' +
        '[linear(t,0,1,' + CX + ',' + TC + '("Hero X End")(1)),linear(t,0,1,' + CY + ',' + TC + '("Hero Y End")(1)),0];';

    function stage(item, name, z, mode, extraScale) {
        var l = main.layers.add(item);
        l.name = name;
        l.threeDLayer = true;
        l.transform.position.setValue([CX, CY, z]);
        var s = (ZOOM + z) / ZOOM * 100 * (extraScale || 1.15);
        l.transform.scale.setValue([s, s, 100]);
        if (mode !== undefined) { l.blendingMode = mode; }
        l.motionBlur = true;
        return l;
    }
    function onRig(layer, z) {
        layer.threeDLayer = true;
        layer.parent = rig;
        layer.transform.position.setValue([0, 0, z]);
        return layer;
    }

    stage(neb, "BG_NEBULA", 3000);
    stage(stars, "BG_STARS", 1400, BlendingMode.ADD);

    // warm halo behind the window (the approved v02 (K("b530,b73b,d55c") + " " + K("d6c4,ad11")))
    var halo = shapeLight(main, "HALO", { size: [2 * R + 40, 2 * R + 40], color: col(1.0, 0.66, 0.38) });
    onRig(halo, 30);
    halo.transform.opacity.expression =
        'var st=' + TC + '("Bloom Start (s)")(1);\n' +
        TC + '("Halo %")(1)*linear(time,st+0.3,st+2.2,0,1)*(0.92+0.08*Math.sin(time*1.3));';
    var hb = addFx(halo, "ADBE Gaussian Blur 2");
    if (hb) { hb.property(1).setValue(110); }

    var winL = main.layers.add(win);
    winL.name = "BABY_WINDOW";
    winL.motionBlur = true;
    onRig(winL, 0);

    // ring: drawn in the same beat as the iris
    var ring = shapeLight(main, "RING", {
        size: [2 * R + 8, 2 * R + 8], stroke: 2.2, color: WARM_WHITE,
        trimEnd: 'var st=' + TC + '("Bloom Start (s)")(1);var t=clamp((time-st)/1.8,0,1);100*(1-Math.pow(1-t,3));',
        trimOffset: '-90'
    });
    onRig(ring, -2);
    glowStack(ring, [[6, 80], [22, 55], [70, 35]], TC);

    // orbit arcs (tech accent, thin, cool).
    // Flat ellipses on a plane BEHIND the window: the opaque window hides the part that would cross the face,
    // so the arcs read as rings passing behind the baby. Centred slightly right so they stay clear of the copy.
    function orbit(name, radius, flat, rotZ, spin, arc, delay, z) {
        var o = shapeLight(main, name, {
            size: [2 * radius, 2 * radius * flat], stroke: 1.4, color: COOL,
            trimStart: '0',
            trimEnd: 'var st=' + TC + '("Bloom Start (s)")(1)+' + delay + ';' + arc + '*clamp((time-st)/2.2,0,1);',
            trimOffset: 'time*' + spin
        });
        onRig(o, z);
        o.transform.position.setValue([ORBIT_SHIFT, 0, z]);
        o.property("ADBE Transform Group").property("ADBE Rotate Z").setValue(rotZ);
        o.transform.opacity.setValue(60);
        glowStack(o, [[8, 60]], TC);
        return o;
    }
    var ORBIT_SHIFT = 90;
    orbit("ORBIT_A", R + 120, 0.30, -12, 14, 45, 0.6, 40);
    orbit("ORBIT_B", R + 230, 0.22, 8, -9, 30, 1.1, 60);

    // sparks precomp rides on the rig so the burst radiates from the window
    var spkL = main.layers.add(spk);
    spkL.name = "SPARKS";
    spkL.blendingMode = BlendingMode.ADD;
    spkL.motionBlur = true;
    onRig(spkL, -10);
    spkL.transform.opacity.expression =
        'var st=' + TC + '("Layout Start (s)")(1);\n' +
        TC + '("Sparks %")(1)*linear(time,st-0.2,st+0.4,0,1)*linear(time,st+2.2,st+3.6,1,0);';

    // the first small light: breathes twice (heartbeat) then becomes the window
    var point = shapeLight(main, "POINT_LIGHT", { size: [14, 14], color: WARM_WHITE });
    onRig(point, -4);
    point.transform.scale.expression =
        'var st=' + TC + '("Point Start (s)")(1), bs=' + TC + '("Bloom Start (s)")(1);\n' +
        'var t=time-st;\n' +
        'var beat=function(c){var x=(t-c)/0.18;return Math.exp(-x*x);};\n' +
        'var s=100*clamp(t/0.5,0,1)*(1+0.35*beat(0.55)+0.25*beat(0.8)+0.35*beat(1.35)+0.25*beat(1.6));\n' +
        's=s*linear(time,bs,bs+0.9,1,6);[s,s,100];';
    point.transform.opacity.expression =
        'var bs=' + TC + '("Bloom Start (s)")(1);linear(time,bs+0.4,bs+1.2,100,0);';
    glowStack(point, [[10, 90], [40, 65], [130, 40]], TC);
    // anamorphic streak through the point
    var flare = shapeLight(main, "POINT_FLARE", { size: [520, 3], color: WARM });
    onRig(flare, -5);
    var fb = addFx(flare, "ADBE Gaussian Blur 2");
    if (fb) { fb.property(1).setValue(5); }
    flare.transform.opacity.expression =
        'var st=' + TC + '("Point Start (s)")(1), bs=' + TC + '("Bloom Start (s)")(1);\n' +
        '45*' + TC + '("Light %")(1)/100*linear(time,st,st+0.6,0,1)*linear(time,bs+0.2,bs+1.0,1,0);';
    flare.transform.scale.expression =
        'var bs=' + TC + '("Bloom Start (s)")(1);var s=linear(time,bs,bs+1.0,100,260);[s,100,100];';

    // ---------- path of light + stars along it ((K("c624,b298,c758") + " " + K("c791,c740") + " " + K("be5b,c774") + ", " + K("c6b0,b9ac,c758") + " " + K("b0b4,c77c,c744") + " " + K("bc1d,d799,b2c8,b2e4"))) ----------
    var pathPts = [[-120, 985], [760, 915], [2060, 880]];
    var pIn = [[0, 0], [-360, 22], [-460, 8]];
    var pOut = [[360, -26], [460, -12], [0, 0]];
    var ps = new Shape();
    ps.vertices = pathPts; ps.inTangents = pIn; ps.outTangents = pOut; ps.closed = false;
    var pathL = shapeLight(main, "PATH_OF_LIGHT", {
        path: ps, stroke: 1.6, color: WARM,
        trimEnd: 'var st=' + TC + '("Path Start (s)")(1);var t=clamp((time-st)/2.8,0,1);100*(1-Math.pow(1-t,3));'
    });
    // path vertices are written in comp coordinates: put the layer origin at the comp origin
    pathL.threeDLayer = true;
    pathL.transform.position.setValue([0, 0, 10]);
    glowStack(pathL, [[5, 80], [24, 50]], TC);
    // stars light up as the head of the path passes (x-based approximation of the trim head)
    function bez(p0, c0, c1, p1, t) {
        var u = 1 - t;
        return [u * u * u * p0[0] + 3 * u * u * t * c0[0] + 3 * u * t * t * c1[0] + t * t * t * p1[0],
                u * u * u * p0[1] + 3 * u * u * t * c0[1] + 3 * u * t * t * c1[1] + t * t * t * p1[1]];
    }
    var k, seg, tt, pt, dot, frac;
    var samples = [0.18, 0.34, 0.5, 0.66, 0.8, 0.92];
    for (k = 0; k < samples.length; k++) {
        frac = samples[k];
        seg = frac < 0.4 ? 0 : 1;
        tt = seg === 0 ? frac / 0.4 : (frac - 0.4) / 0.6;
        pt = bez(pathPts[seg], [pathPts[seg][0] + pOut[seg][0], pathPts[seg][1] + pOut[seg][1]],
                 [pathPts[seg + 1][0] + pIn[seg + 1][0], pathPts[seg + 1][1] + pIn[seg + 1][1]], pathPts[seg + 1], tt);
        dot = shapeLight(main, "PATH_STAR_" + (k + 1), { size: [5, 5], color: WARM_WHITE });
        dot.threeDLayer = true;
        // shape contents sit at the layer origin, so the layer position is the star position
        dot.transform.position.setValue([pt[0], pt[1], 8]);
        dot.transform.opacity.expression =
            'var st=' + TC + '("Path Start (s)")(1);\n' +
            'var hit=st+2.8*(1-Math.pow(1-' + frac + ',1/3));\n' +
            'var a=clamp((time-hit)/0.25,0,1);100*a*(0.75+0.25*Math.sin(time*3+' + k + '));';
        dot.transform.scale.expression =
            'var st=' + TC + '("Path Start (s)")(1);var hit=st+2.8*(1-Math.pow(1-' + frac + ',1/3));\n' +
            'var x=(time-hit)/0.2;var s=100*(1+1.2*Math.exp(-x*x));[s,s,100];';
        glowStack(dot, [[8, 90], [30, 55]], TC);
    }
    L("OK      lights: point + flare + halo + ring + 2 orbits + sparks + path with 6 stars (native shapes)");

    var bokL = stage(bok, "FG_BOKEH", -700, BlendingMode.ADD);

    // ---------- typography ----------
    var fontKR = pickFont(["NotoSerifKR-Light", "NanumMyeongjo", "AppleMyungjo"]);
    var fontKRB = pickFont(["NotoSerifKR-SemiBold", "NanumMyeongjoBold", "AppleMyungjo"]);
    var fontEN = pickFont(["Montserrat-Light", "HelveticaNeue-Light"]);

    function textLayer(str, font, size, tracking, pos, fill, inOffset, revealDur, settle) {
        var t = main.layers.addText(str);
        var tp = t.property("ADBE Text Properties").property("ADBE Text Document");
        var d = tp.value;
        d.font = font; d.fontSize = size; d.tracking = tracking;
        d.fillColor = fill; d.applyFill = true; d.applyStroke = false;
        d.justification = ParagraphJustification.LEFT_JUSTIFY;
        tp.setValue(d);
        t.threeDLayer = true;
        t.motionBlur = true;
        t.transform.position.setValue([pos[0], pos[1], -120]);
        t.transform.position.expression = 'value+[0,0,-linear(time,inPoint,thisComp.duration,0,30)];';
        var anims = t.property("ADBE Text Properties").property("ADBE Text Animators");
        var a1 = anims.addProperty("ADBE Text Animator");
        a1.name = "REVEAL";
        var ap = a1.property("ADBE Text Animator Properties");
        ap.addProperty("ADBE Text Opacity").setValue(0);
        ap = a1.property("ADBE Text Animator Properties");
        ap.addProperty("ADBE Text Blur").setValue([12, 12]);
        ap = a1.property("ADBE Text Animator Properties");
        ap.addProperty("ADBE Text Position 3D").setValue([0, 18, 0]);
        var sel = a1.property("ADBE Text Selectors").addProperty("ADBE Text Selector");
        try { sel.property("ADBE Text Range Advanced").property("ADBE Text Range Shape").setValue(2); } catch (e5) { L("MANUAL  " + str + " range shape=Ramp Up"); }
        sel.property("ADBE Text Percent Offset").expression =
            'var st=' + TC + '("Text Start (s)")(1)+' + inOffset + ';\n' +
            'var t=clamp((time-st)/' + revealDur + ',0,1);t=1-Math.pow(1-t,3);\n' +
            'linear(t,0,1,-100,100);';
        var a2 = anims.addProperty("ADBE Text Animator");
        a2.name = "TRACK_SETTLE";
        a2.property("ADBE Text Animator Properties").addProperty("ADBE Text Tracking Amount").expression =
            'var st=' + TC + '("Text Start (s)")(1)+' + inOffset + ';\n' +
            'ease(time,st,st+2.4,' + settle + ',0);';
        t.transform.opacity.expression = 'linear(time,11.0,11.8,100,0);';
        return t;
    }
    var ivory = [0.96, 0.94, 0.9];
    textLayer((K("c791,c740") + " " + K("c2dc,c791,c774")), fontKR, 56, 10, [176, 486], ivory, 0, 1.2, 14);
    var t2 = textLayer((K("c138,c0c1,c744") + " " + K("bc14,afc9,b2c8,b2e4")), fontKRB, 90, -10, [170, 596], ivory, 0.5, 1.4, 14);
    var t3 = textLayer("A SMALL BEGINNING, A BRIGHTER TOMORROW", fontEN, 16, 360, [178, 664], [0.72, 0.8, 0.95], 1.5, 1.2, 0);
    t3.transform.opacity.expression = 'linear(time,11.0,11.8,65,0);';
    var ls = addFx(t2, "CC Light Sweep");
    if (ls) {
        setP(ls, ["Center"], 1,
            'var st=' + TC + '("Text Start (s)")(1)+1.8;var r=sourceRectAtTime(time,false);var t=ease(time,st,st+1.2,0,1);\n' +
            '[r.left+linear(t,0,1,-300,r.width+300),r.top+r.height/2];', "Light Sweep Center", true);
        setP(ls, ["Width"], 4, 60, "Light Sweep Width=60");
        setP(ls, ["Sweep Intensity"], 5, 45, "Light Sweep Intensity=45");
        setP(ls, ["Light Color"], 8, col(1.0, 0.86, 0.66), "Light Sweep Color");
    }
    L("OK      typography: left column, max width ~700px, clear of the window");

    // ---------- finishing ----------
    var bloom = solid(main, "FX_BLOOM", [0, 0, 0]);
    bloom.adjustmentLayer = true;
    var g = addFx(bloom, "ADBE Glo2", "BLOOM");
    if (g) {
        setP(g, ["Glow Threshold"], 2, 72, "BLOOM threshold=72%");
        setP(g, ["Glow Radius"], 3, 70, "BLOOM radius=70");
        setP(g, ["Glow Intensity"], 4, 0.6, "BLOOM intensity=0.6");
        L("VERIFY  FX_BLOOM threshold reads 72% (only the light cores should bloom)");
    }
    bloom.transform.opacity.expression = TC + '("Bloom %")(1);';

    var vig = solid(main, "FX_VIGNETTE", [0, 0, 0]);
    var vm = addMask(vig, ellipseShape(CX + 80, CY, W * 0.64, H * 0.68), 520);
    vm.inverted = true;
    vig.transform.opacity.setValue(55);

    var grain = solid(main, "FX_GRAIN", [0, 0, 0]);
    grain.adjustmentLayer = true;
    var nz = addFx(grain, "ADBE Noise");
    if (nz) {
        setP(nz, ["Amount of Noise"], 1, 2.5, "Grain amount=2.5%");
        setP(nz, ["Use Color Noise"], 2, 0, "Grain color noise off");
    }

    // ---------- camera: close on the point, pull back as the world widens, then ease in ----------
    var cam = main.layers.addCamera("CAM", [CX, CY]);
    cam.autoOrient = AutoOrientType.NO_AUTO_ORIENT;
    var co = cam.property("ADBE Camera Options Group");
    co.property("ADBE Camera Zoom").setValue(ZOOM);
    try {
        co.property("ADBE Camera Depth of Field").setValue(1);
        co.property("ADBE Camera Blur Level").setValue(100);
        co.property("ADBE Camera Aperture").expression = TC + '("DOF Aperture")(1);';
        co.property("ADBE Camera Focus Distance").expression = '-transform.position[2];';
    } catch (e6) { L("MANUAL  camera DOF: " + e6); }
    cam.transform.position.setValue([CX, CY, -ZOOM]);
    cam.transform.position.expression =
        'var pull=' + TC + '("Camera Pull (px)")(1), push=' + TC + '("Camera Push End (px)")(1);\n' +
        'var d=' + TC + '("Camera Drift (px)")(1);\n' +
        'var a=clamp(time/5.6,0,1);a=' + ease3('a') + ';\n' +
        'var b=clamp((time-5.6)/6.4,0,1);b=' + ease3('b') + ';\n' +
        'var w=wiggle(0.2,d);\n' +
        '[w[0],w[1],value[2]+pull*(1-a)+push*b];';
    L("OK      camera: close -> pull back to 5.6s -> slow push-in");

    // ---------- layer order ----------
    grain.moveToBeginning();
    bloom.moveAfter(grain);
    vig.moveAfter(bloom);
    rig.moveToBeginning();
    ctrl.moveToBeginning();
    cam.moveAfter(ctrl);

    app.endUndoGroup();

    // ---------- save + log ----------
    var here = File($.fileName).parent;
    try {
        if (!proj.file) {
            var out = new File(here.fsName + "/AHENG_SC01_v04_EDITABLE.aep");
            if (out.exists) { out = new File(here.fsName + "/AHENG_SC01_v04_EDITABLE_" + new Date().getTime() + ".aep"); }
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
        var lf = new File(here.fsName + "/BUILD_LOG_SC01_v04_" + new Date().getTime() + ".txt");
        lf.encoding = "UTF-8";
        lf.open("w"); lf.write(header + log.join("\n")); lf.close();
    } catch (e8) {}
    main.openInViewer();
    alert(header + (manual ? "Open BUILD_LOG for the MANUAL list." : "Done."));
})();
