# STM32 Auto-Detecting Digital Multimeter

A dual-display digital multimeter built on the STM32 "Black Pill" (STM32F411).
Three dedicated analogue front-ends &mdash; DC, AC and resistance &mdash; are
selected by a user-operated DIP switch; the firmware detects which front-end has
been enabled, runs the matching acquisition routine, and renders the result on
two displays simultaneously. A 16&times;2 character LCD shows mode-specific
graphics (segmented bar, animated sine wave, analogue-style needle gauge) while
a 128&times;32 SSD1306 OLED reports the numeric value with auto-scaled units.

The project demonstrates embedded sensor acquisition, true-RMS computation from
sample variance, custom CGRAM glyph design, resilient I&sup2;C initialisation,
and conflict detection on resource-constrained hardware.

---

## Table of Contents

1. [Features](#features)
2. [System Architecture](#system-architecture)
3. [Hardware](#hardware)
4. [Pin Mapping](#pin-mapping)
5. [Wiring Overview](#wiring-overview)
6. [Software Architecture](#software-architecture)
7. [Measurement Modes](#measurement-modes)
8. [Build and Flash](#build-and-flash)
9. [Calibration](#calibration)
10. [Troubleshooting](#troubleshooting)
11. [Limitations](#limitations)
12. [Repository Layout](#repository-layout)
13. [Author](#author)

---

## Features

- **DIP-switch mode selection.** The user enables one front-end at a time via a
  DIP switch. The firmware detects which channel is energised and runs the
  matching acquisition routine &mdash; no manual menu navigation, no software
  mode button.
- **Conflict protection.** If the DIP switch (or any wiring fault) drives two
  inputs simultaneously, the instrument refuses to produce a reading and
  flashes a warning on both displays.
- **Dual-display output.** The character LCD shows the *shape* of the signal
  (bar, wave, gauge); the OLED shows the *value* with unit auto-scaling.
- **True-RMS AC measurement.** Computed from the variance of 12-bit samples over
  a 100&nbsp;ms window, so the reading is independent of DC offset and waveform
  shape.
- **Logarithmic resistance gauge.** Needle position is driven by
  log<sub>10</sub>(R), so 10&nbsp;&Omega;, 10&nbsp;k&Omega; and 10&nbsp;M&Omega;
  all occupy visually distinct zones of the dial.
- **Hardened OLED initialisation.** Three-attempt re-init loop with the I&sup2;C
  bus clocked down to 100&nbsp;kHz. If the OLED never responds, the LCD continues
  to function and a flag suppresses all further OLED traffic.
- **Open-circuit detection.** When the ohmmeter probes are unbridged the OLED
  reports `OPEN` and the gauge needle pegs to the maximum-resistance position.

---

## System Architecture

```
   PROBES                FRONT-ENDS                MCU                  DISPLAYS
   ======                ==========          ===============         =================
                         +---------+
   DC input ---DIP-----> | divider |---PA2--+
                         +---------+        |
                         +---------+        |   STM32F411CEU6        16x2 LCD
   Ohm input --DIP-----> | divider |---PA1--+-->  Cortex-M4    --->  (graphics)
                         +---------+        |    100 MHz
                         +---------+        |       |
   AC input ---DIP-----> | bias    |---PA7--+       |
                         +---------+                +-->  SSD1306 OLED 128x32
                                                         (numeric value)
```

The DIP switch routes the active probe to its corresponding front-end. The MCU
samples each ADC channel, identifies which one is above threshold, runs the
mode-specific acquisition routine, then redraws both displays. The control loop
runs at approximately 2.5&nbsp;Hz (400&nbsp;ms cycle), which is comfortable for
human reading and well within the LCD's refresh tolerance.

---

## Hardware

| Component | Role |
|-----------|------|
| **STM32F411 "Black Pill"** | ARM Cortex-M4 host MCU; 12-bit ADC; 100&nbsp;MHz |
| **3-position DIP switch** | User-operated mode selector; routes the active probe to its front-end |
| **16&times;2 character LCD (HD44780-compatible)** | Graphical / animated channel |
| **0.91" SSD1306 OLED (128&times;32, I&sup2;C, addr 0x3C)** | Numeric channel |
| **DC voltage divider (FACTOR&nbsp;=&nbsp;125)** | Scales target DC into 0&ndash;3.3&nbsp;V ADC range |
| **Ohmmeter reference resistor** | R<sub>known</sub>&nbsp;=&nbsp;9.9&nbsp;k&Omega; |
| **AC front-end** | Mid-rail biased to 2047 LSB; signal centred on V<sub>cc</sub>/2 |
| **10&nbsp;k&Omega; LCD contrast trimmer** | Standard HD44780 V<sub>0</sub> divider |

---

## Pin Mapping

### Analogue inputs (all 12-bit ADC)

| Pin | Channel | Purpose | Front-End Notes |
|-----|---------|---------|-----------------|
| `PA2` | ADC1 | DC voltmeter | Resistive divider, gain 1/125 |
| `PA1` | ADC1 | Ohmmeter     | Voltage divider, R<sub>known</sub>&nbsp;=&nbsp;9.9&nbsp;k&Omega; |
| `PA7` | ADC1 | AC voltmeter | Biased to ADC mid-scale (2047 LSB) |

### Character LCD (4-bit parallel)

| LCD Signal | MCU Pin |
|------------|---------|
| `RS` | `PB12` |
| `EN` | `PB13` |
| `D4` | `PB14` |
| `D5` | `PB15` |
| `D6` | `PB4`  |
| `D7` | `PB5`  |

### OLED (I&sup2;C1)

| OLED Signal | MCU Pin | Notes |
|-------------|---------|-------|
| `SCL` | `PB6` | I&sup2;C1 clock |
| `SDA` | `PB7` | I&sup2;C1 data  |
| Bus address | `0x3C` | Standard for SSD1306 |
| Bus clock   | `100&nbsp;kHz` | Reduced from the 400&nbsp;kHz default for reliability |

All pins were chosen to avoid the F411 boot-strap and USB lines, keeping the
on-board USB DFU flow available throughout development.

---

## Wiring Overview

### DC voltmeter (`PA2`)

A two-resistor divider scales the input by 1/125 so that 250&nbsp;V at the probe
corresponds to roughly 2.0&nbsp;V at the ADC pin. The firmware reverses the
scaling in software:

```
V_real = (avg_raw * 3.3 / 4095) * FACTOR   where FACTOR = 125
```

> **Warning** &mdash; this is a student-grade front-end. Mains-voltage handling
> requires correctly rated resistors, isolation, and PCB clearances that are
> beyond the scope of this build.

### Ohmmeter (`PA1`)

A reference resistor R<sub>known</sub> in series with the unknown R<sub>x</sub>
forms a divider between 3.3&nbsp;V and ground. The ADC reads V<sub>x</sub>;
R<sub>x</sub> is solved algebraically:

```
R_x = R_known * (4095 - avg_raw) / avg_raw
```

### AC voltmeter (`PA7`)

The AC signal is biased onto a DC mid-point of V<sub>cc</sub>/2 so the ADC can see
both half-cycles. The firmware does **not** subtract a hard-coded offset &mdash;
it computes the true RMS from the variance of the sample set, which removes any
slow DC drift automatically (see [True-RMS calculation](#true-rms-calculation)).

---

## Software Architecture

The firmware is a single Arduino sketch organised into four layers.

| Layer | Responsibility |
|-------|----------------|
| **HAL** | `analogRead`, `Wire`, `LiquidCrystal` &mdash; library-level primitives |
| **Acquisition** | 50-sample DC/&Omega; averaging, 100&nbsp;ms AC sampling window |
| **Mode arbitration** | Threshold detection, conflict resolution, mode change handling |
| **Presentation** | LCD CGRAM glyphs and OLED render routines |

### Mode arbitration

The DIP switch decides which front-end is electrically active. The firmware
detects that selection by checking which ADC channel is above the noise
threshold, and is also responsible for refusing to read when two channels are
simultaneously active (a wiring fault or an incorrectly set DIP switch).

```c
int active = (detect_DC  > THRESHOLD ? 1 : 0)
           + (detect_Ohm > THRESHOLD ? 1 : 0)
           + (detect_AC  > THRESHOLD ? 1 : 0);

if      (active > 1)      conflict_screen();
else if (detect_DC  > T)  measure_dc();
else if (detect_Ohm > T)  measure_ohm();
else if (detect_AC  > T)  measure_ac();
else                      standby_screen();
```

The threshold (`THRESHOLD = 20` LSB &asymp; 16&nbsp;mV) is high enough to ignore
floating-input noise on the disabled channels but low enough to register any
real signal applied to the enabled probe.

### Mode transitions

A `lastMode` sentinel forces an `lcd.clear()` and reloads CGRAM glyphs whenever
the active mode changes. CGRAM holds only eight 5&times;8 glyphs at a time, so
the DC bar, AC wave and &Omega; needle each have their own glyph set that is
swapped in on mode entry.

---

## Measurement Modes

### DC voltage

- **Front-end** &mdash; passive 1/125 resistive divider on `PA2`.
- **Acquisition** &mdash; 50 successive `analogRead` samples, arithmetic mean.
- **Conversion** &mdash; `V = (avg * 3.3 / 4095) * 125`.
- **LCD visual** &mdash; horizontal bar built from six custom glyphs giving five
  sub-cell sub-pixels per character, yielding 70 distinct fill levels across a
  14-cell row.
- **OLED visual** &mdash; numeric value with two decimal places, suffix `V`.

### Resistance

- **Front-end** &mdash; voltage divider with R<sub>known</sub>&nbsp;=&nbsp;9.9&nbsp;k&Omega;.
- **Acquisition** &mdash; 50-sample mean of `PA1` after a brief settling delay.
- **Conversion** &mdash; `R = R_known * (4095 - avg) / avg`.
- **Open detection** &mdash; if `avg <= 10` LSB the probes are unbridged; the
  needle pegs to full scale and the OLED prints `OPEN`.
- **Gauge mapping** &mdash; `normR = log10(R) / 6`, mapped to needle positions
  along a 12-cell arc. Three needle glyphs (up / mid / down) are loaded based on
  position so the needle visually tilts across the gauge.
- **OLED visual** &mdash; auto-scaled units (`Ohm`, `kOhm`, `MOhm`).

### AC voltage

- **Front-end** &mdash; AC signal biased to ADC mid-scale (2047 LSB).
- **Acquisition** &mdash; free-running 12-bit sampling for 100&nbsp;ms; both the
  sum and the sum-of-squares are accumulated.
- **LCD visual** &mdash; a 16-cell scrolling sine wave built from four bottom-fill
  glyphs cycled through a 12-step sine sequence.
- **OLED visual** &mdash; numeric value with one decimal place, suffix `V`.

#### True-RMS calculation

The AC routine computes RMS from the variance of the sample set rather than from
a fixed offset:

```c
float mean      = (float)sum    / samples;
float mean_sq   = (float)sum_sq / samples;
float variance  = mean_sq - (mean * mean);
float rms_raw   = variance > 0 ? sqrt(variance) : 0;
float final_AC  = rms_raw * AC_CAL;
```

Two implementation details matter:

- **64-bit accumulators.** Squared 12-bit samples can exceed the 32-bit headroom
  over a 100&nbsp;ms window. The sum-of-squares is therefore cast to `uint64_t`
  *before* the multiplication.
- **Variance clamp.** Floating-point round-off can drive `variance` slightly
  negative when the input is essentially zero; the result is clamped to zero
  before `sqrt` to prevent a NaN.

A small noise gate (`final_AC < 5.0 V &rarr; 0`) suppresses residual readings
when no AC source is connected.

---

## Build and Flash

### Prerequisites

- Arduino IDE 2.x with STM32duino core (`stm32duino/Arduino_Core_STM32`)
- Libraries:
  - `Adafruit GFX Library`
  - `Adafruit SSD1306`
  - `LiquidCrystal` (built-in)

### Board configuration

| Setting | Value |
|---------|-------|
| Board | Generic STM32F4 series |
| Board part number | BlackPill F411CE |
| Upload method | STM32CubeProgrammer (DFU) **or** SWD via ST-Link |
| USB support | CDC (generic Serial supersede USART) |
| Optimize | Smallest (-Os) |

### Flashing via DFU

1. Hold `BOOT0`, tap `NRST`, release `BOOT0`.
2. In the Arduino IDE select `Upload Method` &rarr; `STM32CubeProgrammer (DFU)`.
3. Click `Upload`. Once flashed, press `NRST` once more to start the firmware.

---

## Calibration

Two constants in the source require calibration against a reference instrument:

| Constant | Default | Purpose |
|----------|---------|---------|
| `FACTOR`  | `125`     | DC divider gain. Adjust to match your physical divider. |
| `AC_CAL`  | `0.71867` | AC scaling factor. Apply a known AC source and tune until the OLED reading matches a calibrated meter. |

`R_KNOWN` (`9900.0`) should be replaced with the measured (not nominal) value of
your reference resistor for best accuracy in the ohmmeter mode.

---

## Troubleshooting

| Symptom | Likely Cause | Action |
|---------|--------------|--------|
| OLED stays blank, LCD works | I&sup2;C ACK failure during boot | Check `PB6`/`PB7` wiring; the firmware already retries 3&times; at 100&nbsp;kHz, but a missing pull-up will still fail |
| LCD shows garbled glyphs | CGRAM was not reloaded on mode change | Ensure `lastMode` tracking is intact and `loadDCChars()` / `loadACChars()` / `loadOhmChars()` are called on transitions |
| Conflict screen stuck on | Two DIP positions enabled at once, or front-ends coupling | Set only one DIP position; verify front-ends are not bleeding signal into one another |
| AC reading non-zero with no input | DC offset drift or noise | Lower the noise-gate threshold cautiously, or improve the mid-rail bias filtering |
| Ohmmeter reads `OPEN` for a small resistor | ADC below 10 LSB | Reduce `R_known` so the divider stays in a more usable range |

---

## Limitations

- The DC front-end is calibrated for low-voltage bench work. Safe handling of
  mains-level voltages is **not** in scope for this project.
- AC accuracy is bounded by the ADC sample rate within the 100&nbsp;ms window;
  signals with significant content above several kHz will alias.
- The character LCD imposes hard limits on graphical fidelity &mdash; the bar,
  wave and gauge are abstractions, not oscilloscope-grade renderings.
- Conflict resolution is binary: the instrument refuses to read but does not
  attempt to identify which channels are colliding.

---

## Repository Layout

```
.
|-- multimeter.ino        # Main firmware (this file)
|-- README.md             # This document
|-- docs/                 # Schematics
```

---

## Author

**Abdelrahman Ibrahim** **Ali Essam** **Eyad Hany** **Hossam Eldin Ibrahim** 
Final-Year Computer Engineering &mdash; Arab Academy for Science, Technology and Maritime Transport (AAST)
Embedded Systems submission.
