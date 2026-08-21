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
    /* Derivatives, for one job: knowing how much of the desktop behind a hole
     * is squeezed into this pixel. Optional by the letter of GLSL ES 1.0, so
     * it is asked for rather than required, and the code below falls back to a
     * single sample where it is missing. */
    "#extension GL_OES_standard_derivatives : enable\n"
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
    /* Where this canvas sits inside that background, u0,v0,du,dv. Identity for
     * a photograph taken of the canvas itself; a sub-rectangle when the
     * background is a whole screen the star is standing somewhere on, which is
     * the orrery. */
    "uniform vec4 u_bg_rect;\n"
    /* How hard this thing bends what is behind it: 1 at a horizon, about a
     * third of that for a pulsar, and small enough to be invisible for
     * anything with a surface. See star_compactness — the one number that
     * decides how much lensing there is, so that a hole and a neutron star can
     * share the arithmetic instead of one of them being a special case. */
    "uniform float u_lens;\n"
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
    /* How far a hole's disc has formed, 0..1. A hole arrives with nothing
     * around it and the gas comes down onto it; see star_disc_form. 1 for
     * everything that is not a hole, and for a hole that has settled. */
    "uniform float u_form;\n"
    "\n"
    /* ---- red and blue, and the one place they are put back ------------
     *
     * The buffer this shader draws into is ARGB8888, which on a little-endian
     * machine is the bytes B, G, R, A in that order — the format the scene
     * graph, cairo and every other surface in fwm speak. GL knows nothing
     * about that: it puts the first component of gl_FragColor in the first
     * byte. So a shader that writes what it calls red has written the byte the
     * scene reads as blue, and everything painted from a constant in here came
     * out the other way round: an ordinary yellow star was displayed a cold
     * blue, an ember's orange a flat cyan, and a black hole's warm ring the
     * pale blue-white it has always been.
     *
     * It went unnoticed for as long as it did because the one thing that
     * LOOKED right is the one thing that passes straight through — the
     * photograph of the desktop a compact star bends. That is a buffer in the
     * same format, so its blue arrived in the shader's red, went back out
     * through red, and landed in blue again: two errors, cancelling exactly,
     * and the only part of the picture anyone could check against something.
     *
     * Both are undone in one place each: the background is put right where it
     * is sampled, so everything in between is honest RGB, and the whole frame
     * is swapped back on the way out. Nothing else in the shader has to know,
     * and tools/star-render.c does the same on the way to its PNG. */
    "vec4 bg_texel(vec2 uv) {\n"
    "    vec4 t = texture2D(u_bg, uv);\n"
    "    return vec4(t.b, t.g, t.r, t.a);\n"
    "}\n"
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
    /* The clock comes in as seconds since the machine booted, which is a
       number in the tens or hundreds of thousands. A float carries seven
       digits: at t = 20000 the fraction is quantised to about two
       thousandths, and fract() — which is the whole of how noise interpolates
       between lattice points — starts returning steps instead of a ramp. The
       result is not a subtly worse star, it is sand: the disc of a black hole
       fills with crawling speckle, and only on a machine that has been up for
       a few hours, which is why it never showed up in a render.
    
       Wrapping the time offsets fixes it exactly rather than approximately.
       hash() already wraps its input every 256 units, so the whole noise field
       is periodic with that period — and an offset of (x mod 256) therefore
       lands on precisely the same field as an offset of x, with none of the
       precision thrown away. */
    "float wrap_t(float t, float rate) { return mod(t * rate, 256.0); }\n"
    "float turbulence(vec2 p, float t) {\n"
    /* The rates are what make it a star rather than a photograph of one. Slow
       enough and turbulence is indistinguishable from a still image — which is
       exactly how the first version looked. These are tuned so a cell visibly
       changes shape over a couple of seconds. */
    "    vec2 q = vec2(fbm(p + vec2(0.0, wrap_t(t, 0.45))),\n"
    "                  fbm(p + vec2(5.2, 1.3) - vec2(wrap_t(t, 0.38), 0.0)));\n"
    "    vec2 r = vec2(fbm(p + 3.0 * q + vec2(1.7, 9.2) + wrap_t(t, 0.30)),\n"
    "                  fbm(p + 3.0 * q + vec2(8.3, 2.8) - wrap_t(t, 0.24)));\n"
    "    return fbm(p + 3.5 * r);\n"
    "}\n"
    "\n"
    /* ---- angles, and the seam they draw ---------------------------------
     *
     * atan jumps from +pi to -pi across the negative x axis, so any noise
     * field fed the angle as a coordinate has a discontinuity there: a
     * straight line from the middle of the object out to its left edge, with
     * the two sides of it sampling unrelated places in the field. That is a
     * seam through every effect built this way — the supernova shell, the
     * cloud a star condenses out of, the prominences, the corona — and it is
     * the one part of the picture that is unmistakably drawn rather than lit.
     *
     * The cure is to stop using the angle as a coordinate and use a point on a
     * circle instead: the field is then periodic in the angle by construction
     * and there is no line to draw. The radius rides on the circle's own
     * radius, so moving outward still moves through the field. (The hole's
     * disc was given this when its seam was found; everything else kept the
     * old form until now.) */
    "vec2 ring_uv(float ang, float freq, float radial) {\n"
    "    return vec2(cos(ang), sin(ang)) * (freq + radial);\n"
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
    /* ---- the disc, and the light that leaves it ------------------------
     *
     * Everything below works in Schwarzschild units with the horizon at r = 1,
     * which is what u_radius is: the shader's whole coordinate system is the
     * hole's own radius, so no conversion is needed anywhere. In those units
     * the photon sphere is at 1.5, the shadow an unlit observer sees is
     * 3*sqrt(3)/2 = 2.598 across, and the innermost stable orbit — the inner
     * edge of any disc — is at 3. Those three numbers are not chosen, they
     * fall out of the metric, and having them fall out is the difference
     * between a picture of a black hole and a black hole. */
    "const float R_ISCO = 3.0;\n"
    /* The hottest the gas gets, in kelvin, before any shift: the peak of a
     * Shakura-Sunyaev profile sits just outside the inner edge, and everything
     * further out follows from r^-3/4. Nine thousand puts that peak at
     * white-hot and the outer disc in deep orange, which is the disc of a
     * supermassive hole — the only kind whose disc is cool enough to have a
     * colour a person would recognise. */
    "const float DISC_K = 9000.0;\n"
    /* How far out the gas reaches. A real disc runs for thousands of radii;
       what decides this is the canvas, which is only a few radii wide, and a
       disc drawn past its edge is a disc cut off with scissors. Fed in from
       main so it can follow whatever room this hole was given. */
    "\n"
    /* Where the disc lies. incl is how far it is tipped out of edge-on (0 =
       seen exactly in its plane, 1 = seen from directly above) and roll is
       which way that tip is turned in the frame, so the same two numbers the
       orrery's camera already supplies still say what they said. */
    "vec3 disc_normal(float incl, float roll) {\n"
    "    float e = asin(clamp(incl, 0.0, 1.0));\n"
    "    vec3 n = vec3(0.0, cos(e), sin(e));\n"
    "    float c = cos(roll), s = sin(roll);\n"
    "    return vec3(n.x * c - n.y * s, n.x * s + n.y * c, n.z);\n"
    "}\n"
    "\n"
    /* Blackbody-ish colour for a temperature given as a fraction of the
       hottest gas in the picture. Not a fit to Planck's law — the eye is being
       asked whether this is hot metal or a hot star, and the answer is carried
       by the sequence dull red, orange, white, blue-white. */
    /* The colour of something at a temperature, rather than a ramp somebody
     * liked the look of.
     *
     * A thin accretion disc radiates as a blackbody, so its colour is decided
     * by one number — how hot the gas is there — and the sequence dull red,
     * orange, white, blue-white is not a stylistic choice, it is the Planckian
     * locus. This is the usual compact fit to it, good to a few percent across
     * the range a disc actually spans, and it costs two divides.
     *
     * The temperature is in kelvin and the range is chosen for a SUPERMASSIVE
     * hole: those are the ones with discs a few thousand kelvin, which is the
     * black hole everybody has seen a picture of. A stellar-mass hole's inner
     * disc runs to ten million kelvin, and the honest colour for that is a
     * featureless blue-white that says nothing about what it is looking at. */
    "vec3 blackbody(float kelvin) {\n"
    "    float t = clamp(kelvin, 1000.0, 22000.0) / 100.0;\n"
    "    float r, g, b;\n"
    "    if (t <= 66.0) {\n"
    "        r = 1.0;\n"
    "        g = clamp(0.3900816 * log(t) - 0.6318414, 0.0, 1.0);\n"
    "    } else {\n"
    "        r = clamp(1.2929362 * pow(t - 60.0, -0.1332047), 0.0, 1.0);\n"
    "        g = clamp(1.1298909 * pow(t - 60.0, -0.0755148), 0.0, 1.0);\n"
    "    }\n"
    "    if (t >= 66.0)      b = 1.0;\n"
    "    else if (t <= 19.0) b = 0.0;\n"
    "    else                b = clamp(0.5432068 * log(t - 10.0) - 1.1962541, 0.0, 1.0);\n"
    "    return vec3(r, g, b);\n"
    "}\n"
    "\n"
    /* What the gas at one point of the disc sends towards us.
     *
     * P is where the photon crossed the disc's plane, dirv is the direction it
     * was travelling as it crossed — and since the whole march runs BACKWARDS
     * from the eye, the light itself went the other way, which is what makes
     * the Doppler term below have the sign it has.
     *
     * Three things decide the brightness, and all three are physics rather
     * than taste:
     *
     *   the temperature — a thin accretion disc runs as r^-3/4 with a factor
     *     that goes to zero at the inner edge (there is no gas below the last
     *     stable orbit to be heated by), so the disc is not brightest at its
     *     rim but in a band just outside it;
     *   the Doppler shift — the gas is orbiting at a good fraction of c, so
     *     the side coming towards us is brighter, bluer and thinner, and the
     *     receding side dimmer and redder. This is THE thing that tells a
     *     photograph of a disc from a painting of one;
     *   the gravitational shift — light climbing out of the well loses energy,
     *     which reddens and dims the inner disc just where the Doppler term is
     *     shouting loudest.
     *
     * The three multiply into one factor, and brightness goes as its cube:
     * specific intensity over frequency cubed is what is conserved along a ray,
     * so a shift of g multiplies what arrives by g^3. */
    "vec4 disc_emit(vec3 P, vec3 dirv, vec3 N, vec3 A, vec3 B, float t,\n"
    "               float r_in, float r_out) {\n"
    "    float rc = length(P);\n"
    /*  Where the gas IS. Settled, that is everything from the last stable
        orbit out to the rim; while the disc is still forming it is a ring on
        its way down, and r_in is well outside the orbit it will end on. */
    "    if (rc < r_in || rc > r_out) return vec4(0.0);\n"

    "    float ang = atan(dot(P, B), dot(P, A));\n"
    "\n"
    /*  The texture of the gas. Sampled in the disc's own turning frame, with
        the shear kept gentler than Kepler's law: at the true exponent the
        inner edge laps the outer many times over, neighbouring radii sample
        unrelated noise, and the disc tears into concentric rings — a sampling
        artefact that reads as beams of light. */
    /*  The gas, and the one hard problem in drawing it: it has to KEEP
        turning, for as long as the session lasts, without the picture coming
        apart.
    
        Sampling a fixed noise field at an angle that grows with time cannot do
        that. The disc turns differentially — the inside faster than the
        outside — so the phase difference between two neighbouring radii grows
        without bound, and after an hour they are sampling places in the field
        with nothing to do with each other. The disc dissolves into crawling
        speckle. It is not a precision bug, though it looks like one: it is
        winding, and no amount of care with floats fixes it. (fwm hands the
        shader CLOCK_MONOTONIC, so on a machine up for a few hours this was the
        state a black hole was ALWAYS found in — while every render of the same
        shader, made seconds after start, looked perfect.)
    
        So the field is advected instead, in cycles. Two copies run at once,
        each carried round by the flow for at most one cycle before it is
        retired and re-seeded, and the picture cross-fades from the older to
        the younger. Nothing ever winds further than one cycle's worth, the
        seam where a layer is replaced is under the fade, and the disc turns
        for ever. */
    /*  How fast the gas goes round at this radius. Kepler's law would put the
        exponent at 1.5; this is flatter on purpose, because at the true value
        the inner edge laps the outer many times inside one cycle and the two
        stop looking like one disc. */
    "    float w = 2.2 / pow(rc, 0.9);\n"
    "    float cyc = 9.0;\n"
    "    float ph = t / cyc, seg = floor(ph), fr = fract(ph);\n"
    /*  Where the flow has carried each layer, in radians: the young one is
        just starting its trip, the old one is finishing the previous. */
    "    float spinA = fr * cyc * w;\n"
    "    float spinB = (fr - 1.0) * cyc * w;\n"
    /*  And a seed per cycle, so a retired layer comes back as different gas
        rather than the same gas again. Wrapped, because the noise field
        repeats every 256 units and a seed of a hundred thousand is a seed a
        float can no longer tell from its neighbour. */
    "    vec2 seedA = vec2(mod(seg * 37.0, 256.0), mod(seg * 61.0, 256.0));\n"
    "    vec2 seedB = vec2(mod((seg + 1.0) * 37.0, 256.0), mod((seg + 1.0) * 61.0, 256.0));\n"
    "    vec2 pA = vec2(cos(ang + spinA), sin(ang + spinA)) * rc + seedA;\n"
    "    vec2 pB = vec2(cos(ang + spinB), sin(ang + spinB)) * rc + seedB;\n"
    "    float aw = 1.0 - fr, bw = fr;\n"
    /*  Two scales of gas: the broad structure of where the disc is thick, and
        the streaks the flow draws out of it. A third, finer one used to sit
        here; at the size a hole is actually drawn it was below one pixel and
        arrived as grain rather than as gas. */
    /*  Each layer carries its OWN age into the noise's internal boil, the
        young one counting up from zero and the old one finishing the count it
        started last cycle. Handing both the same age is the one way to make
        the handover show: the whole field would reset its boil at the instant
        the layers change hands, and the disc would blink once a cycle. */
    "    float body = aw * turbulence(pA * 0.55, fr * 2.0)\n"
    "               + bw * turbulence(pB * 0.55, (fr - 1.0) * 2.0);\n"
    "    float flow = aw * turbulence(pA * 1.70, fr * 3.0)\n"
    "               + bw * turbulence(pB * 1.70, (fr - 1.0) * 3.0);\n"
    /*  Clumped hard: raising the sum to a power thins the quiet regions and
        leaves the bright filaments standing, which is what hot gas looks like
        and what an evenly lit ring never does. */
    "    float dens  = clamp(pow(clamp(body * 0.80 + flow * 0.62, 0.0, 2.0), 2.0), 0.0, 2.6);\n"
    "\n"
    /*  Shakura-Sunyaev: T goes as r^-3/4, damped by (1 - sqrt(r_in/r))^1/4 so
        the gas fades to nothing at the last stable orbit instead of being
        brightest where it is about to fall in. */
    "    float f = max(0.0, 1.0 - sqrt(min(r_in, R_ISCO) / rc));\n"
    "    float temp = pow(R_ISCO / rc, 0.75) * pow(f, 0.25);\n"
    /*  And how much gas there is at all: thinning out towards the rim, where a
        real disc simply runs out of the matter that was fed to it. */
    "    float amount = pow(1.0 - smoothstep(r_out * 0.45, r_out, rc), 1.3)\n"
    /*  and the inner edge, which is a soft one while the ring is still falling
        — gas on its way in has no sharp boundary, the disc it becomes does. */
    "                 * smoothstep(r_in, r_in * 1.10 + 0.15, rc);\n"
    "\n"
    /*  Doppler. The gas runs on a circular orbit, so its speed is fixed by the
        radius alone: v = sqrt(M/(r - 2M)), which with the horizon at 1 is
        sqrt(0.5/(r - 1)) — 0.5c at the inner edge, and still 0.25c far out. */
    "    vec3 vhat = normalize(cross(N, P));\n"
    "    float beta = clamp(sqrt(0.5 / max(rc - 1.0, 0.35)), 0.0, 0.85);\n"
    "    float gam = 1.0 / sqrt(1.0 - beta * beta);\n"
    /*  The photon's direction of travel towards us is the reverse of the one
        the march was going in. */
    "    vec3 nhat = -normalize(dirv);\n"
    "    float dopp = 1.0 / (gam * (1.0 - beta * dot(vhat, nhat)));\n"
    /*  Climbing out of the well: sqrt(1 - 1/r), which is where the inner disc
        loses back some of what the Doppler term gave it. */
    "    float grav = sqrt(max(0.0, 1.0 - 1.0 / rc));\n"
    "    float g = clamp(dopp * grav, 0.05, 3.5);\n"
    "\n"
    /*  Colour follows the shifted temperature — the approaching side is not
        just brighter, it is BLUER, and that is half of what makes it read as
        something moving. */
    /*  What the gas here is, in kelvin — and what it LOOKS, which is not the
        same number. Light climbing out of the well loses energy and light
        thrown towards us gains it, and both do it to the whole spectrum at
        once: the temperature an observer measures is the emitted one times the
        shift. So the approaching side of the disc is not merely brighter, it
        is genuinely hotter to look at, which is where its blue comes from. */
    "    float kelvin = DISC_K * temp * g;\n"
    "    vec3 col = blackbody(kelvin);\n"
    /*  And how much it BLOCKS. The gas is thin but it is not glass: where it
        is thick enough to shine it is also thick enough to hide what is behind
        it, and without that the desktop showing through the disc washes the
        disc out completely — a black hole over a bright window had a bright
        window's colours and none of its own. */
    "    float opacity = dens * amount * 1.5;\n"
    "    return vec4(col * (0.18 + 2.1 * dens) * temp * amount * pow(g, 2.4), opacity);\n"
    "}\n"
    "\n"
    /* ---- the ray ---------------------------------------------------------
     *
     * A photon's path near a black hole is not a bent straight line and cannot
     * be faked with one: light that passes close enough goes round the back
     * and comes out again, so the sky behind the hole appears TWICE — once
     * around the outside, once as a thin ring hugging the shadow — and the
     * disc appears three times, its top face over the hole, its far underside
     * lifted into an arch above it, and a hairline of both at the photon ring.
     * A one-line deflection formula produces none of that. It produces a
     * smear, which is what stood here.
     *
     * So this integrates the real thing, backwards from the eye. In these
     * units (horizon at r = 1, and thus M = 1/2) a null geodesic obeys
     *
     *     d2r/dl2 = -3 M h^2 r / |r|^5 = -1.5 h^2 r / |r|^5
     *
     * with h the angular momentum of the ray, which is conserved and is
     * exactly the impact parameter it started with. That is one cross product
     * and one multiply-add per step, and a hundred and sixty steps of it are
     * cheaper than the two texture samples this shader was already doing.
     *
     * The step is proportional to r: coarse where nothing is happening, fine
     * where the path is actually bending. */
    "const float R_EYE = 30.0;\n"       /* how far off the camera stands, in radii */
    "const float R_SKY = 3.0;\n"       /* and how far behind the hole the desktop hangs */
    "\n"
    /* Everything the march found, so main can compose it in one place. */
    "struct Ray {\n"
    "    vec3 glow;    \n"   /* what the disc put into this pixel */
    "    vec2 src;     \n"   /* where on the backdrop the ray came from */
    "    float hit;    \n"   /* 1 if it reached the backdrop at all */
    "    float veil;   \n"   /* how much of it the gas hides, 0..1 */
    "    float caught; \n"   /* 1 if it fell in */
    "};\n"
    "\n"
    "Ray trace(vec2 uv, float incl, float roll, float t, float r_in, float r_out) {\n"
    /*  How much light there is in the gas at all. Falling material is cold and
        dark: what heats a disc is the friction of one orbit rubbing against
        the next, and until it is a disc there are no orbits to rub. So it
        arrives as a dim ring, and lights as it settles. */
    "    float lit = mix(0.14, 1.0, smoothstep(0.06, 0.80, u_form));\n"
    "    Ray o;\n"
    "    o.glow = vec3(0.0); o.src = uv; o.hit = 0.0; o.caught = 0.0; o.veil = 0.0;\n"
    "\n"
    "    vec3 N = disc_normal(incl, roll);\n"
    /*  A frame for the disc's own plane, so a crossing can be given an angle.
        Degenerate exactly face-on, where any pair will do. */
    "    vec3 A = abs(N.z) > 0.999 ? vec3(1.0, 0.0, 0.0) : normalize(cross(N, vec3(0.0, 0.0, 1.0)));\n"
    "    vec3 B = cross(N, A);\n"
    "\n"
    "    vec3 pos = vec3(uv, R_EYE);\n"
    "    vec3 vel = vec3(0.0, 0.0, -1.0);\n"
    "    float h2 = dot(uv, uv);\n"
    "    float side = dot(pos, N);\n"
    "    float behind = pos.z + R_SKY;\n"   /* the backdrop plane, as a signed distance */
    "\n"
    "    for (int i = 0; i < 420; i++) {\n"
    "        float r = length(pos);\n"
    /*      Fine where the path is bending hardest, and coarse where it is a
            straight line in all but name. A ray that grazes the photon sphere
            winds most of the way round the hole, and a step too coarse to
            resolve that winding is what turns the hairline ring into a faint
            outline; a ray twenty radii out is going nowhere interesting and
            paying per step for the privilege. */
    "        float dt = r > 8.0 ? 0.30 * r\n"
    "                           : clamp(0.042 * r, 0.014, 1.6) * (r < 3.2 ? 0.5 : 1.0);\n"
    "        vec3 acc = -1.5 * h2 * pos / pow(r, 5.0);\n"
    "        vec3 nvel = vel + acc * dt;\n"
    "        vec3 npos = pos + nvel * dt;\n"
    "\n"
    /*      Through the gas.
    
            The disc has thickness — it is not a sheet of paper, and one drawn
            as a plane vanishes exactly edge-on, which is the angle most of it
            is seen at. But integrating that thickness step by step means
            evaluating the noise dozens of times for every ray that skims along
            the disc, and that is where thirty times the old shader's entire
            cost went.
    
            So the gas is sampled once, where the ray crosses the mid-plane,
            and multiplied by how far through the disc that ray actually
            travelled: a head-on crossing gets one thickness, a grazing one
            gets several, and the limb brightening that produces is the same
            thing that makes the edge of a real disc its brightest part. The
            cap is for the ray that crosses at nearly zero degrees, which would
            otherwise be handed an unbounded amount of gas.
    
            Optically thin, so a ray that crosses twice picks up both — which
            is how the far side of the disc comes to be visible above the near
            one, and how a ray that winds around the photon sphere collects the
            hairline ring. Front to back, so gas met earlier dims what is
            behind it. */
    "        float nside = dot(npos, N);\n"
    "        if (side * nside < 0.0) {\n"
    "            float f = side / (side - nside);\n"
    "            vec3 P = pos + (npos - pos) * f;\n"
    /*      Thicker while it is falling: gas that has not settled yet is a
            ragged shell rather than a disc, and it thins as it grinds into
            one. u_form is 1 for a hole that has arrived, and for everything
            that is not a hole at all. */
    "            float thick = (0.14 + 0.055 * length(P))\n"
    "                        * mix(2.6, 1.0, smoothstep(0.15, 0.95, u_form));\n"
    "            float ct = abs(dot(normalize(nvel), N));\n"
    "            float path = thick * clamp(1.0 / max(ct, 0.09), 1.0, 6.0);\n"
    "            vec4 em = disc_emit(P, nvel, N, A, B, t, r_in, r_out);\n"
    "            o.glow += em.rgb * path * 5.5 * lit * (1.0 - o.veil);\n"
    "            o.veil = clamp(o.veil + em.a * path * 3.0, 0.0, 1.0);\n"
    "        }\n"
    "        side = nside;\n"
    "\n"
    /*      And through the backdrop: the desktop is a flat thing a fixed way
            behind the hole, so the first crossing of that plane is where this
            pixel's picture of it comes from. Bent rays reach it further out
            than they appear to, which is the whole of the lensing. */
    "        float nbehind = npos.z + R_SKY;\n"
    "        if (o.hit < 0.5 && behind > 0.0 && nbehind <= 0.0) {\n"
    "            float f = behind / (behind - nbehind);\n"
    "            vec3 P = pos + (npos - pos) * f;\n"
    "            o.src = P.xy;\n"
    "            o.hit = 1.0;\n"
    "        }\n"
    "        behind = nbehind;\n"
    "\n"
    "        pos = npos; vel = nvel;\n"
    "        float rn = length(pos);\n"
    "        if (rn < 1.0) { o.caught = 1.0; break; }\n"
    /*      Done: past the gas, past the backdrop, and on its way out. There is
            nothing further along this ray for the picture to gain, and
            marching it to some arbitrary far radius was most of what the
            tracer cost.
    
            PAST THE BACKDROP is half the test, and leaving it out is what drew
            a circle round the hole on the desktop. A ray that passes six or
            seven radii out turns round while it is still short of the plane
            the desktop hangs on; stopped there it never reaches it, comes back
            with nothing to show, and the pixel goes transparent — so the bent
            copy ended in a ragged ring at exactly the radius where that starts
            happening, with the untouched desktop outside it. */
    "        if (o.hit > 0.5 && dot(pos, vel) > 0.0 && rn > r_out + 1.0) break;\n"
    "        if (rn > R_EYE * 1.6) break;\n"
    "    }\n"
    "    return o;\n"
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
    "    float lump = 0.35 + 1.15 * turbulence(ring_uv(ang + wrap_t(t, 0.14), 1.8, d * 0.6), t * 0.6);\n"
    /*  Cold and dim at first, warming as it falls in. */
    "    vec3 cold = mix(vec3(0.35, 0.22, 0.45), u_color, birth * birth);\n"
    "    return cold * band * lump * (0.25 + 0.75 * birth) * 0.9;\n"
    "}\n"
    "\n"
