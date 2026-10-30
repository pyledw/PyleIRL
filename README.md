# PyleIRL: SRTLA Receiver Plugin for OBS Studio

A custom, high-performance **SRTLA (SRT Link Aggregation)** Receiver plugin built directly for OBS Studio. This plugin allows OBS to act as a native receiver for bonded SRT streams sent from devices running Belabox, IRL Pro, or other SRTLA-compatible encoders.

## ✨ Key Features
- **Native SRTLA Receiver**: No need for external relay servers like Belabox Cloud or Node servers. Receive directly into OBS!
- **Intelligent Auto-Scene Switcher**: Automatically switches your scenes based on real-time stream bitrate (e.g., jump to a BRB scene if bitrate drops, return when stable).
- **Global Source Visibility Toggling**: Automatically hide or show specific sources across all of your scenes based on the stream's health.
- **Built-in Reverse Proxy (FRP)**: Seamlessly connect to a cloud server to bypass complex home router NAT/firewall setups.
- **Web Interface**: Monitor your connection stats remotely and adjust settings via a built-in web server.

---

## 🚀 Download & Installation (Pre-Compiled Binaries)

Select your operating system below to download and install the pre-compiled version of the plugin:

### 💻 Windows (OBS Studio 33.0+)
1. **Download the Zip Package**:
   * Download the **[PyleIRL_Windows.zip](/Install/PyleIRL_Windows.zip)** archive from the Releases page or `/Install` directory.
2. **Install**:
   * Close OBS Studio.
   * Extract the `PyleIRL` folder into the unified OBS Studio 33+ third-party plugins directory:
     * Default path: `C:\ProgramData\obs-studio\plugins\`
     * (This places `PyleIRL.dll` and `data\` directly inside `C:\ProgramData\obs-studio\plugins\PyleIRL\`).
   * Restart OBS.
   
   *Note:* If you receive a SmartScreen warning or an error when loading, right-click the `PyleIRL.dll` file, select **Properties**, and check **Unblock** if it was blocked by Windows as a downloaded file.

### 🍏 macOS
1. **Download the PKG Installer**:
   * Go to the **Releases** tab of your GitHub repository.
   * Download the `PyleIRL_macOS.pkg` installer.
2. **Install**:
   * Run the `.pkg` installer. It will automatically place the plugin inside the correct directory:
     `/Library/Application Support/obs-studio/plugins/PyleIRL/bin/`
   * Restart OBS.

### 🐧 Linux (Ubuntu/Debian)
1. **Download the DEB Package**:
   * Go to the **Releases** tab of your GitHub repository.
   * Download the `PyleIRL_Linux.deb` installer package.
2. **Install**:
   * Install the package by running the following command in terminal:
     ```bash
     sudo dpkg -i PyleIRL_Linux.deb
     ```
   * Restart OBS.

---

## ⚙️ How to Configure OBS Source Settings

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

## 🎬 Auto-Scene Switcher & Global Source Visibility

The PyleIRL plugin features a highly intelligent Auto-Scene Switcher that responds to your live stream's bitrate (KBps). 

You can access these settings from the top menu in OBS: **Tools -> SRTLA Auto-Switch Settings...**

### Auto-Scene Switcher Logic
1. **Primary Scenes**: You operate normally on your primary scenes.
2. **Fallback Rules**: You can define one or more rules (e.g., 0-500 KBps -> "BRB - Low Bitrate"). 
3. **Triggering**: When your stream's bitrate falls into the specified range for the defined duration, OBS will automatically switch to the designated fallback scene. 
4. **Smart Recovery**: When the bitrate recovers and exits the fallback range, the plugin remembers what scene you were originally on before the failure and switches you right back to it!
5. **Cascading Failure**: If your bitrate drops from 6000 KBps to 3500 KBps, it will trigger your first fallback rule (if set). If it drops further to 0 KBps, it will cascade into your second fallback rule. Once restored back to 6000 KBps, it will snap directly back to the original scene.
6. **Active Listener Requirement**: The Auto-Switcher is only active when an SRTLA Receiver is actively listening in OBS. If you stop the receiver, it will not aggressively switch scenes.

### Global Source Visibility
If you have sources (such as a "Low Bitrate Alert" overlay) that you only want visible when the stream is struggling:
1. Define a visibility rule for a source and a target bitrate range.
2. **Persistent State**: The plugin enforces this state globally! If the source exists in 5 different scenes, it will be hidden or shown in all of them simultaneously. 
3. **No Accidental Overrides**: The plugin persistently hides the source if the condition is not met. Even if a user manually clicks the "eye" icon in OBS to show the source, the plugin will immediately force-hide it again to ensure your stream looks exactly how it should.

---

## 🌐 Reverse Proxy Tunnel (Optional)

If you don't want to configure port forwarding on your home router, you can configure the plugin to use a Reverse Proxy Tunnel via [FRP (Fast Reverse Proxy)](https://github.com/fatedier/frp). 

When enabled, the plugin will seamlessly connect to your cloud server and route inbound SRTLA traffic directly back to OBS, bypassing your local firewall.

### How to use the Reverse Proxy:
1. **Cloud Server Setup**: You must run an `frps` server in the cloud (e.g. AWS, DigitalOcean, Linode). 
   - **👉 [Click here for the full Cloud Server Setup Guide (Linux, Windows, & Docker)](REVERSE_PROXY_SETUP.md)**
2. **Configure OBS**: Go to **Tools** -> **SRTLA Reverse Proxy Settings...**
3. **Settings**:
   - **Enable Reverse Proxy Tunnel**: Check this box.
   - **Server Address**: The IP address or domain of your `frps` server in the cloud.
   - **Server Port**: The control port of your `frps` server (typically `7000`).
   - **Auth Token**: If your `frps` server requires a token, enter it here.
   - **Forward Ports**: Enter the UDP port range you want to forward from the cloud directly to this plugin (e.g., `5000-5010`).

*Note: The required `frpc` executable is bundled automatically with the OS releases and will run invisibly in the background when enabled!*

---

## 🌍 Built-in Web Interface
You can access remote status and settings for the plugin directly from a browser.
1. Navigate to **Tools -> SRTLA Web Interface Settings...**
2. **Enable Web Interface** and define a **Listen Port**.
3. You can then access the interface via `http://<your-ip>:<port>` to view statistics, reload settings, and adjust configurations on the fly!

