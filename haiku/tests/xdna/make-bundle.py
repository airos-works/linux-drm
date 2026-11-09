#!/usr/bin/env python3
# Copyright 2026, air/OS. Distributed under the terms of the MIT License.
"""Makes a design bundle for the Neural Kit's amdxdna back end (numpy only).

  make-bundle.py conv2d <design dir> <bundles dir>
        from the conv2d design build-designs.sh built: the bundle
        <bundles dir>/<graph key>/, with a description of the one-operator
        network the design computes, weights, a test input and the output
        the kit's CPU computes for it; and that network as model.tflite
        (for "neural compare") when Python has flatbuffers and
        ai_edge_litert, which the Neural Kit's host tests need as well
  make-bundle.py analyse
        where the design's arithmetic and the kit's agree, in numbers

A bundle is what the back end loads for a stretch of a network
(src/add-ons/neural/amdxdna/README in the Haiku tree has the format). It is
found by the graph key, computed here exactly as the back end computes it
from the network a program hands the kit.

The network: CONV_2D, 1x1, stride 1, no bias, no activation, int8, zero
points 0, input 32x32x64, 64 output channels. The design (mlir-aie's
conv2dk1_i8, scale 10) computes

    out = saturate_int8(round(sum(act * weight) / 2**10))

The kit computes TensorFlow Lite's fixed point: with the scales chosen so
that input scale * weight scale / output scale = 2**-10 exactly, that is

    out = saturate_int8(R2(R1(sum)))    R1: sum / 2, halves rounded up
                                        R2: / 2**9, halves away from zero

two roundings where the design has one. They are the same number for every
even sum (R1 is exact then, and the design rounds halves away from zero).
For odd sums they differ exactly where sum mod 1024 = 511 and the sum is
positive: the kit rounds 0.4990 up to the half and then away. So the weights
of the bundle's network are all even, which makes every sum even: the
network the kit sees and the design then agree on every input, if the NPU
rounds halves away from zero as the kernel's vector code asks for
(aie::rounding_mode::symmetric_inf). Should it round them up, like the
kernel's scalar code, outputs differ by one where a negative sum is an
exact half; "analyse" counts those for the test input.
"""

import hashlib
import shutil
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import reference  # noqa: E402  (pattern(), the design's model, its layouts)

W, H, IC, OC, SHIFT = reference.W, reference.H, reference.IC, reference.OC, reference.SCALE

# input scale * weight scale / output scale = 2**-SHIFT, all powers of two
INPUT_SCALE, WEIGHT_SCALE, OUTPUT_SCALE = 2.0 ** -4, 2.0 ** -7, 2.0 ** -1
assert INPUT_SCALE * WEIGHT_SCALE / OUTPUT_SCALE == 2.0 ** -SHIFT

# the kit's numbers (NeuralDefs.h, NeuralOperators.h)
B_NEURAL_INT8 = 3
PADDING_VALID = 1
ACTIVATION_NONE = 0


# --- the graph key and the weights digest (Bundle.cpp) -----------------------

def u32(value):
    return struct.pack("<I", value & 0xFFFFFFFF)


def f32(value):
    return struct.pack("<f", value)


def string(text):
    return u32(len(text)) + text.encode()


def conv_parameters(padding, stride_w, stride_h, dilation_w, dilation_h, activation):
    """neural_conv_params of a CONV_2D: 7 fields of 32 bits; the depth
    multiplier, which only depthwise convolutions have, is 0."""
    return u32(28) + b"".join(map(u32, (padding, stride_w, stride_h, dilation_w,
                                        dilation_h, 0, activation)))


def tensor_bytes(shape, scale, constant_size=None):
    """An int8 tensor with one scale and zero point 0."""
    data = u32(B_NEURAL_INT8) + u32(len(shape)) + b"".join(map(u32, shape))
    data += b"\x01" + f32(scale) + u32(0) + u32(0)  # quantized, no channel scales
    if constant_size is None:
        return data + b"\x00"
    return data + b"\x01" + struct.pack("<Q", constant_size)


