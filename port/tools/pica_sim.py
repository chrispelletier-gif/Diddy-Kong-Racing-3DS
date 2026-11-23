#!/usr/bin/env python3
"""
A small PICA200 vertex shader simulator for the port's own shaders (custom shader plan, P65; DKR_TRACK_SLIM,
DKR_WAVE_SHADER). It reads picasso's .shbin (the same bytes the 3DS runs), decodes the instructions and runs one
DVLE's program on given inputs and uniforms, so a new shader's maths can be checked on the PC against the shader it
replaces or against the game's own C code.

What it models: the vertex shader instruction set the port's shaders use (add, dp3, dp4, mul, sge, slt, flr, max, min,
rcp, rsq, mova, mov, sgei, slti, mad, end, nop), operand swizzles, negation and write masks, relative addressing of
float uniforms by a0.x / a0.y, and the DVLE's own constants. Floats are kept at float32 and rounded to the PICA's
24-bit float (1 sign, 7 exponent, 16 mantissa bits) after every operation, which is close to, not exactly, what the
hardware does (rcp/rsq are approximations on the PICA). So comparisons of two shaders are exact (both run here the
same way), and comparisons against C code use a tolerance.

What it refuses (raises): flow control (the port's shaders have none, by rule), two mova in a row (the picasso manual:
"two consecutive mova instructions will freeze the PICA200"), an output register written twice or not written whole
(the rule found after the first shader hung a New 3DS), and reads of temporaries never written.

Usage as a module: sim = PicaShbin(path); out = sim.run(dvle_index, inputs={0: (x, y, z, w), ...},
uniforms={'mtx': [...], 'eye': (..)}). Run as a script for the CI checks: pica_sim.py check BLOB.shbin.
"""
import math
import random
import struct
import sys


def f24(x):
    """Rounds a float to the PICA's 24-bit float (truncating the mantissa like picasso's f32tof24)."""
    if x != x or x == 0.0 or math.isinf(x):
        return x
    b = struct.unpack("<I", struct.pack("<f", x))[0]
    exp = ((b >> 23) & 0xFF) - 127 + 63
    if exp <= 0:
        return 0.0 if not (b >> 31) else -0.0
    if exp >= 0x7F:
        return math.copysign(float("inf"), x)
    b &= 0xFFFFFF80  # 16 mantissa bits kept
    return struct.unpack("<f", struct.pack("<I", b))[0]


def from_f24(w):
    sign = (w >> 23) & 1
    exp = (w >> 16) & 0x7F
    mant = w & 0xFFFF
    if exp == 0 and mant == 0:
        return -0.0 if sign else 0.0
    if exp == 0x7F:
        return -math.inf if sign else math.inf
    b = (sign << 31) | ((exp - 63 + 127) << 23) | (mant << 7)
    return struct.unpack("<f", struct.pack("<I", b))[0]


class ShaderError(Exception):
    pass


class Dvle:
    def __init__(self, blob, off):
        if blob[off:off + 4] != b"DVLE":
            raise ShaderError("not a DVLE at %#x" % off)
        self.geo = blob[off + 6]
        self.main, self.endmain = struct.unpack_from("<II", blob, off + 8)
        self.consts = {}
        coff, ccount = struct.unpack_from("<II", blob, off + 0x18)
        for i in range(ccount):
            e = off + coff + i * 20
            typ, reg = struct.unpack_from("<HH", blob, e)
            if typ == 2:
                vals = struct.unpack_from("<IIII", blob, e + 4)
                self.consts[reg] = [from_f24(v) for v in vals]
        ooff, ocount = struct.unpack_from("<II", blob, off + 0x28)
        self.outputs = {}
        for i in range(ocount):
            typ, reg, mask = struct.unpack_from("<HHH", blob, off + ooff + i * 8)
            self.outputs[reg] = (typ, mask)
        uoff, ucount = struct.unpack_from("<II", blob, off + 0x30)
        soff, _ = struct.unpack_from("<II", blob, off + 0x38)
        self.uniforms = {}
        for i in range(ucount):
            sym, start, end = struct.unpack_from("<IHH", blob, off + uoff + i * 8)
            p = off + soff + sym
            name = blob[p:blob.index(b"\0", p)].decode()
            self.uniforms[name] = (start, end)


