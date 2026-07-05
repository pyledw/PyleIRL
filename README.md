# SRTLA Receiver Plugin for OBS Studio

A custom, high-performance **SRTLA (SRT Link Aggregation)** Receiver plugin built directly for OBS Studio on Windows. This plugin allows OBS to act as a native receiver for bonded SRT streams sent from devices running Belabox, IRL Pro, or other SRTLA-compatible encoders.

---

## Key Features

* **Multi-Instance Support**: Create multiple independent SRTLA Receiver sources in OBS, each bound to its own port and proxying to separate media sources.
* **SRTLA Monitor UI Dock**: Real-time status widget inside OBS (**Docks -> SRTLA Status**) to monitor active connections, bandwidth, packet counts, and connection health per source.
* **Interface Binding (Multi-NIC Support)**: Solves asymmetric routing on multi-homed/multi-NIC Windows systems using the Windows IP Helper API (`IP_UNICAST_IF`), forcing replies back through the correct gateway to maintain stable cellular connection NAT mappings.
* **Server-Side Session Recovery**: Instantly recovers connection groups when receiving orphaned handshakes, preventing encoders from getting stuck in infinite loop reconnections after an OBS restart.
* **WSAECONNRESET Hardening**: Robust Windows socket error handling to prevent ICMP Port Unreachable packets from breaking the UDP receiving loops.
* **Verbose Diagnostic Logging**: Embedded logs showing incoming handshakes (`REG1`/`REG2`) and outgoing responses (`REG3`) to make router firewall debugging extremely simple.

---

## How to Configure OBS Source Properties

When you add a **SRTLA Receiver** source to your scene, you will have three main settings:

1. **SRTLA Bind IP (empty for ANY)**:
   * **Crucial for Multi-NIC / Multi-Homed PCs**: If your streaming PC has multiple network cards (e.g., one connected to the home network/Wi-Fi and one connected to a dedicated cellular router/modem), type the local IP address of the interface receiving the stream (e.g., `192.168.100.49`). 
   * This binds the socket and forces all outbound responses (REG3, ACKs, keep-alives) to go out of that specific network card.
   * If you are testing on your local network/Wi-Fi, leave this **blank** to listen on all interfaces.
2. **SRTLA Listen Port (UDP)**:
   * The UDP port where the external encoder (such as your phone) is sending the stream. Ensure this port matches the UDP port forwarding rule on your router.
3. **Local SRT Port**:
   * The internal port used to bridge the proxy to the OBS Media Source. This is strictly internal (`127.0.0.1`) and **should not** be opened or forwarded in your router.

---

## Network & Port Forwarding Requirements

To ensure external connections from cellular networks function properly, configure the following:

1. **Forward the UDP Listen Port**: Forward the UDP port configured in OBS (e.g. `5000` UDP) on your router to the local IP of your OBS PC.
2. **Docker Port Conflicts**: If you previously used the Docker Belabox portal on port `5000` UDP, you **must stop the Docker container** before running the OBS plugin on port `5000` to prevent address binding conflicts.
3. **Internal Loopback**: The local SRT port (e.g. `4000`/`4006`) is internal-only. Do not forward it.

---

## Reopening the Monitor Dock

If you close the **SRTLA Status** tree widget, you can reopen it at any time from the top menu of OBS:
1. Go to **Docks** in the top menu.
2. Check **SRTLA Status** to snap the monitor back into the interface.

---

## How to Build the Plugin

This plugin has been de-submoduled and is packaged as a flat repository. No external Git submodule management is needed.

### Prerequisites
* **CMake 3.30+**
* **Visual Studio 2022** (with C++ Desktop development workload)

### Build Steps
Run the following commands in the project root:

```powershell
# 1. Configure the build
cmake -B build_x64 -S .

# 2. Compile in Release mode
cmake --build build_x64 --config Release
```

The compiled binaries will be generated at:
`build_x64/Release/SRTLA_Receiver.dll`

---

## Installation & Release Packaging

When publishing releases on GitHub, it is recommended to compile the binaries and package them into simple Archive files (.zip or .tar.gz) that match OBS Studio's plugin directory structure. 

### Package Structure
Create a release archive with the following directory structure:
```text
obs-plugins/
  64bit/
    SRTLA_Receiver.dll   # The compiled plugin library
```

### Installation Paths by Operating System

Users can install the plugin by downloading the package for their operating system and copying the contents into the appropriate OBS Studio directory:

#### Windows
Copy the `obs-plugins` folder into your OBS Studio installation directory:
* Default path: `C:\Program Files\obs-studio\`
* (So the DLL ends up in `C:\Program Files\obs-studio\obs-plugins\64bit\SRTLA_Receiver.dll`)

*Note for Windows users:* If you receive a SmartScreen warning or an error when loading, ensure you right-click the `SRTLA_Receiver.dll` file, select **Properties**, and check **Unblock** if it was blocked by Windows as a downloaded file.

#### macOS
Copy the compiled `.so` or `.dylib` library into the OBS plugins directory:
* System-wide: `/Library/Application Support/obs-studio/plugins/SRTLA_Receiver/bin/`
* User-specific: `~/Library/Application Support/obs-studio/plugins/SRTLA_Receiver/bin/`

#### Linux (Ubuntu/Debian)
Copy the library file into the OBS plugins folder:
* User-specific: `~/.config/obs-studio/plugins/SRTLA_Receiver/bin/`
* System-wide: `/usr/lib/obs-plugins/` or `/usr/share/obs/obs-plugins/`