def graph_key():
    """The key of the network: its operators with their tensors numbered in
    the order they first appear (inputs, then outputs, of each operator),
    those tensors, and the network's inputs and outputs."""
    stream = b"amdxdna-graph-1\n"
    stream += u32(1)  # operators
    stream += string("CONV_2D")
    stream += u32(3) + u32(0) + u32(1) + u32(-1)  # input, weights, no bias
    stream += u32(1) + u32(2)  # output
    stream += conv_parameters(PADDING_VALID, 1, 1, 1, 1, ACTIVATION_NONE)
    stream += u32(3)  # tensors
    stream += tensor_bytes((1, H, W, IC), INPUT_SCALE)
    stream += tensor_bytes((OC, 1, 1, IC), WEIGHT_SCALE, OC * IC)
    stream += tensor_bytes((1, H, W, OC), OUTPUT_SCALE)
    stream += u32(1) + u32(0)  # the network's inputs
    stream += u32(1) + u32(2)  # and outputs
    return hashlib.sha256(stream).hexdigest()[:32]


def weights_digest(weights):
    """The constants the bundle's operators read: operator 0, input 1."""
    stream = b"amdxdna-weights-1\n" + u32(0) + u32(1)
    stream += struct.pack("<Q", weights.size) + weights.tobytes()
    return hashlib.sha256(stream).hexdigest()


# --- arithmetic ---------------------------------------------------------------

def sums(act, weights):
    """act: [H][W][IC] int8, weights: [OC][IC] int8 -> [H][W][OC] sums."""
    return np.einsum("yxi,oi->yxo", act.astype(np.int64), weights.astype(np.int64))


def kit_output(acc):
    """TensorFlow Lite's MultiplyByQuantizedMultiplier for the factor
    2**30 / 2**31 and the shift 1 - SHIFT, as the kit's CPU kernel does it
    (KernelUtils.h), then saturation."""
    product = acc * (1 << 30)
    nudge = np.where(product >= 0, 1 << 30, 1 - (1 << 30))
    total = product + nudge
    # C's division: towards zero
    doubled = np.sign(total) * (np.abs(total) >> 31)
    exponent = SHIFT - 1
    mask = (1 << exponent) - 1
    remainder = doubled & mask
    threshold = (mask >> 1) + (doubled < 0)
    out = (doubled >> exponent) + (remainder > threshold)
    return np.clip(out, -128, 127).astype(np.int8)


def design_output(acc, halves="away"):
    """The design: one rounding; halves away from zero (the kernel's vector
    code) or up (its scalar code)."""
    half = 1 << (SHIFT - 1)
    if halves == "away":
        out = np.sign(acc) * ((np.abs(acc) + half) >> SHIFT)
    else:
        out = (acc + half) >> SHIFT
    return np.clip(out, -128, 127).astype(np.int8)