class PicaShbin:
    OPS1 = {0x00: "add", 0x01: "dp3", 0x02: "dp4", 0x08: "mul", 0x09: "sge", 0x0A: "slt", 0x0B: "flr", 0x0C: "max",
            0x0D: "min", 0x0E: "rcp", 0x0F: "rsq", 0x12: "mova", 0x13: "mov"}
    OPS1I = {0x1A: "sgei", 0x1B: "slti"}

    def __init__(self, path):
        with open(path, "rb") as f:
            blob = f.read()
        if blob[:4] != b"DVLB":
            raise ShaderError("not a shbin")
        n = struct.unpack_from("<I", blob, 4)[0]
        offs = struct.unpack_from("<%dI" % n, blob, 8)
        p = 8 + 4 * n
        if blob[p:p + 4] != b"DVLP":
            raise ShaderError("no DVLP")
        code_off, code_size, op_off, op_count = struct.unpack_from("<IIII", blob, p + 8)
        self.code = struct.unpack_from("<%dI" % code_size, blob, p + code_off)
        self.opdesc = [struct.unpack_from("<I", blob, p + op_off + 8 * i)[0] for i in range(op_count)]
        self.dvles = [Dvle(blob, o) for o in offs]

    # Operand helpers
    @staticmethod
    def swz(sel):
        return [(sel >> (6 - 2 * i)) & 3 for i in range(4)]

    def run(self, index, inputs, uniforms, trace=False):
        d = self.dvles[index]
        if d.geo:
            raise ShaderError("geometry shaders are not simulated")
        c = [[0.0] * 4 for _ in range(96)]
        written_c = set()
        for name, val in uniforms.items():
            if name not in d.uniforms:
                raise ShaderError("no uniform %s in DVLE %d" % (name, index))
            start, end = d.uniforms[name]
            rows = val if isinstance(val[0], (list, tuple)) else [val]
            if len(rows) != end - start + 1:
                raise ShaderError("uniform %s has %d rows, given %d" % (name, end - start + 1, len(rows)))
            for k, row in enumerate(rows):
                c[start - 0x10 + k] = [f24(float(x)) for x in row]
                written_c.add(start - 0x10 + k)
        for reg, vals in d.consts.items():
            c[reg] = list(vals)
            written_c.add(reg)
        v = [[0.0] * 4 for _ in range(16)]
        for reg, val in inputs.items():
            v[reg] = [f24(float(x)) for x in val]
        r = [None] * 16
        o = [[None] * 4 for _ in range(16)]
        a0 = [0, 0]
        last_mova = False
        pc = d.main
        steps = 0
        while True:
            steps += 1
            if steps > 4096:
                raise ShaderError("runaway program")
            ins = self.code[pc]
            op = ins >> 26
            if op == 0x22:  # end
                break
            if op == 0x21:  # nop
                pc += 1
                last_mova = False
                continue
            if op in self.OPS1 or op in self.OPS1I:
                name = self.OPS1.get(op) or self.OPS1I[op]
                desc = self.opdesc[ins & 0x7F]
                dst = (ins >> 21) & 0x1F
                idx = (ins >> 19) & 3
                if op in self.OPS1I:
                    src1 = (ins >> 14) & 0x1F
                    src2 = (ins >> 7) & 0x7F
                else:
                    src1 = (ins >> 12) & 0x7F
                    src2 = (ins >> 7) & 0x1F
                mask = desc & 0xF
                s1 = self.read(src1, (desc >> 5) & 0xFF, (desc >> 4) & 1, v, r, c, written_c,
                               idx if op not in self.OPS1I else 0, a0)
                s2 = self.read(src2, (desc >> 14) & 0xFF, (desc >> 13) & 1, v, r, c, written_c,
                               idx if op in self.OPS1I else 0, a0)
                need = [0, 1, 2] if name == "dp3" else [0, 1, 2, 3] if name == "dp4" else \
                    [0] if name in ("rcp", "rsq") else [k for k in range(4) if mask & (8 >> k)]
                if name in ("dp3", "dp4", "rcp", "rsq"):
                    bad = any(s1[k] is None for k in need) or (name.startswith("dp") and any(s2[k] is None for k in need))
                else:
                    bad = any(s1[k] is None or (name not in ("mov", "mova", "flr") and s2[k] is None) for k in need)
                if bad:
                    raise ShaderError("%s at %#x reads a temporary component never written" % (name, pc))
                s1 = [0.0 if x is None else x for x in s1]
                s2 = [0.0 if x is None else x for x in s2]
                res = self.alu(name, s1, s2)
                if name == "mova":
                    if last_mova:
                        raise ShaderError("two mova in a row at %#x (freezes the PICA200)" % pc)
                    if mask & 8:
                        a0[0] = max(-128, min(127, int(res[0])))
                    if mask & 4:
                        a0[1] = max(-128, min(127, int(res[1])))
                    last_mova = True
                else:
                    last_mova = False
                    self.write(dst, mask, res, r, o)
                if trace:
                    print("%3x %-5s dst %02x mask %x -> %s" % (pc, name, dst, mask, res))
                pc += 1
                continue
            if op >= 0x38 or 0x30 <= op < 0x38:  # mad / madi
                inverted = op < 0x38
                desc = self.opdesc[ins & 0x1F]
                dst = (ins >> 24) & 0x1F
                idx = (ins >> 22) & 3
                src1 = (ins >> 17) & 0x1F
                if inverted:
                    src2 = (ins >> 10) & 0x7F
                    src3 = (ins >> 5) & 0x1F
                else:
                    src2 = (ins >> 12) & 0x1F
                    src3 = (ins >> 5) & 0x7F
                mask = desc & 0xF
                s1 = self.read(src1, (desc >> 5) & 0xFF, (desc >> 4) & 1, v, r, c, written_c, 0, a0)
                s2 = self.read(src2, (desc >> 14) & 0xFF, (desc >> 13) & 1, v, r, c, written_c,
                               idx if inverted else 0, a0)
                s3 = self.read(src3, (desc >> 23) & 0xFF, (desc >> 22) & 1, v, r, c, written_c,
                               0 if inverted else idx, a0)
                res = [f24(s1[i] * s2[i] + s3[i]) for i in range(4)]
                self.write(dst, mask, res, r, o)
                last_mova = False
                pc += 1
                continue
            raise ShaderError("unsupported opcode %#x at %#x (no flow control in the port's shaders)" % (op, pc))
        outs = {}
        for reg, (typ, omask) in d.outputs.items():
            for i in range(4):
                if o[reg][i] is None:
                    raise ShaderError("output o%d.%s never written" % (reg, "xyzw"[i]))
            outs[typ if typ != 9 else ("o%d" % reg)] = list(o[reg])
        return outs

    def read(self, src, sel, neg, v, r, c, written_c, idx, a0):
        if src < 0x10:
            base = v[src]
        elif src < 0x20:
            base = r[src - 0x10]
            if base is None:
                raise ShaderError("temporary r%d read before it is written" % (src - 0x10))
        else:
            reg = src - 0x20
            if idx == 1:
                reg += a0[0]
            elif idx == 2:
                reg += a0[1]
            elif idx == 3:
                raise ShaderError("aL addressing is not simulated")
            if not 0 <= reg < 96:
                raise ShaderError("uniform c%d out of range" % reg)
            if reg not in written_c:
                raise ShaderError("uniform c%d read but never set" % reg)
            base = c[reg]
        out = [base[k] for k in self.swz(sel)]
        if neg:
            out = [None if x is None else -x for x in out]
        return out

    @staticmethod
    def write(dst, mask, res, r, o):
        for i in range(4):
            if not mask & (8 >> i):
                continue
            if dst < 0x10:
                if o[dst][i] is not None:
                    raise ShaderError("output o%d.%s written twice" % (dst, "xyzw"[i]))
                o[dst][i] = res[i]
            else:
                t = dst - 0x10
                if r[t] is None:
                    r[t] = [None] * 4
                r[t][i] = res[i]

    @staticmethod
    def alu(name, a, b):
        if name == "add":
            return [f24(a[i] + b[i]) for i in range(4)]
        if name == "mul":
            return [f24(a[i] * b[i]) for i in range(4)]
        if name == "dp3":
            s = f24(a[0] * b[0] + a[1] * b[1] + a[2] * b[2])
            return [s] * 4
        if name == "dp4":
            s = f24(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3])
            return [s] * 4
        if name in ("sge", "sgei"):
            return [1.0 if a[i] >= b[i] else 0.0 for i in range(4)]
        if name in ("slt", "slti"):
            return [1.0 if a[i] < b[i] else 0.0 for i in range(4)]
        if name == "flr":
            return [float(math.floor(x)) for x in a]
        if name == "max":
            return [a[i] if a[i] > b[i] else b[i] for i in range(4)]
        if name == "min":
            return [a[i] if a[i] < b[i] else b[i] for i in range(4)]
        if name == "rcp":
            x = a[0]
            return [f24(1.0 / x) if x != 0 else math.copysign(math.inf, x)] * 4
        if name == "rsq":
            x = a[0]
            return [f24(1.0 / math.sqrt(x)) if x > 0 else math.inf] * 4
        if name in ("mov", "mova"):
            return list(a)
        raise ShaderError(name)


