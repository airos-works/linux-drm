#!/bin/bash
# Copyright 2026, air/OS. Distributed under the terms of the MIT License.
#
# Builds the two NPU test designs for xdna_test with the open mlir-aie tool
# chain from pip wheels (no XRT, no Vitis): passthrough_kernel and conv2d, for
# the one column NPU2 device (npu2_1col: Strix / Krackan Point). Runs on a
# Linux x86_64 host; nothing here touches an NPU.
#
#   haiku/tests/xdna/build-designs.sh [MLIR_AIE_SOURCE]
#
# MLIR_AIE_SOURCE   mlir-aie source tree, for its examples (default below). The
#                   mlir_aie wheel has to be the one built from its commit.
# VENV              Python environment to create or reuse
# OUT               output directory, one directory per design
# BUILD             scratch directory (removed design by design unless KEEP_BUILD=1)
# WHEEL_DIR         directory with the two wheels already downloaded, used
#                   instead of the release pages (those can be unreachable)
# WITH_XCLBIN=0     skip the xclbin. The wheels have no xclbinutil; by default
#                   the XRT-free one in MLIR_AIE_SOURCE/third_party is compiled
#                   with the host's cmake and C++ compiler (a few seconds).
#
# Each design directory gets design.pdi, insts.bin, the inputs and expected
# outputs, manifest.txt (what xdna_test reads) and NOTES.txt.
set -euo pipefail

SRC=$(realpath "${1:-${MLIR_AIE_SOURCE:-/mnt/HaikuWork/tmp/xdna/research/repos/mlir-aie}}")
VENV=${VENV:-/mnt/HaikuWork/tmp/xdna/venv}
OUT=${OUT:-/mnt/HaikuWork/tmp/xdna/designs}
BUILD=${BUILD:-$(dirname "$OUT")/build}
MLIR_AIE_VERSION=${MLIR_AIE_VERSION:-1.4.6.dev15+ge5a3fa1}
MLIR_AIE_WHEELS=https://github.com/Xilinx/mlir-aie/releases/expanded_assets/latest-wheels-4/
HERE=$(dirname "$(realpath "$0")")
PY=$VENV/bin/python

# The examples and the compiler have to come from the same commit.
commit=$(git -C "$SRC" rev-parse HEAD 2>/dev/null || true)
[[ $commit == "${MLIR_AIE_VERSION##*+g}"* ]] \
	|| echo "warning: $SRC is at '${commit:0:7}', the wheel is $MLIR_AIE_VERSION" >&2

# The environment: mlir_aie (aiecc, bootgen, the IRON Python API; pulls numpy),
# and llvm-aie (Peano, the compiler for the AIE cores) as the source tree pins it.
installed() {
	"$PY" -c "import importlib.metadata as m, sys; sys.exit(m.version('$1') != '$2')" 2>/dev/null
}
peano_version=$(sed -n 's/^llvm-aie==//p' "$SRC/utils/peano-requirements.txt")
if ! installed mlir_aie "$MLIR_AIE_VERSION" || ! installed llvm-aie "$peano_version"; then
	[ -x "$PY" ] || python3 -m venv "$VENV"
	if [ -n "${WHEEL_DIR:-}" ]; then
		"$PY" -m pip install --no-cache-dir -f "$WHEEL_DIR" \
			"mlir_aie==$MLIR_AIE_VERSION" "llvm-aie==$peano_version"
	else
		"$PY" -m pip install --no-cache-dir "mlir_aie==$MLIR_AIE_VERSION" -f "$MLIR_AIE_WHEELS"
		"$PY" -m pip install --no-cache-dir -r "$SRC/utils/peano-requirements.txt"
	fi
