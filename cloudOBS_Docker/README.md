# Cloud OBS Docker with SRTLA

This repository provides a headless, web-accessible instance of OBS Studio running in Docker, complete with the custom [PyleIRL](https://github.com/pyledw/PyleIRL) plugin compiled and installed.

## Features
- **Web UI**: Access the full OBS desktop interface directly from your browser via KasmVNC.
- **SRTLA Support**: Automatically pulls and compiles the SRTLA plugin, exposing the necessary UDP ports for external streaming.
- **WebSocket Ready**: Exposes the OBS WebSocket server port for easy integration with external HTML control panels.
- **Always-on**: Configured to run persistently.

## Portainer Installation Instructions

This setup is designed as a fully self-contained `docker-compose.yml` file. You do NOT need to build a custom Dockerfile. The container uses an initialization script to automatically compile and install the SRTLA plugin on its first boot.

### Deploying via Portainer Web Editor (Recommended)
You can deploy this entirely through Portainer without linking a Git repository or using SSH.

1. Open your **Portainer** dashboard.
2. Select your environment and go to **Stacks**.
3. Click **Add stack**.
4. Give your stack a name (e.g., `cloud-obs`).
5. Choose **Web editor**.
6. Copy the entire contents of the `docker-compose.yml` file and paste it into the editor.
7. Scroll to the bottom and click **Deploy the stack**. 

*Note: On the very first boot, the initialization script will pause the OBS startup for a few minutes while it downloads dependencies and compiles the SRTLA plugin from source. Subsequent reboots will be instantaneous!*

## Accessing the System

- **Visual Web UI**: Navigate to `http://<your-server-ip>:3000`
- **OBS WebSockets**: Connect your HTML control plane to `ws://<your-server-ip>:4455`. (Remember to enable the WebSocket server inside OBS first: *Tools -> WebSocket Server Settings*).
- **SRTLA Ingestion**: Send your bonded SRT streams to `udp://<your-server-ip>:5000` (or any port in the 5000-5010 range).

**Router/Firewall Note**: Ensure that UDP ports `5000-5010` are forwarded to your Docker host if you are receiving streams from external cellular connections.
