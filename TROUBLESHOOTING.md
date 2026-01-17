# CoAP Server Troubleshooting Guide

## Issue: No output on /dev/ttyACM0

### UART Configuration
The XIAO nRF54L15 uses **UART20** for console/shell output at **115200 baud**.

### Step 1: Verify USB Connection
```bash
# Check if device is detected
ls -l /dev/ttyACM*

# Check dmesg for USB enumeration
dmesg | tail -20

# Expected output should show:
# cdc_acm X-X:X.X: ttyACMX: USB ACM device
```

### Step 2: Test Serial Connection
```bash
# Using screen (Ctrl+A then K to exit)
screen /dev/ttyACM0 115200

# OR using minicom
minicom -D /dev/ttyACM0 -b 115200

# OR using pyserial
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
```

### Step 3: Check Build Configuration
Verify these configs are enabled in your build:

```bash
cd /home/ros/zephyrproject/build
grep -E "CONFIG_CONSOLE|CONFIG_UART_CONSOLE|CONFIG_SHELL|CONFIG_LOG" zephyr/.config | grep -v "^#"
```

Expected output should include:
```
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_SHELL=y
CONFIG_LOG=y
CONFIG_LOG_BACKEND_UART=y
```

### Step 4: Flash and Monitor
```bash
source /home/ros/zephyrproject/.venv/bin/activate
cd /home/ros/zephyrproject/zephyr/samples/rosprojects/coap_client

# Flash
west flash

# Immediately after flashing, open serial
screen /dev/ttyACM0 115200
```

Press **RESET button** on the board to see boot messages.

### Expected Boot Output
You should see something like:
```
*** Booting Zephyr OS build v4.2.99 ***
[00:00:00.000,000] <inf> coap: Initializing OpenThread CoAP server
[00:00:00.123,000] <inf> coap: Registering LED rsc
uart:~$
```

### Step 5: Test Shell Commands
Once you see the `uart:~$` prompt, try these commands:

```bash
# Check OpenThread state
uart:~$ ot state

# Should show: disabled, detached, child, router, or leader

# Start OpenThread (if not auto-started)
uart:~$ ot ifconfig up
uart:~$ ot thread start

# Check network info
uart:~$ ot channel
uart:~$ ot panid
uart:~$ ot networkname

# Get IPv6 addresses
uart:~$ ot ipaddr

# Check network data
uart:~$ ot netdata show
```

## Common Issues

### Issue: Device not detected as /dev/ttyACM0

**Cause:** USB CDC ACM driver issue or permissions

**Solution:**
```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER
# Log out and log back in

# Or use sudo temporarily
sudo screen /dev/ttyACM0 115200
```

### Issue: Garbled output or no text

**Cause:** Wrong baud rate

**Solution:** Ensure 115200 baud is used. The nRF54L15 uses 115200 by default.

### Issue: Shell prompt doesn't appear

**Cause:** Shell might be disabled or logging is too verbose

**Solution:**
1. Press Enter a few times to trigger shell prompt
2. Check if `CONFIG_SHELL=y` in build config
3. Reduce log level: Change `CONFIG_LOG_MAX_LEVEL=3` to `CONFIG_LOG_MAX_LEVEL=2`

### Issue: "ot state" shows "disabled"

**Cause:** OpenThread didn't auto-start or network credentials are invalid

**Solution:**
```bash
# Manually start OpenThread
uart:~$ ot ifconfig up
uart:~$ ot thread start

# Check if it attaches
uart:~$ ot state
# Should change to: child, router, or leader

# If still disabled, check network credentials
uart:~$ ot networkname
# Should show: ha-thread-8c41

uart:~$ ot channel
# Should show: 15

uart:~$ ot panid
# Should show: 0x8c41
```

### Issue: Device won't join Home Assistant network

**Possible causes:**
1. Home Assistant Thread Border Router is not running
2. Network credentials don't match
3. Channel mismatch
4. Radio interference

**Solution:**
```bash
# On device, check active dataset
uart:~$ ot dataset active

# Manually set dataset if needed
uart:~$ ot dataset set active 0e080000000000010000000300000f4a0300001135060004001fffe00208ecb645cc6cad637f0708fdfab5fdaf0ae7a30510ec0bed215681cba746525d9562b06112030e68612d7468726561642d3863343101028c4104104fe0a1aa17c1045b23b9d38e1969db900c0402a0f7f8

# Restart Thread
uart:~$ ot ifconfig down
uart:~$ ot ifconfig up
uart:~$ ot thread start
```

## Debugging OpenThread Connection

### Enable more verbose logging
Edit `prj.conf` and increase log level:
```
CONFIG_LOG_MAX_LEVEL=4
CONFIG_OPENTHREAD_LOG_LEVEL_DEBUG=y
```

Rebuild and reflash.

### Check radio is working
```bash
uart:~$ ot channel
15

uart:~$ ot txpower
# Should show transmission power
```

### Monitor network attachment
```bash
uart:~$ ot state
# Watch this change from: disabled -> detached -> child/router

# If it stays "detached", check:
uart:~$ ot scan
# Should show nearby Thread networks including ha-thread-8c41
```

## Getting Help

When reporting issues, include:
1. Output of `dmesg | tail -20` after plugging in device
2. Serial console output (boot messages)
3. Output of `ot state`, `ot ipaddr`, `ot networkname`
4. Build configuration: `grep CONFIG_OPENTHREAD build/zephyr/.config`
