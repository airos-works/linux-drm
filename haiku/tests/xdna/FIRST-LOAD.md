# First load of amdxdna on the Framework 13 (Ryzen AI 5 340)

State on 2026-11-09 (firmware 1.1.2.64, 8 columns, 6 rows): steps 3 to 5
have passed on the laptop, the NPU powered up and down some ten times, and
the convolution's output was the expected one byte for byte: the NPU rounds
exact halves away from zero, as the kit does. Then the laptop stopped
answering at the start of one more `xdna_test run`. Run against a stand-in
for the hardware in a virtual machine afterwards (`haiku_selftest 3`), the
same sessions stopped that machine too, in the second session: a fault of
the compat layer that frees a kernel thread's task twice when the thread's
end and `kthread_stop()` meet, which the laptop's kernel answers with a
panic (`object_cache: object … is already freed`) - over the network, a
machine that no longer answers. Every session stopped about five such
threads (interrupt handlers and work queues).
That fault and a second one found the same way are fixed; whether the first
was the laptop's is what "After the hang" below finds out. Step 6, the
memory taken at load, the launch job and the delayed power down have run in
a virtual machine only. There is no remote power control: have somebody at
the machine. Stop at the first step whose check fails and keep the syslog
(`/var/log/syslog`, lines with `amdxdna`) and the log file.

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

- [ ] syslog, in this order, with `step: …` lines between them (every SMU
      and PSP command and its answer, the interrupt handler installed):
      `starting the NPU: marker made` ·
      `starting <PCI name> (1022:17f0 rev 20)` ·
      `Load firmware amdnpu/17f0_10/npu_7.sbin` ·
      `msix configured for … vectors` (the PCI bus manager) ·
      `Initialized amdxdna_accel_driver 0.10.0 … on minor 0` ·
      `Use carveout mem` ·
      `… started: firmware <a.b.c.d>, NPU Krackan …`
- [ ] the program prints the array (8 columns and 6 rows are expected),
      the firmware version, then `PASS`; five seconds later the syslog
      ends with `NPU powered down` and a line of counts
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
- [ ] five seconds after a run the syslog says `powering the NPU down`, the
      steps of it (`step: …`), `NPU powered down` and a line of counts; the
      next run of anything says `powering the NPU up`, `NPU powered up`
      first (the resume path). Runs that follow one another sooner find the
      NPU running
- [ ] the same many times: `xdna_test -n 200 run designs/conv2d`, and twenty
      times `xdna_test run designs/passthrough_kernel` (see "After the
      hang" for the order: first without power cycles, `keep_powered 1`)

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

## After the hang of 2026-11-09

*Done 2026-11-10:* the old log ended with `NPU powered up` and the sync
errors of a run (the machine died in that session or its power down); the
fixed driver was loaded 70 s after the boot (128 MiB found in 22 ms), and
steps 4 to 7 all passed: `info`, 20 open/close cycles and 200 convolution
runs with `keep_powered 1`, 10 runs with a power down 7 s after each and 30
with `autosuspend_ms 0`, then the kit (`amdxdna_backend_test`: "the NPU
computed: PASS" once the back end flushed its output buffers before the
command, haiku 532d9f7aab; `neural compare` equal). The counts stayed level
(kmalloc 258 to 270 objects over 42 power downs). `-p 4` (turbo) makes a
pass-through command 3.9 ms instead of 112.5; the kit's bundle design takes
0.12 ms in the default mode. The launch job reserved the memory at the next
boot (found in 9 ms, 15.8 s after boot). Still to do: the lid test of step 7.


Nothing opens the node by itself after a boot: the old driver (the build of
2026-11-09, without the fixes) is still installed and must be replaced
before anything does.

1. Before the power cycle, the screen: a kernel debugger's text with
   `object_cache: object … is already freed` and `kthread_stop` in the
   trace is the fault fixed since. A photo of whatever is there.
2. After the boot, read what the old driver left:

   ```
   ls /boot/home/config/settings/kernel/drivers/amdxdna.starting   # no such file
   tr -d '\0' < /boot/home/amdxdna.log | tail -n 20
   grep -a amdxdna /var/log/syslog.1 /var/log/syslog | tail -n 30
   ```

   The log's last line was written before the machine stopped: `NPU powered
   up` (20:44:06) means it died in that session or in the power down at its
   end (the old driver writes nothing between the two), `NPU powered down`
   after it.
3. Within minutes of the boot (the memory), the settings first, then the
   new driver over the old one, then the launch job:

   ```
   cat > /boot/home/config/settings/kernel/drivers/amdxdna <<EOF
   log_file /boot/home/amdxdna.log
   step_log 2
   keep_powered 1
   EOF
   cp amdxdna /boot/home/amdxdna.staged && chmod 755 /boot/home/amdxdna.staged
   mv /boot/home/amdxdna.staged $NP/add-ons/kernel/drivers/bin/amdxdna
   mkdir -p $NP/data/launch && cp launch-amdxdna $NP/data/launch/amdxdna
   grep -a amdxdna /var/log/syslog | tail -n 8
   ```

   expected: `devfs: reload driver`, `parameter keep_powered = 1`, `NPU
   1022:17f0 rev 20 … starts it when opened`, `buffer memory: 128 MiB at …`.
4. With `keep_powered 1` the NPU is started once and never powered down:
   commands are tried without power cycles.

   ```
   cd ~/xdna
   ./xdna_test info
   ./xdna_test -n 5 run designs/passthrough_kernel
   for i in $(seq 20); do ./xdna_test run designs/passthrough_kernel | tail -n 1; done
   ./xdna_test -n 200 run designs/conv2d | tail -n 3
   ./xdna_test -p 4 -n 5 run designs/passthrough_kernel     # turbo: still 112 ms a command?
   grep -a -c 'powering the NPU' /var/log/syslog             # no more than before these
   ```
5. Power down as it is meant to be, five seconds after the last close:

   ```
   ./xdna_test set keep_powered 0
   for i in $(seq 10); do ./xdna_test run designs/passthrough_kernel | tail -n 1; sleep 7; done
   grep -a 'counts at power down' /var/log/syslog | tail -n 3
   ```

   Each round: `powering the NPU up` … `NPU powered up`, and seven seconds
   later `powering the NPU down` … `NPU powered down` and a line of counts,
   whose numbers must not grow from line to line.
6. Power down at once after every session, as on 2026-11-09:

   ```
   ./xdna_test set autosuspend_ms 0
   for i in $(seq 30); do ./xdna_test run designs/passthrough_kernel | tail -n 1; done
   ./xdna_test set autosuspend_ms 5000
   ./xdna_test set step_log 1
   ```
7. Then step 6 (the kit), with the new `amdxdna.neural` and
   `amdxdna_backend_test`.

## If the machine stops

Do not repeat the step. Look at the screen first (a kernel debugger's text
names the function). After the reboot, before anything opens the node: the
end of `/boot/home/amdxdna.log` (`tr -d '\0' < … | tail`; it is synced line
by line, the syslog may lack its last lines) is the last thing the driver
did before the step that did not return: `step: smu: command 4, argument 0`
without its `answered` line is the SMU's power off, `step: mailbox:
removing the handler of interrupt …` the removal of an interrupt handler,
and so on; with `step_log 2` every message to the firmware is there too.
The next try then starts with `keep_powered 1` if the last lines are of
powering down or up, and with the Linux driver's own debug lines (`debug
0x2`) in either case.

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
