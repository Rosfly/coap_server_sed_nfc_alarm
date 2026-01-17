# CoAP Server for XIAO nRF54L15 - Build & Flash Instructions

## Overview
This CoAP server automatically joins your Home Assistant Thread network (ha-thread-8c41) on boot and provides CoAP resources for LED and button control.

## Network Configuration
The device is pre-configured with your Home Assistant Thread Border Router credentials:
- **Network Name:** ha-thread-8c41
- **Channel:** 15
- **PAN ID:** 35905 (0x8c41)
- **Extended PAN ID:** ec:b6:45:cc:6c:ad:63:7f
- **Network Key:** ec:0b:ed:21:56:81:cb:a7:46:52:5d:95:62:b0:61:12

## Build Commands

### Build CoAP Server
```bash
source /home/ros/zephyrproject/.venv/bin/activate
cd /home/ros/zephyrproject/zephyr/samples/rosprojects/coap_client
west build -b xiao_nrf54l15/nrf54l15/cpuapp -p -- -DEXTRA_CONF_FILE=prj_server.conf
```

### Flash to Device
```bash
west flash
```

## What Happens on Boot

1. **Device boots** and initializes OpenThread stack
2. **Automatically joins** the ha-thread-8c41 network using hardcoded credentials
3. **Registers CoAP resources:**
   - `/led` - LED control resource
   - `/button` - Button state resource
4. **Ready to receive CoAP requests** from other devices on the Thread network

## Testing the Server

### Using OpenThread Shell Commands
Connect via serial (115200 baud) and verify:
```
uart:~$ ot state
(should show: router, child, or leader)

uart:~$ ot ipaddr
(shows IPv6 addresses - note the mesh-local address)

uart:~$ ot netdata show
(shows network data from OTBR)
```

### Using CoAP Client
From another Thread device or the OTBR, send CoAP requests:
```bash
# Toggle LED
coap-client -m post coap://[fd00:...]:5683/led -e '{"state":"toggle"}'

# Get button state
coap-client -m get coap://[fd00:...]:5683/button
```

## Build Artifacts
- Binary: `/home/ros/zephyrproject/build/zephyr/zephyr.hex`
- ELF: `/home/ros/zephyrproject/build/zephyr/zephyr.elf`
- Memory usage: ~305KB FLASH, ~80KB RAM

## Configuration Files
- **prj.conf** - Base OpenThread + CoAP configuration with HA network credentials
- **prj_server.conf** - Server-specific config (enables LED/button resources)
- **boards/xiao_nrf54l15_nrf54l15_cpuapp.overlay** - Board-specific devicetree overlay

## Troubleshooting

### Device not joining network
1. Check serial output for errors
2. Verify OTBR is running: `ot state` should show attached
3. Check channel matches (15) and credentials are correct

### Cannot communicate with CoAP server
1. Get device IPv6 address: `ot ipaddr`
2. Ping from OTBR: `ping6 <ipv6-address>`
3. Check firewall rules on HA host

## Alternative: Manual Commissioning

If you want to avoid hardcoding credentials, use manual commissioning:
1. Build without credentials (comment out network configs in prj.conf)
2. Enable `CONFIG_OPENTHREAD_MANUAL_START=y`
3. Use shell commands to join:
   ```
   ot dataset set active 0e080000000000010000000300000f4a0300001135060004001fffe00208ecb645cc6cad637f0708fdfab5fdaf0ae7a30510ec0bed215681cba746525d9562b06112030e68612d7468726561642d3863343101028c4104104fe0a1aa17c1045b23b9d38e1969db900c0402a0f7f8
   ot ifconfig up
   ot thread start
   ```
