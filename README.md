# Thread CoAP Server/Client for Home Assistant Integration

A Zephyr-based Thread CoAP server designed for integration with Home Assistant via the Thread CoAP Bridge add-on. Supports LED control, button input, battery monitoring with real ADC measurements, and automatic network reconnection.

CoAP Observe mechanism is lost after device reset (e.g. battery connection on device installation), cause CoAP server forgets the registered observe resources (button in this case) and will never push again a message on button press unless the client re-registers observe demand. That's why the client monitors uptime: so every minute the client on HA polls the server battery voltage and battery percentage along with uptime. If the uptime declines, that means the device reset and the client immediately refreshes the observe demand. So ~1 minute after reset the server should be ready to push a message on button press.

## Features

- **Thread SED Mode**: Runs as Sleepy End Device for ultra-low power consumption
- **SED Discovery Grace Period**: Stays awake for 2 minutes after boot for bridge discovery
- **CoAP Server**: Exposes `/led`, `/sw` (button), uptime, `/battery`, and `/voltage` resources
- **CoAP Observe**: Push notifications for LED and button state changes (RFC 7641)
- **Battery Monitoring**: Real ADC measurements with LiPo discharge curve lookup table
- **Power Optimization**: TPS22916C load switch enables voltage divider only during measurement
- **Automatic Reconnection**: Network monitor thread handles disconnection recovery
- **NVS Storage**: Thread credentials persist across reboots
- **Auto-Boot**: Joins Thread network automatically on power-up (no NFC support yet, manual shell workaround)

## Hardware Support

- Seeed XIAO nRF54L15 (primary target)
- Other nRF52/nRF53/nRF54 boards with Thread support

### Battery Voltage Measurement Circuit

The XIAO nRF54L15 board includes battery voltage measurement via:

```
VBAT ─── TPS22916C ─── R5(10K) ───┬─── P1.14 (AIN7)
         (load switch)            │
         P1.15 (EN)          R6(10K)
                                  │
                                 GND
```

- **Voltage divider ratio**: 2.0 (20K/10K)
- **ADC**: 12-bit, 4x oversampling, 1/4 gain
- **Power saving**: TPS22916C disables divider when not measuring

## Building

### Standard Build (Battery-Optimized)

Default build disables UART/logging for battery-only boot:

```bash
cd ~/zephyrproject

# Build for XIAO nRF54L15 with expansion board shield
.venv/bin/west build -p always -b xiao_nrf54l15/nrf54l15/cpuapp \
    --shield seeed_xiao_expansion_board \
    -s ~/dev/coap_server

# Flash
.venv/bin/west flash
```

### Debug Build (USB/UART Logging)

For development with USB serial console. **Note**: This build will NOT boot from battery alone - USB connection required.

```bash
cd /home/ros/zephyrproject && west build -p always -b xiao_nrf54l15/nrf54l15/cpuapp --shield seeed_xiao_expansion_board -s ~/dev/coap_server_sleepy -- -DOVERLAY_CONFIG="prj_uart.conf"


# Flash
.venv/bin/west flash

# Monitor serial output (115200 baud)
# Use VSCode Serial Monitor or: screen /dev/ttyACM0 115200
```

### Optional Shell Commands

To enable debug shell commands, edit `boards/xiao_nrf54l15_nrf54l15_cpuapp.conf`:

```kconfig
CONFIG_OT_COAP_SAMPLE_SHELL=y
```

**Warning**: Shell requires UART which prevents battery-only boot.

## First-Time Commissioning

The device needs to be commissioned to your Thread network once. After commissioning, the credentials are stored in NVS and the device will auto-join on subsequent boots.

### Step 1: Get Thread Dataset from Home Assistant

1. In Home Assistant, go to **Settings** > **Devices & Services** > **Thread**
2. Click on your Thread network (e.g., "ha-thread-xxxx")
3. Click **Download diagnostics** or view network details
4. Find the **Active Operational Dataset** (TLV hex string)

Or use the OpenThread Border Router add-on web interface:
1. Go to the OTBR web UI (usually port 8081)
2. Navigate to **Form** > **Active Dataset**
3. Copy the TLV hex string

### Step 2: Commission the Device via Shell

Connect to the device's serial console (115200 baud) and run:

```shell
# Set the Thread dataset (paste your TLV from Home Assistant)
ot dataset set active 0e080000000000010000000300000f35060004001fffe00208...

# Enable IPv6 interface
ot ifconfig up

# Start Thread
ot thread start

# Verify connection (wait 10-30 seconds)
ot state
# Should show: child (or router/leader)

# Verify IPv6 address
ot ipaddr
# Should show multiple addresses including mesh-local
```

### Step 3: Store Credentials (Automatic)

The `ot dataset set active` command automatically stores the dataset in NVS. On next boot, the device will:
1. Load the dataset from NVS
2. Enable IPv6 interface automatically
3. Start Thread and join the network
4. Begin responding to CoAP requests

