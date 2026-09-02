# DMI/SMBIOS Hardware Detection Setup

Junocash can display detailed hardware information (CPU model, memory DIMMs, motherboard) by reading DMI/SMBIOS tables. On Linux, these tables require special permissions.

## Quick Setup

Run the provided setup script:

```bash
cd junocash/contrib
sudo ./setup-dmi-permissions.sh
```

The script will:
- Create a `dmi` group
- Add your user to the `dmi` group
- Configure udev rules for DMI table access
- Apply the changes immediately

## After Running the Script

**Important:** You must log out and log back in for group membership to take effect.

Alternatively, run:
```bash
newgrp dmi
```

Verify the setup:
```bash
groups
# Should show 'dmi' in your groups list
```

## What You'll See

After setup, Junocash mining status will display:

```
CPU          AMD Ryzen 9 5950X 16-Core Processor
Memory       64 GB
  DIMM_A1: 16 GB DDR4 @ 3200 MHz Corsair CMK16GX4M2B3200C16
  DIMM_A2: 16 GB DDR4 @ 3200 MHz Corsair CMK16GX4M2B3200C16
  DIMM_B1: 16 GB DDR4 @ 3200 MHz Corsair CMK16GX4M2B3200C16
  DIMM_B2: 16 GB DDR4 @ 3200 MHz Corsair CMK16GX4M2B3200C16
Motherboard  ASUSTeK COMPUTER INC. TUF GAMING X570-PLUS (WI-FI)
```

## Manual Setup

If you prefer to set it up manually:

```bash
# Create group and add user
sudo groupadd dmi
sudo usermod -a -G dmi $USER

# Create udev rule
sudo tee /etc/udev/rules.d/99-dmi.rules << 'EOF'
SUBSYSTEM=="dmi", KERNEL=="DMI", GROUP="dmi", MODE="0440"
SUBSYSTEM=="dmi", KERNEL=="smbios_entry_point", GROUP="dmi", MODE="0440"
EOF

# Apply rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=dmi

# Log out and log back in
```

## Troubleshooting

If hardware information doesn't appear:

1. **Check group membership:**
   ```bash
   groups
   # Should show 'dmi'
   ```

2. **Check file permissions:**
   ```bash
   ls -la /sys/firmware/dmi/tables/
   # DMI should be readable by 'dmi' group
   ```

3. **Check debug log:**
   ```bash
   tail ~/.junocash/debug.log | grep DMI
   ```

4. **If you see "Failed to read hardware information":**
   - Ensure you logged out and back in after running the setup script
   - Verify udev rules are active: `udevadm info /sys/firmware/dmi/tables/DMI`

## Why is this needed?

DMI/SMBIOS tables contain sensitive hardware information, so Linux restricts access to root by default. This setup grants read-only access to users in the `dmi` group, similar to how hugepages permissions work.

## Security Note

This setup only grants **read-only** access to hardware identification tables. It does not allow:
- Writing to hardware
- Modifying BIOS/UEFI settings
- Accessing other system resources
- Any privileged operations

The DMI tables contain the same information you can see with `sudo dmidecode`, but without requiring root privileges every time.
