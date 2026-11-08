#include "BleConnectionStatus.h"
#include "BLEHostConfiguration.h"

BleConnectionStatus::BleConnectionStatus(void) : _configuration(nullptr)
{
}

void BleConnectionStatus::setConfiguration(const BLEHostConfiguration* config)
{
    _configuration = config;
}

void BleConnectionStatus::onConnect(NimBLEServer *pServer, NimBLEConnInfo& connInfo)
{
    uint16_t minInterval = 6;
    uint16_t maxInterval = 7;
    uint16_t latency = 0;
    uint16_t timeout = 600;
    
    if (_configuration) {
        minInterval = _configuration->getMinConnectionInterval();
        maxInterval = _configuration->getMaxConnectionInterval();
        latency = _configuration->getSlaveLatency();
        timeout = _configuration->getSupervisionTimeout();
    }

    pServer->updateConnParams(connInfo.getConnHandle(), minInterval, maxInterval, latency, timeout);
}

// DIVA Bridge patch: let the app see pairing completions and disconnect reasons.
extern void divaAuthComplete(NimBLEConnInfo& connInfo) __attribute__((weak));
extern void divaDisconnected(NimBLEConnInfo& connInfo, int reason) __attribute__((weak));

void BleConnectionStatus::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason)
{
    if (divaDisconnected) divaDisconnected(connInfo, reason);
    // DIVA Bridge ESP32 patch: with several centrals connected, one leaving
    // must not mute reports to the others (NimBLE has already removed the
    // leaving peer from the count here).
    this->connected = pServer->getConnectedCount() > 0;
}

bool BleConnectionStatus::isConnected(){
    return this->connected;
}

void BleConnectionStatus::onAuthenticationComplete(NimBLEConnInfo& connInfo)
{
    this->connected = true;
    if (divaAuthComplete) divaAuthComplete(connInfo);
}
