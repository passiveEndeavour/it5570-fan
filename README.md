# it5570-fan

Linux hwmon kernel module for the ITE IT5570 embedded controller, providing fan monitoring and control on mini PCs that lack native Linux fan support.

## Features

- Fan RPM monitoring
- PWM fan speed control (manual and automatic modes)
- 6 temperature sensors with labels
- Works with [coolercontrol](https://gitlab.com/coolercontrol/coolercontrol) for custom fan curves
- DKMS support — auto-rebuilds on kernel updates
- Restores automatic fan control on module unload

### sensors output

```
it5570_fan-isa-0000
Adapter: ISA adapter
fan1:        1705 RPM
CPU:          +50.0°C
Board:        +52.0°C
CPU Die:      +53.0°C
Heatsink:     +60.0°C
Chipset:      +55.0°C
EC:           +51.0°C
pwm1:             44%
```

## Is this driver for me?

If you ran `sensors-detect` and got this message:

```
Probing for Super-I/O at 0x4e/0x4f
Trying family `National Semiconductor/ITE'...               Yes
Found unknown chip with ID 0x5570
```

Then yes — this is the driver you need. The ITE IT5570 is an embedded controller not recognized by `sensors-detect` or the in-tree `it87` driver. There is no mainline Linux driver for this chip. This module provides the missing hwmon support.

You may also have found this page by searching for:
- `Found unknown chip with ID 0x5570`
- `ITE IT5570 Linux fan control`
- `IT5570 no driver Linux`
- `IT5570 hwmon driver`
- `IT5570E sensors-detect unknown chip`
- `mini PC fan always on Linux`
- `AceMagic fan control Linux`
- `Beelink fan control Linux`
- `MinisForum fan noise Linux`
- `mini PC fan loud Linux no control`

## Tested Hardware

| Device | APU | BIOS | EC |
|---|---|---|---|
| AceMagic W1 | AMD Ryzen 7 8745HS (Phoenix) | AMI PHXPM7B0 | ITE IT5570 rev 0x02 |

Many white-label mini PCs from various brands share the same motherboard and EC firmware. If your system has an ITE IT5570 EC at Super I/O port 0x4E, this driver will likely work. The DMI strings on these boards are typically "Default string" (unprogrammed), so the driver detects the chip by its hardware ID (0x5570) rather than DMI matching.

## Installation

### AUR (Arch, CachyOS, EndeavourOS, Manjaro)

```bash
yay -S it5570-fan-dkms
```

### Manual (any distro)

```bash
git clone https://github.com/passiveEndeavour/it5570-fan.git
cd it5570-fan

# Build and load
make
sudo insmod it5570_fan.ko

# Or install with DKMS (persists across kernel updates)
make dkms-install
```

The module auto-loads on boot if installed via the AUR package or DKMS. For manual installs, add `it5570_fan` to `/etc/modules-load.d/it5570_fan.conf`.

### CachyOS / Clang-built kernels

If your kernel was built with clang (CachyOS default), pass the compiler flags:

```bash
make CC=clang LD=ld.lld
```

The DKMS and AUR installations handle this automatically.

## Usage

### Reading sensors

```bash
sensors it5570_fan-isa-0000
```

### Manual fan control

```bash
# Switch to manual mode
echo 1 | sudo tee /sys/class/hwmon/hwmon*/pwm1_enable

# Set fan speed (0-255, where 255 = 100%)
echo 128 | sudo tee /sys/class/hwmon/hwmon*/pwm1

# Return to automatic EC control
echo 2 | sudo tee /sys/class/hwmon/hwmon*/pwm1_enable
```

### coolercontrol

Install [coolercontrol](https://gitlab.com/coolercontrol/coolercontrol) and it will automatically detect the hwmon interface. You can then create custom fan curves using any of the 6 temperature sensors as input.

## Technical Background

### The ITE IT5570

The IT5570 is an embedded controller (EC) from ITE Tech, built around an 8051 microcontroller core. Unlike ITE's Super I/O chips (IT8613, IT8720, etc.) which have well-documented hardware monitoring registers, the IT5570 is a programmable EC whose register layout is defined entirely by its firmware. There is no public programming guide for the fan control interface — the register map was determined through reverse engineering.

The IT5570 is commonly found in budget mini PCs, particularly white-label AMD Phoenix/Hawk Point systems sold under brands like AceMagic, Beelink, MinisForum, and others. These systems typically ship with no thermal management under Linux beyond the EC's built-in fan curve.

### How it works

The driver uses two access methods to communicate with the EC:

**ACPI EC interface** (ports 0x62/0x66) — Used for fan control and the primary sensors. The EC exposes a 256-byte register space via the standard ACPI EC read (cmd 0x80) and write (cmd 0x81) commands. The key discovery was register **0x0F**: writing 1–100 sets manual fan duty percentage, writing 0 returns to automatic control.

**SIO indirect SRAM access** (ports 0x4E/0x4F) — Used for the extended temperature sensors. The IT5570's SMFI (Shared Memory Flash Interface) provides indirect access to the full 8KB EC SRAM space via SIO config registers 0x2E/0x2F. The ACPI EC's 256-byte window maps to SRAM 0x400–0x4FF; the additional temperature sensors live outside this window at addresses like 0x05B9, 0x0C44, 0x0C4A, and 0x086A.

### EC register map

#### ACPI EC registers (offset from EC base)

| Offset | R/W | Description |
|---|---|---|
| 0x0E | R | Fan duty status (0–100%) |
| 0x0F | R/W | Fan duty control: 0 = auto, 1–100 = manual % |
| 0x22 | R | Fan RPM high byte |
| 0x23 | R | Fan RPM low byte |
| 0x26 | R | CPU temperature (°C, filtered) |
| 0xF1 | R | Board temperature (°C) |

#### EC SRAM registers (via SIO indirect)

| Address | Description |
|---|---|
| 0x05B9 | CPU die temperature (°C, raw/unfiltered, ~3–5s faster response) |
| 0x086A | EC internal temperature (°C) |
| 0x0C44 | Heatsink temperature (°C) |
| 0x0C4A | Chipset temperature (°C) |

### Reverse engineering methodology

The register map was determined through:

1. **DSDT analysis** — The ACPI tables revealed the EC at `\_SB_.PCI0.SBRG.EC0_` with command port 0x66 and data port 0x62, but contained no fan control methods — the firmware handles everything internally.
2. **EC SRAM diffing** — Dumping the full 8KB SRAM at idle, under CPU stress, and during cooldown, then comparing the dumps to identify registers that track temperature, RPM, and duty cycle.
3. **Brute-force register probing** — Systematically writing to each ACPI EC offset and monitoring fan RPM changes to find the control register (0x0F).
4. **Cross-referencing** — Comparing findings with the [ec-su_axb35](https://github.com/nicman23/ec-su_axb35-linux) driver for a similar ITE EC platform.

## hwmon sysfs interface

| Attribute | Description |
|---|---|
| `fan1_input` | Fan speed in RPM |
| `temp1_input` / `temp1_label` | CPU temperature (filtered) |
| `temp2_input` / `temp2_label` | Board temperature |
| `temp3_input` / `temp3_label` | CPU die temperature (raw, faster response) |
| `temp4_input` / `temp4_label` | Heatsink temperature |
| `temp5_input` / `temp5_label` | Chipset temperature |
| `temp6_input` / `temp6_label` | EC internal temperature |
| `pwm1` | Fan duty cycle (0–255) |
| `pwm1_enable` | 1 = manual, 2 = automatic (EC-controlled) |

## Contributing

If you have a mini PC with an ITE IT5570 EC, please test this driver and report your results by opening an issue with:
- Your device brand and model
- `sudo dmidecode -t system` output
- `sensors` output with the module loaded
- Whether fan control works correctly

## License

GPL-2.0 — see the [SPDX identifier](https://spdx.org/licenses/GPL-2.0-only.html) in the source.