def to_design_activations(tensor, channels):
    """[H][W][C] -> the design's lines of [C/8][W][8]."""
    return tensor.reshape(H, W, channels // 8, 8).transpose(0, 2, 1, 3).reshape(-1)


def to_design_weights(weights):
    """[OC][IC] -> the design's [OC/8][IC/8][8 of IC][8 of OC]."""
    return weights.reshape(OC // 8, 8, IC // 8, 8).transpose(0, 2, 3, 1).reshape(-1)


def test_data(even_weights=True):
    act = reference.pattern(W * H * IC, 2).view(np.int8).reshape(H, W, IC)
    weights = reference.pattern(IC * OC, 3).view(np.int8).reshape(OC, IC)
    if even_weights:
        weights = weights & ~1  # -128 .. 126, the lowest bit cleared
    return act, weights


def analyse():
    for even in (False, True):
        act, weights = test_data(even)
        acc = sums(act, weights)
        kit = kit_output(acc)
        away = design_output(acc, "away")
        up = design_output(acc, "up")
        ties = acc % (1 << SHIFT) == 1 << (SHIFT - 1)
        odd = (acc % (1 << SHIFT) == (1 << (SHIFT - 1)) - 1) & (acc > 0)
        differ = kit != away
        print(f"{'even' if even else 'any'} weights, {acc.size} outputs"
              f" ({np.count_nonzero(np.abs(acc) > 127 << SHIFT)} saturated):")
        print(f"  kit differs from the design (halves away from zero) on"
              f" {np.count_nonzero(differ)}, all by"
              f" {np.abs(kit.astype(int) - away)[differ].max() if differ.any() else 0};"
              f" sums of 511 mod 1024 above zero: {np.count_nonzero(odd)}"
              f" (not saturated: {np.count_nonzero(odd & (np.abs(acc) < 127 << SHIFT))})")
        print(f"  kit differs from the design if it rounded halves up on"
              f" {np.count_nonzero(kit != up)}; negative exact halves:"
              f" {np.count_nonzero(ties & (acc < 0))} of {np.count_nonzero(ties)} halves")
        # the model here against reference.py's, in the design's layout
        ours = to_design_activations(away, OC)
        theirs = reference.conv2d_ref(to_design_activations(act, IC), to_design_weights(weights))
        assert np.array_equal(ours, theirs), "layouts or sums differ from reference.py"

    # the claim of the docstring, on sums of every kind
    acc = np.arange(-200000, 200001, dtype=np.int64)
    differ = kit_output(acc) != design_output(acc)
    expected = (acc % 1024 == 511) & (acc > 0) & (acc < 127 * 1024)
    assert np.array_equal(differ, expected), "the kit and the design differ elsewhere"
    print("every sum from -200000 to 200000: the kit and the design differ exactly"
          " where the sum is positive, 511 mod 1024 and not saturated; never for even sums")
    return 0


def write_tflite(path, weights):
    """The network as a TensorFlow Lite file: the same tensors and the
    same operator (no bias: -1), so that the kit's loader makes of it the
    graph the bundle is for. False without the modules it takes."""
    try:
        import flatbuffers
        from ai_edge_litert import schema_py_generated as schema
    except ImportError:
        return False

    def tensor(name, shape, scale, buffer=0):
        result = schema.TensorT()
        result.name = name
        result.shape = list(shape)
        result.type = schema.TensorType.INT8
        result.buffer = buffer
        result.quantization = schema.QuantizationParametersT()
        result.quantization.scale = [scale]
        result.quantization.zeroPoint = [0]
        return result

    options = schema.Conv2DOptionsT()
    options.padding = schema.Padding.VALID
    options.strideW = options.strideH = 1
    options.dilationWFactor = options.dilationHFactor = 1
    options.fusedActivationFunction = schema.ActivationFunctionType.NONE
    operator = schema.OperatorT()
    operator.opcodeIndex = 0
    operator.inputs = [0, 1, -1]
    operator.outputs = [2]
    operator.builtinOptionsType = schema.BuiltinOptions.Conv2DOptions
    operator.builtinOptions = options

    code = schema.OperatorCodeT()
    code.deprecatedBuiltinCode = code.builtinCode = schema.BuiltinOperator.CONV_2D
    code.version = 1
    subgraph = schema.SubGraphT()
    subgraph.name = "conv2d"
    subgraph.tensors = [tensor("input", (1, H, W, IC), INPUT_SCALE),
                        tensor("weights", (OC, 1, 1, IC), WEIGHT_SCALE, 1),
                        tensor("output", (1, H, W, OC), OUTPUT_SCALE)]
    subgraph.operators = [operator]
    subgraph.inputs = [0]
    subgraph.outputs = [2]
    model = schema.ModelT()
    model.version = 3
    model.description = "1x1 convolution of the amdxdna conv2d design bundle"
    model.operatorCodes = [code]
    model.subgraphs = [subgraph]
    model.buffers = [schema.BufferT(), schema.BufferT()]
    model.buffers[1].data = np.frombuffer(weights.tobytes(), dtype=np.uint8)
    builder = flatbuffers.Builder(8192)
    builder.Finish(model.Pack(builder), b"TFL3")
    path.write_bytes(bytes(builder.Output()))
    return True


def conv2d(design, bundles):
    design, bundles = Path(design), Path(bundles)
    act, weights = test_data()
    acc = sums(act, weights)
    expected = kit_output(acc)
    assert np.array_equal(expected, design_output(acc, "away")), \
        "the kit and the design disagree on the test input"

    key = graph_key()
    directory = bundles / key
    directory.mkdir(parents=True, exist_ok=True)
    for name in ("design.pdi", "insts.bin"):
        shutil.copyfile(design / name, directory / name)

    # the network's own layout: what the kit has in memory
    weights.tofile(directory / "weights.bin")
    act.tofile(directory / "input.bin")
    expected.tofile(directory / "expected.bin")
    # the same in the design's layouts, for xdna_test (without the kit)
    to_design_activations(act, IC).tofile(directory / "input0.bin")
    to_design_weights(weights).tofile(directory / "input1.bin")
    to_design_activations(expected, OC).tofile(directory / "expected2.bin")

    size = W * H * IC
    (directory / "bundle.txt").write_text(f"""\
# Design bundle for the Neural Kit's amdxdna back end (make-bundle.py).
format=1
name=conv2d_1x1_{W}x{H}x{IC}_{OC}
# the network this is for, and its stretch: operators first:count
graph={key}
operators=0:1
# the constants those operators read (here: the weights)
weights={weights_digest(weights)}
# the design: mlir-aie programming_examples/ml/conv2d, one column of an NPU2
device=npu2_1col
columns=1
ops_per_cycle=2048
pdi=design.pdi
insts=insts.bin
opcode=3
insts_size=words
timeout_ms=5000
# the design's buffers in the order of its kernel's arguments:
# <in|out|const>:<bytes>:<tensor of the stretch>:<layout>
buffer=in:{size}:op0.in0:hcwc8
buffer=const:{IC * OC}:op0.in1:oiio8
buffer=out:{W * H * OC}:op0.out0:hcwc8
# what it was made for (the graph key holds all of this):
# CONV_2D 1x1, stride 1, valid padding, no bias, no activation
# input   int8 [1, {H}, {W}, {IC}] scale {INPUT_SCALE!r} zero point 0
# weights int8 [{OC}, 1, 1, {IC}] scale {WEIGHT_SCALE!r} zero point 0, all even
# output  int8 [1, {H}, {W}, {OC}] scale {OUTPUT_SCALE!r} zero point 0
""")
    # for amdxdna_backend_test, which builds this network with the kit
    (directory / "model.txt").write_text(f"""\
operator=CONV_2D
input={H},{W},{IC},{INPUT_SCALE!r}
weights={OC},{IC},{WEIGHT_SCALE!r},weights.bin
output={H},{W},{OC},{OUTPUT_SCALE!r}
test_input=input.bin
test_expected=expected.bin
""")
    # for "xdna_test run": the design alone on the same numbers
    (directory / "manifest.txt").write_text(f"""\
name=conv2d_bundle
device=npu2_1col
columns=1
pdi=design.pdi
insts=insts.bin
opcode=3
buffer=in:input0.bin:{size}
buffer=in:input1.bin:{IC * OC}
buffer=out:expected2.bin:{W * H * OC}
""")
    if not write_tflite(directory / "model.tflite", weights):
        print("no model.tflite: needs the flatbuffers and ai_edge_litert modules",
              file=sys.stderr)
    print(directory)
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "conv2d":
        sys.exit(conv2d(sys.argv[2], sys.argv[3]))
    if len(sys.argv) == 2 and sys.argv[1] == "analyse":
        sys.exit(analyse())
    sys.exit(__doc__)
