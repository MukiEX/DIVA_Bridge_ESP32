/**
 * @file ControllerHost.h
 * @brief USB host side: finds a supported DIVA controller (directly on the
 * OTG port or behind a hub), reads its reports and sends it LED packets.
 *
 * Talks to the controller's endpoints directly instead of going through the
 * HID class driver: the Divaller is a vendor-specific (WinUSB) device, not
 * HID, and the ZhouSensor/Divaller need OUT reports for their LEDs.
 */

#pragma once

#include <Arduino.h>

namespace ControllerHost {

/** Install the USB host library and start the host tasks. */
void begin();

/** One-line description of what's attached, for the status command. */
String describe();

/** Every attached USB device (hubs too) as JSON: addr, parent (0 = OTG
 * port), port, vid, pid, cls, speed, product, mfr, role. */
String usbTreeJson();

/** The OTG root port's hardware state, e.g. "powered, device connected,
 * enabled, full speed, lines D+=1 D-=0". */
String portStatus();

/** IN reports received since boot. */
uint32_t reportCount();

}  // namespace ControllerHost
