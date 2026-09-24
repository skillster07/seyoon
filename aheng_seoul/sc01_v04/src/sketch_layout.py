"""Layout/timing sketch for SC01 v04 (NOT an After Effects render).
Re-implements the builder's camera, rig and timing maths in 2D to show composition per beat."""
import math, random
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H, ZOOM, CX, CY, R = 1920, 1080, 2666.7, 960, 540, 380
C = dict(point=1.2, bloom=2.4, layout=4.0, text=4.8, path=8.2, hx=1330, hy=500, pull=700, push=90)
FONT = "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"

def clamp(v, a=0.0, b=1.0): return max(a, min(b, v))
def e3(t): return 4*t*t*t if t < .5 else 1 - (-2*t + 2) ** 3 / 2
def out3(t): return 1 - (1 - t) ** 3

def cam_z(t):
    a = e3(clamp(t / 5.6)); b = e3(clamp((t - 5.6) / 6.4))
    return -ZOOM + C['pull'] * (1 - a) + C['push'] * b

def proj(x, y, z, t):
    k = ZOOM / (z - cam_z(t))
    return CX + (x - CX) * k, CY + (y - CY) * k, k

def rig(t):
    u = e3(clamp((t - C['layout']) / 1.7))
    return CX + (C['hx'] - CX) * u, CY + (C['hy'] - CY) * u

def glow(img, xy, r, color, alpha, blur):
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0)); d = ImageDraw.Draw(layer)
    x, y = xy; d.ellipse([x - r, y - r, x + r, y + r], fill=color + (int(255 * alpha),))
    if blur: layer = layer.filter(ImageFilter.GaussianBlur(blur))
    img.alpha_composite(layer)

