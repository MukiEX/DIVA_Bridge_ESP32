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
std::atomic<bool> gone{false};
volatile bool rescan = true;  // USB tree changed: scan from the client task
// An endpoint errored/stalled; recovered from the client task (not callbacks).
std::atomic<int> inRecover{0};   // 0 none, 1 error, 2 stall
std::atomic<int> outRecover{0};  // DEV_GONE for the active device

struct Active {
  bool open = false;
  uint8_t addr = 0;
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
      rescan = true;
      if (pendingNewCount < MAX_PENDING_NEW) {
        pendingNew[pendingNewCount] = msg->new_dev.address;
        pendingNewCount++;
      }
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      rescan = true;
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
  if (act.closing) return;
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    if (!submitIn()) inRecover = 1;
  } else if (t->status == USB_TRANSFER_STATUS_STALL) {
    inRecover = 2;  // endpoint halted: clear it from the client task
  } else if (t->status == USB_TRANSFER_STATUS_ERROR ||
             t->status == USB_TRANSFER_STATUS_TIMED_OUT ||
             t->status == USB_TRANSFER_STATUS_OVERFLOW) {
    inRecover = 1;
  }
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
  if (!act.closing && t->status != USB_TRANSFER_STATUS_COMPLETED &&
      t->status != USB_TRANSFER_STATUS_CANCELED &&
      t->status != USB_TRANSFER_STATUS_NO_DEVICE)
    outRecover = t->status == USB_TRANSFER_STATUS_STALL ? 2 : 1;
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
  act.addr = addr;
  act.prof = p;
  act.vid = vid;
  act.pid = pid;
  bool claimed = chooseEndpoints(cfg, *p);
  bool ok = claimed;
  if (ok) ok = usb_host_transfer_alloc(act.mpsIn < 64 ? 64 : act.mpsIn, 0, &act.in) == ESP_OK;
  if (ok && act.epOut)
    ok = usb_host_transfer_alloc(MAX_DEVICE_OUT, 0, &act.out) == ESP_OK;
  if (!ok) {
    Serial.printf("[USB] found the %s but couldn't claim its interface\n", p->name);
    if (act.in) usb_host_transfer_free(act.in);
    if (act.out) usb_host_transfer_free(act.out);
    if (claimed) usb_host_interface_release(client, dev, act.intf);
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
  inRecover = 0;
  outRecover = 0;
  if (!submitIn()) {
    Serial.println("[USB] couldn't start reading the controller");
    gone = true;  // tear it down from the client loop
  }
}

// ---------------------------------------------------- endpoint recovery
std::atomic<bool> ctrlDone{false};
void ctrlCallback(usb_transfer_t *) { ctrlDone = true; }

/** CLEAR_FEATURE(ENDPOINT_HALT) to the device, for a stalled endpoint. */
bool clearHaltOnDevice(uint8_t ep) {
  usb_transfer_t *t;
  if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &t) != ESP_OK) return false;
  usb_setup_packet_t *sp = (usb_setup_packet_t *)t->data_buffer;
  sp->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                      USB_BM_REQUEST_TYPE_RECIP_ENDPOINT;
  sp->bRequest = USB_B_REQUEST_CLEAR_FEATURE;
  sp->wValue = 0;  // ENDPOINT_HALT
  sp->wIndex = ep;
  sp->wLength = 0;
  t->num_bytes = sizeof(usb_setup_packet_t);
  t->device_handle = act.dev;
  t->bEndpointAddress = 0;
  t->callback = ctrlCallback;
  ctrlDone = false;
  bool ok = usb_host_transfer_submit_control(client, t) == ESP_OK;
  for (int i = 0; ok && !ctrlDone && i < 50; i++)
    usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
  ok = ok && ctrlDone && t->status == USB_TRANSFER_STATUS_COMPLETED;
  if (!ctrlDone) return false;  // still owned by the stack; leak rather than free
  usb_host_transfer_free(t);
  return ok;
}