**But the device can not yet be reset on battery supply, so build and flash a firmware version without UART**

## Network Recovery

The firmware includes a network monitor that handles disconnection scenarios:

### Automatic Recovery

When the device loses connection (e.g., moved out of range):

1. **Detection**: Monitor thread detects `DETACHED` state
2. **Wait**: Allows OpenThread's internal reattachment (up to 1 minute)
3. **Force Restart**: If still detached after 1 minute (6 checks x 10s), forces complete Thread stack restart
4. **Rejoin**: Device rejoins network with fresh state

## CoAP Resources

### LED Resource (`/led`)

**GET** - Returns current LED state:
```json
{"device_id": "f4ce3616e67c7a1c", "leds": [{"led_id": 0, "state": 1}]}
```

**PUT** - Control LED:
```json
{"led_id": 0, "state": 1}   // 0=OFF, 1=ON, 2=TOGGLE
```

**Observe** - Subscribe for push notifications on state changes.

### Button Resource (`/sw`)

**GET** - Returns button state:
```json
{"device_id": "f4ce3616e67c7a1c", "btns": [{"btn_id": 0, "state": 0}]}
```

State values: 0 = not pressed, 1 = pressed

**Observe** - Subscribe for push notifications on button press/release.

### Battery Resource (`/battery`)

**GET** - Returns battery percentage (triggers fresh ADC measurement):
```json
{"device_id": "f4ce3616e67c7a1c", "value": 75}
```

Percentage is calculated using a 21-point LiPo discharge curve lookup table with linear interpolation:
- 4.2V = 100% (fully charged)
- 3.7V = 50% (nominal)
- 3.0V = 0% (cutoff)

### Voltage Resource (`/voltage`)

**GET** - Returns battery voltage in millivolts:
```json
{"device_id": "f4ce3616e67c7a1c", "value": 3850}
```

### Uptime Resource (`/uptime`)

**GET** - Returns milliseconds since device boot:
```json
{"device_id": "f4ce3616e67c7a1c", "value": 123456}
```

Used by the bridge to detect device reboots. When the bridge polls uptime and sees it decrease, it knows the device rebooted and re-registers CoAP Observe subscriptions.

### Discovery Resource (`/.well-known/core`)

**GET** - Returns CoRE Link Format:
```
</led>;rt="led",</sw>;rt="button",</battery>;rt="battery",</voltage>;rt="voltage",</uptime>;rt="uptime"
```

## SED (Sleepy End Device) Mode

The device operates as a **Sleepy End Device (SED)** for optimal battery life. In SED mode, the radio is OFF most of the time, waking only:
- Every 60 seconds to poll the parent for queued messages
- Immediately on button press (GPIO interrupt)

### How SED Works

```
MTD (always listening):  Radio always ON → High power consumption (~mA)
SED (sleepy):            Radio ON only during polls → Ultra-low power (~µA)

Timeline:
  |----60s sleep----|poll|----60s sleep----|poll|----60s sleep----|
                     ^                       ^
                Radio ON briefly        Radio ON briefly

Button press: Wakes device immediately, sends notification, returns to sleep
```

### Discovery Grace Period

**Problem:** SED devices can't be discovered via multicast because they're asleep.

**Solution:** After attaching to the Thread network, the device stays awake (RxOnWhenIdle=true) for **2 minutes** to allow the bridge to discover it via multicast. After the grace period, it switches to full SED mode.

```
Boot → Attach to Thread → Grace Period (2 min, awake) → SED Mode (sleeping)
                          ↑
                    Bridge discovers device here
```

### Latency Tradeoffs

| Operation | MTD | SED | Notes |
|-----------|-----|-----|-------|
| Button notification | ~50ms | ~100-200ms | GPIO wakes device immediately |
| GET /battery | ~50ms | Up to 60s | Request queued at parent until poll |
| PUT /led | ~50ms | Up to 60s | Command queued at parent until poll |
| Observe registration | ~50ms | Up to 60s | One-time during commissioning |

**Key insight:** Button notifications are still fast because the device wakes immediately on GPIO interrupt. Only *incoming* requests (from bridge to device) have latency.

### Verify SED Mode

Use the OpenThread shell (build with `prj_uart.conf`):

```shell
ot mode          # Should show "-" (no 'r' = RxOnWhenIdle=false = SED)
ot pollperiod    # Should show 60000 (ms)
ot childtimeout  # Should show 240 (seconds)
```

On the border router, check the neighbor table:
```shell
ot-ctl neighbor table
# R=0 means SED (RxOnWhenIdle=false)
# R=1 means MTD (RxOnWhenIdle=true)
```

## Configuration

Key Kconfig options in `prj.conf`:

