# First load of amdxdna on the Framework 13 (Ryzen AI 5 340)

State on 2026-11-09 (firmware 1.1.2.64, 8 columns, 6 rows): steps 3 to 5
have passed on the laptop, the NPU powered up and down some ten times, and
the convolution's output was the expected one byte for byte: the NPU rounds
exact halves away from zero, as the kit does. Then the laptop stopped
answering at the start of one more `xdna_test run` (cause not known yet; see
"If the machine stops"). Step 6, the memory taken at load and the launch job
have run in a virtual machine only. There is no remote power control: have
somebody at the machine. Stop at the first step whose check fails and keep
the syslog (`/var/log/syslog`, lines with `amdxdna`) and the log file.

Host paths are those of the build server; `$L` is the laptop
(`FW_URL=http://<laptop>:7780/mcp`, `fw13/tools/fwput`, `fwsh`), `$NP` is
`/boot/home/config/non-packaged` there.

## 0. Before

- [ ] `uname -a` on the laptop still says `hrev60207+2496`. If not: export
      the headers of the commit it runs
      (`git -C src/haiku archive <commit> headers build/config_headers | tar -x -C <dir>`),
      rebuild with `HAIKU_TOP=<dir>`, and run step 1's check against the
      laptop's `/boot/system/kernel_x86_64`.
- [ ] Nobody else is working on the laptop; unsaved work is saved.
- [ ] `ls /boot/home/config/settings/kernel/drivers/amdxdna.starting` says
      there is no such file (see "The marker").

## 1. Build (host)

```
cd /mnt/HaikuWork/src/forks/linux-drm-xdna
make -f haiku/build/Makefile -j24 amdxdna HAIKU_TOP=/mnt/HaikuWork/tmp/xdna/haiku-e0f9ff15cf
haiku/tools/check-kernel-symbols.sh out-xdna/amdxdna <kernel_x86_64 fetched from the laptop>
. /data1/airos/sdk/x86_64/env.sh
${CROSS}gcc --sysroot=$SYSROOT -O2 -Ilinux/include/uapi -Ihaiku/compat/include \
    -o out-xdna/tests/xdna_test haiku/tests/xdna_test.c
haiku/tests/xdna/build-designs.sh                    # designs/passthrough_kernel, designs/conv2d
/mnt/HaikuWork/tmp/neural/venv/bin/python haiku/tests/xdna/make-bundle.py conv2d \
    /mnt/HaikuWork/tmp/xdna/designs/conv2d /mnt/HaikuWork/tmp/xdna/bundles
```

- [ ] the symbol check says `0 problem(s)`
- [ ] in the Haiku tree (branch neural-kit): `jam -q libneural.so neural amdxdna.neural amdxdna_backend_test`

## 2. Files onto the laptop (nothing starts)

- [ ] firmware: `linux-firmware/amdnpu/17f0_10/npu.sbin.1.1.2.64` as
      `$NP/data/firmware/amdnpu/17f0_10/npu_7.sbin` and `npu.sbin.1.0.0.63`
      as `.../npu.sbin` (or install the `amdnpu_firmwares` package)
- [ ] `~/xdna/`: `xdna_test`, `designs/`, `bundles/`, `reference.py`,
      `amdxdna_backend_test`
- [ ] for a log that survives a hang:
      `dd if=/dev/zero of=/boot/home/amdxdna.log bs=1M count=4` and
      `log_file /boot/home/amdxdna.log` in
      `~/config/settings/kernel/drivers/amdxdna`
- [ ] the launch job that loads the driver at every boot (the package has it
      in `data/launch`): `haiku/amdxdna/launch` as
      `$NP/data/launch/amdxdna`
- [ ] the kit: `libneural.so` to `$NP/lib/`, `neural` to `$NP/bin/`,
      `amdxdna.neural` to `$NP/add-ons/neural/` (skip what the installed
      system already has)

## 3. The driver (loads, takes its memory, touches no hardware)

Never copy a new file straight into a drivers directory: stage it outside
and move it in.

```
cp amdxdna /boot/home/amdxdna.staged && chmod 755 /boot/home/amdxdna.staged
mkdir -p $NP/add-ons/kernel/drivers/bin $NP/add-ons/kernel/drivers/dev/npu
ln -sf ../../bin/amdxdna $NP/add-ons/kernel/drivers/dev/npu/amdxdna
mv /boot/home/amdxdna.staged $NP/add-ons/kernel/drivers/bin/amdxdna
```

- [ ] syslog: `amdxdna: NPU 1022:17f0 rev 20 at 194:0.1: /dev/npu/amdxdna/0 starts it when opened`,
      then `amdxdna: buffer memory: 128 MiB at 0x… (found in … ms)`. On a
      machine that has been up for hours: `no … MiB of contiguous memory
      free` for each size and `no contiguous memory for the NPU's buffers`
      instead; the driver stays loaded, and the memory is there after the
      next boot (the launch job) - skip to that
- [ ] `ls /dev/npu/amdxdna` shows `0`; no other `amdxdna` line (nothing
      about firmware or power)
- [ ] `neural devices` lists `amdxdna/0` with `path` and `driver` only, and
      still no further `amdxdna` line in the syslog

Replacing the driver later is the same `mv` with the node closed (syslog:
`devfs: reload driver`, `uninit_driver`, and the lines above again: the
memory is given back and taken anew, which on a machine that has been up
for long can fail).

## 4. First start: `xdna_test info`

Run as root (the default user is). This opens the node: firmware, power
(and the buffer memory, if there was none at load).

