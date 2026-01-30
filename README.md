# Thread CoAP Server for Home Assistant Integration

A Zephyr-based Thread CoAP server designed for integration with Home Assistant via the Thread CoAP Bridge add-on. Supports LED control, button input, battery monitoring with real ADC measurements, NFC-based commissioning, and automatic network reconnection.

https://github.com/Rosfly/thread-coap-bridge-addon

## Features

- **NFC Commissioning**: Provision Thread credentials via Android phone NFC - no cables needed
- **Thread SED Mode**: Runs as Sleepy End Device for ultra-low power consumption
- **SED Discovery Grace Period**: Stays awake for 2 minutes after boot for bridge discovery
- **CoAP Server**: Exposes `/led`, `/sw` (button), `/uptime`, `/battery`, and `/voltage` resources
- **CoAP Observe**: Push notifications for LED and button state changes (RFC 7641)
- **Battery Monitoring**: Real ADC measurements with LiPo discharge curve lookup table
- **Power Optimization**: TPS22916C load switch enables voltage divider only during measurement to save power
- **Automatic Reconnection**: Network monitor thread handles disconnection recovery
- **NVS Storage**: Thread credentials persist across reboots

## Hardware Support

- Seeed XIAO nRF54L15 (primary target)
- NFC antenna connected to NFC1/NFC2 pads
- Other nRF52/nRF53/nRF54 boards with Thread support

### Battery Voltage Measurement Circuit

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

**Important**: This project requires **nRF Connect SDK**, not mainline Zephyr. The NFC libraries are only available in nRF Connect SDK.

### Production Build (Battery-Optimized) and Flash

Default build disables UART/logging for battery-only boot:

```bash
cd /home/ros/ncs
source .venv/bin/activate
west build -p always -b xiao_nrf54l15/nrf54l15/cpuapp \
    --shield seeed_xiao_expansion_board \
    -s /home/ros/dev/coap_server_sed_nfc
west flash
```

Or use nRF side bar in VSCode

### Debug Build (USB/UART + Shell) and Flash

For development with USB serial console and OpenThread shell. **Note**: This build will NOT boot from battery alone - USB connection required.

```bash
cd /home/ros/ncs
source .venv/bin/activate
west build -p always -b xiao_nrf54l15/nrf54l15/cpuapp \
    --shield seeed_xiao_expansion_board \
    -s /home/ros/dev/coap_server_sed_nfc \
    -- -DOVERLAY_CONFIG="prj_uart.conf"
west flash

# Monitor serial output (115200 baud)
# Use VSCode Serial Monitor or: screen /dev/ttyACM0 115200
```

The debug build enables:
- UART console logging
- OpenThread shell (`ot` commands)
- Useful for troubleshooting and manual commissioning

## Commissioning

The device needs to be commissioned to your Thread network once. After commissioning, credentials are stored in NVS and the device auto-joins on subsequent boots.

### Method 1: NFC Commissioning (Recommended)

No cables needed - just tap your phone to provision the device.

#### Step 1: Get Thread Dataset

From Home Assistant:
1. Go to **Settings** → **Devices & Services** → **Thread**
2. Click on your Thread network
3. Find the **Active Operational Dataset** (TLV hex string)

Or export (-x) from OpenThread Border Router:
```bash
# SSH into OTBR container
docker exec -it addon_core_openthread_border_router ot-ctl dataset active -x
```

Example output:
```
0e080000000000010000000300000f35060004001fffe00208dead...
```

#### Step 2: Install NFC Tools App

