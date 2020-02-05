//
// Created by ziwang on 2020/1/20.
//

#include "ZSPI_LED.h"
#include "CodalConfig.h"
#include "ErrorNo.h"
#include "CodalDmesg.h"
#include "codal-core/inc/driver-models/Timer.h"
#include "MessageBus.h"
#include "Event.h"
#include "CodalFiber.h"

#include "dma.h"
#include "pinmap.h"
#include "PeripheralPins.h"

#define LOG DMESG

namespace codal {

ZSPI_LED::ZSPI_LED(codal::Pin &mosi, codal::Pin &miso, codal::Pin &sclk) : codal::ZSPI(mosi, miso, sclk) {

    // freq = 3200000;
    // needsInit = true;
    // uint8_t txBuffer[8 * 3 * 4 + 1] = {};
    // uint32_t txSize = 8 * 3 * 4 + 1;
    // // init spi data
    // transfer(txBuffer, txSize, NULL, 0);
    // fiber_sleep(1000);
    // transfer(txBuffer, txSize, NULL, 0);
}

int ZSPI_LED::transfer(const uint8_t *txBuffer, uint32_t txSize, uint8_t *rxBuffer, uint32_t rxSize) {
    fiber_wake_on_event(DEVICE_ID_NOTIFY, transferCompleteEventCode);
    auto res = startTransfer(txBuffer, txSize, rxBuffer, rxSize, NULL, NULL);
    LOG("SPI ->");
//    schedule();
    LOG("SPI <-");
    return res;
}

int ZSPI_LED::startTransfer(const uint8_t *txBuffer, uint32_t txSize, uint8_t *rxBuffer,
                            uint32_t rxSize, PVoidCallback doneHandler, void *arg) {
    int res;

    init_internal();

    LOG("SPI start %p/%d %p/%d D=%p", txBuffer, txSize, rxBuffer, rxSize, doneHandler);

    this->doneHandler = doneHandler;
    this->doneHandlerArg = arg;

    // disable IRQ or else risk a race in HAL, between starting DMA request
    // and getting the DMA-done IRQ
    if (doneHandler)
        target_disable_irq();

    if (txSize && rxSize) {
        CODAL_ASSERT(txSize == rxSize, DEVICE_SPI_ERROR); // we could support this if needed
        res = HAL_SPI_TransmitReceive_DMA(&spi, (uint8_t *) txBuffer, rxBuffer, txSize);
    } else if (txSize) {
        res = HAL_SPI_Transmit(&spi, (uint8_t *) txBuffer, txSize, 100);
//        res = HAL_SPI_Transmit_DMA(&spi, (uint8_t *) txBuffer, txSize);
    } else if (rxSize) {
        res = HAL_SPI_Receive_DMA(&spi, rxBuffer, rxSize);
    } else {
        res = HAL_OK;
    }

    if (doneHandler)
        target_enable_irq();

    CODAL_ASSERT(res == HAL_OK, DEVICE_SPI_ERROR);
    return 0;
}

int ZSPI_LED::setFrequency(uint32_t frequency) {
    // Do not use set frequency
    return DEVICE_OK;
}

}
