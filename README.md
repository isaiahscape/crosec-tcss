# Chromebook TCSS MUX Driver

`Chromebook TCSS MUX Driver` is a Windows Kernel-Mode Driver Framework (KMDF) driver that provides USB Type-C Subsystem (TCSS) crossbar muxing for Intel-based Chromebooks running Windows.

On Chromebooks running coreboot / UEFI firmware, the physical USB-C pins are routed through an internal Intel Power Management Controller (PMC) North Mux-Agent crossbar. This driver bridges the ChromeOS Embedded Controller (`ACPI\GOOG0014`) with the Intel PMC IPC MMIO registers, dynamically configuring:
- USB 2.0 / USB 3.x data routing
- DisplayPort (DP) Alternate Mode & Hotplug Detect (HPD)
- Thunderbolt 3 / Thunderbolt 4 / USB4 tunneling

---

## Supported Platforms

* **Tiger Lake** (11th Gen Intel Core)
* **Alder Lake** (12th Gen Intel Core)
* **Raptor Lake** (13th Gen Intel Core)
* **Meteor Lake** (14th Gen Intel Core)

---

## Hardware Architecture

```
┌─────────────────────────────────┐
│     USB-C Port Partner / Cable  │
└────────────────┬────────────────┘
                 │
                 ▼
┌─────────────────────────────────┐
│    ChromeOS Embedded Controller │ (PD negotiation, SOP' cable discovery)
└────────────────┬────────────────┘
                 │ ACPI Notify (0x80) / LPC Host Commands
                 ▼
┌─────────────────────────────────┐
│       CrosTcssMux.sys           │ (Binds to ACPI\GOOG0014)
└────────────────┬────────────────┘
                 │ MMIO IPC (PWRMBASE + 0x2000)
                 ▼
┌─────────────────────────────────┐
│ Intel PMC North Mux-Agent       │ (Physical silicon crossbar routing)
└───────┬────────────────┬────────┘
        │                │
        ▼                ▼
   Intel xHCI       Intel Thunderbolt 4 / USB4
   Controller       Host Router
```

---

## Prerequisites

- **Visual Studio 2022** with Desktop development with C++
- **Windows Driver Kit (WDK)** 10.0.22621.0 or newer
- **KMDF 1.33**

---

## Building

Run the automated build script:
```cmd
build.cmd Release
```

The compiled driver package will be placed in `x64\Release\CrosTcssMux\`.

---

## Installation

1. Enable Test Mode (if self-signing):
   ```cmd
   bcdedit /set testsigning on
   ```
2. Restart your computer.
3. Right-click `install-driver.cmd` and select **Run as administrator**.

---

## License

MIT License. See [LICENSE](LICENSE) for details.