/** Flush + clear a halted endpoint (and the device side for a stall). */
bool recoverEndpoint(uint8_t ep, bool stall) {
  usb_host_endpoint_halt(act.dev, ep);  // no-op if the error already halted it
  usb_host_endpoint_flush(act.dev, ep);
  for (int i = 0; i < 10 && ep == act.epIn && act.inFlight > 0; i++)
    usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
  if (stall && !clearHaltOnDevice(ep)) return false;
  return usb_host_endpoint_clear(act.dev, ep) == ESP_OK;
}

void recover() {
  if (int r = inRecover.exchange(0)) {
    Serial.printf("[USB] IN endpoint %s - recovering\n", r == 2 ? "stalled" : "error");
    if (!recoverEndpoint(act.epIn, r == 2) || !submitIn()) {
      Serial.println("[USB] recovery failed - dropping the controller");
      gone = true;
      return;
    }
  }
  if (int r = outRecover.exchange(0)) {
    Serial.printf("[USB] OUT endpoint %s - recovering\n", r == 2 ? "stalled" : "error");
    act.outBusy = false;
    if (!recoverEndpoint(act.epOut, r == 2)) {
      Serial.println("[USB] LED endpoint unusable - LEDs off until replug");
      act.epOut = 0;
    }
  }
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

// ------------------------------------------------------------- USB tree
struct UsbNode {
  uint8_t addr, parent, port;  // parent 0 = the ESP32's OTG port
  uint16_t vid, pid;
  uint8_t cls;
  uint8_t speed;  // 0 low, 1 full, 2 high
  char product[40];
  char mfr[32];
};
constexpr int MAX_NODES = 16;
UsbNode tree[MAX_NODES];
int treeCount = 0;
portMUX_TYPE treeMux = portMUX_INITIALIZER_UNLOCKED;

void strDesc(const usb_str_desc_t *d, char *out, size_t max) {
  size_t n = 0;
  if (d && d->bLength > 2) {
    size_t chars = (d->bLength - 2) / 2;
    for (size_t i = 0; i < chars && n + 1 < max; i++) {
      uint16_t c = d->wData[i];
      out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
  }
  out[n] = 0;
  while (n && out[n - 1] == ' ') out[--n] = 0;
}

/** Walk every enumerated device (hubs included) and record who hangs off
 * which hub port. Runs in the client task; logs attach/detach. */
void scanTree() {
  uint8_t addrs[MAX_NODES];
  int n = 0;
  if (usb_host_device_addr_list_fill(MAX_NODES, addrs, &n) != ESP_OK) return;
  UsbNode nodes[MAX_NODES];
  usb_device_handle_t handles[MAX_NODES], parents[MAX_NODES];
  bool opened[MAX_NODES];
  int count = 0;
  for (int i = 0; i < n && count < MAX_NODES; i++) {
    usb_device_handle_t h;
    bool mine = act.open && addrs[i] == act.addr;
    if (mine) h = act.dev;
    else if (usb_host_device_open(client, addrs[i], &h) != ESP_OK) continue;
    UsbNode &nd = nodes[count];
    memset(&nd, 0, sizeof(nd));
    nd.addr = addrs[i];
    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(h, &dd) == ESP_OK) {
      nd.vid = dd->idVendor;
      nd.pid = dd->idProduct;
      nd.cls = dd->bDeviceClass;
    }
    usb_device_info_t info;
    parents[count] = nullptr;
    if (usb_host_device_info(h, &info) == ESP_OK) {
      nd.speed = info.speed == USB_SPEED_LOW ? 0 : info.speed == USB_SPEED_FULL ? 1 : 2;
      nd.port = info.parent.port_num;
      parents[count] = info.parent.dev_hdl;
      strDesc(info.str_desc_product, nd.product, sizeof(nd.product));
      strDesc(info.str_desc_manufacturer, nd.mfr, sizeof(nd.mfr));
    }
    handles[count] = h;
    opened[count] = !mine;
    count++;
  }
  for (int i = 0; i < count; i++) {
    nodes[i].parent = 0;
    for (int j = 0; j < count; j++)
      if (parents[i] && handles[j] == parents[i]) nodes[i].parent = nodes[j].addr;
  }
  for (int i = 0; i < count; i++)
    if (opened[i]) usb_host_device_close(client, handles[i]);

  // Log what came and went.
  for (int i = 0; i < count; i++) {
    bool seen = false;
    for (int j = 0; j < treeCount; j++)
      seen |= tree[j].addr == nodes[i].addr && tree[j].vid == nodes[i].vid && tree[j].pid == nodes[i].pid;
    if (!seen) {
      const UsbNode &d = nodes[i];
      char where[32];
      if (d.parent) snprintf(where, sizeof(where), "hub %u port %u", d.parent, d.port);
      else snprintf(where, sizeof(where), "OTG port");
      Serial.printf("[USB] attached: %04X:%04X %s%s%s (%s, %s speed, addr %u)\n", d.vid, d.pid,
                    d.mfr, d.mfr[0] && d.product[0] ? " " : "", d.product,
                    d.cls == USB_CLASS_HUB ? "hub" : where, d.speed == 0 ? "low" : d.speed == 1 ? "full" : "high",
                    d.addr);
    }
  }
  for (int j = 0; j < treeCount; j++) {
    bool still = false;
    for (int i = 0; i < count; i++) still |= tree[j].addr == nodes[i].addr && tree[j].vid == nodes[i].vid;
    if (!still) Serial.printf("[USB] detached: %04X:%04X %s\n", tree[j].vid, tree[j].pid, tree[j].product);
  }
  portENTER_CRITICAL(&treeMux);
  memcpy(tree, nodes, sizeof(UsbNode) * count);
  treeCount = count;
  portEXIT_CRITICAL(&treeMux);
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
    if (act.open && !act.closing && (inRecover || outRecover)) {
      xSemaphoreTake(lock, portMAX_DELAY);
      recover();
      xSemaphoreGive(lock);
    }
    if (gone && act.open) closeActive();
    while (pendingNewCount > 0) {
      uint8_t addr = pendingNew[pendingNewCount - 1];
      pendingNewCount--;
      openDevice(addr);
    }
    static uint32_t lastScan = 0;
    if (rescan || millis() - lastScan > 2000) {
      rescan = false;
      lastScan = millis();
      scanTree();
    }
  }
}

