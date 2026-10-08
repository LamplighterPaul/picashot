#version 450
// Puts the filtered picture in the window and draws the few bits of interface on top:
// countdown, flash, recording timer, filter dots and the grid highlight.
layout(set = 0, binding = 0) uniform sampler2D img;
layout(push_constant) uniform PC {
    vec4 a;   // window w, h (pixels), picture aspect, time
    vec4 b;   // flash 0..1, countdown seconds left (<=0 off), recording seconds (<0 off), 1 if the surface is sRGB
    vec4 c;   // filter count, current filter, grid columns (0 = no grid), hovered tile (-1 none)
    vec4 d;   // button under the pointer (0 none, 1 photo, 2 video, 3 timer, 4 sound), 1 if sound is on, 1 if the timer is on
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;

float sd_seg(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));
}

// Distance to a seven-segment digit. p.x is in -0.5..0.5 and p.y in -1..1, y up.
float sd_digit(vec2 p, int d) {
    const int masks[10] = int[10](0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F);
    int m = masks[clamp(d, 0, 9)];
    float w = 0.40, h = 0.82, r = 1e9;
    if ((m & 1) != 0)  r = min(r, sd_seg(p, vec2(-w, h), vec2(w, h)));
    if ((m & 2) != 0)  r = min(r, sd_seg(p, vec2(w, h), vec2(w, 0)));
    if ((m & 4) != 0)  r = min(r, sd_seg(p, vec2(w, 0), vec2(w, -h)));
    if ((m & 8) != 0)  r = min(r, sd_seg(p, vec2(-w, -h), vec2(w, -h)));
    if ((m & 16) != 0) r = min(r, sd_seg(p, vec2(-w, 0), vec2(-w, -h)));
    if ((m & 32) != 0) r = min(r, sd_seg(p, vec2(-w, h), vec2(-w, 0)));
    if ((m & 64) != 0) r = min(r, sd_seg(p, vec2(-w, 0), vec2(w, 0)));
    return r;
}

// Blends a white shape with a soft dark outline; d is a distance in pixels.
vec3 ink(vec3 col, float d, vec3 fill, float alpha) {
    col = mix(col, vec3(0.0), alpha * 0.55 * (1.0 - smoothstep(0.0, 6.0, d)));
    return mix(col, fill, alpha * (1.0 - smoothstep(-0.75, 0.75, d)));
}

// True when p is within `reach` of the box centred on c with half-size h. Used to skip the distance
// maths for the great majority of pixels, which are nowhere near any control.
bool near(vec2 p, vec2 c, vec2 h, float reach) {
    vec2 d = abs(p - c) - h;
    return max(d.x, d.y) < reach;
}

// A small round toggle: ring, then the glyph distance `glyph` (already in pixels) inside it.
vec3 toggle(vec3 col, vec2 p, float r, float glyph, bool on, bool hot) {
    float a = on ? 1.0 : 0.45;
    col = ink(col, abs(length(p) - r) - r * 0.07, vec3(1.0), hot ? 1.0 : a * 0.8);
    col = ink(col, glyph, vec3(1.0), hot ? 1.0 : a);
    if (!on) col = ink(col, sd_seg(p, vec2(-r, r) * 0.62, vec2(r, -r) * 0.62) - r * 0.07, vec3(1.0), 0.9);   // struck through
    return col;
}

