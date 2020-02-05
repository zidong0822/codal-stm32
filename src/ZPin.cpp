/*
The MIT License (MIT)

Copyright (c) 2016 British Broadcasting Corporation.
This software is provided by Lancaster University by arrangement with the BBC.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

/**
 * Class definition for ZPin.
 *
 * Commonly represents an I/O pin on the edge connector.
 */
#include "ZPin.h"
#include "Button.h"
#include "Timer.h"
#include "codal_target_hal.h"
#include "codal-core/inc/types/Event.h"
#include "PinNamesTypes.h"
#include "PeripheralPins.h"
#include "pinmap.h"

#define IO_STATUS_CAN_READ                                                                         \
    (IO_STATUS_DIGITAL_IN | IO_STATUS_EVENT_ON_EDGE | IO_STATUS_EVENT_PULSE_ON_EDGE | IO_STATUS_INTERRUPT_ON_EDGE)

#define PORTPINS 16
#define PINMASK (PORTPINS - 1)

#define GPIO_PORT() ((GPIO_TypeDef *)(GPIOA_BASE + 0x400 * ((int)name >> 4)))
#define GPIO_PIN() (1 << ((uint32_t)name & 0xf))

namespace codal
{

static ZPin *eventPin[16];
static ADC_HandleTypeDef AdcHandle;
static bool adcInited = false;

struct ZEventConfig
{
    CODAL_TIMESTAMP prevPulse;
};

inline int map(codal::PullMode pinMode)
{
    switch (pinMode)
    {
    case PullMode::Up:
        return GPIO_PULLUP;
    case PullMode::Down:
        return GPIO_PULLDOWN;
    case PullMode::None:
        return GPIO_NOPULL;
    }

    return GPIO_NOPULL;
}

/**
 * Constructor.
 * Create a ZPin instance, generally used to represent a pin on the edge connector.
 *
 * @param id the unique EventModel id of this component.
 *
 * @param name the mbed PinName for this ZPin instance.
 *
 * @param capability the capabilities this ZPin instance should have.
 *                   (PIN_CAPABILITY_DIGITAL, PIN_CAPABILITY_ANALOG, PIN_CAPABILITY_AD,
 * PIN_CAPABILITY_ALL)
 *
 * @code
 * ZPin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_ALL);
 * @endcode
 */
ZPin::ZPin(int id, PinNumber name, PinCapability capability) : codal::Pin(id, name, capability)
{
    this->pullMode = DEVICE_DEFAULT_PULLMODE;

    // Power up in a disconnected, low power state.
    // If we're unused, this is how it will stay...
    this->status = 0x00;

    this->pwmCfg = NULL;
}

void ZPin::disconnect()
{
    if (this->status & IO_STATUS_ANALOG_OUT)
    {
        if (this->pwmCfg)
            delete this->pwmCfg;
        this->pwmCfg = NULL;
    }

    if (this->status & (IO_STATUS_EVENT_ON_EDGE | IO_STATUS_EVENT_PULSE_ON_EDGE | IO_STATUS_INTERRUPT_ON_EDGE))
    {
        EXTI->IMR &= ~GPIO_PIN();

        int pin = (int)name & PINMASK;

#ifdef STM32F1
        volatile uint32_t *ptr = &AFIO->EXTICR[pin >> 2];
#else
        volatile uint32_t *ptr = &SYSCFG->EXTICR[pin >> 2];
#endif
        int shift = (pin & 3) * 4;
        int port = (int)name >> 4;

        // take over line for ourselves
        *ptr = (*ptr & ~(0xf << shift));


        if (this->evCfg)
            delete this->evCfg;
        this->evCfg = NULL;
    }

    if (this->status & IO_STATUS_TOUCH_IN)
    {
        if (this->btn)
            delete this->btn;
        this->btn = NULL;
    }

    status = 0;
}

// void ZPin::config(int status)
// {
//     disconnect();

//     int mode = STM_PIN_INPUT;
//     int pull = GPIO_NOPULL;

//     if (status & IO_STATUS_DIGITAL_OUT)
//         mode = STM_PIN_OUTPUT;
//     else if (this->status & IO_STATUS_ANALOG_IN)
//         mode = STM_PIN_ANALOG;

//     if (status & IO_STATUS_CAN_READ)
//         pull = map(this->pullMode);

//     pin_function(name, STM_PIN_DATA(mode, pull, 0));
// }

/**
 * Configures this IO pin as a digital output (if necessary) and sets the pin to 'value'.
 *
 * @param value 0 (LO) or 1 (HI)
 *
 * @return DEVICE_OK on success, DEVICE_INVALID_PARAMETER if value is out of range, or
 * DEVICE_NOT_SUPPORTED if the given pin does not have digital capability.
 *
 * @code
 * ZPin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_BOTH);
 * P0.setDigitalValue(1); // P0 is now HI
 * @endcode
 */
int ZPin::setDigitalValue(int value)
{
    // Ensure we have a valid value.
    if (value < 0 || value > 1)
        return DEVICE_INVALID_PARAMETER;

    // Move into a Digital input state if necessary.
    if (!(status & IO_STATUS_DIGITAL_OUT))
    {
        disconnect();
        pin_function(name, STM_PIN_DATA(STM_PIN_OUTPUT, GPIO_NOPULL, 0));
        status |= IO_STATUS_DIGITAL_OUT;
    }

    HAL_GPIO_WritePin(GPIO_PORT(), GPIO_PIN(), (GPIO_PinState)value);

    return DEVICE_OK;
}

/**
 * Configures this IO pin as a digital input (if necessary) and tests its current value.
 *
 *
 * @return 1 if this input is high, 0 if input is LO, or DEVICE_NOT_SUPPORTED
 *         if the given pin does not have digital capability.
 *
 * @code
 * ZPin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_BOTH);
 * P0.getDigitalValue(); // P0 is either 0 or 1;
 * @endcode
 */
int ZPin::getDigitalValue()
{
    // Move into a Digital input state if necessary.
    if (!(status &
          (IO_STATUS_DIGITAL_IN | IO_STATUS_EVENT_ON_EDGE | IO_STATUS_EVENT_PULSE_ON_EDGE | IO_STATUS_INTERRUPT_ON_EDGE)))
    {
        disconnect();
        pin_function(name, STM_PIN_DATA(STM_PIN_INPUT, map(this->pullMode), 0));
        status |= IO_STATUS_DIGITAL_IN;
    }

    return HAL_GPIO_ReadPin(GPIO_PORT(), GPIO_PIN());
}

/**
 * Configures this IO pin as a digital input with the specified internal pull-up/pull-down
 * configuraiton (if necessary) and tests its current value.
 *
 * @param pull one of the mbed pull configurations: PullUp, PullDown, PullNone
 *
 * @return 1 if this input is high, 0 if input is LO, or DEVICE_NOT_SUPPORTED
 *         if the given pin does not have digital capability.
 *
 * @code
 * ZPin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_BOTH);
 * P0.getDigitalValue(PullUp); // P0 is either 0 or 1;
 * @endcode
 */
int ZPin::getDigitalValue(PullMode pull)
{
    setPull(pull);
    return getDigitalValue();
}

int ZPin::obtainAnalogChannel()
{
    // Move into an analogue output state if necessary
    if (!(status & IO_STATUS_ANALOG_OUT))
    {
        disconnect();
        pin_function(name, STM_PIN_DATA(STM_PIN_OUTPUT, map(this->pullMode), 0));
        auto cfg = this->pwmCfg = new pwmout_t;
        pwmout_init(cfg, name);
        status = IO_STATUS_ANALOG_OUT;
    }

    return DEVICE_OK;
}

int ZPin::setPWM(uint32_t value, uint32_t period)
{
    // sanitise the level value
    if (value > period)
        value = period;

    if (obtainAnalogChannel())
        return DEVICE_INVALID_PARAMETER;

    auto cfg = this->pwmCfg;

    if (cfg->period != period)
        pwmout_period_us(cfg, period);
    pwmout_write(cfg, value);

    return DEVICE_OK;
}

/**
 * Configures this IO pin as an analog/pwm output, and change the output value to the given level.
 *
 * @param value the level to set on the output pin, in the range 0 - 1024
 *
 * @return DEVICE_OK on success, DEVICE_INVALID_PARAMETER if value is out of range, or
 * DEVICE_NOT_SUPPORTED if the given pin does not have analog capability.
 */
int ZPin::setAnalogValue(int value)
{
    // sanitise the level value
    if (value < 0 || value > DEVICE_PIN_MAX_OUTPUT)
        return DEVICE_INVALID_PARAMETER;

    uint32_t period = 20000;
    if (status & IO_STATUS_ANALOG_OUT)
        period = this->pwmCfg->period;

    return setPWM((uint64_t)value * period / DEVICE_PIN_MAX_OUTPUT, period);
}

/**
 * Configures this IO pin as an analog/pwm output (if necessary) and configures the period to be
 * 20ms, with a duty cycle between 500 us and 2500 us.
 *
 * A value of 180 sets the duty cycle to be 2500us, and a value of 0 sets the duty cycle to be 500us
 * by default.
 *
 * This range can be modified to fine tune, and also tolerate different servos.
 *
 * @param value the level to set on the output pin, in the range 0 - 180.
 *
 * @param range which gives the span of possible values the i.e. the lower and upper bounds (center
 * +/- range/2). Defaults to DEVICE_PIN_DEFAULT_SERVO_RANGE.
 *
 * @param center the center point from which to calculate the lower and upper bounds. Defaults to
 * DEVICE_PIN_DEFAULT_SERVO_CENTER
 *
 * @return DEVICE_OK on success, DEVICE_INVALID_PARAMETER if value is out of range, or
 * DEVICE_NOT_SUPPORTED if the given pin does not have analog capability.
 */
int ZPin::setServoValue(int value, int range, int center)
{
    // check if this pin has an analogue mode...
    if (!(PIN_CAPABILITY_ANALOG & capability))
        return DEVICE_NOT_SUPPORTED;

    // sanitise the servo level
    if (value < 0 || range < 1 || center < 1)
        return DEVICE_INVALID_PARAMETER;

    // clip - just in case
    if (value > DEVICE_PIN_MAX_SERVO_RANGE)
        value = DEVICE_PIN_MAX_SERVO_RANGE;

    // calculate the lower bound based on the midpoint
    int lower = (center - (range / 2)) * 1000;

    value = value * 1000;

    // add the percentage of the range based on the value between 0 and 180
    int scaled = lower + (range * (value / DEVICE_PIN_MAX_SERVO_RANGE));

    return setServoPulseUs(scaled / 1000);
}

/**
 * Configures this IO pin as an analogue input (if necessary), and samples the ZPin for its analog
 * value.
 *
 * @return the current analogue level on the pin, in the range 0 - 1024, or
 *         DEVICE_NOT_SUPPORTED if the given pin does not have analog capability.
 *
 * @code
 * ZPin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_BOTH);
 * P0.getAnalogValue(); // P0 is a value in the range of 0 - 1024
 * @endcode
 */
int ZPin::getAnalogValue()
{
    //check if this pin has an analogue mode...
    if(!(PIN_CAPABILITY_ANALOG & capability))
        return DEVICE_NOT_SUPPORTED;

    uint32_t function = pinmap_function(name, PinMap_ADC);
    uint8_t ch = STM_PIN_CHANNEL(function);

    // Move into an analogue input state if necessary.
    if (!(status & IO_STATUS_ANALOG_IN))
    {
        disconnect();

        if (!adcInited)
        {
             // init adc handler
            __HAL_RCC_ADC1_CLK_ENABLE(); // enable adc clock
            AdcHandle.Instance = ADC1;

#ifndef STM32F1
            AdcHandle.Init.ClockPrescaler        = ADC_CLOCKPRESCALER_PCLK_DIV4;          /* Asynchronous clock mode, input ADC clock not divided */
            AdcHandle.Init.Resolution            = ADC_RESOLUTION_10B;            /* 12-bit resolution for converted data */
            AdcHandle.Init.EOCSelection          = DISABLE;                       /* EOC flag picked-up to indicate conversion end */
            AdcHandle.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE; /* Parameter discarded because software trigger chosen */
            AdcHandle.Init.DMAContinuousRequests = DISABLE;                       /* DMA one-shot mode selected (not applied to this example) */
#endif
            AdcHandle.Init.DataAlign             = ADC_DATAALIGN_RIGHT;           /* Right-alignment for converted data */
            AdcHandle.Init.ScanConvMode          = DISABLE;                       /* Sequencer disabled (ADC conversion on only 1 channel: channel set on rank 1) */
            AdcHandle.Init.ContinuousConvMode    = DISABLE;                       /* Continuous mode disabled to have only 1 conversion at each conversion trig */
            AdcHandle.Init.NbrOfConversion       = 1;                             /* Parameter discarded because sequencer is disabled */
            AdcHandle.Init.DiscontinuousConvMode = DISABLE;                       /* Parameter discarded because sequencer is disabled */
            AdcHandle.Init.NbrOfDiscConversion   = 0;                             /* Parameter discarded because sequencer is disabled */
            AdcHandle.Init.ExternalTrigConv      = ADC_EXTERNALTRIGCONV_T1_CC1;   /* Software start to trig the 1st conversion manually, without external event */

            CODAL_ASSERT(HAL_ADC_Init(&AdcHandle) == HAL_OK, DEVICE_HARDWARE_CONFIGURATION_ERROR);
            adcInited = true;
        }

        pin_function(name, function);
        status |= IO_STATUS_ANALOG_IN;
    }

    ADC_ChannelConfTypeDef sConfig;
    sConfig.Channel = ch;
    sConfig.Rank = 1;
#ifdef STM32F1
    sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
#else
    sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
    sConfig.Offset = 0;
#endif

    CODAL_ASSERT(HAL_ADC_ConfigChannel(&AdcHandle, &sConfig) == HAL_OK, DEVICE_HARDWARE_CONFIGURATION_ERROR);

    //perform a read!
    HAL_ADC_Start(&AdcHandle);

    int adcValue = DEVICE_CANCELLED;

    if (HAL_ADC_PollForConversion(&AdcHandle, 500) == HAL_OK)
        adcValue = HAL_ADC_GetValue(&AdcHandle);

    HAL_ADC_Stop(&AdcHandle);

    return adcValue;
}

/**
 * Determines if this IO pin is currently configured as an input.
 *
 * @return 1 if pin is an analog or digital input, 0 otherwise.
 */
int ZPin::isInput()
{
    return (status & (IO_STATUS_DIGITAL_IN | IO_STATUS_ANALOG_IN)) == 0 ? 0 : 1;
}

/**
 * Determines if this IO pin is currently configured as an output.
 *
 * @return 1 if pin is an analog or digital output, 0 otherwise.
 */
int ZPin::isOutput()
{
    return (status & (IO_STATUS_DIGITAL_OUT | IO_STATUS_ANALOG_OUT)) == 0 ? 0 : 1;
}

/**
 * Determines if this IO pin is currently configured for digital use.
 *
 * @return 1 if pin is digital, 0 otherwise.
 */
int ZPin::isDigital()
{
    return (status & (IO_STATUS_DIGITAL_IN | IO_STATUS_DIGITAL_OUT)) == 0 ? 0 : 1;
}

/**
 * Determines if this IO pin is currently configured for analog use.
 *
 * @return 1 if pin is analog, 0 otherwise.
 */
int ZPin::isAnalog()
{
    return (status & (IO_STATUS_ANALOG_IN | IO_STATUS_ANALOG_OUT)) == 0 ? 0 : 1;
}

/**
 * Configures this IO pin as a "makey makey" style touch sensor (if necessary)
 * and tests its current debounced state.
 *
 * Users can also subscribe to Button events generated from this pin.
 *
 * @return 1 if pin is touched, 0 if not, or DEVICE_NOT_SUPPORTED if this pin does not support touch
 * capability.
 *
 * @code
 * DeviceMessageBus bus;
 *
 * ZPin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_ALL);
 * if(P0.isTouched())
 * {
 *     //do something!
 * }
 *
 * // subscribe to events generated by this pin!
 * bus.listen(DEVICE_ID_IO_P0, DEVICE_BUTTON_EVT_CLICK, someFunction);
 * @endcode
 */
int ZPin::isTouched()
{
    // check if this pin has a touch mode...
    if (!(PIN_CAPABILITY_DIGITAL & capability))
        return DEVICE_NOT_SUPPORTED;

    // Move into a touch input state if necessary.
    if (!(status & IO_STATUS_TOUCH_IN))
    {
        disconnect();
        this->btn = new Button(*this, id);
        status |= IO_STATUS_TOUCH_IN;
    }

    return this->btn->isPressed();
}

/**
 * Configures this IO pin as an analog/pwm output if it isn't already, configures the period to be
 * 20ms, and sets the pulse width, based on the value it is given.
 *
 * @param pulseWidth the desired pulse width in microseconds.
 *
 * @return DEVICE_OK on success, DEVICE_INVALID_PARAMETER if value is out of range, or
 * DEVICE_NOT_SUPPORTED if the given pin does not have analog capability.
 */
int ZPin::setServoPulseUs(int pulseWidth)
{
    // check if this pin has an analogue mode...
    if (!(PIN_CAPABILITY_ANALOG & capability))
        return DEVICE_NOT_SUPPORTED;

    // sanitise the pulse width
    if (pulseWidth < 0)
        return DEVICE_INVALID_PARAMETER;

    return setPWM(pulseWidth, DEVICE_DEFAULT_PWM_PERIOD);
}

/**
 * Configures the PWM period of the analog output to the given value.
 *
 * @param period The new period for the analog output in microseconds.
 *
 * @return DEVICE_OK on success.
 */
int ZPin::setAnalogPeriodUs(int period)
{
    if (status & IO_STATUS_ANALOG_OUT)
        // keep the % of duty cycle
        return setPWM((uint64_t)this->pwmCfg->pulse * period / this->pwmCfg->period, period);
    else
        return setPWM(0, period);
}

/**
 * Configures the PWM period of the analog output to the given value.
 *
 * @param period The new period for the analog output in milliseconds.
 *
 * @return DEVICE_OK on success, or DEVICE_NOT_SUPPORTED if the
 *         given pin is not configured as an analog output.
 */
int ZPin::setAnalogPeriod(int period)
{
    return setAnalogPeriodUs(period * 1000);
}

/**
 * Obtains the PWM period of the analog output in microseconds.
 *
 * @return the period on success, or DEVICE_NOT_SUPPORTED if the
 *         given pin is not configured as an analog output.
 */
uint32_t ZPin::getAnalogPeriodUs()
{
    if (!(status & IO_STATUS_ANALOG_OUT))
        return DEVICE_NOT_SUPPORTED;

    return this->pwmCfg->period;
}

/**
 * Obtains the PWM period of the analog output in milliseconds.
 *
 * @return the period on success, or DEVICE_NOT_SUPPORTED if the
 *         given pin is not configured as an analog output.
 */
int ZPin::getAnalogPeriod()
{
    return getAnalogPeriodUs() / 1000;
}

/**
 * Configures the pull of this pin.
 *
 * @param pull one of the mbed pull configurations: PullUp, PullDown, PullNone
 *
 * @return DEVICE_NOT_SUPPORTED if the current pin configuration is anything other
 *         than a digital input, otherwise DEVICE_OK.
 */
int ZPin::setPull(PullMode pull)
{
    if (pullMode == pull)
        return DEVICE_OK;

    pullMode = pull;

    // have to disconnect to flush the change to the hardware
    disconnect();
    getDigitalValue();

    return DEVICE_OK;
}

/**
 * This member function manages the calculation of the timestamp of a pulse detected
 * on a pin whilst in IO_STATUS_EVENT_PULSE_ON_EDGE or IO_STATUS_EVENT_ON_EDGE modes.
 *
 * @param eventValue the event value to distribute onto the message bus.
 */
void ZPin::pulseWidthEvent(int eventValue)
{
    Event evt(id, eventValue, CREATE_ONLY);
    auto now = evt.timestamp;
    auto previous = this->evCfg->prevPulse;

    if (previous != 0)
    {
        evt.timestamp -= previous;
        evt.fire();
    }

    this->evCfg->prevPulse = now;
}

void ZPin::eventCallback()
{
    bool isRise = HAL_GPIO_ReadPin(GPIO_PORT(), GPIO_PIN());

    if (status & IO_STATUS_EVENT_PULSE_ON_EDGE)
        pulseWidthEvent(isRise ? DEVICE_PIN_EVT_PULSE_LO : DEVICE_PIN_EVT_PULSE_HI);

    if (status & IO_STATUS_EVENT_ON_EDGE)
        Event(id, isRise ? DEVICE_PIN_EVT_RISE : DEVICE_PIN_EVT_FALL);

    if (status & IO_STATUS_INTERRUPT_ON_EDGE && gpio_irq)
        gpio_irq(isRise);
}

static void irq_handler()
{
    int pr = EXTI->PR;
    EXTI->PR = pr; // clear all pending bits

    for (int i = 0; i < 16; ++i)
    {
        if ((pr & (1 << i)) && eventPin[i])
            eventPin[i]->eventCallback();
    }
}

#define DEF(nm)                                                                                    \
    extern "C" void nm() { irq_handler(); }

DEF(EXTI0_IRQHandler)
DEF(EXTI1_IRQHandler)
DEF(EXTI2_IRQHandler)
DEF(EXTI3_IRQHandler)
DEF(EXTI4_IRQHandler)
DEF(EXTI9_5_IRQHandler)
DEF(EXTI15_10_IRQHandler)

static bool irqs_enabled = false;
static void enable_irqs()
{
    if(irqs_enabled)
        return;

    irqs_enabled = true;
    NVIC_SetPriority(EXTI0_IRQn,0);
    NVIC_SetPriority(EXTI1_IRQn,0);
    NVIC_SetPriority(EXTI2_IRQn,0);
    NVIC_SetPriority(EXTI3_IRQn,0);
    NVIC_SetPriority(EXTI4_IRQn,0);
    NVIC_SetPriority(EXTI9_5_IRQn,0);
    NVIC_SetPriority(EXTI15_10_IRQn,0);

    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);
    NVIC_EnableIRQ(EXTI2_IRQn);
    NVIC_EnableIRQ(EXTI3_IRQn);
    NVIC_EnableIRQ(EXTI4_IRQn);
    NVIC_EnableIRQ(EXTI9_5_IRQn);
    NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/**
 * This member function will construct an TimedInterruptIn instance, and configure
 * interrupts for rise and fall.
 *
 * @param eventType the specific mode used in interrupt context to determine how an
 *                  edge/rise is processed.
 *
 * @return DEVICE_OK on success
 */
int ZPin::enableRiseFallEvents(int eventType)
{
    // if we are in neither of the two modes, configure pin as a TimedInterruptIn.
    if (!(status & (IO_STATUS_EVENT_ON_EDGE | IO_STATUS_EVENT_PULSE_ON_EDGE | IO_STATUS_INTERRUPT_ON_EDGE)))
    {
        if (!(status & IO_STATUS_DIGITAL_IN))
            getDigitalValue();

        enable_irqs();

        int pin = (int)name & PINMASK;

        eventPin[pin] = this;

#ifdef STM32F1
        volatile uint32_t *ptr = &AFIO->EXTICR[pin >> 2];
#else
        volatile uint32_t *ptr = &SYSCFG->EXTICR[pin >> 2];
#endif
        int shift = (pin & 3) * 4;
        int port = (int)name >> 4;

        // take over line for ourselves
        *ptr = (*ptr & ~(0xf << shift)) | (port << shift);

        EXTI->EMR &= ~GPIO_PIN();
        EXTI->IMR |= GPIO_PIN();
        EXTI->RTSR |= GPIO_PIN();
        EXTI->FTSR |= GPIO_PIN();

        if (this->evCfg == NULL)
            this->evCfg = new ZEventConfig;

        auto cfg = this->evCfg;
        cfg->prevPulse = 0;
    }

    status &= ~(IO_STATUS_EVENT_ON_EDGE | IO_STATUS_EVENT_PULSE_ON_EDGE | IO_STATUS_INTERRUPT_ON_EDGE);

    // set our status bits accordingly.
    if (eventType == DEVICE_PIN_EVENT_ON_EDGE)
        status |= IO_STATUS_EVENT_ON_EDGE;
    else if (eventType == DEVICE_PIN_EVENT_ON_PULSE)
        status |= IO_STATUS_EVENT_PULSE_ON_EDGE;
    else if (eventType == DEVICE_PIN_INTERRUPT_ON_EDGE)
        status |= IO_STATUS_INTERRUPT_ON_EDGE;

    return DEVICE_OK;
}

/**
 * If this pin is in a mode where the pin is generating events, it will destruct
 * the current instance attached to this ZPin instance.
 *
 * @return DEVICE_OK on success.
 */
int ZPin::disableEvents()
{
    if (status & (IO_STATUS_EVENT_ON_EDGE | IO_STATUS_EVENT_PULSE_ON_EDGE | IO_STATUS_INTERRUPT_ON_EDGE | IO_STATUS_TOUCH_IN))
    {
        disconnect();
        getDigitalValue();
    }

    return DEVICE_OK;
}

/**
 * Configures the events generated by this ZPin instance.
 *
 * DEVICE_PIN_EVENT_ON_EDGE - Configures this pin to a digital input, and generates events whenever
 * a rise/fall is detected on this pin. (DEVICE_PIN_EVT_RISE, DEVICE_PIN_EVT_FALL)
 * DEVICE_PIN_EVENT_ON_PULSE - Configures this pin to a digital input, and generates events where
 * the timestamp is the duration that this pin was either HI or LO. (DEVICE_PIN_EVT_PULSE_HI,
 * DEVICE_PIN_EVT_PULSE_LO) DEVICE_PIN_EVENT_ON_TOUCH - Configures this pin as a makey makey style
 * touch sensor, in the form of a Button. Normal button events will be generated using the ID of
 * this pin. DEVICE_PIN_EVENT_NONE - Disables events for this pin.
 *
 * @param eventType One of: DEVICE_PIN_EVENT_ON_EDGE, DEVICE_PIN_EVENT_ON_PULSE,
 * DEVICE_PIN_EVENT_ON_TOUCH, DEVICE_PIN_EVENT_NONE
 *
 * @code
 * DeviceMessageBus bus;
 *
 * ZPin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_BOTH);
 * P0.eventOn(DEVICE_PIN_EVENT_ON_PULSE);
 *
 * void onPulse(Event evt)
 * {
 *     int duration = evt.timestamp;
 * }
 *
 * bus.listen(DEVICE_ID_IO_P0, DEVICE_PIN_EVT_PULSE_HI, onPulse, MESSAGE_BUS_LISTENER_IMMEDIATE)
 * @endcode
 *
 * @return DEVICE_OK on success, or DEVICE_INVALID_PARAMETER if the given eventype does not match
 *
 * @note In the DEVICE_PIN_EVENT_ON_PULSE mode, the smallest pulse that was reliably detected was
 * 85us, around 5khz. If more precision is required, please use the InterruptIn class supplied by
 * ARM mbed.
 */
int ZPin::eventOn(int eventType)
{
    switch (eventType)
    {
    case DEVICE_PIN_INTERRUPT_ON_EDGE:
    case DEVICE_PIN_EVENT_ON_EDGE:
    case DEVICE_PIN_EVENT_ON_PULSE:
        enableRiseFallEvents(eventType);
        break;

    case DEVICE_PIN_EVENT_ON_TOUCH:
        isTouched();
        break;

    case DEVICE_PIN_EVENT_NONE:
        disableEvents();
        break;

    default:
        return DEVICE_INVALID_PARAMETER;
    }

    return DEVICE_OK;
}
} // namespace codal