def dvle_with_entry(sim, uniform_names):
    """The first DVLE whose uniforms include all these names."""
    for i, d in enumerate(sim.dvles):
        if not d.geo and all(n in d.uniforms for n in uniform_names):
            yield i


# ---------------------------------------------------------------------------------------------------------------------
# CI checks


def random_model_uniforms(rng, lit, texc):
    m = [[rng.uniform(-2, 2) for _ in range(3)] + [rng.uniform(-500, 500)] for _ in range(4)]
    m[3] = [rng.uniform(-0.01, 0.01), rng.uniform(-0.01, 0.01), rng.uniform(0.5, 1.5), rng.uniform(100, 3000)]
    return {
        "eye": (rng.choice([0.0, rng.uniform(-0.05, 0.05)]), rng.choice([0.0, rng.uniform(-0.05, 0.05)]), 0, 0),
        "mtx": m,
        "camoff": (rng.randint(-3000, 3000), rng.randint(-500, 500), rng.randint(-3000, 3000), 0),
        "texa": tuple(rng.uniform(-0.01, 0.01) for _ in range(4)),
        "texc": tuple(rng.uniform(-0.01, 0.01) for _ in range(4)) if texc else (0, 0, 0, 0),
        "texb": tuple(rng.uniform(-2, 2) for _ in range(4)),
        "fogp": (rng.uniform(0, 2), rng.uniform(-1, 1), rng.choice([0.0, 1.0]), 1.0 if lit else 0.0),
        "lightv": (rng.uniform(-0.01, 0.01), rng.uniform(-0.01, 0.01), rng.uniform(-0.01, 0.01), rng.uniform(0, 1)),
    }


