#!/usr/bin/env python3
# Copyright 2026, air/OS. Distributed under the terms of the MIT License.
"""Host reference for the two NPU test designs (numpy only).

  reference.py gen <design> <design dir>      inputs, expected outputs, manifest.txt
  reference.py check <design dir> <output>    compare an output captured from the NPU
  reference.py insts <insts.bin>              list the transaction, DDR patches marked
  reference.py describe <design>              the buffer notes of a design
  reference.py selftest [scalar kernel .so]   check the conv2d model against others

Designs: passthrough_kernel (output = input) and conv2d (1x1 int8 convolution,
the arithmetic of mlir-aie's aie_kernels/conv/conv2dk1_i8.cc, vector path).
"""

import ctypes
import sys
from pathlib import Path

import numpy as np

# conv2d: the defaults of programming_examples/ml/conv2d/conv2d.py
W, H, IC, OC, SCALE = 32, 32, 64, 64, 10


def pattern(count, seed):
    """Test bytes anyone can regenerate, in C:
    h = (i + seed) * 0x9E3779B1; h ^= h >> 15; h *= 0x85EBCA6B; h ^= h >> 13;
    byte = h >> 24;  (uint32_t arithmetic, i = byte index)"""
    h = (np.arange(count, dtype=np.uint64) + seed) * 0x9E3779B1 & 0xFFFFFFFF
    h ^= h >> 15
    h = h * 0x85EBCA6B & 0xFFFFFFFF
    h ^= h >> 13
    return (h >> 24).astype(np.uint8)


