# linux-drm for air/OS

Linux's DRM core, TTM, the GPU scheduler, dma-buf fences and the AMD GPU
driver (amdgpu, Display Core, power management), built as a Haiku kernel
add-on for air/OS.

- `linux/` - the Linux sources this port compiles, imported unchanged by
  `tools/import-linux.sh` from a stable release (see `linux/VERSION`). Haiku
  changes to them are separate commits, so a re-import shows them as a diff.
- `haiku/compat/` - the Linux kernel API on Haiku's kernel.
- `haiku/amdgpu/` - the Haiku driver: PCI device, interrupts, `/dev/dri`
  nodes, memory mapping into teams.
- `haiku/amdxdna/` - a second add-on from the same compat layer and DRM
  core: Linux's driver for the NPU of AMD Ryzen AI processors (see "The AMD
  NPU" below).

Licensing (details in COPYING): the Linux files keep their own licences
(most of DRM and amdgpu is MIT; dma-buf, parts of the DRM core and the
copied Linux library code are GPL-2.0). The add-on built from them is
therefore distributed under the GPL-2.0; the compat layer and the Haiku
driver code are MIT. No firmware is kept here.

## Stock Haiku releases

`haiku/tools/build-release.sh r1beta6` builds the add-on and the amdgpu_kms
accelerant against Haiku R1/beta6 itself (its packages from Haiku's
repository, its headers, its kernel and libroot) and packages them for it
(`out-packages-r1beta6/`); the package requires `amdgpu_firmwares`
(`haiku/tools/package.sh`). On a stock Haiku, app_server drives the monitors
as one screen through the accelerant's classic hooks; arranging and scaling
them one by one needs air/OS's app_server. Southern and Sea Islands GPUs stay
with Haiku's radeon_hd unless `si_support 1` / `cik_support 1` are set.

Verified so far in QEMU on a stock R1/beta6 (no AMD GPU there): pkgman
installs the package, and with `haiku_selftest 1` in the driver settings the
add-on starts the DRM core and amdgpu and passes its self test (19 checks of
the compat layer). Not yet run on beta6 with a GPU. The GPU user land (Mesa,
libdrm, libva) is not built for releases yet.

## The AMD NPU (amdxdna)