def random_model_inputs(rng):
    return {
        0: (rng.randint(-3000, 3000), rng.randint(-500, 500), rng.randint(-3000, 3000), 1),
        1: (rng.randint(-2000, 2000), rng.randint(-2000, 2000), 0, 0),
        2: (0, 0, 0, 0),  # the fixed attribute (gfx_c3d.c init_vertex_formats)
        3: tuple(rng.randint(0, 255) for _ in range(4)),
    }


def check_track_slim(sim, transform_i, track_i, count, break_it=False):
    """track.v.pica against transform.v.pica on random model draws with no light and texc 0: every output identical.
    break_it (rule K6): lights the vertices (fogp.w = 1, which only transform.v.pica reads), so the check must report."""
    rng = random.Random(1234)
    differ = 0
    for n in range(count):
        u = random_model_uniforms(rng, lit=break_it, texc=False)
        ins = random_model_inputs(rng)
        a = sim.run(transform_i, ins, u)
        b = sim.run(track_i, ins, {k: u[k] for k in u if k in sim.dvles[track_i].uniforms})
        if a != b:
            differ += 1
            if differ <= 3:
                print("  vertex %d differs:\n    transform %s\n    track     %s" % (n, a, b))
    return differ


def check_anim(sim, transform_i, anim_i, count, break_it=False):
    """anim.v.pica (keyframe A, step d, frac) against transform.v.pica fed the position obj_animate makes,
    A + ((u32) (d * frac) >> 4) as s16: every output identical. break_it (rule K6): the shader is given frac + 1."""
    rng = random.Random(4321)
    differ = 0
    for n in range(count):
        u = random_model_uniforms(rng, lit=rng.random() < 0.5, texc=rng.random() < 0.5)
        ins = random_model_inputs(rng)
        a = [rng.randint(-8000, 8000) for _ in range(3)]
        d = [rng.randint(-128, 127) for _ in range(3)]
        frac = rng.randint(0, 15)
        pos = []
        for k in range(3):
            step = ((d[k] * frac) & 0xFFFFFFFF) >> 4
            step = step & 0xFFFF
            step = step - 0x10000 if step >= 0x8000 else step
            pos.append(((a[k] + step + 0x8000) & 0xFFFF) - 0x8000)
        want = dict(ins)
        want[0] = (pos[0], pos[1], pos[2], 1)
        got = dict(ins)
        got[0] = (a[0], a[1], a[2], 1)
        got[4] = (d[0], d[1], d[2], 1)
        ua = dict(u)
        ua["animf"] = (((frac + 1) if break_it else frac) / 16.0, 0, 0, 0)
        x = sim.run(transform_i, want, u)
        y = sim.run(anim_i, got, {k: ua[k] for k in ua if k in sim.dvles[anim_i].uniforms})
        if x != y:
            differ += 1
            if differ <= 3 and not break_it:
                print("  vertex %d differs (A %s d %s frac %d):\n    transform %s\n    anim      %s" % (n, a, d, frac, x, y))
    return differ