def conv2d_acc(act, wts, w=W, ic=IC, oc=OC):
    """The 32 bit sums of the kernel, for any number of lines.
    act: lines of [ic/8][w][8] int8; wts: [oc/8][ic/8][8 in][8 out] int8;
    result: [lines][oc/8][w][8]."""
    a = act.view(np.int8).reshape(-1, ic // 8, w, 8).astype(np.int64)
    k = wts.view(np.int8).reshape(oc // 8, ic // 8, 8, 8).astype(np.int64)
    return np.einsum("yixc,oicp->yoxp", a, k)


def conv2d_ref(act, wts, scale=SCALE, rounding="away", **dims):
    """out = saturate_int8(round(sum / 2**scale)). The vector kernel the design
    runs rounds halves away from zero (aie::rounding_mode::symmetric_inf); the
    scalar C fallback in the same file rounds them up ("up")."""
    acc = conv2d_acc(act, wts, **dims)
    half = 1 << (scale - 1)
    if rounding == "away":
        out = np.sign(acc) * ((np.abs(acc) + half) >> scale)
    else:
        out = (acc + half) >> scale
    return np.clip(out, -128, 127).astype(np.int8).reshape(-1)


def passthrough_buffers():
    data = pattern(4096, 1)
    return [("in", "input0.bin", data), ("out", "expected1.bin", data)]


def conv2d_buffers():
    act = pattern(W * H * IC, 2)
    wts = pattern(IC * OC, 3)
    out = conv2d_ref(act, wts)
    return [("in", "input0.bin", act), ("in", "input1.bin", wts),
            ("out", "expected2.bin", out.view(np.uint8))]


DESIGNS = {
    "passthrough_kernel": (passthrough_buffers, """\
bo0  in   4096 bytes uint8, any content (the test uses pattern(4096, 1))
bo1  out  4096 bytes, must equal bo0
The core copies 1024 byte objects; one shim DMA transfer of 4096 bytes each way."""),
    "conv2d": (conv2d_buffers, f"""\
width {W}, height {H}, {IC} input channels, {OC} output channels, scale {SCALE}
bo0  in   {W * H * IC} bytes int8 activations: {H} lines of [channel/8][x][8]
          (byte y*{W * IC} + (c/8)*{W * 8} + x*8 + c%8), pattern({W * H * IC}, 2)
bo1  in   {IC * OC} bytes int8 weights [out/8][in/8][in%8][out%8]
          (byte (o/8)*{IC * 8} + (i/8)*64 + (i%8)*8 + o%8), pattern({IC * OC}, 3)
bo2  out  {W * H * OC} bytes int8, {H} lines of [channel/8][x][8]
out = saturate_int8(round_half_away_from_zero(sum over in of act * weight / 2**{SCALE}))
The weights are a separate buffer, sent once; the core keeps them for all lines."""),
}


def read_manifest(directory):
    pairs = [line.split("=", 1) for line in
             (Path(directory) / "manifest.txt").read_text().splitlines()
             if "=" in line and not line.startswith("#")]
    buffers = [value.split(":") for key, value in pairs if key == "buffer"]
    return dict(pairs), buffers


def gen(name, directory):
    directory = Path(directory)
    buffers = DESIGNS[name][0]()
    lines = [f"name={name}", "device=npu2_1col", "columns=1", "pdi=design.pdi",
             "insts=insts.bin", "opcode=3",
             "# one line per runtime_sequence argument = kernel argument bo0, bo1, ...",
             f"# the kernel declares 5 buffers (bo0-bo4), the host passes only these {len(buffers)}"]
    for kind, file, data in buffers:
        data.tofile(directory / file)
        lines.append(f"buffer={kind}:{file}:{data.size}")
    (directory / "manifest.txt").write_text("\n".join(lines) + "\n")


def check(directory, actual_file):
    manifest, buffers = read_manifest(directory)
    files = [np.fromfile(Path(directory) / file, np.uint8) for _, file, _ in buffers]
    (expected,) = [data for (kind, _, _), data in zip(buffers, files) if kind == "out"]
    actual = np.fromfile(actual_file, np.uint8)
    if actual.size != expected.size:
        print(f"FAIL: {actual.size} bytes, expected {expected.size}")
        return 1
    wrong = np.flatnonzero(actual != expected)
    if wrong.size == 0:
        print(f"PASS: {manifest['name']}, {expected.size} bytes match")
        return 0
    print(f"FAIL: {manifest['name']}, {wrong.size} of {expected.size} bytes differ")
    for i in wrong[:16]:
        print(f"  byte {i}: expected 0x{expected[i]:02x}, got 0x{actual[i]:02x}")
    if manifest["name"] == "conv2d":
        # A sum exactly half way between two results shows the rounding mode.
        ties = conv2d_acc(files[0], files[1]).reshape(-1) % (1 << SCALE) == 1 << (SCALE - 1)
        print(f"  {np.count_nonzero(ties[wrong])} of them where the sum is an exact half"
              f" ({np.count_nonzero(ties)} such sums): those depend on the rounding mode")
    return 1


# Words of each transaction operation: (name, index of its size-in-bytes word).
TXN_OPS = {0: ("write32", 5), 1: ("blockwrite", 3), 3: ("maskwrite32", 6),
           4: ("maskpoll32", 6), 0x80: ("sync", 1), 0x81: ("DDR_PATCH", 1)}


def insts(file):
    words = np.fromfile(file, "<u4")
    head = words[0]
    print(f"header: version {head & 255}.{head >> 8 & 255}, device generation {head >> 16 & 255},"
          f" {head >> 24} rows, {words[1] & 255} columns, {words[1] >> 8 & 255} mem tile rows,"
          f" {words[2]} operations, {words[3]} bytes")
    at = 4
    while at < words.size:
        name, size_word = TXN_OPS.get(int(words[at]) & 0xFF, (None, 0))
        if name is None:
            print(f"word {at}: unknown operation 0x{words[at]:x}, stopping")
            return 1
        op = words[at:at + words[at + size_word] // 4]
        if name == "DDR_PATCH":
            text = (f"register 0x{op[6]:x} <- address of buffer argument {op[8]}"
                    f" + 0x{int(op[10]) | int(op[11]) << 32:x}")
        elif name == "sync":
            text = (f"column {op[2] >> 16 & 255} row {op[2] >> 8 & 255} direction {op[2] & 255}"
                    f" channel {op[3] >> 24}")
        elif name == "blockwrite":
            text = f"0x{op[2]:x}: " + " ".join(f"{word:08x}" for word in op[4:])
        else:
            text = f"0x{op[2]:x} " + " ".join(f"0x{word:x}" for word in op[4:size_word])
        print(f"word {at:3}: {name:11} {text}")
        at += op.size
    return 0


def selftest(scalar_library=None):
    """The model against: a plain loop, the numpy reference mlir-aie ships for
    this kernel (when importable), and the scalar path of the kernel source
    compiled for the host (rounds halves up, so compared in that mode)."""
    rng = np.random.default_rng(1)
    dims = dict(w=32, ic=16, oc=8)
    act = rng.integers(-128, 128, 3 * 32 * 16, dtype=np.int8)
    wts = rng.integers(-128, 128, 16 * 8, dtype=np.int8)
    loop = np.zeros((3, 1, 32, 8), np.int64)
    for y, x, o, i in np.ndindex(3, 32, 8, 16):
        loop[y, 0, x, o] += int(act[y * 512 + (i // 8) * 256 + x * 8 + i % 8]) * int(wts[i * 8 + o])
    assert np.array_equal(loop, conv2d_acc(act, wts, **dims)), "plain loop"
    print("plain loop: sums match")

    act, wts, out = (data.view(np.int8) for _, _, data in conv2d_buffers())
    acc = conv2d_acc(act, wts).reshape(-1)
    ties = acc % (1 << SCALE) == 1 << (SCALE - 1)
    print(f"conv2d test data: {out.size} outputs, {np.count_nonzero(out < 0)} negative,"
          f" {np.count_nonzero((np.abs(acc) + (1 << SCALE - 1)) >> SCALE > 127)} saturated,"
          f" {np.count_nonzero(ties)} exact halves ({np.count_nonzero(ties & (acc < 0))} negative)")
    try:
        from aie.iron.kernels.conv import conv2dk1_i8_ref
        theirs = conv2dk1_i8_ref(act.reshape(H, -1), wts, W, IC, OC, SCALE)
        assert np.array_equal(theirs.reshape(-1), out), "mlir-aie conv2dk1_i8_ref"
        print("mlir-aie conv2dk1_i8_ref: identical")
    except ImportError:
        print("mlir-aie conv2dk1_i8_ref: not importable, skipped")
    if scalar_library:
        kernel = ctypes.CDLL(scalar_library).conv2dk1_i8
        theirs = np.zeros_like(out)
        for y in range(H):  # one call per line, as the core does
            line = np.ascontiguousarray(act[y * W * IC:(y + 1) * W * IC])
            kernel(line.ctypes, wts.ctypes, theirs[y * W * OC:].ctypes, W, IC, OC, SCALE)
        assert np.array_equal(theirs, conv2d_ref(act, wts, rounding="up")), "scalar kernel"
        print(f"scalar kernel on the host: identical in round-half-up mode;"
              f" {np.count_nonzero(theirs != out)} outputs differ from the vector mode")
    return 0


if __name__ == "__main__":
    command, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ("", [])
    if command == "describe" and len(args) == 1:
        print(DESIGNS[args[0]][1])
    elif command == "gen" and len(args) == 2:
        gen(*args)
    elif command in ("check", "insts", "selftest") and len(args) <= 2:
        sys.exit({"check": check, "insts": insts, "selftest": selftest}[command](*args))
    else:
        sys.exit(__doc__)
