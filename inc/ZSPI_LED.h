//
// Created by ziwang on 2020/1/20.
//

#ifndef CODAL_ZSPI_LED_H
#define CODAL_ZSPI_LED_H

#include "ZSPI.h"

namespace codal {
class ZSPI_LED : public codal::ZSPI {
public:
    /**
     * Initialize SPI instance with given pins. But transfer with out dma and do not call schedule
     *
     * Default setup is 1 MHz, 8 bit, mode 0.
     */
    ZSPI_LED(codal::Pin &mosi, codal::Pin &miso, codal::Pin &sclk);

    /**
     * Writes and reads from the SPI bus concurrently. Waits un-scheduled for transfer to finish.
     *
     * Either buffer can be NULL.
     */
    int transfer(const uint8_t *txBuffer, uint32_t txSize, uint8_t *rxBuffer,
                 uint32_t rxSize) override;

    int startTransfer(const uint8_t *txBuffer, uint32_t txSize, uint8_t *rxBuffer,
                      uint32_t rxSize, PVoidCallback doneHandler, void *arg) override;
};
}

#endif //CODAL_ZSPI_LED_H