void main() {
    vec2 win = pc.a.xy;
    vec2 px = v_uv * win;
    float unit = min(win.x, win.y);
    float margin = 8.0;   // room for the soft outline around every shape

    // Fit the picture inside the window, keeping its shape.
    float wa = win.x / win.y, ia = pc.a.z;
    vec2 scale = wa > ia ? vec2(ia / wa, 1.0) : vec2(1.0, wa / ia);
    vec2 iuv = (v_uv - 0.5) / scale + 0.5;
    bool inside = iuv.x >= 0.0 && iuv.x <= 1.0 && iuv.y >= 0.0 && iuv.y <= 1.0;
    vec3 col = inside ? texture(img, iuv).rgb : vec3(0.035);
    vec2 pic_px = win * scale;   // size of the picture on screen

    float n = pc.c.x;
    int cols = int(pc.c.z);
    if (cols > 0 && inside) {
        // Grid of every filter: thin gaps, an outline on the hovered and the current tile.
        float rows = ceil(n / float(cols));
        vec2 g = iuv * vec2(float(cols), rows);
        vec2 cell = floor(g);
        vec2 f = fract(g);
        float idx = cell.y * float(cols) + cell.x;
        vec2 tile_px = pic_px / vec2(float(cols), rows);
        vec2 e = min(f, 1.0 - f) * tile_px;      // distance to the tile edge in pixels
        float edge = min(e.x, e.y);
        if (idx >= n) col = vec3(0.035);
        col = mix(col, vec3(0.035), 1.0 - smoothstep(1.0, 2.5, edge));
        float border = max(3.0, unit * 0.006);
        if (idx == pc.c.y) col = mix(col, vec3(1.0), (1.0 - smoothstep(border, border + 1.5, edge)) * 0.55);
        if (idx == pc.c.w) col = mix(col, vec3(1.0), 1.0 - smoothstep(border, border + 1.5, edge));
    }

    if (cols == 0 && px.y > win.y - unit * 0.30) {
        // The controls sit on a soft shadow that fades in towards the bottom edge, so they read on any picture.
        float shade = smoothstep(win.y - unit * 0.30, win.y, px.y);
        col = mix(col, vec3(0.0), shade * shade * 0.6);

        // Along the bottom, centred: timer toggle, photo button, video button, sound toggle.
        // Keep the numbers in step with button_at() in main.c.
        float radius = unit * 0.046, small = unit * 0.024;
        vec2 mid = vec2(win.x * 0.5, win.y - unit * 0.095);
        vec2 photo = mid - vec2(unit * 0.075, 0.0), video = mid + vec2(unit * 0.075, 0.0);
        vec2 timer = mid - vec2(unit * 0.185, 0.0), sound = mid + vec2(unit * 0.185, 0.0);
        vec3 red = vec3(0.93, 0.13, 0.17);
        bool recording = pc.b.z >= 0.0;
        float hot = pc.d.x;

        if (near(px, photo, vec2(radius * 1.1), margin)) {
            float r = radius * (hot == 1.0 ? 1.08 : 1.0);
            float d = length(px - photo);
            col = ink(col, abs(d - r) - r * 0.065, vec3(1.0), 1.0);
            col = ink(col, d - r * (pc.b.y > 0.0 ? 0.62 : 0.78), vec3(1.0), hot == 1.0 ? 1.0 : 0.92);
        }
        if (near(px, video, vec2(radius * 1.1), margin)) {
            float r = radius * (hot == 2.0 ? 1.08 : 1.0);
            vec2 q = px - video;
            col = ink(col, abs(length(q) - r) - r * 0.065, vec3(1.0), 1.0);
            // A red disc to start; a red rounded square to stop.
            vec2 box = abs(q) - vec2(r * 0.30);
            float stop = length(max(box, 0.0)) + min(max(box.x, box.y), 0.0) - r * 0.10;
            col = ink(col, recording ? stop : length(q) - r * 0.78, red, 1.0);
        }
        if (near(px, timer, vec2(small * 1.15), margin)) {
            // The countdown length as a digit.
            float r = small * (hot == 3.0 ? 1.1 : 1.0);
            vec2 q = (px - timer) / (r * 0.5);
            float glyph = (sd_digit(vec2(q.x, -q.y), 3) - 0.16) * r * 0.5;
            col = toggle(col, px - timer, r, glyph, pc.d.z > 0.5, hot == 3.0);
        }
        if (near(px, sound, vec2(small * 1.15), margin)) {
            // A microphone: capsule, cradle, stem and foot.
            float r = small * (hot == 4.0 ? 1.1 : 1.0);
            vec2 q = px - sound;
            float capsule = sd_seg(q, vec2(0.0, -r * 0.42), vec2(0.0, r * 0.02)) - r * 0.20;
            float cradle = max(abs(length(q - vec2(0.0, r * 0.02)) - r * 0.40) - r * 0.055, r * 0.02 - q.y);
            float stem = sd_seg(q, vec2(0.0, r * 0.42), vec2(0.0, r * 0.62)) - r * 0.055;
            float foot = sd_seg(q, vec2(-r * 0.22, r * 0.62), vec2(r * 0.22, r * 0.62)) - r * 0.055;
            col = toggle(col, q, r, min(min(capsule, cradle), min(stem, foot)), pc.d.y > 0.5, hot == 4.0);
        }

        // One dot per filter above the buttons; the current one is larger and solid.
        float gap = unit * 0.028;
        vec2 origin = vec2(win.x * 0.5 - gap * (n - 1.0) * 0.5, win.y - unit * 0.172);
        if (n > 1.0 && near(px, vec2(win.x * 0.5, origin.y), vec2(gap * n * 0.5, unit * 0.0075), margin)) {
            float i = clamp(floor((px.x - origin.x) / gap + 0.5), 0.0, n - 1.0);
            float current = i == pc.c.y ? 1.0 : 0.0;
            float d = length(px - vec2(origin.x + i * gap, origin.y)) - unit * mix(0.0045, 0.0075, current);
            col = ink(col, d, vec3(1.0), mix(0.55, 1.0, current));
        }
    }

    float rec = pc.b.z;
    float rh = unit * 0.022;                         // half the digit height
    vec2 ro = vec2(unit * 0.05, unit * 0.055);
    if (rec >= 0.0 && near(px, ro + vec2(rh * 3.6, 0.0), vec2(rh * 4.6, rh * 1.2), margin)) {
        // Blinking red dot and a minutes:seconds timer in the top-left corner.
        float blink = 0.35 + 0.65 * step(0.5, fract(rec));
        col = ink(col, length(px - ro) - rh * 0.75, vec3(0.93, 0.13, 0.17), blink);
        int secs = int(rec);
        int digits[3] = int[3]((secs / 60) % 10, (secs % 60) / 10, secs % 10);
        float x = ro.x + rh * 2.4;
        for (int k = 0; k < 3; k++) {
            vec2 p = (px - vec2(x, ro.y)) / rh;
            col = ink(col, (sd_digit(vec2(p.x, -p.y), digits[k]) - 0.13) * rh, vec3(1.0), 1.0);
            x += rh * 1.5;
            if (k == 0) {   // the colon
                float dc = min(length(px - vec2(x - rh * 0.35, ro.y - rh * 0.4)), length(px - vec2(x - rh * 0.35, ro.y + rh * 0.4))) - rh * 0.14;
                col = ink(col, dc, vec3(1.0), 1.0);
                x += rh * 0.8;
            }
        }
    }

    float cd = pc.b.y;
    if (cd > 0.0 && near(px, win * 0.5, vec2(unit * 0.12, unit * 0.22), margin)) {
        // Big digit in the middle that shrinks a little through each second.
        float phase = fract(cd);                      // 1 at the start of a second, 0 at its end
        float h = unit * (0.15 + 0.05 * phase * phase);
        vec2 p = (px - win * 0.5) / h;
        float d = (sd_digit(vec2(p.x, -p.y), int(ceil(cd))) - 0.12) * h;
        col = ink(col, d, vec3(1.0), smoothstep(0.0, 0.15, phase) * 0.95);
    }

    col = mix(col, vec3(1.0), clamp(pc.b.x, 0.0, 1.0));
    if (pc.b.w > 0.5) col = pow(col, vec3(2.2));     // the surface will re-encode to sRGB
    o_col = vec4(col, 1.0);
}