/* Where a ray that appears to arrive at `uv` really came from.
   
   Two effects, and both are the real ones. The bending goes as 1/b — the
   Einstein deflection 4GM/(c^2 b) — so the picture is sampled from FURTHER OUT
   than it appears and whatever sat directly behind is dragged around the edge.
   The twist is frame dragging: a spinning mass pulls spacetime round with it,
   so the picture arrives rotated, and rotated more the closer it passed. It
   falls off as 1/b^2, which makes it a whirlpool at the rim and nothing at all
   further out.
   
   `k` is the compactness — it scales both, which is precisely why a pulsar
   needs no code of its own: it is the same lens turned down. */
    "vec2 bend(vec2 uv, float b, float k, float reach) {\n"
    "    float defl  = 7.5 * k / max(b, 0.5) * reach;\n"
    "    float twist = 5.2 * k / max(b * b, 0.6) * reach;\n"
    "    float ca = cos(twist), sa = sin(twist);\n"
    "    vec2 spun = vec2(uv.x * ca - uv.y * sa, uv.x * sa + uv.y * ca);\n"
    "    return normalize(spun + vec2(1e-5)) * (b + defl);\n"
    "}\n"
    "\n"
    /* ---- one ray's worth of the picture --------------------------------
     *
     * Pulled out of main so that a pixel can be traced more than once, which
     * exactly one part of this picture needs: see the photon ring below.
     * Returns the colour and how much of the pixel this ray covers. */
    "vec4 hole_ray(vec2 uv, float rs, float dmax, float incl, float tt,\n"
    "              float r_in, float r_out) {\n"
    "    float b = length(uv);\n"
    "    Ray ray = trace(uv, incl, u_roll, tt, r_in, r_out);\n"
    "    vec3 acc = ray.glow;\n"
    "    vec3 back = vec3(0.0);\n"
    "    float lensed = 0.0;\n"
    "\n"
    /*  The backdrop, seen from wherever the ray actually came from.
    
           Faded back to the straight-line answer at the edge of the canvas:
           out there the node's own alpha is fading too, and where a partly
           transparent bent copy lies over the untouched original the window
           underneath appears twice. Bending that dies exactly where the
           coverage does makes the two the same picture. */
    "    float reach = (1.0 - smoothstep(dmax * 0.45, dmax * 0.85, b));\n"
    "    vec2 src = mix(uv, ray.src, reach);\n"
    /*  How much work the lens did on this pixel: how far the light it shows
           came from, compared with where it appears to be. Near the shadow the
           answer is radii, at the edge of the canvas it is nothing — and it is
           what decides whether this pixel may be see-through. Anywhere the
           lens moved the picture, the pixel belongs to the hole and has to
           cover what is underneath, because what is underneath is the same
           desktop drawn straight. */
    "    float bent = smoothstep(0.08, 0.40, length(src - uv));\n"
    /*  A ray that neither fell in nor ever reached the desktop went round
           the back and left sideways or came out towards the eye. There is
           nothing behind it to show and nothing in front either: it is black.
           It must not be TRANSPARENT black, though — that is a window seen
           unbent through the gap between the shadow and the disc, which is
           precisely where these rays live. */
    "    float lost = (1.0 - ray.hit) * (1.0 - ray.caught);\n"
    "    float offpage = 0.0;\n"
    "    if (ray.caught < 0.5 && ray.hit > 0.5) {\n"
    "        if (u_has_bg > 0.5) {\n"
    "        vec2 bguv = (src * rs + u_res * 0.5) / u_res;\n"
    "        bguv = u_bg_rect.xy + bguv * u_bg_rect.zw;\n"
    "        vec2 clamped = clamp(bguv, vec2(0.002), vec2(0.998));\n"
    /*          Off the edge of the photograph there is nothing to show, so
                   what the lens does there fades out instead of smearing the
                   border pixel across the frame. */
    "        float inside = step(length(bguv - clamped), 0.0001);\n"
    "        offpage = 1.0 - inside;\n"
    "        vec4 bg = bg_texel(clamped);\n"
    /*          Just outside the shadow the lens squeezes the whole of the
                   desktop into a band a few pixels wide. One sample of a
                   texture at that compression is not a picture of it, it is a
                   picture of whichever pixel the ray happened to land on — and
                   since the ray next door lands somewhere else entirely, the
                   band comes out as a mess of speckle that crawls as anything
                   moves. So where a pixel covers a lot of the photograph, it
                   is sampled across that whole footprint instead of at a
                   point. Four taps, only where the compression is real. */
    "#ifdef GL_OES_standard_derivatives\n"
    "        vec2 foot = (abs(dFdx(bguv)) + abs(dFdy(bguv))) * 0.5;\n"
    "        if (max(foot.x, foot.y) > 1.2 / min(u_res.x, u_res.y)) {\n"
    "            vec2 fx = vec2(foot.x, 0.0), fy = vec2(0.0, foot.y);\n"
    "            vec4 acc4 = bg_texel(clamp(bguv + fx, vec2(0.002), vec2(0.998)))\n"
    "                      + bg_texel(clamp(bguv - fx, vec2(0.002), vec2(0.998)))\n"
    "                      + bg_texel(clamp(bguv + fy, vec2(0.002), vec2(0.998)))\n"
    "                      + bg_texel(clamp(bguv - fy, vec2(0.002), vec2(0.998)));\n"
    "            bg = mix(bg, acc4 * 0.25, 0.75);\n"
    "        }\n"
    "#endif\n"
    /*          Where the ray came from beyond the edge of the photograph
                   there is nothing to show, and the honest answer is to show
                   nothing: this pixel stays transparent and the real desktop
                   underneath comes through untouched. Dimming the border
                   sample instead — which is what stood here — drew a dark ring
                   round the hole at exactly the radius where the lens starts
                   reaching past its own photograph, and that ring is the
                   "lensed area" you could see on the desktop. */
    "        back = bg.rgb * inside * (1.0 - ray.veil);\n"
    /*          Where the desktop was re-drawn bent, this pixel IS the
                   desktop and has to cover what lies underneath completely:
                   drawn semi-transparent it would sit as a film of curved
                   windows over the straight ones. Weighted by what was
                   actually there, so an empty patch bends to nothing rather
                   than to black. */
    "        lensed = inside * bg.a;\n"
    "        } else {\n"
    "        back = sky(src) * (1.0 - ray.veil);\n"
    "        }\n"
    "    }\n"
    "\n"
    /*  Tone-mapped gently, so the approaching side of the disc keeps climbing
        instead of clipping to a flat white slab — the highlight rolls off the
        way a camera's does, which is where the white core with colour
        surviving around it comes from.

        The GAS is tone-mapped; the desktop behind it is not, and is added
        afterwards. Running the desktop through the same curve dimmed and
        warmed every window inside the node while the same window outside it
        stayed as it was, and the join between the two was a great circle drawn
        across the screen. What the lens does to the desktop is move it, not
        tint it. */
    "    acc = acc / (1.0 + acc * 0.30);\n"
    "    acc = pow(max(acc, vec3(0.0)), vec3(0.88));\n"
    "    acc += back;\n"
    "    float a = clamp(max(acc.r, max(acc.g, acc.b)) * 1.4, 0.0, 1.0);\n"
    "    a = max(a, lensed);\n"
    /*  The shadow is opaque black, full stop: a black hole you can see the
        wallpaper through is the one failure that ruins the whole picture. And
        opaque wherever the lens did the work, whether or not it found anything
        to show there — off the edge of the photograph near the shadow, and
        along every path that never reached the desktop at all, the honest
        picture is black, but it has to be a black that covers. Out at the rim,
        where the lens moves nothing, this is zero and the desktop comes
        through untouched, which is what keeps the node's edge invisible. */
    "    a = max(a, ray.caught);\n"
    "    a = max(a, max(lost, offpage * bent));\n"
    "    return vec4(acc, a);\n"
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
    /* ---- what anything compact does to the desktop behind it ----------- */
    /* A hole is not the only thing that bends light, it is only the thing that
       bends it hardest. A neutron star is a sun's worth of mass inside twenty
       kilometres, and light grazing it is deflected so far that you see most
       of the far side of the sphere as well as the near one — it is why a
       pulsar's light curve looks the way it does, and it is plainly visible in
       any honest rendering of one.
    
       So this is the same lens as the hole's, turned down by u_lens: about a
       third as strong for a pulsar, and for a white dwarf or an ordinary star
       so far below the threshold below that they never reach this code. No
       phase is named anywhere in here. What is missing compared to a hole is
       everything that needs a horizon — no shadow, no photon ring, no disc:
       there is a SURFACE here, and it is drawn over this in the ordinary way.
    
       The hole has its own copy of this a few lines down because it needs the
       result for so much else; here the bent desktop is simply what the star
       is standing in front of. */
    /* Premultiplied, because what is sampled is premultiplied and because the
       thing that matters here is that it can be EMPTY: see below. */
    "    vec3 lens_pre = vec3(0.0);\n"
    "    float lens_a = 0.0;\n"
    /* And the same thing with no desktop to bend: the orrery, where the strip
       has hidden the world it is drawing cards of. A hole there falls back to
       the procedural sky, so this does too — the difference being that a
       pulsar ADDS the bent starfield instead of replacing everything with it.
       Empty space is transparent: what a lens does to a starfield is move the
       stars, not paint a black disc over the ring of desktops behind. */
    "    if (u_phase < 2.5 && u_lens > 0.02) {\n"
    "        float rs0 = max(u_radius, 1.0);\n"
    "        vec2 uv0 = px / rs0;\n"
    "        float b0 = length(uv0);\n"
    "        float dm = min(u_res.x, u_res.y) * 0.5 / rs0;\n"
    "        float rch = (1.0 - smoothstep(dm * 0.55, dm * 0.98, b0));\n"
    "        vec2 dir0 = bend(uv0, b0, u_lens, rch);\n"
    "        vec2 bguv0 = (dir0 * rs0 + u_res * 0.5) / u_res;\n"
    "        bguv0 = u_bg_rect.xy + bguv0 * u_bg_rect.zw;\n"
    "        vec2 cl0 = clamp(bguv0, vec2(0.002), vec2(0.998));\n"
    /*     Off the edge of the photograph there is nothing to bend. */
    "        float inside0 = step(length(bguv0 - cl0), 0.0001);\n"
    /*     And only where the bending is actually worth something.
    
           The bent copy REPLACES the desktop under it, and the copy is not a
           perfect one: the wallpaper in it is photographed from the half-size
           duplicate that wallpaper.c keeps. A hole distorts everything within
           reach so far that nobody could tell, but a pulsar's lens is gentle
           at the edges — and covering the whole canvas with a copy that is
           only slightly softer than the real thing draws a rectangle of blur
           around the star, which is the one shape that gives the trick away.
           So coverage follows the displacement: below about a pixel of it,
           the desktop shows through as itself. */
    "        float shift = length(dir0 - uv0) * rs0;\n"
    /*     Only against a photograph. With none — the orrery — there is
           nothing here worth bending and two ways to get it wrong, both
           measured rather than guessed:
    
           The procedural sky is POINTS. Displacing a scatter of dots does not
           read as space bending, it reads as dots in slightly other places:
           switching the lens on moved 269 pixels of a 512-pixel square. A hole
           gets away with the same sky because what you are really watching
           there is its disc and photon ring, which are continuous.
    
           And painting that sky the way a hole paints it — opaque, as a
           background — turns the star into a black disc over the ring of
           desktops, which is precisely the failure expo_orrery_toggle warns
           about. A hole is allowed to be a black disc. A pulsar is not.
    
           So in the orrery a pulsar lenses nothing, and the thing worth
           bending there is the ring itself, which this canvas has never been
           handed. */
    "        float cover = u_has_bg > 0.5 ? inside0 * smoothstep(0.4, 1.6, shift) : 0.0;\n"
    /*     What is bent is not always a picture. In the orrery the background
           is expo's own pass, and where the ring has no card there is nothing
           in it at all — cleared to transparent. Taking only the colour there
           and calling it opaque paints a black disc: the lens covers the strip
           with the one colour that is not in it, and the bigger the star the
           more obvious. So the background's OWN coverage comes through, and
           empty stays empty: what is behind expo shows through the lens
           exactly as it does beside it. */
    "        vec4 bgs = u_has_bg > 0.5 ? bg_texel(cl0) : vec4(0.0);\n"
    "        lens_pre = bgs.rgb * cover;\n"
    "        lens_a   = bgs.a   * cover;\n"
    "    }\n"
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
    "        float f = turbulence(ring_uv(ang + u_angle, 2.6, d * 3.4 - wrap_t(u_time, 0.30)),\n"
    "                            u_time * 1.4);\n"

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
    "        float streak = turbulence(ring_uv(ang, 2.3, log(d) * 2.2 - wrap_t(u_time, 0.12)),\n"
    "                                 u_time * 0.5);\n"
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
     * corona, none of the above applies. Every pixel of it is one photon run
     * backwards from the eye until it either falls in, reaches the disc, or
     * gets out to the desktop behind; see trace() above. */
    "    if (u_phase > 2.5) {\n"
    "        float rs = max(u_radius, 1.0);\n"
    "        vec2 uv = px / rs;\n"
    "        float b = length(uv);\n"
    /*     How many radii of canvas this hole was given — star_draw.c hands it
           STAR_HOLE_BOX of them and sizes the node to match, so this is nine
           unless the buffer hit its cap. The disc has to END before the fade
           at the canvas edge begins, or its outer rim is cut off in a straight
           line across the picture, which is what a hole fed past its buffer
           used to look like. */
    "        float dmax = min(u_res.x, u_res.y) * 0.5 / rs;\n"
    "        float r_settled = clamp(dmax * 0.60, R_ISCO * 1.25, 16.0);\n"
    /*     Where the gas is right now.
    
           Settled, it runs from the last stable orbit out to r_settled. A hole
           that has just appeared has none: what becomes the disc is the part
           of the star's envelope that failed to escape, and it starts as a
           wide ring near the edge of the picture and comes down. Both edges
           fall, the inner one first and faster, so the ring arrives at the
           orbit it cannot cross and then spreads back out into a disc —
           which is the shape of the real thing: matter grinds inward, piles up
           at the innermost stable orbit, and what is behind it fans out.
    
           Squared, so it falls the way anything falls: slowly at first, and
           then all at once. */
    "        float fall = u_form * u_form;\n"
    "        float r_in  = mix(dmax * 0.62, R_ISCO, smoothstep(0.0, 0.72, fall));\n"
    "        float r_out = mix(dmax * 0.74, r_settled, smoothstep(0.12, 1.0, fall));\n"
    "        r_out = max(r_out, r_in * 1.04);\n"
    "        float incl = clamp(u_incl, 0.012, 0.995);\n"
    "\n"
    "        float tt = u_time + u_angle * 0.2;\n"
    "        vec4 px4 = hole_ray(uv, rs, dmax, incl, tt, r_in, r_out);\n"
    "\n"
    /*     The photon ring, drawn round instead of dotted.
    
           Light that grazes the photon sphere goes round the hole and comes
           back out, so at b = 3*sqrt(3)/2 — 2.598 radii, where capture begins
           — everything behind the hole appears again, squeezed into a band far
           thinner than a pixel. That ring is real and it is in the photographs
           of the real thing; what was wrong with it here was only that one ray
           per pixel samples a sub-pixel band at one point, so it came out as a
           dotted circle of stray pixels rather than as a ring.
    
           So a pixel that can contain any of it is traced across its own width
           — radially, since that is the direction everything varies in, and
           hardly at all round the circle. Eight rays cannot resolve a band
           that thin, but they do not need to: averaged, they are what the ring
           actually looks like at this scale, which is a thin even circle.
    
           The band is kept narrow on purpose. These are the rays that wind
           around the hole for hundreds of steps and cross the gas over and
           over — the most expensive in the frame — and sampling a wide band
           this way once took the compositor from sixty frames a second to
           twenty. A quarter of a radius either side of the critical one is
           where the ring can be, and nowhere else in the picture pays. */
    "        if (abs(b - 2.598) < 0.14) {\n"
    "            vec2 rad = uv / max(b, 0.001);\n"
    "            vec4 sum = vec4(0.0);\n"
    "            for (int k = 0; k < 4; k++) {\n"
    "                float o = (float(k) + 0.5) / 4.0 - 0.5;\n"
    "                sum += hole_ray(uv + rad * (o / rs), rs, dmax, incl, tt,\n"
    "                                r_in, r_out);\n"
    "            }\n"
    "            px4 = sum / 4.0;\n"
    "        }\n"
    "        vec3 acc = px4.rgb;\n"
    "        float a = px4.a;\n"
    "\n"
    /*     No painted-on glow around it. There used to be one — a warm halo,
           on the reasoning that gas this hot lights the dust around it — and
           over a desktop it is exactly wrong: it lays a brown haze across
           every window within nine radii and hides the one thing the lens does
           that nobody has seen before, which is the windows themselves bent
           round the shadow. Whatever glow there is here is light that came
           from somewhere, and it is already in the disc. */
    /*     The envelope, still on its way out.
    
           A hole is drawn by an early return, so for as long as there has been
           one it has been the only remnant that arrived without its own
           supernova: the shell that a dwarf and a pulsar are born inside was
           computed for it, and then never reached. That is most of why it
           appeared out of nothing. It expands and fades over the same seconds
           the fallback is coming down in — one envelope, the half that got
           away and the half that did not — and it passes IN FRONT of the
           shadow as well as around it, which is where half of a shell you are
           standing outside of is. */
    "        if (u_blast > 0.0) {\n"
    /*         Sized to the canvas rather than to a constant. The shell that
               other remnants are born inside expands in units of the star's
               own radius, and a hole's radius is a fraction of that — the same
               figure would run off the edge of the picture while it was still
               at its brightest, and be cut off in a circle. This reaches the
               rim just as it fades, where the node's own edge takes it. */
    "            float far = 0.6 + (dmax * 0.95 - 0.6) * u_blast * u_blast;\n"
    "            float thk = (0.35 + 2.4 * u_blast) * max(dmax / 9.0, 0.4);\n"
    "            float ring = exp(-pow(abs(b - far) / thk, 2.0));\n"
    "            float rag = 0.55 + 0.75 * turbulence(ring_uv(ang, 2.2, b * 0.5), u_time * 0.7);\n"
    "            float fade = pow(1.0 - u_blast, 1.6);\n"
    "            vec3 hot = mix(vec3(1.0, 0.55, 0.18), vec3(1.0, 0.98, 0.95),\n"
    "                           clamp(1.0 - u_blast * 1.8, 0.0, 1.0));\n"
    "            acc += hot * ring * rag * fade * 5.5;\n"
    "        }\n"
    "\n"
    "        float rim2 = (1.0 - smoothstep(dmax * 0.86, dmax, b));\n"
    "        vec3 out3 = acc * a * rim2;\n"
    "        gl_FragColor = vec4(out3.b, out3.g, out3.r, a * rim2);\n"
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
    "        float rag = 0.55 + 0.75 * turbulence(ring_uv(ang, 2.2, d * 0.5), u_time * 0.7);\n"
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
    /* The star over what it bent, in that order and by the ordinary rule: the
       surface is opaque and hides it, the corona is thin and lets it through.
       The bent copy carries the SAME rim fade as the star, so at the edge of
       the canvas the bending, the coverage and the star all reach zero
       together — which is what makes the bent desktop and the real one it is
       drawn over identical there, and the buffer's border invisible. With
       nothing bent (lens_a 0) this is exactly the line it replaces. */
    "    float la = lens_a * rim;\n"
    "    vec3 out3 = col * alpha + lens_pre * rim * (1.0 - alpha);\n"
    "    gl_FragColor = vec4(out3.b, out3.g, out3.r,\n"
    "                        alpha + la * (1.0 - alpha));\n"  /* premultiplied */
    "}\n";

static const char star_vert_src[] =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

#endif /* FWM_STAR_SHADER_H */
