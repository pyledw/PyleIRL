# SRTLA Receiver Plugin for OBS Studio

A custom, high-performance **SRTLA (SRT Link Aggregation)** Receiver plugin built directly for OBS Studio. This plugin allows OBS to act as a native receiver for bonded SRT streams sent from devices running Belabox, IRL Pro, or other SRTLA-compatible encoders.

---

## 🚀 Download & Installation (Pre-Compiled Binaries)

Select your operating system below to download and install the pre-compiled version of the plugin:

### 💻 Windows
1. **Download the Zip Package**:
   * Download the **[SRTLA_Receiver_Windows.zip](/Install/SRTLA_Receiver_Windows.zip)** archive directly from the `/Install` directory of this repository.
2. **Install**:
   * Close OBS Studio.
   * Extract the contents of the zip file directly into your OBS Studio installation folder:
     * Default path: `C:\Program Files\obs-studio\`
     * (This automatically copies `SRTLA_Receiver.dll` into `C:\Program Files\obs-studio\obs-plugins\64bit\`).
   * Restart OBS.
   
   *Note:* If you receive a SmartScreen warning or an error when loading, right-click the `SRTLA_Receiver.dll` file, select **Properties**, and check **Unblock** if it was blocked by Windows as a downloaded file.

### 🍏 macOS
1. **Download the PKG Installer**:
   * Once you run the GitHub Release workflow, go to the **Releases** tab of your GitHub repository.
   * Download the `SRTLA_Receiver_macOS.pkg` installer.
2. **Install**:
   * Run the `.pkg` installer. It will automatically place the plugin inside the correct directory:
     `/Library/Application Support/obs-studio/plugins/SRTLA_Receiver/bin/`
   * Restart OBS.

### 🐧 Linux (Ubuntu/Debian)
1. **Download the DEB Package**:
   * Once you run the GitHub Release workflow, go to the **Releases** tab of your GitHub repository.
   * Download the `SRTLA_Receiver_Linux.deb` installer package.
2. **Install**:
   * Install the package by running the following command in terminal:
     ```bash
     sudo dpkg -i SRTLA_Receiver_Linux.deb
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

## 🌐 Reverse Proxy Tunnel (Optional)

If you don't want to configure port forwarding on your home router, you can configure the plugin to use a Reverse Proxy Tunnel via [FRP (Fast Reverse Proxy)](https://github.com/fatedier/frp). 

When enabled, the plugin will seamlessly connect to your cloud server and route inbound SRTLA traffic directly back to OBS, bypassing your local firewall.

### How to use the Reverse Proxy:
1. **Cloud Server Setup**: You must run an `frps` server in the cloud (e.g. AWS, DigitalOcean, Linode). 
   - **👉 [Click here for the full Cloud Server Setup Guide (Linux, Windows, & Docker)](REVERSE_PROXY_SETUP.md)**
2. **Configure OBS**: Go to **Tools** -> **SRTLA Receiver** -> **Reverse Proxy Settings...**
3. **Settings**:
   - **Enable Reverse Proxy Tunnel**: Check this box.
   - **Server Address**: The IP address or domain of your `frps` server in the cloud.
   - **Server Port**: The control port of your `frps` server (typically `7000`).
   - **Auth Token**: If your `frps` server requires a token, enter it here.
   - **Forward Ports**: Enter the UDP port range you want to forward from the cloud directly to this plugin (e.g., `5000-5010`).

*Note: The required `frpc` executable is bundled automatically with the OS releases and will run invisibly in the background when enabled!*

---

## 🔌 Network & Port Forwarding Requirements

To ensure external connections from cellular networks function properly, configure the following:

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
   * Under the list of workflows on the left, click **Push**.
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
   The compiled library will output to `build_x64/Release/SRTLA_Receiver.dll`.
