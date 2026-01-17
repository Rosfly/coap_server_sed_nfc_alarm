# Thread CoAP Server/Client for Home Assistant Integration

A Zephyr-based Thread CoAP server designed for integration with Home Assistant via the Thread CoAP Bridge add-on. Supports LED control and button input with automatic network reconnection.

## Features

- **Thread MTD Mode**: Runs as Minimal Thread Device (child) for mobile/battery-powered use
- **CoAP Server**: Exposes `/led` and `/sw` (button) resources
- **Automatic Reconnection**: Network monitor thread handles disconnection recovery
- **NVS Storage**: Thread credentials persist across reboots
- **Auto-Boot**: Joins Thread network automatically on power-up (no shell intervention required)

## Hardware Support

- Seeed XIAO nRF54L15 (primary target)
- Other nRF52/nRF53/nRF54 boards with Thread support

## Building

```bash
cd ~/zephyrproject

# Build for XIAO nRF54L15
west build -p always -b xiao_nrf54l15/nrf54l15/cpuapp \
    -s zephyr/samples/rosprojects/coap_client

# Flash
west flash
```

## First-Time Commissioning

The device needs to be commissioned to your Thread network once. After commissioning, the credentials are stored in NVS and the device will auto-join on subsequent boots.

### Step 1: Get Thread Dataset from Home Assistant

1. In Home Assistant, go to **Settings** → **Devices & Services** → **Thread**
2. Click on your Thread network (e.g., "ha-thread-xxxx")
3. Click **Download diagnostics** or view network details
4. Find the **Active Operational Dataset** (TLV hex string)

Or use the OpenThread Border Router add-on web interface:
1. Go to the OTBR web UI (usually port 8081)
2. Navigate to **Form** → **Active Dataset**
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

## Network Recovery

The firmware includes a network monitor that handles disconnection scenarios:

### Automatic Recovery

When the device loses connection (e.g., moved out of range):

1. **Detection**: Monitor thread detects `DETACHED` state
2. **Wait**: Allows OpenThread's internal reattachment (up to 1 minute)
3. **Force Restart**: If still detached after 1 minute (6 checks × 10s), forces complete Thread stack restart:
   - Disables Thread
   - Disables IPv6
   - Re-enables IPv6
   - Re-enables Thread
4. **Rejoin**: Device rejoins network with fresh state

**Note:** The 1-minute timeout (`MAX_DETACHED_COUNT=6`) provides a balance between allowing automatic recovery and forcing a clean restart when needed.

### Monitor Logging

The network monitor logs state changes:

```
[network_monitor] Thread role changed: 2 -> 1  (child -> detached)
[network_monitor] Device lost network connection
[network_monitor] Device detached (1/6) - waiting for reattach
...
[network_monitor] Device detached (6/6) - waiting for reattach
[network_monitor] Detached for too long - forcing Thread restart
[network_monitor] Force restarting Thread stack...
[network_monitor] Thread stack restarted - waiting for attachment...
[network_monitor] Thread role changed: 0 -> 2  (disabled -> child)
[network_monitor] Device attached to network as child (MTD)
```

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

#### LED State Tracking

The firmware tracks LED state using a software variable (not by reading the GPIO):
- On boot, LED is initialized to OFF and internal state is set to 0
- PUT commands update both the GPIO and the internal state variable
- GET commands return the internal state variable

This is a common embedded pattern because:
- Many MCUs cannot reliably read back OUTPUT pin states
- Reading GPIO outputs can cause faults on some architectures (e.g., nRF54L15)
- Software tracking is simpler and more reliable

**Important**: The state variable and actual GPIO are synchronized as long as all LED changes go through the CoAP PUT handler.

### Button Resource (`/sw`)

**GET** - Returns button state:
```json
{"device_id": "f4ce3616e67c7a1c", "btns": [{"btn_id": 0, "state": 0}, {"btn_id": 1, "state": 0}]}
```

State values: 0 = not pressed, 1 = pressed

### Discovery Resource (`/.well-known/core`)

**GET** - Returns CoRE Link Format:
```
</led>;rt="led";if="actuator",</sw>;rt="button";if="sensor"
```

## Configuration

Key Kconfig options in `prj.conf`:

```kconfig
# Thread device mode (MTD = child, better for mobile devices)
CONFIG_OPENTHREAD_MTD=y
CONFIG_OPENTHREAD_FTD=n

# Auto-start Thread on boot (requires dataset in NVS)
CONFIG_OPENTHREAD_MANUAL_START=n

# Child timeout before parent removes device (4 minutes)
CONFIG_OPENTHREAD_MLE_CHILD_TIMEOUT=240

# TX power for range (+8 dBm max for nRF54L15)
CONFIG_OPENTHREAD_DEFAULT_TX_POWER=8
```

## Shell Commands

Available shell commands for debugging:

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

## Integration with Thread CoAP Bridge

This firmware is designed to work with the [Thread CoAP Bridge](https://github.com/Rosfly/thread-coap-bridge-addon) Home Assistant add-on:

1. Install the Thread CoAP Bridge add-on in Home Assistant
2. Flash this firmware to your device
3. Commission the device to your Thread network
4. The bridge will automatically discover the device via multicast
5. Device appears in Home Assistant with LED control and button sensor

### Re-Discovery After Extended Disconnection

If the device goes out of range for an extended period (15+ minutes):

1. The bridge marks the device as offline after 5 consecutive poll failures
2. Polling continues for 30 more attempts, then stops
3. When the device returns to range, it rejoins Thread (using stored credentials)
4. The bridge's **unicast re-discovery** mechanism probes the device at its last-known IPv6
5. Device is re-registered and appears online in Home Assistant

**Note:** SLAAC addresses (based on EUI-64) are stable across Thread reconnections, so the device retains the same IPv6 address after rejoining.

## License

Apache-2.0 (see Zephyr project license)