def frame(t, seed=3):
    random.seed(seed)
    img = Image.new("RGBA", (W, H), (3, 5, 14, 255))
    # nebula base
    neb = Image.new("RGBA", (W, H), (0, 0, 0, 0)); nd = ImageDraw.Draw(neb)
    nx, ny, _ = proj(1150, 520, 3000, t)
    for i in range(14, 0, -1):
        rr = 110 * i; a = int(10 + 4 * (14 - i))
        nd.ellipse([nx - rr * 1.3, ny - rr, nx + rr * 1.3, ny + rr], fill=(18, 34, 78, a))
    img.alpha_composite(neb.filter(ImageFilter.GaussianBlur(60)))
    # stars (z 1400)
    d = ImageDraw.Draw(img)
    for _ in range(260):
        x, y = random.uniform(-200, W + 200), random.uniform(-200, H + 200)
        sx, sy, k = proj(x, y, 1400, t); s = random.choice([1, 1, 1, 2])
        d.ellipse([sx - s, sy - s, sx + s, sy + s], fill=(200, 215, 255, random.randint(90, 200)))
    hx, hy = rig(t)
    px, py, k = proj(hx, hy, 0, t)
    bl = C['bloom']
    # halo (z 30, behind window)
    hal = clamp((t - bl - 0.3) / 1.9) * 0.7
    if hal > 0:
        hx2, hy2, k2 = proj(hx, hy, 30, t)
        glow(img, (hx2, hy2), (R + 20) * k2, (255, 168, 97), 0.55 * hal, 70 * k2)
    # window (iris)
    ir = out3(clamp((t - bl) / 1.6)); rad = max(0, (R + 70) * ir - 70) * k
    if rad > 1:
        win = Image.new("RGBA", (W, H), (0, 0, 0, 0)); wd = ImageDraw.Draw(win)
        for i in range(30, 0, -1):
            rr = rad * i / 30; v = int(40 + 75 * (1 - i / 30))
            wd.ellipse([px - rr, py - rr, px + rr, py + rr], fill=(v + 10, v, v - 4, 255))
        mask = Image.new("L", (W, H), 0); ImageDraw.Draw(mask).ellipse([px - rad, py - rad, px + rad, py + rad], fill=255)
        mask = mask.filter(ImageFilter.GaussianBlur(18 * k))
        img.paste(win, (0, 0), mask)
        f = ImageFont.truetype(FONT, int(22 * k))
        ImageDraw.Draw(img).text((px, py), "실사 아기 클로즈업", font=f, fill=(255, 255, 255, 110), anchor="mm")
    # ring
    rt = out3(clamp((t - bl) / 1.8))
    if rt > 0:
        rr = (R + 4) * k; ring = Image.new("RGBA", (W, H), (0, 0, 0, 0)); rd = ImageDraw.Draw(ring)
        rd.arc([px - rr, py - rr, px + rr, py + rr], -90, -90 + 360 * rt, fill=(255, 237, 209, 255), width=max(2, int(2.2 * k)))
        img.alpha_composite(ring.filter(ImageFilter.GaussianBlur(10))); img.alpha_composite(ring.filter(ImageFilter.GaussianBlur(2))); img.alpha_composite(ring)
    # orbits
    for rad_o, tilt, spin, arc, dly in ((R + 120, 0.30, 14, 45, 0.6), (R + 230, 0.22, -9, 30, 1.1)):
        prog = clamp((t - bl - dly) / 2.2)
        if prog > 0:
            ro = rad_o * k; orb = Image.new("RGBA", (W, H), (0, 0, 0, 0)); ox = px + 90 * k
            st = (t * spin) % 360
            ImageDraw.Draw(orb).arc([ox - ro, py - ro * tilt, ox + ro, py + ro * tilt], st, st + 3.6 * arc * prog, fill=(140, 184, 255, 150), width=2)
            # behind the window: cut the part inside the circle
            cut = Image.new("L", (W, H), 255); ImageDraw.Draw(cut).ellipse([px - R * k, py - R * k, px + R * k, py + R * k], fill=0)
            orb.putalpha(Image.composite(orb.getchannel("A"), Image.new("L", (W, H), 0), cut))
            img.alpha_composite(orb.filter(ImageFilter.GaussianBlur(3))); img.alpha_composite(orb)
    # sparks
    sa = clamp((t - C['layout'] + 0.2) / 0.6) * clamp(1 - (t - C['layout'] - 2.2) / 1.4)
    if sa > 0:
        d = ImageDraw.Draw(img); random.seed(9)
        for _ in range(70):
            ang = random.uniform(0, 2 * math.pi); dist = (R + 40 + random.uniform(0, 520) * clamp((t - C['layout'] + 0.2) / 2.2)) * k
            x, y = px + math.cos(ang) * dist, py + math.sin(ang) * dist
            d.ellipse([x - 2, y - 2, x + 2, y + 2], fill=(255, 220, 170, int(200 * sa)))
    # point light
    pt = t - C['point']
    if pt > 0 and t < bl + 1.2:
        beat = lambda c: math.exp(-((pt - c) / 0.18) ** 2)
        s = clamp(pt / 0.5) * (1 + .35 * beat(.55) + .25 * beat(.8) + .35 * beat(1.35) + .25 * beat(1.6))
        s *= 1 + 5 * clamp((t - bl) / 0.9); op = 1 - clamp((t - bl - 0.4) / 0.8)
        fl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(fl).ellipse([px - 260 * k * (1 + 1.6 * clamp((t - bl) / 1.0)), py - 2, px + 260 * k * (1 + 1.6 * clamp((t - bl) / 1.0)), py + 2], fill=(255, 204, 143, int(115 * op)))
        img.alpha_composite(fl.filter(ImageFilter.GaussianBlur(4)))
        glow(img, (px, py), 7 * s * k, (255, 237, 209), 0.4 * op, 60)
        glow(img, (px, py), 7 * s * k, (255, 237, 209), 0.65 * op, 18)
        glow(img, (px, py), 7 * s * k, (255, 245, 230), op, 0)
    # path of light
    pp = out3(clamp((t - C['path']) / 2.8))
    if pp > 0:
        pts = []
        P = [(-120, 985), (760, 915), (2060, 880)]; O = [(360, -26), (460, -12)]; I = [(-360, 22), (-460, 8)]
        for sgi in range(2):
            p0, p1 = P[sgi], P[sgi + 1]; c0 = (p0[0] + O[sgi][0], p0[1] + O[sgi][1]); c1 = (p1[0] + I[sgi][0], p1[1] + I[sgi][1])
            for j in range(60):
                u = j / 59; v = 1 - u
                x = v**3*p0[0] + 3*v*v*u*c0[0] + 3*v*u*u*c1[0] + u**3*p1[0]; y = v**3*p0[1] + 3*v*v*u*c0[1] + 3*v*u*u*c1[1] + u**3*p1[1]
                pts.append(proj(x, y, 10, t)[:2])
        n = max(2, int(len(pts) * pp)); lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(lay).line(pts[:n], fill=(255, 204, 143, 230), width=2)
        img.alpha_composite(lay.filter(ImageFilter.GaussianBlur(8))); img.alpha_composite(lay)
        for fr in (0.18, 0.34, 0.5, 0.66, 0.8, 0.92):
            hit = C['path'] + 2.8 * (1 - (1 - fr) ** (1 / 3))
            if t > hit:
                x, y = pts[min(len(pts) - 1, int(fr * len(pts)))]
                glow(img, (x, y), 10, (255, 237, 209), 0.5, 10); glow(img, (x, y), 3, (255, 245, 230), 1, 0)
    # typography
    def txt(s, size, x, y, delay, dur, fill):
        ts = C['text'] + delay; p = out3(clamp((t - ts) / dur))
        if p <= 0 or t > 11.8: return
        f = ImageFont.truetype(FONT, int(size * 1.047))
        sx, sy, _ = proj(x, y, -120, t)
        n = max(1, int(round(len(s) * p)))
        ImageDraw.Draw(img).text((sx, sy), s[:n], font=f, fill=fill, anchor="ls")
    txt("작은 시작이", 56, 176, 486, 0, 1.2, (245, 240, 230, 255))
    txt("세상을 바꿉니다", 90, 170, 596, 0.5, 1.4, (245, 240, 230, 255))
    txt("A SMALL BEGINNING, A BRIGHTER TOMORROW", 16, 178, 664, 1.5, 1.2, (184, 204, 242, 170))
    # vignette
    vig = Image.new("L", (W, H), 0); ImageDraw.Draw(vig).ellipse([CX + 80 - W * .64, CY - H * .68, CX + 80 + W * .64, CY + H * .68], fill=255)
    vig = vig.filter(ImageFilter.GaussianBlur(200)); dark = Image.new("RGBA", (W, H), (0, 0, 0, 140))
    img.paste(dark, (0, 0), Image.eval(vig, lambda v: 255 - v))
    return img.convert("RGB")