```kconfig
# Thread device mode (MTD = child, better for mobile devices)
CONFIG_OPENTHREAD_MTD=y
CONFIG_OPENTHREAD_FTD=n

# Enable SED (Sleepy End Device) mode
CONFIG_OPENTHREAD_MTD_SED=y

# Poll period: Wake every 60 seconds to check for messages
CONFIG_OPENTHREAD_POLL_PERIOD=60000

# Auto-start Thread on boot (requires dataset in NVS)
CONFIG_OPENTHREAD_MANUAL_START=n

# Child timeout before parent removes device (4 minutes)
# Must be > 4x poll period to allow missed polls without disconnection
CONFIG_OPENTHREAD_MLE_CHILD_TIMEOUT=240

# Child supervision (parent probes child periodically)
CONFIG_OPENTHREAD_CHILD_SUPERVISION_INTERVAL=129
CONFIG_OPENTHREAD_CHILD_SUPERVISION_CHECK_TIMEOUT=190

# TX power for range (+8 dBm max for nRF54L15)
CONFIG_OPENTHREAD_DEFAULT_TX_POWER=8

# ADC support for battery voltage measurement
CONFIG_ADC=y
```

Board-specific options in `boards/xiao_nrf54l15_nrf54l15_cpuapp.conf`:

```kconfig
# Enable CoAP Server mode
CONFIG_OT_COAP_SAMPLE_SERVER=y

# Enable resources
CONFIG_OT_COAP_SAMPLE_LED=y
CONFIG_OT_COAP_SAMPLE_SW=y
CONFIG_OT_COAP_SAMPLE_BATTERY=y

# Optional: Enable shell commands for debugging
CONFIG_OT_COAP_SAMPLE_SHELL=y
```

## Shell Commands

Available shell commands for debugging (when `CONFIG_OT_COAP_SAMPLE_SHELL=y`):

```shell
# OpenThread commands
ot state                    # Show current role
ot ipaddr                   # Show IPv6 addresses
ot dataset active           # Show active dataset
ot ping <ipv6>             # Ping another device

# CoAP commands
ot_coap led get             # Get LED state
ot_coap led set 0 on        # Turn LED on
ot_coap led set 0 off       # Turn LED off
ot_coap led set 0 toggle    # Toggle LED
```

## Troubleshooting

### Device Not Joining Network

1. Verify dataset is correct: `ot dataset active`
2. Check channel matches your network: `ot channel`
3. Verify Thread is enabled: `ot state` (should not be "disabled")
4. Check IPv6 is enabled: `ot ifconfig` (should show "up")

### Device Keeps Detaching

1. Check signal strength - move closer to border router
2. Increase TX power in `prj.conf` if supported
3. Check for interference on Thread channel

### CoAP Not Responding

1. Verify device is attached: `ot state` shows "child" or "router"
2. Check IPv6 address is reachable from border router
3. Verify CoAP port 5683 is not blocked

### Battery Reading Issues

1. Verify `CONFIG_OT_COAP_SAMPLE_BATTERY=y` is enabled
2. Check ADC initialization in logs: "Battery ADC initialized"
3. Verify voltage divider circuit is connected

## Integration with Thread CoAP Bridge

This firmware is designed to work with the [Thread CoAP Bridge](https://github.com/Rosfly/thread-coap-bridge-addon) Home Assistant add-on:

1. Install the Thread CoAP Bridge add-on in Home Assistant
2. Flash this firmware to your device
3. Commission the device to your Thread network
4. **Wait for device to attach** - grace period starts automatically
5. **Within 2 minutes**, the bridge will discover the device via multicast
6. Device appears in Home Assistant with LED control, button sensor, and battery monitoring
7. After 2 minutes, device enters SED sleep mode for power savings

### SED Support in Bridge

The bridge (v0.4.0+) fully supports SED devices:

- **75-second timeouts**: All CoAP operations wait up to 75s for SED to poll and respond
- **Unicast re-discovery**: Probes offline SED devices at their last-known IPv6 address
- **Queued commands**: PUT/GET requests are queued at the parent router until SED polls

### Resource Monitoring

The bridge uses different strategies for different resources:
- **LED/Button**: CoAP Observe for real-time push notifications
- **Battery/Voltage/Uptime**: Polling every 60s (aligns with SED poll period)

### Observe Re-Registration

The bridge automatically re-registers as an observer every 60 seconds. This handles:
- **Device reboots**: Device loses observer list on restart, bridge re-registers within 60s
- **Network hiccups**: Connection issues are detected and observation is re-established

This ensures button presses and LED state changes are always reported, even after the device reboots or temporarily loses network connectivity.

### SED Re-discovery After Extended Offline

When an SED device goes offline for an extended period and returns:

1. Device rejoins Thread network, enters SED sleep mode
2. Bridge's unicast re-discovery probes the device's last-known IPv6 (every 60s)
3. Request is queued at parent router
4. SED polls parent, receives request, responds
5. Bridge receives response within 65s timeout → device re-discovered
6. Polling and observe resume automatically

## License

MIT License - see [LICENSE](LICENSE) file.
