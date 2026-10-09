# USB EP0 disconnect-path correction

## Confirmed software defect
The project-generated RP2040 DCD (cmake/PatchTinyUsb.cmake) cleared USB
PULLUP_EN and disabled all controller interrupts when an EP0 ABORT remained
pending for 1 ms. This happened inside chuni_dcd_task(), before the application
recovery switch could act. With recovery disabled, service_usb ignored its
false result, leaving USB disconnected. This is a proven code path, not proof
that every observed disconnect was caused by it.

## Changes
- EP0 timeout now records one event per deferred request, retains the pending
  operation, and restores USBCTRL_IRQ. It neither detaches nor reinitializes.
- One completion check per main-loop iteration; no wait loop. SETUP remains
  masked while abort is outstanding; other endpoint interrupts remain enabled.
- A host bus reset cancels the stale deferred request and restores SETUP IRQ.
- Input acquisition and HID report submission precede CDC RX/command work.
- Failed HID submission explicitly preserves dirty state; no synchronous retry.
- SAVE no longer forces Flash commit after ten seconds of active input. It
  requires two seconds of continuous idle. Flash itself still pauses service.

## HID audit
SDK 2.3.0 / bundled TinyUSB: tud_hid_n_report claims an endpoint, copies the
16-byte report into its class buffer and submits a transfer. It does not wait
for host polling or completion. Application busy handling returns immediately.
The unbounded abort loop found in rp2040_usb.c is under #if 0; the active EP0
abort path is handled by the project patch. No automatic recovery/watchdog was
enabled by this change. No hardware or host fault has been ruled out.

## Validation and limits
Default firmware build passed. Inspected generated dcd_rp2040_nonblocking.c:
the timeout branch no longer clears pull-up or all IRQ enables. Source order
is USB service, inputs/HID, CDC commands, SAVE. No device fault-injection or
long-duration hardware test was available. A genuinely non-completing EP0
abort can still leave control requests pending; this change keeps the CPU and
other endpoints serviced rather than forcing a disconnect. Hardware power
loss, host reset and signal faults cannot be prevented by this software patch.
