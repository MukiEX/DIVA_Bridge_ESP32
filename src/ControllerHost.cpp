#include "ControllerHost.h"

#include <string.h>

#include <atomic>
#include <usb/usb_helpers.h>
#include <usb/usb_host.h>

#include "DivaState.h"

using namespace diva;

namespace ControllerHost {

namespace {

constexpr int MAX_PENDING_NEW = 8;

usb_host_client_handle_t client = nullptr;
TaskHandle_t clientTask = nullptr;
TaskHandle_t ledTask = nullptr;
SemaphoreHandle_t lock = nullptr;  // guards `act` setup/teardown vs ledTask

// Addresses reported by NEW_DEV, opened from the client task.
uint8_t pendingNew[MAX_PENDING_NEW];
std::atomic<int> pendingNewCount{0};
std::atomic<bool> gone{false};  // DEV_GONE for the active device

struct Active {
  bool open = false;
  bool closing = false;
  usb_device_handle_t dev = nullptr;
  const Profile *prof = nullptr;
  uint16_t vid = 0, pid = 0;
  uint8_t intf = 0;
  uint8_t epIn = 0, epOut = 0;  // epOut 0 = none
  uint16_t mpsIn = 0, mpsOut = 0;
  usb_transfer_t *in = nullptr;
  usb_transfer_t *out = nullptr;
  volatile int inFlight = 0;
  // LED OUT state (chunked into MPS-sized packets, as libusb does on the wire)
  volatile bool outBusy = false;
  uint8_t outBuf[MAX_DEVICE_OUT];
  size_t outLen = 0, outPos = 0;
  uint8_t lastReport[MAX_DEVICE_OUT];
  size_t lastLen = 0;
};
Active act;

std::atomic<uint32_t> reports{0};

void inCallback(usb_transfer_t *t);
void outCallback(usb_transfer_t *t);

void clientEvent(const usb_host_client_event_msg_t *msg, void *) {
  switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      if (pendingNewCount < MAX_PENDING_NEW) {
        pendingNew[pendingNewCount] = msg->new_dev.address;
        pendingNewCount++;
      }
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      if (act.open && msg->dev_gone.dev_hdl == act.dev) gone = true;
      break;
    default:
      break;
  }
}

bool submitIn() {
  act.in->num_bytes = act.mpsIn;  // IN transfers must be a multiple of MPS
  if (usb_host_transfer_submit(act.in) != ESP_OK) return false;
  act.inFlight = act.inFlight + 1;
  return true;
}

void inCallback(usb_transfer_t *t) {
  act.inFlight = act.inFlight - 1;
  if (t->status == USB_TRANSFER_STATUS_COMPLETED && act.prof) {
    uint8_t pkt[MAX_IN_PACKET];
    uint16_t ext = 0;
    size_t n = toDivaller(*act.prof, DivaState::options(), t->data_buffer,
                          t->actual_num_bytes, pkt, ext);
    if (n) {
      DivaState::setInput(pkt, n, ext);
      reports++;
    }
  } else if (t->status != USB_TRANSFER_STATUS_COMPLETED &&
             t->status != USB_TRANSFER_STATUS_CANCELED &&
             t->status != USB_TRANSFER_STATUS_NO_DEVICE) {
    Serial.printf("[USB] IN transfer status %d\n", (int)t->status);
  }
  bool retry = t->status == USB_TRANSFER_STATUS_COMPLETED ||
               t->status == USB_TRANSFER_STATUS_TIMED_OUT ||
               t->status == USB_TRANSFER_STATUS_ERROR;
  if (!act.closing && retry) submitIn();
}

bool submitOutChunk() {
  size_t n = act.outLen - act.outPos;
  if (n > act.mpsOut) n = act.mpsOut;
  memcpy(act.out->data_buffer, act.outBuf + act.outPos, n);
  act.out->num_bytes = n;
  act.outPos += n;
  return usb_host_transfer_submit(act.out) == ESP_OK;
}

void outCallback(usb_transfer_t *t) {
  bool more = t->status == USB_TRANSFER_STATUS_COMPLETED &&
              act.outPos < act.outLen && !act.closing;
  if (more && submitOutChunk()) return;
  act.outBusy = false;
  if (ledTask) xTaskNotifyGive(ledTask);  // pick up anything newer
}

/** Find the interface/endpoints to use, as diva_bridge.py does: prefer the
 * interface with the profile's IN endpoint, else the first with any IN. */
bool chooseEndpoints(const usb_config_desc_t *cfg, const Profile &p) {
  struct Choice {
    bool ok = false;
    uint8_t intf = 0, alt = 0, epIn = 0, epOut = 0;
    uint16_t mpsIn = 0, mpsOut = 0;
  } best, fallback;

  int offset = 0;
  const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
  while ((d = usb_parse_next_descriptor_of_type(
              d, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset))) {
    const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;
    Choice c;
    c.intf = intf->bInterfaceNumber;
    c.alt = intf->bAlternateSetting;
    bool hasPreferred = false;
    for (int i = 0; i < intf->bNumEndpoints; i++) {
      int epOff = offset;
      const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(
          intf, i, cfg->wTotalLength, &epOff);
      if (!ep) break;
      uint8_t type = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
      if (type != USB_BM_ATTRIBUTES_XFER_INT && type != USB_BM_ATTRIBUTES_XFER_BULK)
        continue;
      uint8_t addr = ep->bEndpointAddress;
      uint16_t mps = USB_EP_DESC_GET_MPS(ep);
      if (addr & 0x80) {
        if (addr == p.epIn) hasPreferred = true;
        if (!c.epIn || addr == p.epIn) {
          c.epIn = addr;
          c.mpsIn = mps;
        }
      } else if (!c.epOut || addr == p.epOut) {
        c.epOut = addr;
        c.mpsOut = mps;
      }
    }
    Serial.printf("[USB]   interface %d alt %d: class 0x%02x, IN 0x%02x, OUT 0x%02x\n",
                  c.intf, c.alt, intf->bInterfaceClass, c.epIn, c.epOut);
    c.ok = c.epIn != 0;
    if (!best.ok && hasPreferred) best = c;
    if (!fallback.ok && c.ok) fallback = c;
  }
  Choice &c = best.ok ? best : fallback;
  if (!c.ok) return false;
  act.intf = c.intf;
  act.epIn = c.epIn;
  act.epOut = c.epOut;
  act.mpsIn = c.mpsIn ? c.mpsIn : 64;
  act.mpsOut = c.mpsOut ? c.mpsOut : 64;
  return usb_host_interface_claim(client, act.dev, c.intf, c.alt) == ESP_OK;
}

void openDevice(uint8_t addr) {
  usb_device_handle_t dev;
  if (usb_host_device_open(client, addr, &dev) != ESP_OK) return;

  const usb_device_desc_t *dd;
  if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK) {
    usb_host_device_close(client, dev);
    return;
  }
  uint16_t vid = dd->idVendor, pid = dd->idProduct;
  const Profile *p = findProfile(vid, pid);
  if (!p) {
    if (const char *hint = wrongModeHint(vid, pid))
      Serial.printf("[USB] found %04X:%04X: %s\n", vid, pid, hint);
    else if (dd->bDeviceClass != USB_CLASS_HUB)
      Serial.printf("[USB] ignoring %04X:%04X (not a supported controller)\n", vid, pid);
    usb_host_device_close(client, dev);
    return;
  }
  if (act.open) {
    Serial.printf("[USB] %s plugged in, but %s is already in use - ignoring it\n",
                  p->name, act.prof->name);
    usb_host_device_close(client, dev);
    return;
  }

