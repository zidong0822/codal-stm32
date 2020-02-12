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
     * Constructor SPI instance with given pins. But transfer with out dma and do not call schedule
     *
     * Default setup is 3.2 MHz, 8 bit, mode 0.
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

    /**
     * LED use const 3.2 MHz this function is empty
     * @param frequency
     * @return
     */
    int setFrequency(uint32_t frequency) override;

    /**
     * Light LED by id
     * @param id which led
     * @param red 0~255
     * @param green 0~255
     * @param blue 0~255
     * @param auto_delay function will sleep 1ms for spi data write, if do not want auto delay 1ms, set it false
     * @return error code
     */
    int show(int id, uint8_t red, uint8_t green, uint8_t blue, bool auto_delay = true);

private:
    bool firstLight;
    // LED SPI BUFFER
    uint8_t buf_bytes[4] = {0x88, 0x8E, 0xE8, 0xEE};
    uint8_t txBuffer[8 * 3 * 4 + 1] = {};
    uint32_t txSize = 8 * 3 * 4 + 1;
    uint8_t mask = 0x03;
};
}

#endif //CODAL_ZSPI_LED_H
