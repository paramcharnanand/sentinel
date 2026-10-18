# Sentinel Demo Profiles

Two device baselines can be selected from the board's five-way switch. The first is
FAN; the second is temporarily called TEMP until its appliance is chosen. Selection must
discard the previous device's partial measurement window and alarm history.

SW2 left selects FAN, right selects TEMP, and up learns/relearns the selected
device from 30 normal one-second windows. A saved baseline automatically starts
monitoring when selected. An empty slot stays IDLE and cannot claim OK until
learning succeeds. The LCD and UART identify the selected device and whether
learning was saved.

Learned means and standard deviations for the existing four features survive a
power-off in the E8's reserved user MRAM. Writes must never target the firmware,
TOC, or any other user-MRAM data. A torn write, corrupt record, or unexpected
content must not be accepted as a valid profile or cause an unsafe overwrite.
No profile ships pre-learned. Firmware flashing may or may not preserve user MRAM;
that must be checked with the actual flashing workflow.

The existing z-score ranges, standard-deviation floors, and three-window
hysteresis are retained: OK means the live readings are close to the selected
baseline, CHECK/ALERT mean sensor change, not a proven mechanical fault.
Gyroscope and chip temperature are not part of this change because the second
appliance and sensor coupling are unverified.