  const usb_config_desc_t *cfg;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
    usb_host_device_close(client, dev);
    return;
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  act = Active();
  act.dev = dev;
  act.prof = p;
  act.vid = vid;
  act.pid = pid;
  bool ok = chooseEndpoints(cfg, *p);
  if (ok) ok = usb_host_transfer_alloc(act.mpsIn < 64 ? 64 : act.mpsIn, 0, &act.in) == ESP_OK;
  if (ok && act.epOut)
    ok = usb_host_transfer_alloc(MAX_DEVICE_OUT, 0, &act.out) == ESP_OK;
  if (!ok) {
    Serial.printf("[USB] found the %s but couldn't claim its interface\n", p->name);
    if (act.in) usb_host_transfer_free(act.in);
    if (act.out) usb_host_transfer_free(act.out);
    act = Active();
    xSemaphoreGive(lock);
    usb_host_device_close(client, dev);
    return;
  }
  act.in->device_handle = dev;
  act.in->bEndpointAddress = act.epIn;
  act.in->callback = inCallback;
  if (act.out) {
    act.out->device_handle = dev;
    act.out->bEndpointAddress = act.epOut;
    act.out->callback = outCallback;
  }
  act.open = true;
  gone = false;
  xSemaphoreGive(lock);

  Serial.printf("[USB] %s connected (%04X:%04X; IN 0x%02x%s; %s)\n", p->name, vid,
                pid, act.epIn, act.epOut ? "" : ", no OUT endpoint", p->ledNote);
  DivaState::setController(p, vid, pid);
  submitIn();
}

