# pico-faces on Ethos-U85 with ExecuTorch: hackathon guide

The Arm ExecuTorch example, on its `hackathon` branch: a generative model on
a microcontroller. [pico-faces](https://github.com/cpldcpu/pico-faces) by
cpldcpu, a latent diffusion transformer (2.5M parameters) with a small
decoder, generates 128x128 faces; exported through ExecuTorch, every layer of
it runs on the Ethos-U85 NPU while the Cortex-M drives the sampling loop. One
CMSIS solution runs it on the **Alif Ensemble E8 DevKit** (Cortex-M55 HP core;
the faces appear on the board's LCD, a new one per joystick press) and on the
**Corstone-320 FVP**; you switch between them by target-type. The model is
exported from PyTorch in three steps: the CMSIS-Toolbox describes the target,
`create_ai_layer.py` turns that into the AI layer, the toolbox builds the
application. This page takes you from an empty machine to a debug session on
the board. Everything about the example itself is in
[documentation/example.md](documentation/example.md).

## 1. Host tools

1. Install [VS Code](https://code.visualstudio.com/).
2. Install the extensions **Keil Studio Pack** (`Arm.keil-studio-pack`, which
   brings the CMSIS Solution extension 1.72.0 or newer with CMSIS-Toolbox
   2.15) and **Python** (`ms-python.python`). Sign in with an
   Arm account when Keil Studio asks; the free Keil MDK Community license is
   enough.
3. Nothing else by hand: when you open the project, the Arm Tools Environment
   Manager offers to install the tools pinned in `vcpkg-configuration.json`
   (CMSIS-Toolbox, Arm Compiler 6, GCC, CMake, Ninja, and on Linux and
   Windows the Corstone-320 FVP; macOS runs it in Docker, see step 6). Accept.
4. Optional: the **CMSIS Developer Assistant** extension lets an AI agent
   (Claude Code or GitHub Copilot Chat) build, flash and debug the board
   through an MCP server. Install it, install one of the agents, run
   **CMSIS Developer Assistant: Configure Agents and Skills** from the command
   palette and pick at least the `cmsis-debug-live` and `cmsis-help` skills.

## 2. Alif and SEGGER tools (board only)

1. **Alif SETOOLS** V1.110.00 or later (V1.112.00 is current) from the
   [Alif software and tools page](https://alifsemi.com/support/software-tools/ensemble/)
   (login required). Unpack it; on Linux and macOS make the tools executable
   and install the Python packages its README lists. Add the root directory
   (the one with `app-gen-toc` and `app-write-mram`) to your VS Code user
   settings:

   ```json
   "alif.setools.root": "/absolute/path/to/setools"
   ```

2. **SEGGER J-Link Software** V8.42 or later from
   [segger.com](https://www.segger.com/downloads/jlink/). The board has an
   on-board J-Link.

## 3. Board

- Connect a USB-C cable to **PRG USB** (the connector in the corner). It
  powers the board and carries the J-Link and a USB-to-UART bridge. Leave
  **MCU USB** unconnected.
- Jumpers at their defaults: **JP5 on 1-2**, **JP7 on 3-4**. Never move
  jumpers with power applied.
- **SW4** selects what the UART bridge is connected to:

  | SW4 | Connected to | Used for |
  |-----|--------------|----------|
  | `SEUART` (default) | Secure Enclave UART | SETOOLS (step 5) |
  | `UART4` | Application UART4, 115200 8N1 | The example's console (step 6) |

- With the board attached, run these once: in the SETOOLS directory
  `updateSystemPackage -d` (SW4 on `SEUART`; picks the serial port, checks the
  system firmware, offers to make the E8 the default target: answer yes), and
  J-Link Commander (`JLinkExe`, `JLink.exe` on Windows), which updates the
  on-board J-Link firmware and installs its serial-port drivers.

## 4. Project

```bash
git clone https://github.com/Arm-Examples/CMSIS-Executorch.git
cd CMSIS-Executorch
git checkout hackathon
```

Open the folder in VS Code and accept the tool activation and the pack
installation (`PyTorch::ExecuTorch`, `AlifSemiconductor::Ensemble`, CMSIS).
In the CMSIS view open **Manage Solution**, choose the target-type
**DevKit-E8** (or **SSE-320-U85** for the FVP) and click **Apply**.

The model data is generated rather than committed, so a fresh checkout
needs Python once before the first build:

1. **Terminal > Run Task > Setup Python virtual environment** (several GB of
   PyTorch, takes a while; the **(uv)** variant of the task uses uv and can
   download the Python version it asks for). It also downloads the
   pico-faces checkpoints (about 60 MB, checked by SHA-256) into
   `model/pico_faces/`.
2. **Terminal > Run Task > Create AI layer** exports the model for the NPU
   and writes `ai_layer/` (a few minutes). It reads
   `cmsis-executorch.cbuild-mlops.yml`, which the CMSIS Solution extension
   writes when it loads the solution (click **Build** once if the file is
   missing). Run the task again after changing `model/model.py`.

## 5. Prepare the board once

The Secure Enclave boots the M55 cores from a table of contents in MRAM; the
debugger needs that table to point at a debug stub.

1. SW4 to **SEUART**, PRG USB attached.
2. **Terminal > Run Task > Alif: Install M55_HP debug stubs (DevKit-E8, single core configuration)**. Choose COM port
   discovery (`-d`) the first time; SETOOLS remembers the port. The task
   selects the DevKit-E8's part in SETOOLS (`tools-config -p 'E8
   (AE822FA0E5597LS0) ...' -r A0`: the part goes into the table of contents,
   and the Secure Enclave does not boot a table built for another part),
   copies the configuration and stub from `.alif/` into the SETOOLS tree and
   runs `app-gen-toc` and `app-write-mram`. If `app-write-mram` reports a
   different revision of the board, answer `y`: only the part number decides
   whether the table boots.
3. SW4 to **UART4**.

Repeat this after another project has reprogrammed the table.

## 6. Build, run, debug

1. With SW4 on **UART4**, open the **Serial Monitor** panel on the PRG USB
   port, 115200 baud.
2. In the CMSIS view click **Build**, then **Debug** (or **Run**). Keil Studio
   starts the J-Link GDB server over SWD, loads the image into MRAM and stops
   at `main`; continue with F5. The console shows the Ethos-U banner, the
   program and its two methods, the timings of the boot demo, the CRC of the
   image, an ASCII preview of the face and the pass marker:

   ```text
   Ethos-U version info:
       Arch:       v2.0.0
       MACs/cc:    256
       Cmd stream: v1
   ExecuTorch pico-faces (m3_long_cfg, a16w8/a8w8): 2796016 byte program
   Methods:
     dit_step  2 input(s), 1 output(s), 16384 planned byte(s)
     decode    1 input(s), 1 output(s), 245760 planned byte(s)
   Generating: seed 3, 4 steps, class 1, w 4.0
   Display: 480x800 RGB888 panel started
     dit_step: 8 call(s), 68 ms total (8 ms each)
     decode:   3 ms
     total:    78 ms at 400 MHz (wall clock; not meaningful on the FVP)
     dit_step: NPU 27132 kcycles, active 95%, MAC active 39%, 32 MAC/cycle, read 21279 kB on AXI0 + 0 kB on AXI1
     decode:   NPU 1303 kcycles, active 50%, MAC active 37%, 83 MAC/cycle, read 434 kB on AXI0 + 0 kB on AXI1
   Image: 128x128x3, CRC32 6b938c66
     |::::::::.......  .    ..........|
     |::::::::...........     .....   |
     ...
   Test_result: PASS
   Interactive: send "G <seed> [k_steps] [class] [w]" (viewer/view_serial.py) or "I"
   Joystick: left = one new image, right = start/stop continuous generation
   ```

   A face with guidance takes 78 ms: eight `dit_step` calls of 8.5 ms on the
   NPU and a 3 ms decode. The same face appears on the LCD, scaled to
   384 x 384 in the centre of the screen; it is bit for bit the image the FVP
   produces. (The CRC depends on the exported program, which can differ
   slightly between the machines that run **Create AI layer**.)
3. Press the **SW2 joystick** to the left for a new face (the next seed; class
   and guidance follow from it), to the right to generate faces back to back
   until you press right again. Each one is reported on the console.
4. Or request faces from the host with pico-faces' viewer (close the Serial
   Monitor first, the viewer needs the port; in a clone of
   [pico-faces](https://github.com/cpldcpu/pico-faces),
   `pip install pyserial pillow`):

   ```bash
   python viewer/view_serial.py --port /dev/tty.usbmodemXXXX --seed 3 --steps 4 --class 1 --cfg 4 --show
   ```

   Classes are 0 (female, neutral), 1 (female, smiling), 2 (male, neutral),
   3 (male, smiling) and 4 (unconditional); `--cfg` is the guidance strength
   (0 = plain), `--steps` 8, 4, 2 or 1.
5. Set a breakpoint after `generate()` in `src/app_main.cpp` and inspect the
   timings in `tm`, or ask the CMSIS Developer Assistant to do it: "Build for
   the DevKit-E8, load it, break after the first generate() and show me the
   NPU cycles per method."

**FVP instead of the board:** choose the **SSE-320-U85** target-type and click
**Run** or **Debug**; the same output appears in the terminal (a run takes a
few minutes), and the image lands in `out/fvp_image.bin`.
`python3 model/verify_export.py --compare out/fvp_image.bin <reference.png>`
compares it with the host's rendering of the same seed (see
[documentation/example.md](documentation/example.md)). On macOS, where Arm
ships no FVP build, the FVP runs in Docker: set `model:` in the csolution's
SSE-320-U85 target-set to `${workspaceFolder}/.vscode/fvp.sh`, copy
`.vscode/launch.json.mac` to `.vscode/launch.json`, and keep Docker Desktop
running (the first run builds the image, about 100 MB).

## 7. If something does not work

- **J-Link connects but never stops at `main`:** either the table of contents
  does not point at the debug stub or was built for another part, which the
  Secure Enclave skips (repeat step 5), or the image cannot boot.
  Look before reprogramming: in the debugger, read the vector table at
  `0x80200000` and the fault registers (CFSR/HFSR); a PC of `0xEFFFFFFE` is a
  lockup at reset, which an image linked to run from ITCM produces when the
  code that copies it there is itself in ITCM.
- **Debug hangs at "Connecting":** the target-set was switched to
  `protocol: jtag`. Keep SWD: the generated load task blocks on JLinkExe's
  JTAG-chain prompt, and the device stays in SWD mode after any SWD use
  until it is power-cycled.
- **No console output:** SW4 is still on `SEUART`, or the port was opened
  before the switch was moved. Set `UART4` and reopen the port.
- **Every SETOOLS tool prints `Revision is invalid!`:** SETOOLS stores the
  selected part and revision in `utils/global-cfg.db`, and a
  `tools-config -p` without `-r` (another project switching to an E7 or the
  AppKit-E8, for example) keeps a revision the new part does not have. From
  then on even `tools-config` refuses to run. The stub task of step 5 writes
  the DevKit's part and revision into that file before it calls
  `tools-config`, so it recovers by itself; for SETOOLS commands of your own,
  set `"Part#"` to `"E8 (AE822FA0E5597LS0) - 5.5 MRAM / 9.75 SRAM"` and
  `"Revision"` to `"A0"` there.
- **`app-write-mram` gets no answer:** press reset while it waits, check SW4
  is on `SEUART`, close any terminal holding the port.
- **The build cannot find `ai_layer/model_pte.c`:** the model data is not
  committed; run the tasks **Setup Python virtual environment** and **Create
  AI layer** (step 4).
- **"`.venv` does not exist" or "`model/pico_faces/...` is missing":** run the
  task **Setup Python virtual environment** first; it creates the
  environment and downloads the checkpoints.
- `.vscode/launch.json` is generated by the extension for the target-types
  and is not committed; after **Apply** it has a J-Link entry for the
  DevKit-E8. That is expected.

## Read on

- [documentation/example.md](documentation/example.md): the example, the
  command-line flow, how the model is generated.
- [board/DevKit-E8/README.md](board/DevKit-E8/README.md): the board layer,
  memory map and RTE configuration.
- [documentation/mlops-flow.md](documentation/mlops-flow.md): the `mlops:`
  node and `*.cbuild-mlops.yml` in detail.
- [documentation/pack-provenance.md](documentation/pack-provenance.md): where
  the `PyTorch::ExecuTorch` pack comes from and how to move to a new version.

## Sentinel: machine-health monitor (DevKit-E8)

Sentinel scores the DevKit-E8's PDM microphones and BMI323 accelerometer
against a baseline learned from a machine running normally. In the default
status-only build the LCD shows text only, landscape and left to right:
SENTINEL at the top, the state (IDLE, LEARNING, OK, CHECK or ALERT) large
below it, the selected profile, the learning progress, and one line each for
Sound and Vibration.
The status-only build loads no model and generates no faces; the pico-faces
example stays in the repository and runs when `APP_STATUS_ONLY` is not
defined.

### Architecture

| Part | File | Work and timing |
|------|------|-----------------|
| Microphones | `board/DevKit-E8/mic_probe.c` | PDM channels 4/5, 16 kHz, 16-bit, `ARM_PDM_MODE_AUDIOFREQ_16K_DECM_64`, channel settings from the pack's `demo_pdm.c`. The PDM callback restarts the capture into a ring of 4 x 100 ms static blocks (25.6 kB), as the pack's `vstream_audio_in.c` does; the main loop consumes the blocks. 0 FIFO errors and 0 ring-full stops in the status-only build, in idle runs and (in the face-demo mode) in 65 s of continuous face generation. A 49 kB viewer frame on the console still costs one ~4.5 s ring-full stop, so do not run `view_serial.py` with Sentinel. |
| Accelerometer | `board/DevKit-E8/imu_probe.c` | BMI323 on I3C through the pack driver, read every 20 ms from the main loop (~120 us per read). The installed driver writes a fixed configuration and offers no rate or filter control; measured rate 50-52 new samples/s (`APP_IMU_VIBRATION_PROBE`, `imu_vib_probe.c`). |
| Display | `board/DevKit-E8/display.c`, `sentinel.c` | `board_display_line()` and `board_display_rect()` draw 5 x 7 text (A-Z, 0-9, `: / - .`) scaled up, in an 800 x 480 landscape view rotated into the panel's 480 x 800 RGB888 frame buffer (`APP_DISPLAY_LANDSCAPE`). Layout: title band, state band in the state colour, info line with a 30-step progress bar while learning, `SOUND:` and `VIBRATION:` lines reading MATCHES BASELINE (green), CHANGED (amber), LEARNING or NOT LEARNED. Each part is redrawn only when its text changes, from the main loop. |
| Scoring | `board/DevKit-E8/sentinel.c` | 1 s windows (10 microphone blocks). Features: f0/f1 log power of channel 4/5, f2 log high-frequency ratio of channel 4 (first difference / signal; no CMSIS-DSP or FFT is installed, so no band energies), f3 log variance of the acceleration magnitude. Baseline = mean/std over 30 windows with std floors. A sensor is anomalous after 3 windows with \|z\| > 4.0 and normal after 3 windows with \|z\| < 2.5; 0/1/2 anomalous sensors = OK/CHECK/ALERT. About 68 us (worst 192 us) per 100 ms block, 1-6 us per window, all in the main loop. |
| Profiles | `board/DevKit-E8/sentinel_profile_store.c` | Two separately learned baselines, stored as checksummed alternating records in the final 256 bytes of reserved user MRAM. The 16-byte write routine runs from ITCM because the pack warns against executing from MRAM while writing it. A partial or invalid record is rejected; a new profile is verified after saving. The first slot is FAN; the second is temporarily named TEMP until its appliance is chosen. TEMP is a label, not a temperature sensor. |

Compile-time modes in `board/DevKit-E8/Board-U85.clayer.yml`: the default is
`APP_SENTINEL` + `APP_STATUS_ONLY` + `APP_DISPLAY_LANDSCAPE` (status-only text
demo, no model, no faces). Without `APP_STATUS_ONLY` the pico-faces demo runs
as well, and its faces would be drawn over part of the status text (not
tested with the text layout). Instead of
`APP_SENTINEL` (and `APP_STATUS_ONLY`): `APP_IMU_PROBE` + `APP_MIC_PROBE` for
raw sensor prints, or `APP_IMU_VIBRATION_PROBE` for the accelerometer rate
diagnostic (one at a time). `DISPLAY_LANDSCAPE_FLIP: 1` turns the landscape
view the other way round. After switching modes build with `--rebuild`: an
incremental build can keep objects compiled with the previous define and
fail to link.

### Build, flash, run

```
cbuild cmsis-executorch.csolution.yml --active DevKit-E8
# flash (the "CMSIS Load" task does the same):
printf 'Connect\nLoadFile "out/cmsis-executorch/DevKit-E8/Debug/cmsis-executorch.hex"\nExit\n' > .cmd.jlink
JLinkExe -device AE822FA0E5597LS0_M55_HP -if swd -speed 4000 -commandfile .cmd.jlink
```

The load leaves the core halted: press reset (or J-Link `r`, `g`). Console:
SW4 = UART4, 115200 8N1. After boot Sentinel prints one `SN t=...` line per
second (state, z-scores, `!` = sensor anomalous, features, IMU new/read,
block and window time in us, FIFO errors, ring-full stops). SW2 **left**
selects FAN; **right** selects TEMP; **up** learns/relearns the selected
normal state (30 s). Push the joystick firmly; a light press may not register.
On boot the board waits for a selection. If that slot has a saved baseline,
selection checks three fresh windows (blue VERIFYING) before showing OK or
CHECK; if empty, it stays
IDLE until learned. Changing slots clears the partial sensor window and alarm
history. Do not interpret OK before the appliance is running at the same
normal condition and in the same board position as it was during learning.

After a successful learn the UART must say `SN PROFILE ... saved in MRAM`.
`WARNING: PROFILE NOT SAVED` on the LCD means the current RAM baseline works
only until power-off; inspect the UART storage error before relying on it.
The profile store has host tests and the firmware builds, but power-cycle and
re-flash persistence have **not yet been verified on the physical board**.
The firmware image does not contain learned data; a mass-erase flashing
workflow might clear user MRAM even though this image has no records there.
The gyroscope and chip temperature are not yet included in the score.

The LCD after boot (hold the board landscape): SENTINEL, a large grey IDLE,
`SW2 LEFT FAN RIGHT TEMP`, and `SOUND: NOT LEARNED`,
`VIBRATION: NOT LEARNED`.

### Safe demo: fan speed change (microphone)

1. Fix the board and fan in marked positions, guard on, nothing touching the blades.
2. Press SW2 left to select FAN. If it is empty or the setup changed, let the
   fan run steadily at medium for about 10 s, room quiet; press SW2 up. The LCD
   shows a large blue LEARNING, `BASELINE n/30 S` and a filling bar, then a
   large green OK, `NO CHANGE FROM BASELINE` and both sensor lines
   `MATCHES BASELINE` in green. Learn again if the fan or the room was not
   steady: a baseline learned during changing sound makes later scores
   meaningless (see the status-only result below).
3. Leave medium for 45 s (expect OK), switch to high (expect a large amber
   CHECK, `ONE SENSOR CHANGED`, `SOUND: CHANGED` in amber), return to the
   same medium setting (expect green OK).

#### Result: status-only text demo, current fan (2026-10-17, operator present)

Same current fan as the result below, but moved farther from the board since
then, so the baseline differs. The operator confirmed every LCD screen
described above: IDLE layout, LEARNING with counter and bar, OK, CHECK with
`SOUND: CHANGED` and `VIBRATION: MATCHES BASELINE`, and OK again.

- First learning attempt: the sound during learning was louder and less
  steady than the medium that followed (f0 1.35 +/- 0.29 against 0.63), so
  high (f0 1.08) stayed inside the baseline and gave no CHECK. Re-learned
  with the fan settled at medium: f0 0.519 +/- 0.072, f1 -0.282 +/- 0.103,
  f2 -0.376 +/- 0.060, f3 0.037 +/- 0.100 (floor).
- Medium 45 s: OK throughout, mic score at most 3.3.
- High: the mic score jumped to 7.1 and the sound was flagged 2 s later;
  CHECK held on high (mic scores about 5). The vibration score rose to 17
  while the speed control was handled, which gave CHECK just before and
  2 ALERT windows just after the sound change; that is handling, not fan
  vibration detection.
- Back to medium: OK 2 s after the sound dropped; 26 windows OK after.
- 0 FIFO errors, 0 ring-full stops, about 99.6 % new accelerometer samples,
  worst 192 us per microphone block, no unexpected restart in 10 minutes.

#### Result: current fan (validated 2026-10-17, operator present)

Current fan, fixed with the board in marked positions, guard on, only the
speed control touched (operator report). The operator confirmed on the LCD:
grey IDLE at boot, blue LEARNING then green OK after SW2 up, amber CHECK on
high (both times), green OK back on medium (both times).

- Baseline (30 windows, about 30 s): f0 1.991 +/- 0.148, f1 0.690 +/- 0.093,
  f2 -1.038 +/- 0.052, f3 0.882 +/- 0.189.
- Medium before the first change (86 windows, including the 45 s
  observation): OK throughout, mic score at most 1.8.
- High, twice: the mic score jumps in one window (9.4 and 10.9) and CHECK
  follows 2 s later (the 3-window rule); mic scores about 9-11 while on high,
  f1 up about 1 decade. The operator's confirmations arrived 7 s and 7 s
  after the jump, so the state had already changed when the change was
  confirmed; latency is measured from the jump in the sound.
- Back to medium, twice: the mic score drops in one window and OK follows
  2 s later (operator confirmations 6 s and 56 s after the drop).
- Medium between and after the cycles: OK, mic score at most 2.5; no
  microphone false alarm.
- Vibration: never flagged during the speed changes (vib score at most 5.5,
  never more than 2 windows in a row above 4). In the last 15 s of the run f3 rose to about 2.0
  and vibration alone gave 5 CHECK windows while the microphone stayed
  normal; the operator reported no handling, so the cause is unknown. This is
  not fan vibration detection.
- 0 FIFO errors, 0 ring-full stops, about 99 % new accelerometer samples,
  worst 183 us per microphone block.

#### Historical runs (earlier setup, fan identity unknown)

Which fan was used in these runs was not recorded; they must not be carried
over to the current fan. Times are from the operator prompt, so they include
the operator and the fan changing speed.

- Fixed-mount run (baseline f0 = 2.50): CHECK 17 s and 22 s after the prompt
  to go to high (about 4 s after the first window above the threshold); OK
  10 s and 12 s after the prompt to return to medium; no false alarm in the
  medium phases.
- Final demo (baseline f0 = 1.61): CHECK 13 s and 17 s after the prompt to go
  to high (2 s and 4 s after the first window above the threshold); no false
  alarm in the medium phases. After the second return to medium, OK after
  10 s. After the first return, the microphone features stayed at their
  high-speed level and the state stayed CHECK for about 77 s before it went
  to OK; the cause was not established (the fan's control position was not
  recorded). In that period 15 windows were ALERT because the vibration score
  rose while the setup was being handled; that is not a two-sensor fan
  detection.

This is a speed-change demonstration, not blade-fault detection.

### Limitations

- The installed BMI323 driver delivers only ~50 Hz samples; fan vibration at
  medium and high speed stays at the noise floor (~0.45 mg^2) and is not
  resolved, so the vibration score is unsupported for this fan. ALERT
  (two sensors) has not been demonstrated on a fan fault; vibration flags
  seen so far came from handling the setup or from disturbances of unknown
  cause, never from the fan.
- A phone vibrating against the setup produced a strong accelerometer signal
  (~10^4 x the noise floor): this tests the sensor chain only, not a fan fault.
- Microphone features are absolute levels: moving the fan or board shifts
  them like a fault would. Re-learn after moving anything.
- OK means no change from the learned baseline in these two simple
  features; it is not a statement that the machine is healthy.