void ledLoop(void *) {
  uint8_t dla[MAX_DEVICE_OUT];
  uint8_t report[MAX_DEVICE_OUT];
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (act.open && !act.closing && act.out && act.epOut && !act.outBusy) {
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

String usbTreeJson() {
  UsbNode copy[MAX_NODES];
  int n;
  portENTER_CRITICAL(&treeMux);
  n = treeCount;
  memcpy(copy, tree, sizeof(UsbNode) * n);
  portEXIT_CRITICAL(&treeMux);
  uint8_t ctlAddr = act.open ? act.addr : 0;
  String o = "[";
  for (int i = 0; i < n; i++) {
    const UsbNode &d = copy[i];
    const Profile *p = findProfile(d.vid, d.pid);
    const char *hint = wrongModeHint(d.vid, d.pid);
    String role = d.cls == USB_CLASS_HUB ? "hub"
                  : d.addr == ctlAddr    ? String("controller: ") + (p ? p->name : "?")
                  : p                    ? String("supported, not in use (one controller at a time)")
                  : hint                 ? String(hint)
                                         : String("not a supported controller");
    char head[160];
    snprintf(head, sizeof(head),
             "%s{\"addr\":%u,\"parent\":%u,\"port\":%u,\"vid\":\"%04X\",\"pid\":\"%04X\","
             "\"cls\":%u,\"speed\":\"%s\",",
             i ? "," : "", d.addr, d.parent, d.port, d.vid, d.pid, d.cls,
             d.speed == 0 ? "low" : d.speed == 1 ? "full" : "high");
    o += head;
    // Names come from the device: keep them JSON-safe.
    auto safe = [](const char *s) {
      String r;
      for (; *s; s++) r += (*s == '"' || *s == '\\') ? '\'' : *s;
      return r;
    };
    o += "\"product\":\"" + safe(d.product) + "\",\"mfr\":\"" + safe(d.mfr) + "\",\"role\":\"" +
         safe(role.c_str()) + "\"}";
  }
  return o + "]";
}

}  // namespace ControllerHost
