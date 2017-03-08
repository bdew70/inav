/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>

#include <platform.h>

#if defined(SONAR)

#include "build/build_config.h"

#include "common/time.h"

#include "time.h"
#include "exti.h"
#include "io.h"
#include "gpio.h"
#include "nvic.h"
#include "rcc.h"

#include "logging.h"

#include "drivers/rangefinder.h"
#include "drivers/sonar_hcsr04.h"

#define HCSR04_MAX_RANGE_CM 400 // 4m, from HC-SR04 spec sheet
#define HCSR04_DETECTION_CONE_DECIDEGREES 300 // recommended cone angle30 degrees, from HC-SR04 spec sheet
#define HCSR04_DETECTION_CONE_EXTENDED_DECIDEGREES 450 // in practice 45 degrees seems to work well


/* HC-SR04 consists of ultrasonic transmitter, receiver, and control circuits.
 * When triggered it sends out a series of 40KHz ultrasonic pulses and receives
 * echo from an object. The distance between the unit and the object is calculated
 * by measuring the traveling time of sound and output it as the width of a TTL pulse.
 *
 * *** Warning: HC-SR04 operates at +5V ***
 *
 */

static volatile timeDelta_t hcsr04SonarPulseTravelTime = 0;
static volatile uint32_t lastMeasurementReceivedAt;
static timeMs_t lastMeasurementStartedAt = 0;

#ifdef USE_EXTI
static extiCallbackRec_t hcsr04_extiCallbackRec;
#endif

static IO_t echoIO;
static IO_t triggerIO;

#if !defined(UNIT_TEST)
void hcsr04_extiHandler(extiCallbackRec_t* cb)
{
    static timeUs_t timing_start;
    UNUSED(cb);

    if (IORead(echoIO) != 0) {
        timing_start = micros();
    } else {
        const timeUs_t timing_stop = micros();
        if (timing_stop > timing_start) {
            lastMeasurementReceivedAt = millis();
            hcsr04SonarPulseTravelTime = timing_stop - timing_start;
        }
    }
}
#endif

void hcsr04_init(void)
{
}

/*
 * Start a range reading
 * Called periodically by the scheduler
 * Measurement reading is done asynchronously, using interrupt
 */
void hcsr04_start_reading(void)
{
#if !defined(UNIT_TEST)
    // the firing interval of the trigger signal should be greater than 60ms
    // to avoid interference between consecutive measurements.
    #define HCSR04_MinimumFiringIntervalMs 60
    const timeMs_t timeNowMs = millis();
    if (timeNowMs > lastMeasurementStartedAt + HCSR04_MinimumFiringIntervalMs) {
        lastMeasurementStartedAt = timeNowMs;
#ifdef SONAR_TRIG_INVERTED
        IOLo(triggerIO);
        delayMicroseconds(11);
        IOHi(triggerIO);
#else
        IOHi(triggerIO);
        delayMicroseconds(11);
        IOLo(triggerIO);
#endif
    }
#endif
}

/**
 * Get the distance that was measured by the last pulse, in centimeters.
 */
int32_t hcsr04_get_distance(void)
{
    const timeMs_t timeNowMs = millis();
    static int32_t lastCalculatedDistance = RANGEFINDER_OUT_OF_RANGE;

    /* 3 possible scenarios:
     *   1. Response was after request - good, calculate new response
     *   2. Request was no earlier than 60ms ago and no response since then - still good, return last valid response
     *   3. Request was earlier than 60ms ago and no response since then - hardware failure
     */

    if ((lastMeasurementReceivedAt > lastMeasurementStartedAt)) {
        /* 
         * The speed of sound is 340 m/s or approx. 29 microseconds per centimeter.
         * The ping travels out and back, so to find the distance of the
         * object we take half of the distance traveled.
         *
         * 340 m/s = 0.034 cm/microsecond = 29.41176471 *2 = 58.82352941 rounded to 59 
         */

        lastCalculatedDistance = hcsr04SonarPulseTravelTime / 59;
        if (lastCalculatedDistance > HCSR04_MAX_RANGE_CM) {
            lastCalculatedDistance = RANGEFINDER_OUT_OF_RANGE;
        }
    }
    else if ((timeNowMs - lastMeasurementStartedAt) > HCSR04_MinimumFiringIntervalMs) {
        lastCalculatedDistance = RANGEFINDER_HARDWARE_FAILURE;
    }

    return lastCalculatedDistance;
}

bool hcsr04Detect(rangefinderDev_t *dev, const rangefinderHardwarePins_t * sonarHardwarePins)
{
    bool detected = false;

#ifdef STM32F10X
    // enable AFIO for EXTI support
    RCC_ClockCmd(RCC_APB2(AFIO), ENABLE);
#endif

#if defined(STM32F3) || defined(STM32F4)
    RCC_ClockCmd(RCC_APB2(SYSCFG), ENABLE);
#endif

    triggerIO = IOGetByTag(sonarHardwarePins->triggerTag);
    echoIO = IOGetByTag(sonarHardwarePins->echoTag);

    if (IOGetOwner(triggerIO) != OWNER_FREE) {
        addBootlogEvent4(BOOT_EVENT_HARDWARE_IO_CONFLICT, BOOT_EVENT_FLAGS_WARNING, IOGetOwner(triggerIO), OWNER_SONAR);
        return false;
    }

    if (IOGetOwner(echoIO) != OWNER_FREE) {
        addBootlogEvent4(BOOT_EVENT_HARDWARE_IO_CONFLICT, BOOT_EVENT_FLAGS_WARNING, IOGetOwner(echoIO), OWNER_SONAR);
        return false;
    }

    // trigger pin
    IOInit(triggerIO, OWNER_SONAR, RESOURCE_OUTPUT, 0);
    IOConfigGPIO(triggerIO, IOCFG_OUT_PP);
    IOLo(triggerIO);
    delay(100);

    // echo pin
    IOInit(echoIO, OWNER_SONAR, RESOURCE_INPUT, 0);
    IOConfigGPIO(echoIO, IOCFG_IN_FLOATING);

    /* HC-SR04 echo line should be low by default and should return a response pulse when triggered */
    if (IORead(echoIO) == false) {
        for (int i = 0; i < 5 && !detected; i++) {
            timeMs_t requestTime = millis();
            hcsr04_start_reading();

            while ((millis() - requestTime) < HCSR04_MinimumFiringIntervalMs) {
                if (IORead(echoIO) == true) {
                    detected = true;
                    break;
                }
            }
        }
    }

    if (detected) {
        /* Hardware detected - configure the driver*/
#ifdef USE_EXTI
        EXTIHandlerInit(&hcsr04_extiCallbackRec, hcsr04_extiHandler);
        EXTIConfig(echoIO, &hcsr04_extiCallbackRec, NVIC_PRIO_SONAR_EXTI, EXTI_Trigger_Rising_Falling); // TODO - priority!
        EXTIEnable(echoIO, true);
#endif

        dev->delayMs = 100;
        dev->maxRangeCm = HCSR04_MAX_RANGE_CM;
        dev->detectionConeDeciDegrees = HCSR04_DETECTION_CONE_DECIDEGREES;
        dev->detectionConeExtendedDeciDegrees = HCSR04_DETECTION_CONE_EXTENDED_DECIDEGREES;

        dev->init = &hcsr04_init;
        dev->update = &hcsr04_start_reading;
        dev->read = &hcsr04_get_distance;

        return true;
    }
    else {
        /* Not detected - free resources */
        IORelease(triggerIO);
        IORelease(echoIO);
        return false;
    }
}

#endif