fi
versions=$("$PY" -c "import importlib.metadata as m, sys
print('python ' + sys.version.split()[0], *['%s %s' % (p, m.version(p))
	for p in ('mlir_aie', 'llvm-aie', 'numpy')], sep=', ')")

mkdir -p "$BUILD" "$OUT"
export NPU_CACHE_HOME=$BUILD/cache		# keeps the JIT's cache out of ~/.npu

# xclbinutil, only for the xclbin: built from the copy in the source tree.
unset AIE_XCLBINUTIL
if [ "${WITH_XCLBIN:-1}" = 1 ] && command -v cmake >/dev/null; then
	if [ ! -x "$BUILD/hrx-xclbinutil/bld/tools/hrx-xclbinutil" ]; then
		rm -rf "$BUILD/hrx-xclbinutil"
		cp -r "$SRC/third_party/hrx-xclbinutil" "$BUILD/hrx-xclbinutil"
		cmake -S "$BUILD/hrx-xclbinutil" -B "$BUILD/hrx-xclbinutil/bld" \
			-DCMAKE_BUILD_TYPE=Release >"$BUILD/hrx-xclbinutil.log"
		cmake --build "$BUILD/hrx-xclbinutil/bld" --target hrx-xclbinutil -j "$(nproc)" \
			>>"$BUILD/hrx-xclbinutil.log"
	fi
	export AIE_XCLBINUTIL=$BUILD/hrx-xclbinutil/bld/tools/hrx-xclbinutil
else
	echo "no xclbin: WITH_XCLBIN=0, or cmake is missing"
fi

# Compiles one example, unmodified, from the source tree: its IRON description
# becomes MLIR, the kernel is compiled with Peano, and one aiecc run writes
# every artifact. The examples themselves would need an NPU: they compile for
# the device XRT finds, and then run on it.
build_design() {		# <name> <example .py> <design object> <specialize arguments>
	local name=$1 work=$BUILD/$1 dir=$OUT/$1
	rm -rf "$work"
	mkdir -p "$work" "$dir"
	"$PY" - "$SRC/programming_examples/$2" "$3" "$4" "$work" <<'EOF'
import importlib.util, logging, os, sys
example, design_name, arguments, work = sys.argv[1:]

# The log gets the exact command lines of the kernel compile and of aiecc.
logging.basicConfig(filename=f"{work}/build.log", level=logging.WARNING, format="%(message)s")
logging.getLogger("aie.utils.compile").setLevel(logging.DEBUG)

from aie.iron.device import from_name
from aie.utils.compile import compile_mlir_module
from aie.utils.hostruntime import set_current_device

device = from_name("npu2", n_cols=1)		# as conv2d.py does: NPU2Col1 = npu2_1col
set_current_device(device)
spec = importlib.util.spec_from_file_location("example", example)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
mlir = getattr(module, design_name).specialize(**eval(arguments)).as_mlir()
compile_mlir_module(
    mlir, work_dir=work, device=device,
    pdi_path=f"{work}/design.pdi", insts_path=f"{work}/insts.bin",
    elf_path=f"{work}/insts.elf",		# the instructions wrapped in an ELF
    xclbin_path=f"{work}/final.xclbin" if "AIE_XCLBINUTIL" in os.environ else None,
    options=["--no-progress",
             "--get-full-elf", f"--full-elf-name={work}/full.elf",	# PDI + instructions
             "--get=npu_lowered.mlir,kernels_{0}.json,partition_{0}.json,memTopology_{0}.json"])
EOF
	rm -f "$dir"/*
	cp "$work"/{design.pdi,insts.bin,insts.elf,full.elf,aie.mlir,npu_lowered.mlir,build.log} "$dir/"
	cp "$work/kernels_main.json" "$dir/kernels.json"
	cp "$work/partition_main.json" "$dir/partition.json"
	cp "$work/memTopology_main.json" "$dir/mem_topology.json"
	[ -z "${AIE_XCLBINUTIL:-}" ] || cp "$work/final.xclbin" "$dir/"
	"$PY" "$HERE/reference.py" gen "$name" "$dir"
	write_notes "$name" "$2" >"$dir/NOTES.txt"
	[ "${KEEP_BUILD:-0}" = 1 ] || rm -rf "$work"
	echo "built $dir"
}

write_notes() {		# <name> <example .py>
	local dir=$OUT/$1
	cat <<EOF
$1 for the AMD NPU2 (AIE2P), device npu2_1col: one column, placed in column 0
(shim tile 0,0, core tile 0,2). Written by build-designs.sh.

Source: mlir-aie ${commit:-unknown}, programming_examples/$2, not modified.
The example compiles for the device XRT detects (passthrough_kernel: all of
npu2) or for one column (conv2d); here both are built for one column.
Tools: $versions
Compiler: Peano (llvm-aie) for the core, no xchesscc. Bootgen is inside aiecc.

Commands (build.log; reference.py then wrote the buffers and manifest.txt):
$(sed -e 's/^Compiling with: /  kernel: /' -e 's/^Running: /  aiecc: /' "$dir/build.log" | grep '^  ')

Files (bytes, name, sha256):
$(cd "$dir" && for file in *; do
	[ "$file" = NOTES.txt ] || printf '  %7d  %-18s %s\n' "$(stat -c %s "$file")" "$file" \
		"$(sha256sum "$file" | cut -c1-64)"; done)
  design.pdi  the configuration (bootgen image of the CDOs: core ELF, tile
              init, enable); final.xclbin and full.elf hold these same bytes
  insts.bin   the runtime sequence as a transaction, little endian uint32 words
  insts.elf   insts.bin in an ELF (xrt::elf + xrt::module); full.elf: PDI and
              instructions in one ELF (newer XRT flow, no xclbin)
  final.xclbin  absent when built with WITH_XCLBIN=0: made by hrx-xclbinutil
              (third_party/hrx-xclbinutil of the source tree) from
              mem_topology.json, kernels.json and partition.json. It differs
              from build to build (aiecc draws a new PDI uuid for
              partition.json); design.pdi and insts.bin do not.

Buffers (manifest order = runtime_sequence arguments = kernel bo0, bo1, ...):
$("$PY" "$HERE/reference.py" describe "$1")

Runtime sequence (aie.mlir); each shim DMA transfer covers its whole buffer,
"len" is in elements, here bytes, and equals the manifest size:
$(grep -E 'runtime_sequence|aie.dma_bd\(%arg' "$dir/aie.mlir" | sed 's/^ */  /')

Transaction (insts.bin; "reference.py insts"). A DDR_PATCH (operation 0x81,
12 words: register address in word 6, argument index in word 8, offset in
words 10-11) stands for aiex.npu.address_patch {addr, arg_idx} with arg_plus
in npu_lowered.mlir: the host address of buffer argument arg_idx, plus
arg_plus, goes into the buffer descriptor whose blockwrite precedes it
(descriptor word 0 is the length in 32 bit words, word 1 the address). Nothing
else in insts.bin depends on host addresses. Argument N is buffer N of the
manifest. aiecc adds 0x80000000 to arg_plus only for arguments 5 and up
(--fold-ddr-addr-offset, on by default); these designs have none. Its source
(include/aie/Runtime/TxnEncoding.h) gives the reason: "NPU firmware adds
kDDRAIEAddrOffset [0x80000000] to host buffer addresses for only the first
kNumFirmwareTranslatedArgs [5] args".
$("$PY" "$HERE/reference.py" insts "$dir/insts.bin" | sed 's/^/  /')

Kernel of the xclbin (kernels.json): name MLIR_AIE, instance MLIRAIE, type dpu,
dpu_kernel_id 0x901; arguments (name, offset, type):
$("$PY" -c "import json, sys
for a in json.load(open(sys.argv[1]))['ps-kernels']['kernels'][0]['arguments']:
	print('  %-7s %s  %-9s %s' % (a['name'], a['offset'], a['type'], a.get('memory-connection', 'scalar')))
" "$dir/kernels.json")
aiecc always declares at least bo0-bo4; a host passes only the buffers of the
runtime sequence, in slots 3, 4, ... (no dummy buffers in these examples).
partition.json: column_width 1, start_columns 0-7, operations_per_cycle 2048.

Host code of mlir-aie (not run here):
  Python (aie.utils.hostruntime.xrtruntime, used by the examples):
    instructions: pyxrt.bo(device, bytes of insts.bin, pyxrt.bo.cacheable,
      kernel.group_id(1)), filled and synced to the device
    each buffer: pyxrt.bo(device, bytes, pyxrt.bo.host_only, 0); all of them
      synced to the device before the run, and from the device when read
    run = pyxrt.run(kernel); run.set_arg(i, ...) for
      [3, instructions bo, BYTES of insts.bin, bo0, bo1, ...]; start(); wait()
  C++ (runtime_lib/test_lib/xrt_test_wrapper.h, basic/passthrough_dmas/test.cpp):
    xrt::bo(device, bytes, XCL_BO_FLAGS_CACHEABLE, kernel.group_id(1)) for the
    instructions, xrt::bo(device, bytes, XRT_BO_FLAGS_HOST_ONLY,
    kernel.group_id(3 + N)) for buffer N; kernel(opcode = 3, instructions bo,
    WORDS of insts.bin, bo0, bo1, ...). So the third argument is a byte count
    in the Python runtime and a word count in the C++ tests.
EOF
}

build_design passthrough_kernel basic/passthrough_kernel/passthrough_kernel.py \
	my_passthrough_kernel "dict(n=4096)"
build_design conv2d ml/conv2d/conv2d.py conv2d \
	"dict(width=32, height=32, in_channels=64, out_channels=64, scale=10, fuse_relu=False)"

# Checks the conv2d model of reference.py: against mlir-aie's own numpy model
# and, with a host compiler, against the scalar path of the kernel source
# (compiled as it is, with empty stand-ins for the AIE headers it includes).
scalar=
if command -v g++ >/dev/null; then
	rm -rf "$BUILD/scalar"
	mkdir -p "$BUILD/scalar"/{conv,common,stub/aie_api}
	cp "$SRC/aie_kernels/conv/conv2dk1_i8.cc" "$BUILD/scalar/conv/"
	touch "$BUILD/scalar"/{common/table_i8.h,conv/conv_bias.h,stub/aie_api/aie.hpp}
	echo 'static inline void event0(void) {} static inline void event1(void) {}' \
		>"$BUILD/scalar/aie_kernel_utils.h"
	scalar=$BUILD/scalar/libconv2dk1_i8_scalar.so
	g++ -O1 -shared -fPIC -DSCALAR -DINT8_ACT -I"$BUILD/scalar/stub" -o "$scalar" \
		"$BUILD/scalar/conv/conv2dk1_i8.cc"
fi
"$PY" "$HERE/reference.py" selftest $scalar
du -sh "$VENV" "$OUT"