`make -f haiku/build/Makefile -j32 amdxdna HAIKU_TOP=<haiku tree>
HAIKU_BUILD=<configured haiku build dir>` builds `out-xdna/amdxdna`, the
driver for the NPU of Ryzen AI processors (PCI 1022:1502 Phoenix and Hawk
Point, 1022:17f0 Strix Point, Strix Halo and Krackan Point). It is an add-on
of its own, not part of `amdgpu_drm`: a laptop runs both. It publishes
`/dev/npu/amdxdna/0` with Linux's ioctls (`linux/include/uapi/drm/
amdxdna_accel.h`), so that user code written for Linux's driver ports with
little more than `open()`; what Linux does with `mmap()` is
`DRM_HAIKU_IOCTL_MAP`, as for `amdgpu_drm` (`haiku/compat/include/drm/
drm_haiku.h`).

**Sources.** `linux/drivers/accel/amdxdna` is Linux 7.4-rc2's
(`tools/import-amdxdna.sh`, version in `linux/drivers/accel/amdxdna/VERSION`),
on the 6.18.52 DRM core. 6.18.52's own amdxdna can only run with an IOMMU:
it hands the NPU the address space of the process (PASID, shared virtual
addressing). Haiku has no IOMMU driver. Linux 7.2 gave the driver a second
mode ("carveout", `amdxdna_cbuf.c`): all buffers are ranges of one piece of
physical memory and the NPU is given physical addresses. Backporting that to
6.18.52 would have meant rewriting its buffer code; the newer driver compiles
against the 6.18.52 core with a handful of compat additions (`lx/accel.h`),
and also brings the newer command formats. (Its command wait ioctl answers
`EOPNOTSUPP` on these NPUs: a client waits for a command on the hardware
context's sync object, `DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT` with the command's
sequence number as the point, as `xdna_test.c` does.) Two changes to its files, both marked in them:
`aie2_pci.c` starts without an IOMMU group, `aie_psp.c` takes the firmware
buffer from physically contiguous pages (Haiku's kernel heap is not).

**Memory and addresses.** When the driver is loaded, it takes a run of wired
pages and keeps it until it is unloaded: 128 MiB unless `carveout_mb` in the
settings says otherwise. Such a run is only to be had while the machine is
fresh (after some hours of use Haiku's page allocator finds none, at once),
so the driver's package has launch_daemon look into `/dev/npu` at every boot
(`data/launch/amdxdna`, from `haiku/amdxdna/launch`), which loads the driver
and touches no hardware. The run is looked for 64 MiB aligned first, then
anywhere (it will do if an aligned heap fits into it), then with 96 and 72
MiB. If there is none, the driver stays loaded without and looks again
whenever the NPU is started. `carveout_at_load 0` in the settings leaves the
memory alone until the NPU is first started: nothing is held on a machine
that never uses its NPU, and the NPU may then not be usable when it is
wanted. (devfs loads the driver anew when its file is replaced: the run is
given back and looked for again.) Every buffer object is a range of it: each client's device heap
(64 MiB, aligned to its size: the designs' configuration and instructions),
command buffers and data buffers. So 128 MiB serve one client with 64 MiB of
data; every further client needs 64 MiB more for its heap.
`DRM_IOCTL_AMDXDNA_GET_BO_INFO` returns a data buffer's physical address as
`xdna_addr`, which goes into commands where Linux's clients put the address
of their mapping. The sync ioctl answers `EOPNOTSUPP` for these buffers, as
on Linux in this mode; a client flushes the cache lines itself
(`haiku/tests/xdna_test.c`).

**No isolation.** The NPU reads and writes whatever physical address a
command names: instruction sequences carry buffer addresses in a form the
kernel cannot check (the firmware patches them into the NPU's DMA
descriptors). A client can therefore have the NPU read or overwrite any
memory of the machine. The driver lets only root open the node, and only for
reading and writing (a program that merely opens files in /dev must not
start the NPU). Clients are not protected from each other either. Giving the
node to unprivileged programs needs an IOMMU driver first.

**Power.** Loading the driver touches no hardware: devfs loads it when
`/dev/npu` is first looked into (the launch job, or when the binary is
installed), and it only finds the device in the PCI bus manager's list and
takes the buffer memory. The first open starts the Linux side, loads the
firmware
(`data/firmware/amdnpu/<device>_<revision>/npu_7.sbin`, else `npu.sbin`, in
the usual firmware directories) and powers the NPU up; five seconds after
the last client has closed the node (`autosuspend_ms` in the settings; 0: at
once) the driver's suspend path powers it down again (firmware stopped, the
SMU told to cut power, bus mastering off), and the next open powers it up.
Programs that open the node one after the other find it running, and SMU,
PSP and firmware are not sent through a power cycle for each of them.
`keep_powered 1` never powers it down once it was started (a bring-up aid).
Linux powers it down after five idle seconds even with the node open, and
puts the PCI function into D3hot; here it is powered for as long as the node
is open and stays in D0, as before the driver was loaded.
When the system suspends to idle (the kernel's deep sleep; power hooks), an
NPU in use is suspended through the same path and resumed afterwards; one
not in use is left alone. A start that stops the machine is not repeated:
`~/config/settings/kernel/drivers/amdxdna.starting` exists during the first
start after a boot, and while it exists the NPU is not started.

**Bring-up log.** With `log_file <path>` in the settings every message also
goes into that file, synced line by line (the file has to exist, filled
with zeros). `step_log` says how much is written before and after the steps
that touch the hardware (`lx_step()` in the Linux files): 1, the default,
every SMU and PSP command, the waits for the firmware, the interrupt
handlers installed and removed, contexts made and destroyed; 2 every message
to and from the firmware as well; 0 nothing. After each power down there is
a line of counts (`counts at power down <n>: …`: memory allocated by the
Linux side and its largest allocation sites, interrupt handlers installed,
the system's threads and semaphores): what is taken and not given back
shows as a number that grows. A machine that stopped is read from the end
of that file.

**Firmware and packages.** `haiku/tools/package-amdxdna.sh [out dir
[linux-firmware dir [LICENSE.amdnpu]]]` makes `amdnpu_firmwares` (the
`amdnpu` files of linux-firmware, unchanged, with their licence) and
`amdxdna` (the driver, linked from `dev/npu`, and the launch job that loads
it at boot). No firmware is kept here.

**Tests.** `haiku/tests/xdna_test.c` (C, no XRT): `info` prints the NPU's
geometry and versions, `buffers` exercises buffer objects and mappings, `run
<dir>` loads a design (raw PDI and instruction sequence, as mlir-aie's aiecc
writes them) and runs it against expected output.
`haiku/tests/xdna/build-designs.sh` builds two such designs on a Linux host
from mlir-aie's examples with its pip wheels (no XRT, no Vitis), for one
column of the NPU of Strix and Krackan Point: `passthrough_kernel` (output =
input) and `conv2d` (a 1x1 int8 convolution, 32x32, 64 to 64 channels);
`reference.py` there makes the inputs and the expected outputs and checks a
captured output (`xdna_test -o`). The convolution's expected output follows
the kernel's source; whether the NPU rounds exact halves the same way shows
only on hardware (`reference.py check` says if mismatches are such halves). Without an NPU: `haiku_selftest 1` in the
settings runs the driver's checks of the compat layer when it is loaded,
`haiku_selftest 2` makes the node a device without hardware: buffers and
mappings are real (`xdna_test buffers`), questions about the array get
made-up answers, hardware contexts hold nothing and every command is
refused (`fake_submit_error <errno>`: 19, the default, as by a device that
is gone, 5 as a command that failed), which is what the Neural Kit's back end is tried against in a
virtual machine (`ADDON=amdxdna haiku/tools/qemu-vm.sh selftest`).
`haiku_selftest 3` goes further (`haiku/amdxdna/fakefw.c`): amdxdna's own
code for the hardware runs against a stand-in, a PCI function with MSI-X of
the virtual machine (QEMU: `-device megasas-gen2`) dressed up as the NPU,
whose BARs are memory in which a thread plays SMU, PSP and firmware: power
on and off, the firmware's start, the mailbox channels with their
interrupts (on the vectors Haiku allotted to the MSI-X entries), contexts
and commands, which are answered and compute nothing (`xdna_test -x`). So
starting, stopping, power down and up and the life of a context can be run
thousands of times, by several programs at once
(`haiku/tests/xdna/stress.sh`); a build with `KMALLOC_DEBUG=1` checks every
`kfree()` meanwhile and keeps freed memory for a while to see who still
writes to it. `xdna_test set <parameter> <value>` changes the driver's
parameters while it runs (how soon the NPU is powered down, for one). This is how two faults of the
compat layer were found that the first sessions on hardware ran into (a
kernel thread's task freed twice when the thread's end and `kthread_stop()`
met; an interrupt thread spinning for a lock whose first user it had
preempted).
`haiku/tests/xdna/make-bundle.py` turns the conv2d design into a design
bundle for that back end (`amdxdna.neural` in the Haiku tree) and says where
the design's arithmetic and the kit's agree;
`haiku/tests/xdna/FIRST-LOAD.md` is the checklist for the first start on
hardware.
`haiku/tools/check-kernel-symbols.sh <add-on> <kernel_x86_64>` checks a build
against the kernel of the machine it is meant for.

**Both add-ons at once.** Each embeds its own copy of the compat layer and
the DRM core: Haiku's kernel resolves an add-on's symbols against the kernel
only, and the add-ons are linked with `-Bsymbolic`, so nothing of one is
visible to the other. Their device nodes are in different directories (the
NPU's DRM minor is an "accel" minor of its own DRM core, and no `/dev/dri`
node). Settings, log and marker files and lock names carry the add-on's
name. `haiku/compat/src/accel.c` and `lx/accel.h` hold what the NPU driver
adds to the compat layer; what would change `amdgpu_drm`'s behaviour is
compiled into the NPU add-on only (`LX_ADDON_AMDXDNA`).