def main(argv):
    if len(argv) < 3 or argv[1] != "check":
        print(__doc__)
        return 2
    sim = PicaShbin(argv[2])
    names = {}
    for i, d in enumerate(sim.dvles):
        print("DVLE %d: %s, main %#x, uniforms %s" % (i, "geometry" if d.geo else "vertex", d.main,
                                                      ", ".join(sorted(d.uniforms))))
    transform_i = next(i for i, d in enumerate(sim.dvles) if not d.geo and "lightv" in d.uniforms and
                       sim.code[d.main] >> 26 == 0x00 and i == 1)
    track_i = None
    for i, d in enumerate(sim.dvles):
        if i > 1 and not d.geo and "mtx" in d.uniforms and "stab" not in d.uniforms and "animf" not in d.uniforms and \
                "ipk" not in d.uniforms:
            track_i = i
            break
    failed = False
    if track_i is not None:
        differ = check_track_slim(sim, transform_i, track_i, 2000)
        print("track slim shader (DVLE %d) against transform (DVLE %d): 2000 vertices, %d differ" %
              (track_i, transform_i, differ))
        failed |= differ != 0
        broken = check_track_slim(sim, transform_i, track_i, 200, break_it=True)
        print("  broken on purpose (K6: vertices lit, which only the transform shader does): %d of 200 differ" % broken)
        failed |= broken == 0
    anim_i = next((i for i, d in enumerate(sim.dvles) if not d.geo and "animf" in d.uniforms), None)
    if anim_i is not None:
        differ = check_anim(sim, transform_i, anim_i, 2000)
        print("anim shader (DVLE %d) against transform (DVLE %d) fed obj_animate's position: 2000 vertices, %d differ" %
              (anim_i, transform_i, differ))
        failed |= differ != 0
        broken = check_anim(sim, transform_i, anim_i, 200, break_it=True)
        print("  broken on purpose (K6: frac one step on): %d of 200 differ" % broken)
        failed |= broken < 100
    try:
        import pica_wave_check
        failed |= not pica_wave_check.run(sim, argv[3:])
    except ImportError:
        pass
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
