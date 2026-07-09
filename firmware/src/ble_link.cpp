#include "ble_link.h"
#include <NimBLEDevice.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string>
#include <algorithm>
#include <cstring>

#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  /* host writes */
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  /* we notify */

static NimBLECharacteristic *tx_char = nullptr;
static volatile bool connected_flag = false;
static SemaphoreHandle_t rx_mutex = nullptr;
static std::string rx_accum;

class BuddyServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
        connected_flag = true;
    }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
        connected_flag = false;
        NimBLEDevice::startAdvertising();
    }
};

class BuddyRxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        NimBLEAttValue v = c->getValue();
        xSemaphoreTake(rx_mutex, portMAX_DELAY);
        rx_accum.append((const char *)v.data(), v.length());
        if (rx_accum.size() > 4096) rx_accum.clear();  /* runaway guard */
        xSemaphoreGive(rx_mutex);
    }
};

void ble_link_init(void)
{
    rx_mutex = xSemaphoreCreateMutex();
    NimBLEDevice::init("ClaudeBuddy");
    NimBLEDevice::setMTU(185);

    NimBLEServer *server = NimBLEDevice::createServer();
    server->setCallbacks(new BuddyServerCallbacks());

    NimBLEService *svc = server->createService(NUS_SERVICE_UUID);
    NimBLECharacteristic *rx = svc->createCharacteristic(
        NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new BuddyRxCallbacks());
    tx_char = svc->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setName("ClaudeBuddy");
    adv->addServiceUUID(svc->getUUID());
    adv->start();
}

bool ble_link_connected(void)
{
    return connected_flag;
}

void ble_link_send(const char *line)
{
    if (!connected_flag || tx_char == nullptr) return;
    std::string s(line);
    s += "\n";
    const size_t chunk = 180;   /* fits in MTU 185 minus ATT header */
    for (size_t i = 0; i < s.size(); i += chunk) {
        size_t n = std::min(chunk, s.size() - i);
        tx_char->setValue((const uint8_t *)s.data() + i, n);
        tx_char->notify();
    }
}

int ble_link_read_line(char *buf, size_t maxlen)
{
    int out = 0;
    xSemaphoreTake(rx_mutex, portMAX_DELAY);
    size_t pos = rx_accum.find('\n');
    if (pos != std::string::npos) {
        size_t n = std::min(pos, maxlen - 1);
        memcpy(buf, rx_accum.data(), n);
        buf[n] = '\0';
        out = (int)n;
        rx_accum.erase(0, pos + 1);
    }
    xSemaphoreGive(rx_mutex);
    return out;
}