void closeActive() {
  Serial.printf("[USB] %s lost\n", act.prof ? act.prof->name : "controller");
  DivaState::setController(nullptr, 0, 0);
  act.closing = true;
  // Cancel what's in flight; the callbacks arrive through handle_events.
  usb_host_endpoint_halt(act.dev, act.epIn);
  usb_host_endpoint_flush(act.dev, act.epIn);
  if (act.epOut) {
    usb_host_endpoint_halt(act.dev, act.epOut);
    usb_host_endpoint_flush(act.dev, act.epOut);
  }
  for (int i = 0; i < 100 && (act.inFlight > 0 || act.outBusy); i++)
    usb_host_client_handle_events(client, pdMS_TO_TICKS(10));

  xSemaphoreTake(lock, portMAX_DELAY);
  usb_host_interface_release(client, act.dev, act.intf);
  usb_host_transfer_free(act.in);
  if (act.out) usb_host_transfer_free(act.out);
  usb_host_device_close(client, act.dev);
  act = Active();
  gone = false;
  xSemaphoreGive(lock);
}

void libTask(void *arg) {
  usb_host_config_t cfg = {};
  cfg.skip_phy_setup = false;
  cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  ESP_ERROR_CHECK(usb_host_install(&cfg));
  xTaskNotifyGive((TaskHandle_t)arg);
  while (true) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

void clientLoop(void *) {
  usb_host_client_config_t cc = {};
  cc.is_synchronous = false;
  cc.max_num_event_msg = 8;
  cc.async.client_event_callback = clientEvent;
  cc.async.callback_arg = nullptr;
  ESP_ERROR_CHECK(usb_host_client_register(&cc, &client));
  while (true) {
    usb_host_client_handle_events(client, pdMS_TO_TICKS(100));
    if (gone && act.open) closeActive();
    while (pendingNewCount > 0) {
      uint8_t addr = pendingNew[pendingNewCount - 1];
      pendingNewCount--;
      openDevice(addr);
    }
  }
}

void ledLoop(void *) {
  uint8_t dla[MAX_DEVICE_OUT];
  uint8_t report[MAX_DEVICE_OUT];
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (act.open && !act.closing && act.out && !act.outBusy) {
      size_t n = DivaState::takeLeds(dla, sizeof(dla));
      size_t m = n ? ledsToDevice(*act.prof, DivaState::options(), dla, n, report) : 0;
      // Don't resend identical frames (except to a Divaller, as the PC bridge).
      bool same = m && m == act.lastLen && !memcmp(report, act.lastReport, m) &&
                  act.prof->kind != Kind::Divaller;
      if (m && !same) {
        memcpy(act.outBuf, report, m);
        act.outLen = m;
        act.outPos = 0;
        act.outBusy = true;
        if (submitOutChunk()) {
          memcpy(act.lastReport, report, m);
          act.lastLen = m;
        } else {
          act.outBusy = false;
        }
      }
    }
    xSemaphoreGive(lock);
  }
}

}  // namespace

void begin() {
  lock = xSemaphoreCreateMutex();
  Serial.println("[USB] Installing USB host library (hubs supported)...");
  xTaskCreatePinnedToCore(libTask, "usb_lib", 4096, xTaskGetCurrentTaskHandle(),
                          4, nullptr, 0);
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
  xTaskCreatePinnedToCore(clientLoop, "usb_client", 6144, nullptr, 5, &clientTask, 0);
  xTaskCreatePinnedToCore(ledLoop, "usb_leds", 4096, nullptr, 4, &ledTask, 0);
  DivaState::setLedListener(ledTask);
  Serial.println("[USB] Ready - plug in a Divaller, ZhouSensor (HDD mode) or HORI (arcade mode)");
}

String describe() {
  if (!act.open || !act.prof) return "none";
  char buf[96];
  snprintf(buf, sizeof(buf), "%s (%04X:%04X)", act.prof->name, act.vid, act.pid);
  return buf;
}

uint32_t reportCount() { return reports; }

}  // namespace ControllerHost