---

## 🔌 Network & Port Forwarding Requirements

To ensure external connections from cellular networks function properly (when not using the reverse proxy):
1. **Forward the UDP Listen Port**: Forward the UDP port configured in OBS (e.g. `5000` UDP) on your router to the local IP of your OBS PC.
2. **Docker Port Conflicts**: If you previously used the Docker Belabox portal on port `5000` UDP, you **must stop the Docker container** before running the OBS plugin on port `5000` to prevent address binding conflicts.
3. **Internal Loopback**: The local SRT port (e.g. `4000`/`4006`) is internal-only. Do not forward it.

---

## 📊 Reopening the Monitor Dock

If you close the **SRTLA Status** tree widget, you can reopen it at any time from the top menu of OBS:
1. Go to **Docks** in the top menu.
2. Check **SRTLA Status** to snap the monitor back into the interface.

---

## 📦 How to Build and Package Installers (Windows, macOS, Linux)

### Method A: Build Automatically via GitHub (Recommended)
This repository includes a pre-configured GitHub Actions build workflow. **Autobuilding is disabled on general code pushes** to keep your git push clean. It will only run when you explicitly request it:

1. **Build Manually (Workflow Dispatch)**:
   * Go to your repository on GitHub.
   * Click the **Actions** tab.
   * Under the list of workflows on the left, click **Build Project**.
   * Click the **Run workflow** dropdown on the right side.
   * Select your branch (e.g., `main` or `master`) and click the green **Run workflow** button.
   * GitHub will automatically compile the code for Windows, macOS, and Linux in parallel and attach the pre-compiled `.zip`, `.exe`, `.pkg`, and `.deb` installers directly to the completed run for you to download!
2. **Build on Release Tag**:
   * Pushing a tag (e.g. `v1.0.0`) automatically compiles all platform installers and attaches them directly to a **Draft Release** in your repository.

### Method B: Build Locally (Windows Only)
To compile the dll on your local Windows machine:
1. Ensure you have **CMake 3.30+** and **Visual Studio 2022** installed.
2. Open terminal in the project root and run:
   ```powershell
   cmake -B build_x64 -S .
   cmake --build build_x64 --config Release
   ```
   The compiled library will output to `build_x64/Release/PyleIRL.dll`.

---

## 📜 Open Source Credits & Attributions

This project utilizes several open-source libraries and components. We are incredibly grateful to the developers and maintainers of these projects for their work:

- **[BELABOX srtla](https://github.com/BELABOX/srtla)**: A multi-link bonding transport proxy for connection aggregation. Licensed under the [GNU Affero General Public License v3.0 (AGPL-3.0)](src/srtla/LICENSE).
- **[frp (Fast Reverse Proxy)](https://github.com/fatedier/frp)**: A fast reverse proxy to help expose local servers to the internet. Licensed under the [Apache License 2.0](https://github.com/fatedier/frp/blob/dev/LICENSE).
- **[cpp-httplib](https://github.com/yhirose/cpp-httplib)**: A C++ header-only HTTP/HTTPS server and client library by yhirose. Licensed under the [MIT License](https://github.com/yhirose/cpp-httplib/blob/master/LICENSE).
- **[OBS Studio (libobs / obs-frontend-api)](https://github.com/obsproject/obs-studio)**: Open Broadcaster Software API. Licensed under the [GNU General Public License v2.0 (GPL-2.0)](LICENSE).
- **[Qt Framework](https://www.qt.io/)**: Cross-platform software development framework for the UI. Licensed under [LGPLv3 / GPLv3](https://www.qt.io/licensing/).

