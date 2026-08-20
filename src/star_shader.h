/*
 * fwm — a Wayland compositor
 * Copyright (C) 2026 Ilu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef FWM_STAR_SHADER_H
#define FWM_STAR_SHADER_H

/* The star, as a fragment shader.
 *
 * Kept in its own header, as a string, for one reason: it is the only part of
 * fwm that cannot be looked at without a GPU and a compositor session. Having
 * it here lets an offscreen EGL harness compile the exact same source and
 * write out a PNG, so what ships is what somebody actually looked at.
 *
 * Why a shader at all, when cairo already drew a star: cairo is a vector
 * rasteriser on the CPU. It has gradients and fills, and no noise, no HDR and
 * no cheap blur — so its ceiling is a tidy illustration, which is exactly what
 * it produced. Everything that makes a photographed star look photographed is
 * per-pixel: turbulence on the surface, light that blooms because it is
 * brighter than the display can show, a corona with structure at every scale.
 * Those are the things a fragment shader does for free and a rasteriser cannot
 * do at all.
 *
 * GLSL ES 1.0 (no textureLod, no dynamic loop bounds, mediump by default) —
 * whatever wlroots' GLES2 renderer will take. */

static const char star_frag_src[] =
    "precision highp float;\n"
    "uniform vec2  u_res;\n"      /* canvas size, px */
    "uniform float u_time;\n"     /* seconds */
    "uniform float u_radius;\n"   /* star radius, px */
    "uniform vec3  u_color;\n"    /* surface colour */
    "uniform float u_lum;\n"      /* 1 = an ordinary main-sequence day */
    "uniform float u_phase;\n"    /* 0 burning, 1 collapsing, 2 pulsar */
    "uniform float u_beam;\n"     /* pulsar beam bearing, radians */
    /* The desktop behind the hole — wallpaper, windows and all — photographed
     * this frame. Present only for a hole; u_has_bg is 0 otherwise, and 0 also
     * when the snapshot could not be taken, in which case the sky below stands
     * in for it. */
    "uniform sampler2D u_bg;\n"
    "uniform float u_has_bg;\n"
    /* Which way round it is. A spin is only visible if there is something on
     * the surface to carry round with it, which is exactly what the turbulence
     * provides — so the whole of "it turns" is sampling the surface in a
     * rotated frame. */
    "uniform float u_angle;\n"
    /* How far the disc is tilted out of edge-on, 0..1. Not a constant because
     * the orrery lets you fly around the ring: the disc has to open and close
     * with the camera, or the hole is the one thing in the scene that ignores
     * where you are standing. */
    "uniform float u_incl;\n"
    /* And which way round the disc lies. u_incl only ever said how SQUASHED it
     * is, which pinned its long axis horizontal for ever: the disc could be
     * opened and closed but never turned. This is the angle its plane is
     * rolled through, so it can stand on edge, lie flat, or anything between. */
    "uniform float u_roll;\n"
    /* The supernova, 0..1: how far the blown-off envelope has got. */
    "uniform float u_blast;\n"
    /* Ignition, 0..1: a cloud falling together and lighting. 1 = an ordinary
     * star, and the whole of this costs nothing then. */
    "uniform float u_birth;\n"
    /* How head-on the pulsar's beam is this instant, 0..1: the pulse. */
    "uniform float u_aim;\n"
    "\n"
    /* ---- value noise. Cheap, and enough: what sells a star's surface is the
       SPECTRUM (structure at every scale), not the quality of one octave. --- */
    "float hash(vec2 p) {\n"
    /* Wrapped before hashing. The fine octaves of a domain-warped fbm reach
       coordinates in the hundreds, and multiplying those by 443 overruns what
       a float can resolve — the hash then quantises into blocks and the star
       grows rectangular patches along its limb, which is precisely the tell.
       Wrapping costs a repeat every 256 units, far outside anything on
       screen. */
    "    p = mod(p, 256.0);\n"
    "    p = fract(p * vec2(443.897, 441.423));\n"
    "    p += dot(p, p.yx + 19.19);\n"
    "    return fract((p.x + p.y) * p.x);\n"
    "}\n"
    "float noise(vec2 p) {\n"
    "    vec2 i = floor(p), f = fract(p);\n"
    "    f = f * f * (3.0 - 2.0 * f);\n"
    "    float a = hash(i), b = hash(i + vec2(1.0, 0.0));\n"
    "    float c = hash(i + vec2(0.0, 1.0)), d = hash(i + vec2(1.0, 1.0));\n"
    "    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
    "}\n"
    "float fbm(vec2 p) {\n"
    "    float v = 0.0, amp = 0.5;\n"
    "    for (int i = 0; i < 5; i++) {\n"
    "        v += amp * noise(p);\n"
    "        p = p * 2.03 + vec2(17.3, 9.1);\n"
    "        amp *= 0.5;\n"
    "    }\n"
    "    return v;\n"
    "}\n"
    /* Domain warping: fbm whose input is itself fbm. This is the whole trick
       behind convective turbulence — plain fbm looks like cloud, warped fbm
       looks like something boiling, because the features get dragged along by
       a flow instead of sitting still. */
    "float turbulence(vec2 p, float t) {\n"
    /* The rates are what make it a star rather than a photograph of one. Slow
       enough and turbulence is indistinguishable from a still image — which is
       exactly how the first version looked. These are tuned so a cell visibly
       changes shape over a couple of seconds. */
    "    vec2 q = vec2(fbm(p + vec2(0.0, t * 0.45)),\n"
    "                  fbm(p + vec2(5.2, 1.3) - vec2(t * 0.38, 0.0)));\n"
    "    vec2 r = vec2(fbm(p + 3.0 * q + vec2(1.7, 9.2) + t * 0.30),\n"
    "                  fbm(p + 3.0 * q + vec2(8.3, 2.8) - t * 0.24));\n"
    "    return fbm(p + 3.5 * r);\n"
    "}\n"
    "\n"
    /* Blackbody-ish ramp from the star's own colour: the hotter the material,
       the whiter it goes, and the cooler it is the deeper into orange it
       falls. One curve, so the surface, the limb and the loops all agree. */
    "vec3 heat(float x) {\n"
    "    x = clamp(x, 0.0, 1.0);\n"
    /* Relative to the star's own colour, never absolute: written as fixed
       oranges this turned a blue-white pulsar's core golden, because the ramp
       was really a ramp for one kind of star. Cooler means darker and a little
       redder than whatever this star is. */
    "    vec3 cool = u_color * vec3(0.42, 0.26, 0.16);\n"
    "    vec3 warm = u_color * vec3(1.00, 0.82, 0.60);\n"
    /* Not white: the whitening belongs to the tone map at the end, where it
       happens because the value is above what the display can show. Baking it
       into the ramp instead is what turned the disc into a pearl. */
    "    vec3 hot  = mix(u_color, vec3(1.0), 0.35);\n"
    "    return x < 0.5 ? mix(cool, warm, x * 2.0) : mix(warm, hot, (x - 0.5) * 2.0);\n"
    "}\n"
    "\n"
    /* ---- the black hole -------------------------------------------------
     *
     * Screen-space, and unashamed about it: a real one is integrated along
     * bent geodesics, which is minutes a frame. What follows keeps the three
     * things that make the picture read as a black hole and approximates the
     * rest.
     *
     *   the shadow   — larger than the horizon, about 2.6 times its radius,
     *                  because light grazing it is swallowed too;
     *   the photon   — light that went round the back and came out anyway,
     *      ring        which is the bright hairline on the shadow's edge;
     *   the disc     — gas orbiting faster on the inside than the outside,
     *                  and brighter on the side coming towards you.
     *
     * The background is lensed rather than composited: the stars behind it are
     * drawn HERE and bent on the way in, so the ring of them stretched around
     * the shadow costs nothing and needs no snapshot of the desktop. */

    /* A procedural sky. Points, not blobs — a star is smaller than a pixel and
     * what you actually see is the airy disc of your optics. */
    "vec3 sky(vec2 dir) {\n"
    "    vec3 c = vec3(0.0);\n"
    "    for (int i = 0; i < 3; i++) {\n"
    "        float sc = 6.0 + float(i) * 9.0;\n"
    "        vec2 g = dir * sc;\n"
    "        vec2 id = floor(g), f = fract(g) - 0.5;\n"
    "        float h = hash(id + float(i) * 31.7);\n"
    "        if (h > 0.90) {\n"
    "            vec2 off = vec2(hash(id + 3.1), hash(id + 7.7)) - 0.5;\n"
    "            float d2 = length(f - off * 0.7);\n"
    "            float mag = (h - 0.90) / 0.10;\n"
    "            float point = exp(-d2 * d2 * 420.0) * mag;\n"
    "            vec3 tint = mix(vec3(0.75, 0.83, 1.0), vec3(1.0, 0.88, 0.72), hash(id + 11.3));\n"
    "            c += tint * point;\n"
    "        }\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "\n"
    /* The disc, sampled by where you are IN it: how far out, and how far round.
     * Both images of it — the near face and the one lensing wraps around the
     * shadow — are this same function fed different coordinates, which is the
     * only way they can plausibly be the same disc. */
    /* A supermassive hole's disc reaches far further out than the shadow is
       wide — in the Gargantua stills it runs clean off both sides of the frame
       — so this is much larger than the tidy ring a first guess produces. */
    "const float DISC_IN = 3.0;\n"
    "const float DISC_OUT = 11.0;\n"
    "vec3 disc_at(float rad, float ang, float t, float flip) {\n"
    "    if (rad < DISC_IN || rad > DISC_OUT) return vec3(0.0);\n"
    /*  Differential rotation, but SHEARED rather than spun.
    
        The gas orbits faster on the inside, and the honest way to say so is to
        advance the sampling angle by omega*t. Done at full strength that winds
        the inner disc through several turns while the outer barely moves, so
        two neighbouring radii sample the noise field at completely unrelated
        places and the texture tears into concentric rings — the "beams of
        light" look, which is a sampling artefact and not gas at all.
    
        So the shear is kept gentle and the angle is fed through cos/sin, which
        keeps neighbouring radii neighbouring and lets the noise itself supply
        the structure. */
    /*  Softened from r^-1.5 to r^-0.9. The exponent is what tears the texture
        into rings: at the true Kepler value the inner edge laps the outer one
        many times over and neighbouring radii sample unrelated noise. Dropping
        the SPEED instead, which is what the last attempt did, cured the rings
        by making the disc almost stationary. Flattening the exponent keeps the
        inside visibly faster than the outside — which is the whole point —
        while keeping neighbours neighbours. */
    "    float omega = 3.4 / pow(rad, 0.9);\n"
    "    float phi = ang + t * omega * flip;\n"
    "    vec2 sp = vec2(cos(phi), sin(phi)) * rad;\n"
    "\n"
    /*  Gas, in three sizes. A big soft field for where the disc is thick and
        where it is thin, a middle one dragged along the flow for the streaks,
        and a fine one for the grain — none of them alone reads as matter. */
    "    float body = turbulence(sp * 0.42, t * 0.9);\n"
    "    float flow = turbulence(sp * 1.15 + vec2(phi * 0.8, 0.0), t * 1.6);\n"
    "    float grain = turbulence(sp * 3.1, t * 2.4);\n"
    "    float dens = body * 0.55 + flow * 0.35 + grain * 0.18;\n"
    /*  Clumped, not smooth: raising it to a power thins the quiet regions and
        leaves the bright filaments standing, which is what hot gas looks like
        and what an evenly lit ring never does. */
    "    dens = clamp(pow(clamp(dens * 1.35, 0.0, 2.0), 1.7), 0.0, 2.2);\n"
    "\n"
    /*  How much disc there is at this radius: fades in off the innermost
        stable orbit and thins out towards the rim, with the falloff steeper
        than linear because there is simply less gas further out. */
    "    float edge = smoothstep(DISC_IN, DISC_IN * 1.22, rad) *\n"
    "                 pow((1.0 - smoothstep(DISC_OUT * 0.55, DISC_OUT, rad)), 1.4);\n"
    "\n"
    /*  Temperature: the inner disc is white-hot, the rim is a dull ember. A
        real one runs roughly as r^-3/4; the exact law matters less than the
        fact that the range is huge, which is what gives the picture its depth. */
    "    float temp = pow(clamp(DISC_IN / rad, 0.0, 1.0), 0.75);\n"
    "    vec3 col = mix(vec3(1.00, 0.28, 0.06),\n"
    "                   vec3(1.00, 0.72, 0.34), smoothstep(0.15, 0.55, temp));\n"
    "    col = mix(col, vec3(1.00, 0.96, 0.90), smoothstep(0.55, 0.95, temp));\n"
    "\n"
    /*  Relativistic beaming: the side coming towards you is brighter and
        bluer, the receding side dimmer and redder. The most recognisable thing
        about a photographed disc — one of even brightness reads as paint. */
    "    float beta = 0.46 / sqrt(rad / DISC_IN);\n"
    "    float toward = -sin(ang) * flip;\n"
    "    float boost = clamp(pow(max(0.0, 1.0 + beta * toward), 3.4), 0.0, 7.0);\n"
    "    col *= boost;\n"
    "    col = mix(col, col * vec3(0.88, 0.95, 1.18), clamp(toward, 0.0, 1.0) * 0.45);\n"
    "\n"
    /*  Brightness rides the density, and the disc is BRIGHT: a hair of ambient
        so the thin parts are still gas rather than holes, and a long way up
        from there. */
    "    return col * (0.12 + 2.6 * dens) * edge * temp;\n"
    "}\n"
    "\n"
    /* Only the half of the disc that is IN FRONT of the hole.
     *
     * The disc's plane passes through the centre, so half of it lies between
     * you and the shadow and half lies behind. Over the shadow only the near
     * half may be drawn — the far half is hidden by the hole itself. Drawing
     * all of it painted the entire disc across the shadow, which is what
     * "I can see the disc through it" was: not transparency, the wrong half.
     *
     * Which half is near is decided in the disc's own frame, so it follows the
     * roll: the near side is simply the one below the disc's mid-line from
     * where you are standing. */
    "float disc_front_mask(vec2 uv) {\n"
    "    float cr = cos(-u_roll), sr = sin(-u_roll);\n"
    "    vec2 p = vec2(uv.x * cr - uv.y * sr, uv.x * sr + uv.y * cr);\n"
    /*  Softened, so the join is not a knife edge across the shadow. */
    "    return smoothstep(-0.06, 0.06, p.y);\n"
    "}\n"
    "\n"
    "vec3 disc_near(vec2 uv, float t, float incl) {\n"
    /*  Into the disc's own frame first, then squash. Squashing in screen
        coordinates is what held the long axis horizontal. */
    "    float cr = cos(-u_roll), sr = sin(-u_roll);\n"
    "    vec2 p = vec2(uv.x * cr - uv.y * sr, uv.x * sr + uv.y * cr);\n"
    "    float y = p.y / max(incl, 0.06);\n"
    "    return disc_at(length(vec2(p.x, y)), atan(y, p.x), t, 1.0);\n"
    "}\n"
    "\n"
    /* The far face, wrapped around the shadow.
     *
     * Light leaving the underside of the disc behind the hole is bent right
     * over the top of it and arrives as a band hugging the photon ring. So
     * this maps the RADIUS ON SCREEN just outside the shadow onto the radius
     * in the disc — the far side is not somewhere else on screen, it is
     * smeared into a ring around the shadow. That is the arch, and it is the
     * one feature no unbent rendering can produce. */
    "vec3 disc_far(vec2 uv, float b, float shadow, float t, float incl) {\n"
    /*  The far image is CROWDED against the ring, not spread over the sky:
        light from the whole far half of the disc arrives in a narrow band just
        outside the shadow. Mapping it gently spread that band across the
        entire frame and turned the picture into concentric rings — a target,
        not a black hole. */
    "    float rad = DISC_IN + (b - shadow) * 4.5;\n"
    /*  In the disc's frame too, so the arch rides round with it instead of
        staying stubbornly above and below. */
    "    float cr = cos(-u_roll), sr = sin(-u_roll);\n"
    "    vec2 p = vec2(uv.x * cr - uv.y * sr, uv.x * sr + uv.y * cr);\n"
    "    float ang = atan(p.y, p.x);\n"
    /*  Compressed against the ring: the closer to the shadow, the more of the
        disc is squeezed into each pixel, so it brightens as it tightens. */
    "    float squeeze = 1.0 / (1.0 + (b - shadow) * 1.10);\n"
    /*  And it fades out well before the edge of the frame: past a couple of
        shadow radii there is no more far side left to see. */
    "    float band = (1.0 - smoothstep(shadow * 1.02, shadow * 1.9, b));\n"
    /*  WHERE round the shadow it shows, and how strongly.
    
        Seen edge-on, the far half of the disc is bent up over the shadow and
        down under it, and hands over to the near disc at the sides: an ARCH,
        not a hoop. Drawn as an even ring at every angle — which is what this
        did — it becomes a bright doughnut facing the viewer that never changes
        however the disc is tipped. That ring was the thing staring back, and
        no amount of tilting could have touched it.
    
        It also fades as the disc opens: looking down on a disc there is no
        "far half lifted over the top", there is only more disc. */
    "    float updown = pow(abs(sin(ang)), 0.7);\n"
    "    float arch = mix(0.10, 1.0, updown) * (1.0 - 0.85 * incl);\n"
    "    return disc_at(rad, ang, t, -1.0) * (0.75 + 1.6 * squeeze) * band * arch;\n"
    "}\n"
    "\n"
    /* A note on smoothstep: every use below runs LOW edge first.
     *
     * Written the other way round — smoothstep(hi, lo, x), to mean "fade out
     * as x grows" — it is undefined behaviour by the spec, and drivers differ
     * on it. Mesa happens to compute something sensible, which is why the
     * offscreen renders looked right while the same shader on another card
     * left the shadow of the black hole see-through. Anything that has to fall
     * off is written 1.0 - smoothstep(lo, hi, x). */
    /* The cloud a star condenses out of: wide, cold, ragged, and falling
     * inward. Fades out as the star it is becoming takes over. */
    "vec3 protostar(float d, float ang, float t, float birth) {\n"
    "    float fall = 1.0 - birth;\n"
    /*  It starts many radii out and comes down onto the star. */
    "    float shell = 1.0 + 9.0 * fall * fall;\n"
    "    float width = 0.5 + 3.5 * fall;\n"
    "    float band = exp(-pow(abs(d - shell) / width, 2.0));\n"
    /*  Ragged and turning: gas, and gas with angular momentum at that. */
    "    float lump = 0.35 + 1.15 * turbulence(vec2(ang * 1.8 + t * 0.25, d * 0.6), t * 0.6);\n"
    /*  Cold and dim at first, warming as it falls in. */
    "    vec3 cold = mix(vec3(0.35, 0.22, 0.45), u_color, birth * birth);\n"
    "    return cold * band * lump * (0.25 + 0.75 * birth) * 0.9;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 px = gl_FragCoord.xy - u_res * 0.5;\n"
    "    float r = u_radius;\n"
    "    float d = length(px) / max(r, 1.0);\n"   /* 1.0 = the limb */
    "    float ang = atan(px.y, px.x);\n"
    "    vec3 col = vec3(0.0);\n"
    "    float alpha = 0.0;\n"
    "\n"
    /* ---- the disc ------------------------------------------------------ */
    "    if (d < 1.0) {\n"
    "        float mu = sqrt(max(0.0, 1.0 - d * d));\n"  /* cos of view angle */
    /*     Sampling the sphere, not the screen: dividing by mu stretches the
           cells towards the limb exactly as foreshortening would, which is
           what stops the surface looking like a printed circle. */
    /*     Clamped well away from zero: at the very limb the divide blows the
           sampling frequency up and the noise turns into a bright wire around
           the edge of the disc. */
    "        float ca0 = cos(u_angle), sa0 = sin(u_angle);\n"
    "        vec2 rp = vec2(px.x * ca0 - px.y * sa0, px.x * sa0 + px.y * ca0);\n"
    "        vec2 sp = vec2(rp.x / r / max(mu, 0.42), rp.y / r / max(mu, 0.42));\n"
    "        float gran = turbulence(sp * 3.4, u_time * 1.1);\n"
    "        float fine = turbulence(sp * 11.0 - 4.0, u_time * 2.6);\n"
    "        float cells = mix(gran, fine, 0.35);\n"
    /*     Limb darkening, the classical linear law. */
    "        float limb = 1.0 - 0.62 * (1.0 - mu);\n"
    "        float t = clamp(cells * 1.35 + 0.12, 0.0, 1.0);\n"
    "        col = heat(t * limb + (1.0 - limb) * 0.12);\n"
    /*     Faculae: the bright network in the lanes between cells, which is
           what keeps a real surface from reading as noise. */
    "        col += heat(1.0) * pow(max(0.0, cells - 0.55), 2.0) * 2.4 * limb;\n"
    "        col *= mix(0.55, 1.35, limb);\n"
    "        alpha = 1.0;\n"
    /*     Antialiasing, measured in PIXELS. Written as a fraction of the
           radius it was 4.5% of the star wide: the outer twentieth of the disc
           came out semi-transparent, the wallpaper showed through it, and the
           result was a dark ring around a bright star. One pixel is all the
           edge needs. */
    "        float aa = 1.2 / max(r, 1.0);\n"
    "        float edge = (1.0 - smoothstep(1.0 - aa, 1.0 + aa, d));\n"
    /*     Only the coverage is feathered, never the colour: dimming the last
           ring of the disc as well left a grey hoop between the limb and the
           glow — the same fault the cairo version had, arrived at by a
           different road. */
    "        alpha *= edge;\n"
    "    }\n"
    "\n"
    /* ---- prominences: turbulence sampled in polar coordinates just off the
           limb, so what stands out is a tongue of gas rather than a shape
           somebody placed there. ------------------------------------------ */
    "    if (d >= 0.93 && d < 1.9) {\n"
    "        float h = (d - 1.0);\n"
    /*     The churn is the life: sampled fast enough, the turbulence itself
           brings tongues up and takes them away. An extra slow term on top of
           it was tried and made them swell into pink clouds instead. */
    "        float f = turbulence(vec2((ang + u_angle) * 2.6, d * 3.4 - u_time * 0.30), u_time * 1.4);\n"

    /*     Tight thresholds and a short reach: loosened, these stop being
           tongues of gas off the limb and become pink smoke across the whole
           canvas. A prominence is a local event. */
    "        float tongue = smoothstep(0.60, 0.80, f) * (1.0 - smoothstep(0.0, 0.34, h));\n"
    "        vec3 halpha = vec3(1.0, 0.28, 0.24);\n"
    "        col += halpha * tongue * 2.2;\n"
    "        alpha = max(alpha, tongue * 0.95);\n"
    "    }\n"
    "\n"
    /* ---- corona ------------------------------------------------------- */
    "    if (d >= 0.97) {\n"
    "        float streak = turbulence(vec2(ang * 2.3, log(d) * 2.2 - u_time * 0.12), u_time * 0.5);\n"
    "        float fall = 1.0 / (1.0 + pow((d - 0.97) * 4.2, 2.4));\n"
    "        float k = fall * (0.50 + 0.50 * streak);\n"
    "        col += u_color * k * 0.40;\n"
    "        alpha = max(alpha, clamp(k * 0.75, 0.0, 1.0));\n"
    "    }\n"
    "\n"
    /* Bloom: scattered light around a source too bright for the sensor, which
       is what a camera does with a star and what the eye does with the sun.
       Smooth, wide, and the colour of the light that made it. */
    "    {\n"
    "        float g = exp(-pow(max(0.0, d - 0.9) * 1.9, 1.3));\n"
    "        vec3 warm = mix(u_color, vec3(1.0), 0.25);\n"
    /*     Only outside the limb. Added over the disc as well it lifted the
           darkened edge back up and drew a bright wire right round it. */
    /*     Overlaps the limb rather than starting past it, so the disc hands
           over to the glow with nothing dark in between. */
    "        float outside = smoothstep(0.90, 1.03, d);\n"
    "        col += warm * g * outside * 0.75 * u_lum;\n"
    "        alpha = max(alpha, clamp(g * outside * 0.85, 0.0, 1.0));\n"
    "    }\n"
    "\n"
    /* ---- the pulsar's beams ------------------------------------------- */
    /* A hole is drawn by its surroundings and nothing else — no surface, no
     * corona, none of the above applies. */
    "    if (u_phase > 2.5) {\n"
    "        float rs = max(u_radius, 1.0);\n"
    "        vec2 uv = px / rs;\n"
    "        float b = length(uv);\n"
    "        float shadow = 2.6;\n"
    /*     How far the disc is tilted out of edge-on. Down to a hair: at zero
           it is exactly edge-on and infinitely thin, so the floor only keeps
           the divide below finite — it is not a refusal to lie flat. */
    "        float incl = clamp(u_incl, 0.012, 0.98);\n"
    "        vec3 acc = vec3(0.0);\n"
    "\n"
    /*     The background, bent. Deflection goes as 1/b, so the sky is sampled
           from further out than it appears: what was behind the hole is
           dragged around its edge into a ring. */
    /*     Deflection goes as 1/b — the Einstein result, 4GM/(c^2 b) — so the
           background is sampled from FURTHER OUT than it appears. Whatever was
           directly behind the hole is dragged out around its edge into a ring,
           and a straight window edge passing nearby bows.
    
           Two backgrounds: the desktop, when a snapshot of it was taken, and
           the procedural sky when it was not. The desktop is the interesting
           one — it is what makes this lensing rather than decoration. */
    /*     Faded out towards the edge of the canvas, together with the node's
           own alpha below. Where the node is only partly opaque the bent copy and
           the untouched original are both visible, and if the bending is still
           strong there they are visibly different pictures — the window
           appears twice, one straight and one curved. Bending that reaches
           zero exactly where the coverage does makes the two identical, so the
           seam cannot be seen. */
    "        float dmax0 = min(u_res.x, u_res.y) * 0.5 / rs;\n"
    "        float reach = (1.0 - smoothstep(dmax0 * 0.55, dmax0 * 0.98, b));\n"
    "        float defl = 7.5 / max(b, 0.5) * reach;\n"
    /*     And the twist.
    
           A spinning hole does not merely bend light, it drags spacetime
           round with it — frame dragging, the Lense-Thirring effect — so the
           picture behind it arrives ROTATED, and rotated more the closer it
           passed. That is the thing that makes the sky around Gargantua look
           stirred rather than magnified, and it is why a straight edge near
           one comes out curled instead of merely bowed.
    
           Falls off as 1/b^2, faster than the bending itself, so it is a
           local whirlpool at the ring and nothing at all further out. */
    "        float twist = 5.2 / max(b * b, 0.6) * reach;\n"
    "        float ca = cos(twist), sa = sin(twist);\n"
    "        vec2 spun = vec2(uv.x * ca - uv.y * sa, uv.x * sa + uv.y * ca);\n"
    "        vec2 dir = normalize(spun + vec2(1e-5)) * (b + defl);\n"
    "        float seen = smoothstep(shadow * 0.99, shadow * 1.06, b);\n"
    "        float lensed = 0.0;\n"
    "        if (u_has_bg > 0.5) {\n"
    /*         Back to buffer coordinates: the bent direction is in units of
               the Schwarzschild radius, centred on the hole. */
    "            vec2 bguv = (dir * rs + u_res * 0.5) / u_res;\n"
    "            vec2 clamped = clamp(bguv, vec2(0.002), vec2(0.998));\n"
    /*         Off the edge of the photograph there is nothing to bend, so the
               lensing fades out rather than smearing the border pixel. */
    "            float inside = step(length(bguv - clamped), 0.0001);\n"
    "            vec3 bg = texture2D(u_bg, clamped).rgb;\n"
    "            acc += bg * seen * mix(0.35, 1.0, inside);\n"
    /*         How much of this pixel is the bent picture. It has to be a
               REPLACEMENT, not an overlay: drawn semi-transparent, the lensed
               copy lay over the unbent original and you saw both at once — a
               film of curved windows floating over straight ones. Opaque
               everywhere the lens reaches, falling off only at the edge of the
               photograph and at the edge of the canvas. */
    "            lensed = seen * mix(0.0, 1.0, inside);\n"
    "        } else {\n"
    "            acc += sky(dir) * seen;\n"
    "        }\n"
    "\n"
    /*     The far side, lifted over the top and dropped under the bottom by
           the same bending — the arch that makes the picture unmistakable.
           It is the same disc seen at a steeper angle, mirrored, and it is
           masked out inside the shadow: light coming round the back appears
           AROUND the hole, never across it. Drawn before the near side, which
           passes in front of it. */
    "        float outside = smoothstep(shadow * 0.99, shadow * 1.04, b);\n"
    "        acc += disc_far(uv, b, shadow, u_time, incl) * outside;\n"
    "\n"
    /*     The photon ring: light that orbited and escaped. Thin, and brighter
           than anything else in the frame. */
    /*     A hairline, not a hoop. It is the brightest thing in the frame and
           it should be, but it is also THIN — light that orbited once and got
           out — and a wide white band around the shadow reads as a lamp rather
           than as a horizon. */
    "        float ring = exp(-pow(abs(b - shadow) * 26.0, 1.4));\n"
    "        acc += vec3(1.0, 0.95, 0.84) * ring * 5.5;\n"
    "\n"
    /*     And the near side, in front of the shadow. */
    "        acc += disc_near(uv, u_time + u_angle * 0.35, incl);\n"
    "\n"
    /*     Nothing escapes from inside, so whatever was accumulated there goes
           out — except the disc in FRONT of it, which is between you and the
           hole and is not falling in. */
    "        float inside = (1.0 - smoothstep(shadow * 0.96, shadow * 1.02, b));\n"
    "        vec3 front = disc_near(uv, u_time, incl) * disc_front_mask(uv);\n"
    "        acc = mix(acc, front, inside);\n"
    "\n"
    /*     The glow the whole thing sits in. A disc this hot lights the dust
           around it, and without that the object is a bright shape pasted on
           black instead of something enormous a long way off. Sampled from the
           disc itself so it follows the tilt and the beaming rather than being
           a symmetrical halo painted on afterwards. */
    "        float aura = exp(-pow(max(0.0, b - shadow) * 0.16, 1.25));\n"
    /*     Never inside the shadow. Nothing comes out of there — a hole with a
           warm glow in the middle of it is a hole with a light on. */
    "        acc += vec3(1.00, 0.72, 0.40) * aura * 0.30 *\n"
    "               (0.35 + 0.65 * clamp(incl, 0.0, 1.0)) * (1.0 - inside);\n"
    "\n"
    /*     Tone-mapped gently, so the bright half of the disc keeps climbing
           instead of clipping to a flat white slab. */
    "        acc = acc / (1.0 + acc * 0.26);\n"
    "        acc = pow(max(acc, vec3(0.0)), vec3(0.85));\n"
    "        float a = clamp(max(acc.r, max(acc.g, acc.b)) * 1.4, 0.0, 1.0);\n"
    /*     Wherever the desktop was re-drawn bent, this pixel IS the desktop
           and must cover what is underneath it completely. */
    "        a = max(a, lensed);\n"
    /*     The shadow itself is opaque black, not transparent: you must not see
           the wallpaper through a black hole. */
    /*     Belt and braces: inside the shadow the pixel is opaque, full stop.
           Derived from the arithmetic it was at the mercy of whatever the
           driver did with the terms feeding it, and a black hole you can see
           the desktop through is the one failure that ruins the whole thing.
           Nothing escapes from in there, so nothing needs computing. */
    "        if (b < shadow * 0.985) a = 1.0;\n"
    "        else a = max(a, inside);\n"
    "        float rim2 = (1.0 - smoothstep(dmax0 * 0.70, dmax0, b));\n"
    "        gl_FragColor = vec4(acc * a * rim2, a * rim2);\n"
    "        return;\n"
    "    }\n"
    "\n"
    "    if (u_phase > 1.5) {\n"
    "        float rel = ang - u_beam;\n"
    "        float lobe = max(abs(cos(rel)), 0.0);\n"
    "        float cone = pow(lobe, 220.0);\n"
    "        float reach = 1.0 / (1.0 + d * 0.05);\n"
    /*     The pulse itself. The beam is always there; what changes is whether
           it is pointed at you, and that is what turns a rotating spotlight
           into something that flashes. A floor, because the far lobe and the
           lit gas around the star never go entirely dark. */
    "        float pulse = 0.18 + 0.82 * clamp(u_aim, 0.0, 1.0);\n"
    "        cone *= pulse;\n"
    "        vec3 blue = mix(u_color, vec3(1.0), 0.6);\n"
    "        col += blue * cone * reach * 1.6;\n"
    "        alpha = max(alpha, clamp(cone * reach, 0.0, 1.0));\n"
    "    }\n"
    "\n"
    /* ---- brightness, then tone mapping.
           A star is far brighter than the display, so the picture is made in
           linear light and squeezed at the end. That squeeze is what produces
           a white core with colour surviving around it — the look a camera
           gives a bright source, and the one thing a rasteriser cannot fake
           because it has nowhere to put values above 1. -------------------- */
    /* The blast. A shell of the star's own outer layers, thrown off when the
       core rebounded, expanding and thinning as it goes. Ragged, because it is
       gas and not a soap bubble, and it outshines everything for a moment
       before fading through white to orange as it cools — which is the whole
       reason a supernova looks like an event rather than a light being turned
       up. */
    "    if (u_blast > 0.0) {\n"
    "        float far = 0.6 + 13.0 * u_blast * u_blast;\n"
    "        float thick = 0.35 + 2.4 * u_blast;\n"
    "        float ring = exp(-pow(abs(d - far) / thick, 2.0));\n"
    "        float rag = 0.55 + 0.75 * turbulence(vec2(ang * 2.2, d * 0.5), u_time * 0.7);\n"
    "        float fade = pow(1.0 - u_blast, 1.6);\n"
    "        vec3 hot = mix(vec3(1.0, 0.55, 0.18), vec3(1.0, 0.98, 0.95),\n"
    "                       clamp(1.0 - u_blast * 1.8, 0.0, 1.0));\n"
    "        col += hot * ring * rag * fade * 5.5;\n"
    /*     And the flash itself: for the first instant the whole frame is lit. */
    "        float flash = pow(clamp(1.0 - u_blast * 5.0, 0.0, 1.0), 2.0);\n"
    "        col += vec3(1.0, 0.96, 0.90) * flash * 3.0;\n"
    "        alpha = max(alpha, clamp((ring * rag * fade + flash), 0.0, 1.0));\n"
    "    }\n"
    "\n"
    /* Still condensing: the star fades UP out of the cloud that is falling
       onto it, rather than being switched on. */
    "    if (u_birth < 1.0) {\n"
    "        float grow = smoothstep(0.15, 1.0, u_birth);\n"
    "        col *= grow;\n"
    "        alpha *= grow;\n"
    "        vec3 cloud = protostar(d, ang, u_time, u_birth);\n"
    "        col += cloud;\n"
    "        alpha = max(alpha, clamp(max(cloud.r, max(cloud.g, cloud.b)) * 1.6, 0.0, 1.0));\n"
    "    }\n"
    "\n"
    "    col = max(col, vec3(0.0));\n"
    "    col *= u_lum;\n"
    "    col = col / (1.0 + col * 0.55);\n"       /* Reinhard, softened */
    "    col = pow(col, vec3(0.85));\n"
    /* Coverage at least as large as the brightest channel, or a premultiplied
       pixel carries more colour than it has alpha to justify — which showed as
       stray cyan specks along the prominences: dark wallpaper leaking through
       a pixel that was already bright. */
    "    alpha = clamp(max(alpha, max(col.r, max(col.g, col.b))), 0.0, 1.0);\n"
    "\n"
    /* Fade to nothing before the edge of the buffer, and do it LAST.
       The corona, the bloom and a pulsar's beam all still had alpha at the
       border, and alpha that stops rather than ends draws the border: a faint
       SQUARE around the star, the one shape guaranteed not to be
       astronomical. Doing this before the tone map does not work — pow with an
       exponent below 1 lifts small values, and the coverage is taken from the
       colour afterwards, so both climb straight back up. */
    "    float dmax = min(u_res.x, u_res.y) * 0.5 / max(u_radius, 1.0);\n"
    "    float rim = (1.0 - smoothstep(dmax * 0.70, dmax, d));\n"
    "    col *= rim;\n"
    "    alpha *= rim;\n"
    "    gl_FragColor = vec4(col * alpha, alpha);\n"  /* premultiplied */
    "}\n";

static const char star_vert_src[] =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

#endif /* FWM_STAR_SHADER_H */