Download [NFC Tools](https://play.google.com/store/apps/details?id=com.wakdev.wdnfc) (free) on your Android phone.

#### Step 3: Write Dataset via NFC

1. Open NFC Tools
2. Tap **Write** → **Add a record** → **Text**
3. Paste the hex string from Step 1
4. Tap **Write**
5. Hold phone to device's NFC antenna

#### LED Feedback

| State | LED Pattern | Meaning |
|-------|-------------|---------|
| Slow blink (500ms) | Waiting for NFC | Ready to receive dataset |
| Rapid blink (100ms) | Phone detected | Processing data |
| Solid 1 second | Success | Dataset applied, joining network |
| 3 rapid blinks | Error | Invalid data, retry |

### Method 2: Shell Commissioning (Debug Build Only)

Use this method when debugging or if NFC isn't available.

```shell
# Connect to serial console (115200 baud)

# Set the Thread dataset
ot dataset set active 0e080000000000010000000300000f35060004001fffe00208...

# Enable IPv6 interface
ot ifconfig up

# Start Thread
ot thread start

# Verify connection (wait 10-30 seconds)
ot state
# Should show: child
```

**Note**: After shell commissioning with debug build, flash the production build for battery operation.

## Boot Flow

```
BOOT
  │
  ▼
Check sw0 button
  │
  ├─── sw0 held ───────► Clear stored dataset
  │                         │
  ▼                         ▼
Check NVS for dataset     NFC mode (LED blinks)
  │                         │
  ├─── Dataset found ──► Start Thread ──► Join network ──► SED mode
  │                         │
  └─── No dataset ────► NFC mode (LED blinks)
                            │
                            ▼
                        Phone taps NFC
                            │
                            ▼
                        Parse & apply dataset
                            │
                            ▼
                        Start Thread ──► Join network ──► SED mode
```

### Re-commissioning

To switch to a different Thread network, hold the **sw0** button while pressing **reset** switch. The device clears its stored dataset and enters NFC commissioning mode. Then provision the new network dataset via NFC as usual.

## Network Recovery

The firmware includes a network monitor that handles disconnection:

1. **Detection**: Monitor thread detects `DETACHED` state
2. **Wait**: Allows OpenThread's internal reattachment (up to 1 minute)
3. **Force Restart**: If still detached after 1 minute, forces Thread stack restart
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

**Observe** - Subscribe for push notifications on button press/release.

### Battery Resource (`/battery`)

**GET** - Returns battery percentage:
```json
{"device_id": "f4ce3616e67c7a1c", "value": 75}
```

### Voltage Resource (`/voltage`)

**GET** - Returns battery voltage in millivolts:
```json
{"device_id": "f4ce3616e67c7a1c", "value": 3850}
```

### Uptime Resource (`/uptime`)

**GET** - Returns milliseconds since boot:
```json
{"device_id": "f4ce3616e67c7a1c", "value": 123456}
```

Used by the bridge to detect device reboots and re-register CoAP Observe subscriptions.

### Discovery Resource (`/.well-known/core`)

**GET** - Returns CoRE Link Format:
```
</led>;rt="led",</sw>;rt="button",</battery>;rt="battery",</voltage>;rt="voltage",</uptime>;rt="uptime"
```

## SED (Sleepy End Device) Mode

The device operates as a Sleepy End Device for optimal battery life:

- Radio OFF most of the time
- Wakes every 5 seconds to poll parent for queued messages (CONFIG_OPENTHREAD_POLL_PERIOD=5000 in prj.conf:70)
- Wakes immediately on button press (GPIO interrupt)

### Discovery Grace Period

After joining the network, the device stays awake for **2 minutes** to allow the bridge to discover it via multicast. After the grace period, it switches to full SED sleep mode.

```
Boot → Join Thread → Grace Period (2 min, awake) → SED Mode (sleeping)
                      ↑
                Bridge discovers device here
```

### Latency Tradeoffs

| Operation | Latency | Notes |
|-----------|---------|-------|
| Button notification | ~100-200ms | GPIO wakes device immediately |
| GET /battery | Up to 15s | Request queued until next poll |
| PUT /led | Up to 15s | Command queued until next poll |

## Configuration

### Key Kconfig Options (prj.conf)

```kconfig
# Thread SED mode
CONFIG_OPENTHREAD_MTD=y
CONFIG_OPENTHREAD_MTD_SED=y
CONFIG_OPENTHREAD_POLL_PERIOD=15000

# NFC Commissioning
CONFIG_OT_COAP_NFC_COMMISSION=y
CONFIG_NFC_T4T_NRFXLIB=y
CONFIG_NFC_NDEF=y
CONFIG_NFC_NDEF_MSG=y
CONFIG_NFC_NDEF_PARSER=y

# System workqueue stack (required for NFC processing)
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096

# TX power for range
CONFIG_OPENTHREAD_DEFAULT_TX_POWER=8
```

### Board-specific Options (boards/*.conf)

```kconfig
CONFIG_OT_COAP_SAMPLE_SERVER=y
CONFIG_OT_COAP_SAMPLE_LED=y
CONFIG_OT_COAP_SAMPLE_SW=y
CONFIG_OT_COAP_SAMPLE_BATTERY=y
```

## Shell Commands (Debug Build)

Available when built with `prj_uart.conf`:

```shell
# OpenThread commands
ot state                    # Show current role (child/detached/disabled)
ot ipaddr                   # Show IPv6 addresses
ot dataset active           # Show active dataset
ot dataset active -x        # Show dataset as hex
ot ping <ipv6>              # Ping another device

# Check SED mode
ot mode                     # Should show "-" (no 'r' = SED)
ot pollperiod               # Should show 15000 (ms)
```

## Troubleshooting

### NFC: Device doesn't detect phone

- Ensure NFC antenna is properly connected to NFC1/NFC2 pads
- Check that phone NFC is enabled
- Try different phone positioning (center of antenna)

### NFC: "Invalid data" error (3 blinks)

- Verify hex string is valid (even number of characters, 0-9 and a-f only)
- Ensure you're using a **Text** record, not URI or other types
- Check dataset length (max 254 bytes = 508 hex chars)

### NFC: Want to re-provision

- Hold **sw0** while pressing **reset** to clear the stored dataset and enter NFC mode
- Then tap your phone with the new Thread dataset as usual

### Device Not Joining Network

1. Verify dataset is correct: `ot dataset active` (debug build)
2. Check channel matches your network
3. Verify Thread is enabled: `ot state` should not be "disabled"

### Device Keeps Detaching

1. Check signal strength - move closer to border router
2. Check for interference on Thread channel
3. TX power is set to maximum (+8 dBm)

### Build Fails with Undefined NFC Symbols

You're building with mainline Zephyr instead of nRF Connect SDK. NFC libraries are only in nRF Connect SDK.

## Integration with Thread CoAP Bridge

This firmware works with the [Thread CoAP Bridge](https://github.com/Rosfly/thread-coap-bridge-addon) Home Assistant add-on:

1. Install the Thread CoAP Bridge add-on
2. Flash this firmware and commission via NFC
3. **Within 2 minutes**, the bridge discovers the device via multicast
4. Device appears in Home Assistant with LED control, button sensor, battery monitoring
5. After 2 minutes, device enters SED sleep mode

The bridge fully supports SED devices with 75-second timeouts for all CoAP operations.

## Implementation Notes

### NFC Processing Architecture

The NFC implementation uses a two-phase approach to avoid crashes:

1. **Work Queue Phase**: NDEF parsing runs in system workqueue (handles NFC callback safely)
2. **Main Thread Phase**: OpenThread API calls (`otDatasetSetActiveTlvs`, `otIp6SetEnabled`, `otThreadSetEnabled`) run in main thread

This separation is necessary because OpenThread APIs require specific thread context and sufficient stack space.

### OpenThread API (nRF Connect SDK 2.x)

Use the current non-deprecated API:
```c
// Get instance directly
otInstance *ot = openthread_get_default_instance();

// Lock/unlock without context parameter
openthread_mutex_lock();
// ... OpenThread API calls ...
openthread_mutex_unlock();
```

### NVS Persistence

Thread dataset persistence is handled automatically:
- `CONFIG_SETTINGS` is auto-enabled by OpenThread when `CONFIG_FLASH=y`
- `otDatasetSetActiveTlvs()` saves to NVS through the Settings subsystem
- On boot, OpenThread loads the dataset from NVS automatically

## File Structure

| File | Purpose |
|------|---------|
| `src/main.c` | Entry point, NFC commissioning check |
| `src/nfc_commission.c` | NFC T4T handling, NDEF parsing |
| `src/nfc_commission.h` | NFC public API |
| `src/coap_utils.c` | CoAP server initialization |
| `src/led.c` | LED resource |
| `src/button.c` | Button resource with observe |
| `src/battery.c` | Battery/voltage ADC measurement |
| `src/network_monitor.c` | Connection recovery, SED grace period |
| `prj.conf` | Production config (no UART) |
| `prj_uart.conf` | Debug overlay (UART + shell) |

## Future Enhancements

- [x] Hold button on boot to clear dataset and re-enter NFC mode
- [ ] QR code fallback for phones without NFC
- [ ] BLE commissioning as alternative method
- [ ] Home Assistant add-on to generate NFC tags with dataset

## References

- [nRF Connect SDK NFC samples](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/samples/nfc.html)
- [OpenThread Dataset API](https://openthread.io/reference/group/api-operational-dataset)
- [Thread CoAP Bridge Add-on](https://github.com/Rosfly/thread-coap-bridge-addon)

## License

MIT License - see [LICENSE](LICENSE) file.