if __name__ == "__main__":
    import sys
    out = sys.argv[1]
    beats = [(0.6, "0.6s 어둠"), (1.9, "1.9s 작은 빛 · 심장 박동 (내레이션 1)"), (3.2, "3.2s 빛이 창이 된다 · 후광·링"),
             (4.9, "4.9s 창이 오른쪽으로 · 빛 퍼짐 · 카피 시작 (내레이션 2)"), (6.8, "6.8s 카피 정착 · 카메라 풀백 끝"),
             (9.4, "9.4s 빛의 길 · 별이 켜짐 (내레이션 3)"), (11.2, "11.2s 여운 · SC02로")]
    tw, th = 640, 360; sheet = Image.new("RGB", (tw * 2 + 30, (th + 40) * 4 + 10), (14, 14, 16))
    f = ImageFont.truetype(FONT, 18); sd = ImageDraw.Draw(sheet)
    for i, (t, label) in enumerate(beats):
        fr = frame(t).resize((tw, th), Image.LANCZOS)
        x = 10 + (i % 2) * (tw + 10); y = 10 + (i // 2) * (th + 40)
        sheet.paste(fr, (x, y + 28)); sd.text((x, y + 4), label, font=f, fill=(230, 230, 230))
    sd.text((10 + tw + 10, 10 + 3 * (th + 40) + 40), "SC01 v04 레이아웃 스케치\nAE 렌더 아님 · 구도와 타이밍 확인용\n빌더와 같은 카메라·리그·타이밍 계산", font=f, fill=(170, 170, 175))
    sheet.save(out, quality=90)
    print("saved", out)
