/*  AHENG SEOUL | SC01 v03 | After Effects skeleton builder
    ------------------------------------------------------------------
    What this script builds (structure only, look is tuned by eye in AE):
      - 16bpc + linear blending project settings
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
    var VERSION = "SC01 v03.0";
    var W = 1920, H = 1080, FPS = 30, DUR = 12;
    var ZOOM = 2666.7;                 // 50mm-equivalent zoom for a 1920 comp
    var HERO = [1340, 470];            // circle centre in MAIN (from storyboard cut 01)
    var R = 420;                       // baby window radius
    var MAIN_NAME = "SC01_v03_MAIN";

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
    try { proj.linearBlending = true; L("OK      blend colors using 1.0 gamma (linear blending)"); } catch (e2) { L("MANUAL  linear blending: " + e2); }

    // ---------- folders ----------
    var fRoot = proj.items.addFolder("AHENG_SC01_v03");
    var fSrc = proj.items.addFolder("01_REPLACE_SOURCE"); fSrc.parentFolder = fRoot;
    var fPre = proj.items.addFolder("02_PRECOMPS"); fPre.parentFolder = fRoot;
    var fSol = proj.items.addFolder("03_SOLIDS"); fSol.parentFolder = fRoot;

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
    slider(ctrl, "Warm Core Opacity", 30);
    slider(ctrl, "Text Start (s)", 4.6);
    L("OK      CTRL sliders");

    // ---------- SRC_BABY_PLATE (replace slot) ----------
    var src = proj.items.addComp("SRC_BABY_PLATE  <- 실제 촬영본으로 교체", W, H, 1, 14, FPS);
    src.parentFolder = fSrc;
    var ph = solid(src, "PLACEHOLDER_BG", [0.2, 0.16, 0.14]);
    var phRamp = addFx(ph, "ADBE Ramp");
    if (phRamp) {
        phRamp.property(1).setValue([W / 2, H / 2]);
        phRamp.property(2).setValue(col(0.95, 0.8, 0.68));
        phRamp.property(3).setValue([W / 2 + 520, H / 2]);
        phRamp.property(4).setValue(col(0.18, 0.13, 0.12));
        phRamp.property(5).setValue(2);
    }
    // slow drift so the placeholder is never a still frame
    ph.transform.scale.expression = 'var s=linear(time,0,thisComp.duration,100,108);[s,s];';
    var phT = src.layers.addText("REPLACE: 아기 클로즈업 실사 | 4K 권장 | 14초 이상 | 얼굴을 화면 중앙에");
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
    addMask(core, ellipseShape(HERO[0], HERO[1], R, R), 120);
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

    function nebulaLayer(name, tintWhite, opacity, mode, scale, evo, contrast, brightness) {
        var l = solid(neb, name, [0, 0, 0]);
        var fn = addFx(l, "ADBE Fractal Noise");
        if (fn) {
            setP(fn, ["Contrast", "대비"], 4, contrast, name + " Fractal Contrast=" + contrast);
            setP(fn, ["Brightness", "밝기"], 5, brightness, name + " Fractal Brightness=" + brightness);
            setP(fn, ["Complexity", "복잡도"], 8, 7, name + " Fractal Complexity=7");
            setP(fn, ["Uniform Scaling", "균일 비율"], null, 1, name + " Uniform Scaling on");
            setP(fn, ["Scale", "비율"], null, scale, name + " Fractal Scale=" + scale);
            setP(fn, ["Evolution", "진화"], 10, "time*" + evo, name + " Evolution=time*" + evo, true);
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
    nebulaLayer("NEBULA_COOL", col(0.28, 0.46, 0.95), 55, BlendingMode.SCREEN, 900, 10, 150, -25);
    var warm = nebulaLayer("NEBULA_WARM_SPILL", col(1.0, 0.72, 0.45), 22, BlendingMode.ADD, 500, 16, 180, -35);
    addMask(warm, ellipseShape(HERO[0], HERO[1], 900, 650), 500);
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
        setP(bfn, ["Contrast", "대비"], 4, 420, "BOKEH Fractal Contrast=420");
        setP(bfn, ["Brightness", "밝기"], 5, -115, "BOKEH Fractal Brightness=-115");
        setP(bfn, ["Complexity", "복잡도"], 8, 1, "BOKEH Fractal Complexity=1");
        setP(bfn, ["Uniform Scaling", "균일 비율"], null, 1, "BOKEH Uniform Scaling on");
        setP(bfn, ["Scale", "비율"], null, 520, "BOKEH Fractal Scale=520");
        setP(bfn, ["Evolution", "진화"], 10, "time*8", "BOKEH Evolution", true);
    }
    var bbl = addFx(bl, "ADBE Box Blur2");
    if (bbl) { bbl.property(1).setValue(28); bbl.property(2).setValue(3); }
    var btn = addFx(bl, "ADBE Tint");
    if (btn) { btn.property(1).setValue(col(0, 0, 0)); btn.property(2).setValue(col(0.85, 0.8, 1.0)); }
    bl.blendingMode = BlendingMode.ADD;
    bl.transform.opacity.setValue(35);
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
    saberLayer("LIGHT_RING_HERO", R + 6, 0, 0, 0, col(1.0, 0.82, 0.6), 22, 1.2, drawOn, null, 0);
    saberLayer("ORBIT_A", R + 230, 40, 72, -14, col(0.45, 0.65, 1.0), 9, 0.6,
        'var st=' + rs + '("Reveal Start (s)")(1);linear(time,st+0.8,st+3.6,0,62);', null, 7);
    saberLayer("ORBIT_B", R + 420, -60, 78, 10, col(0.55, 0.72, 1.0), 6, 0.45,
        'var st=' + rs + '("Reveal Start (s)")(1);linear(time,st+1.4,st+4.4,0,38);', null, -4);
    L("OK      Saber ring + 2 orbits (verify Core Type / Composite in Saber UI)");

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
    var t1 = textLayer("작은 시작이", fontKR, 66, 20, [190, 505], -160, ivory, 0, 1.4);
    var t2 = textLayer("세상을 바꿉니다", fontKRB, 104, 0, [184, 640], -160, ivory, 0.55, 1.6);
    var t3 = textLayer("A SMALL BEGINNING, A BRIGHTER TOMORROW", fontEN, 19, 420, [194, 712], -160, [0.72, 0.8, 0.95], 1.8, 1.4);
    t3.transform.opacity.expression = 'linear(time,10.9,11.7,70,0);';

    // light sweep across the hero line
    var ls = addFx(t2, "CC Light Sweep");
    if (ls) {
        setP(ls, ["Center"], 1,
            'var r=sourceRectAtTime(time,false);var t=ease(time,7.6,9.2,0,1);\n' +
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
    glow("GLOW_TIGHT", 70, 12, 0.6);
    glow("GLOW_MID", 62, 60, 0.45);
    glow("GLOW_WIDE", 55, 260, 0.3);

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
