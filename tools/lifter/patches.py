"""Per-instruction replacements applied by gen.py: guest address -> function(gen, addr) returning C code.

Every patch checks the original instruction word first, so lifting another game version (or a different build)
fails loudly instead of changing the wrong code. Runtime switches live in src/gfx/nvn/hle_nvn.c.
"""


def expect(gen, addr, word, what):
    got = gen.word(addr)
    if got != word:
        raise SystemExit(f'patch {what} at {addr:#x}: expected {word:#010x}, found {got:#010x} (not TOTK 1.4.3?)')


def drs_off(gen, addr):
    """Dynamic resolution off (1.4.3). main+0x584300 is the scene resolution controller: each frame it fits the
    recent frame times and returns {scale (float), state} to the renderer (main+0x581968), which turns the scale into
    the scene size (+0xccc). Its first test is the controller's enable bit (+0x110 bit 0); with the bit clear it
    takes its own disabled path (main+0x584c3c): scale 1.0, history untouched. The patch takes that path unless
    TOTK_DRS=1. Why: the game flips the scene targets between 1600x896 and 1432x808 over the same memory and our
    per-size images then hand stale content to the other size (terrain gone, clouds over the scene);
    image quality is better at a fixed size and upscaling will come from FSR."""
    expect(gen, addr, 0x360042e8, 'drs_off')   # tbz w8, #0, 0x800584c3c
    return ('{ extern int totk_drs_enabled(void); '
            'if (!totk_drs_enabled() || ((c->x[8] >> 0) & 1) == 0) { goto L_800584c3c; } }')


# ---- High frame rates (1.4.3), ported from MacNativeGames games/totk/runtime/game_patches_v143.c (MIT). There the
# patches rewrite main's code and branch to veneers; here each site lifts the same instruction words, guarded by the
# runtime switch (src/host/frame_step.c), so turning a switch off runs the original instruction. The veneers' data page
# is totk_frame_step: {30 Hz ratio, seconds, 60 Hz ratio} of the measured frame (guest addresses are host addresses).

def lifted(gen, addr, word):
    """C for one instruction word at addr: a replacement or veneer instruction, or the original."""
    if word == 0xd503201f:   # nop
        return ''
    ins = gen.lifter.lift(word, addr)
    if ins.kind == 'bl':     # the original call (its target may be in another chunk: through the dispatcher)
        return f'c->x[30] = {addr + 4:#x}ull; c->pc = {ins.target:#x}ull; rt_dispatch(c);'
    if ins.kind != 'op':
        raise SystemExit(f'patch at {addr:#x}: {word:#010x} lifts to {ins.kind}, expected straight-line code')
    return ins.code


def switched(gen, addr, switch, new, original):
    """`new` (C) while the runtime switch is on, else the original instruction."""
    return (f'{{ extern int {switch}(void); if ({switch}()) {{ {new} }} else {{ {lifted(gen, addr, original)} }} }}')


STEP = '{ extern float totk_frame_step[]; c->x[%d] = (uint64_t)(uintptr_t)totk_frame_step; }'


def fps60(original, replacement):
    """The present-interval controller (main+0x2db174) may pick interval 1 (60 fps) instead of 2: seven compares and
    selects whose minimum becomes 1. TOTK_FPS60=0 keeps the game's own 30 fps controller."""
    def patch(gen, addr):
        expect(gen, addr, original, 'fps60')
        return switched(gen, addr, 'totk_fps60', lifted(gen, addr, replacement), original)
    return patch


def veneer(original, reg, words, switch='totk_realtime'):
    """The site runs a MacNativeGames veneer: x<reg> = totk_frame_step, then `words` (the site's own instruction is
    among them where the veneer keeps it). reg None: no data pointer."""
    def patch(gen, addr):
        expect(gen, addr, original, 'frame step')
        body = (STEP % reg if reg is not None else '') + ' '.join(lifted(gen, addr, w) for w in words)
        return switched(gen, addr, switch, body, original)
    return patch


def realtime_producer(gen, addr):
    """main+0x3e5658 (fmul s8, s0, s1): the nominal 60/FPS interval s0, before the original half-step selector scales
    it, becomes the measured 60 Hz ratio. Both FrameRateMgr call sites use s8; the rest of its arithmetic is kept."""
    return veneer(0x1e210808, 8, [0xbd400900, 0x1e210808])(gen, addr)   # ldr s0, [x8, #8]; fmul s8, s0, s1


# main+0x2daa54..0x2daa78: the FrameRateMgr ratio (60 / FPS * 0.5) becomes the measured 30 Hz ratio:
# ldr s0, [x19, #0x250] -> x9 = totk_frame_step; mov w8, #60.0 -> ldr s0, [x9]; scvtf / fdiv / fmov / fmul -> nop.
REALTIME = {
    0x8003e5658: realtime_producer,
    0x8002daa54: veneer(0xbd425260, 9, []),
    0x8002daa58: veneer(0x52a84e08, None, [0xbd400120]),
    0x8002daa6c: veneer(0x5e21d800, None, []),
    0x8002daa70: veneer(0x1e201820, None, []),
    0x8002daa74: veneer(0x1e2c1001, None, []),
    0x8002daa78: veneer(0x1e210800, None, []),
}

# Cloth and hair: the actor animation update has a second clock that still advances {factor / 30, factor} per rendered
# frame (4x real time at 120 fps). Two inline sites store the ratio-scaled factor and leave the seconds-scaled one; the
# two calls of the shared update (+0x2a0e40) scale s2/s3 by the ratio. Its third caller already passes measured time.
CLOTH = {
    0x800246eac: veneer(0x1e200940, 8, [0xbd400100, 0x1e200940, 0xbd0367e0, 0xbd400500, 0x1e200940], 'totk_cloth_realtime'),
    0x800345904: veneer(0x1e210801, 8, [0xbd400101, 0x1e210801, 0xbd0117e1, 0xbd400501, 0x1e210801], 'totk_cloth_realtime'),
    0x80024944c: veneer(0x94015e7d, 8, [0xbd400104, 0x1e240842, 0x1e240863, 0x94015e7d], 'totk_cloth_realtime'),
    0x80028bc98: veneer(0x9400546a, 8, [0xbd400104, 0x1e240842, 0x1e240863, 0x9400546a], 'totk_cloth_realtime'),
}

# Trampolines and bouncy contacts (main+0x562a64): the bounce is cut to 0.1 when X+0x28 (30 / (ratio * time scale))
# exceeds 100, meaning "time slowed below 0.3" at 30 Hz but true above 100 fps with time running normally. Compare
# X+0x28 * X+0x38 (30 / time scale) instead: ldr s0, [x9, #0x28]; ldr s1, [x9, #0x38]; fmul s0, s0, s1.
BOUNCE = {0x800562a64: veneer(0xbd402920, None, [0xbd402920, 0xbd403921, 0x1e210800], 'totk_highfps_fix')}

FPS60 = {0x8002db204: fps60(0x6b08015f, 0x7100055f), 0x8002db208: fps60(0x1a89b10a, 0x1a9fa52a),
         0x8002db22c: fps60(0xb940468a, 0xd503201f), 0x8002db360: fps60(0x6b0801bf, 0x710005bf),
         0x8002db36c: fps60(0x1a88c1ac, 0x1a9fc5ac), 0x8002db340: fps60(0xb9400288, 0x52800028),
         0x8002db34c: fps60(0x2a0a03f5, 0x1a9fa555)}

PATCHES = {
    0x8005843e0: drs_off,
    **FPS60, **REALTIME, **CLOTH, **BOUNCE,
}