- [ ] syslog, in this order:
      `starting the NPU: marker made` ·
      `starting <PCI name> (1022:17f0 rev 20)` ·
      `Load firmware amdnpu/17f0_10/npu_7.sbin` ·
      `msix configured for … vectors` (the PCI bus manager) ·
      `Initialized amdxdna_accel_driver 0.10.0 … on minor 0` ·
      `Use carveout mem` ·
      `… started: firmware <a.b.c.d>, NPU Krackan …`
- [ ] the program prints the array (8 columns and 6 rows are expected),
      the firmware version, then `PASS`; the syslog ends with
      `NPU powered down`
- [ ] no `amdxdna.starting` file is left

What a failure looks like (the open fails, nothing hangs; every wait has a
timeout: 1 s for registers, 5 s for the firmware's answers):
`failed to request_firmware amdnpu/17f0_10/, ret -2` · `no … MiB of contiguous memory free` (reboot and
try soon after, or a smaller `carveout_mb`) · `smu cmd … timed out` ·
`PSP is not ready` · `firmware is not alive` · `MSI-X table does not read
back as configured` · `… MSI-X settings were lost`.

## 5. Buffers and designs, without the kit

- [ ] `xdna_test buffers` → `PASS`
- [ ] `xdna_test run designs/passthrough_kernel` → `ok: design loaded`,
      `command … completed in … us`, `4096 bytes as expected`, `PASS`
      (seen: 156 ms for the first command of a context, 112.5 ms for every
      further one, whatever the design - not understood yet; `-p 4`, turbo,
      tells whether the NPU's power mode is in it)
- [ ] `xdna_test -o conv.out run designs/conv2d` → `PASS`; if bytes differ:
      `reference.py check designs/conv2d conv.out` on the host says whether
      they are the exact halves the rounding mode decides
- [ ] `xdna_test run bundles/<key>` (the bundle's data, even weights) → `PASS`
- [ ] if a command is not answered, try `xdna_test -b run …` (the size of
      the instructions in bytes) and note which one works
- [ ] after each run the syslog says `powering the NPU down`, `NPU powered
      down`; the second run of anything says `powering the NPU up`, `NPU
      powered up` first (the resume path)
- [ ] the same many times: `xdna_test -n 200 run designs/conv2d`, and twenty
      times `xdna_test run designs/passthrough_kernel`

## 6. The Neural Kit

- [ ] `neural devices` → `amdxdna/0 NPU AMD AMD XDNA NPU (quantized)`;
      syslog: nothing new (with a session open elsewhere it also shows the
      firmware version and the array)
- [ ] `./amdxdna_backend_test -v --cycles 20 bundles/<key>` → ends with
      `(b) the NPU computed: PASS` and says how the NPU rounds halves.
      `(a) …` here means no program could be made: the syslog says why
      (`amdxdna.neural: …`)
- [ ] `NEURAL_AMDXDNA_BUNDLES=$PWD/bundles neural compare bundles/<key>/model.tflite -d amdxdna/0`
      → `amdxdna/0 runs 1 of 1 operators`, `0 of 65536 differ` (or only
      the halves, by one); exit status 4 and `FAILED … the CPU did its
      part` mean a command failed
- [ ] `xdna_test buffers` afterwards → `PASS` with as much room as in step 5
- [ ] to keep the bundle: `cp -r bundles/<key> $NP/data/neural/amdxdna/`

## 7. Later, one at a time

- lid close and open with nothing using the NPU (it must not matter);
  then with `xdna_test -n 2000 run …` running (`NPU powered down` before
  the sleep, `NPU powered up` after, the runs go on)
- a reboot: with the launch job the syslog has the driver's two lines
  (`… starts it when opened`, `buffer memory: …`) from the boot on and
  nothing else of `amdxdna` until something opens the node
  (`launch_roster info x-vnd.airos-amdxdna-reserve` shows the job)

## If the machine stops

Do not repeat the step. Look at the screen first (a kernel debugger's text
names the function). After the reboot, before anything opens the node: the
end of `/boot/home/amdxdna.log` (it is synced line by line; the syslog may
lack its last lines) says how far the driver was - `powering the NPU up`
without `NPU powered up`: in the resume path; `NPU powered up` last: in a
session (context, command) or right at its end; `powering the NPU down`
last: in the suspend path. `debug 0x2` in the driver's settings before the
next try adds the Linux driver's own debug lines to that log.

## The marker

`~/config/settings/kernel/drivers/amdxdna.starting` exists from just before
the NPU is started for the first time after a boot until the start has
ended, well or badly. If the machine stops in between, the file is still
there after the reboot and the driver refuses to start the NPU
(`the last start of the NPU did not finish`; opens fail, the kit uses the
CPU), so that no program can stop the machine again at every boot. Read the
log first; delete the file to allow another try.

## Undo

```
rm $NP/add-ons/kernel/drivers/bin/amdxdna $NP/add-ons/kernel/drivers/dev/npu/amdxdna
rm -r $NP/data/firmware/amdnpu $NP/data/neural/amdxdna $NP/data/launch/amdxdna
rm $NP/add-ons/neural/amdxdna.neural       # and libneural.so, neural, if put there
rm -f ~/config/settings/kernel/drivers/amdxdna ~/config/settings/kernel/drivers/amdxdna.starting
```

devfs unloads the driver when its binary goes (with the node closed);
`/dev/npu/amdxdna/0` is gone after that or after the next boot. The driver
changes nothing outside these files.
