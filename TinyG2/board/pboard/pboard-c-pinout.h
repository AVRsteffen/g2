/*
 * pboard-c-pinout.h - tinyg2 board pinout specification
 * This file is part of the TinyG project
 *
 * Copyright (c) 2013 - 2016 Robert Giseburt
 * Copyright (c) 2013 - 2016 Alden S. Hart Jr.
 *
 * This file is part of the Motate Library.
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software. If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef pboard_c_pinout_h
#define pboard_c_pinout_h

/*
 * USAGE NOTES
 *
 * Read this first:
 * https://github.com/synthetos/g2/wiki/Adding-a-new-G2-board-(or-revision)-to-G2#making-a-new-pin-assignment
 *
 *  USAGE:
 *
 *  This file is lays out all the pin capabilities of the SAM3X8C organized by pin number.
 *  Each pin has its associated functions listed at the bottom of the file, and is essentially
 *  immutable for each processor.
 *
 *  To use, assign Motate pin numbers to the first value in the _MAKE_MOTATE_PIN() macro.
 *  ALL PINS MUST BE ASSIGNED A NUMBER, even if they are not used. There will NOT be a
 *  code-size or speed penalty for unused pins, but the WILL be a compiler-failure for
 *  unassigned pins. This new restriction allows for simplification of linkages deep in
 *  Motate.
 */
/*  See motate_pin_assignments.h for pin names to be used int he rest of the G2 code.
 *  EXAMPLES:
 *
 *  *** Vanilla pin example ***
 *
 *      _MAKE_MOTATE_PIN(4, A, 'A', 27);	// SPI0_SCKPinNumber
 *
 *      This assigns Motate pin 4 to Port A, pin 27 (A27)
 *      Look in motate_pin_assignments.h to see that this is kSPI_SCKPinNumber
 *
 *  ** Other pin functions ***
 *
 *      Please look in <Motate>/platform/atmel_sam/motate_chip_pin_functions.h
 */


#include <MotateTimers.h>

// We don't have all of the inputs, so we have to indicate as much:
#define INPUT1_AVAILABLE 1
#define INPUT2_AVAILABLE 0
#define INPUT3_AVAILABLE 0
#define INPUT4_AVAILABLE 1
#define INPUT5_AVAILABLE 1
#define INPUT6_AVAILABLE 0
#define INPUT7_AVAILABLE 0
#define INPUT8_AVAILABLE 1
#define INPUT9_AVAILABLE 0
#define INPUT10_AVAILABLE 0
#define INPUT11_AVAILABLE 0
#define INPUT12_AVAILABLE 0
#define INPUT13_AVAILABLE 0

#define ADC0_AVAILABLE 1
#define ADC1_AVAILABLE 1
#define ADC2_AVAILABLE 1
#define ADC3_AVAILABLE 1

#define XIO_HAS_USB 1
#define XIO_HAS_UART 1
#define XIO_HAS_SPI 0
#define XIO_HAS_I2C 0

#define TEMPERATURE_OUTPUT_ON 1

// Some pins, if the PWM capability is turned on, it will cause timer conflicts.
// So we have to explicitly enable them as PWM pins.
// Generated with:
// perl -e 'for($i=1;$i<14;$i++) { print "#define OUTPUT${i}_PWM 0\n";}'
#define OUTPUT1_PWM 1
#define OUTPUT2_PWM 1
#define OUTPUT3_PWM 1
#define OUTPUT4_PWM 1
#define OUTPUT5_PWM 1
#define OUTPUT6_PWM 0 // Can't PWM anyway
#define OUTPUT7_PWM 0 // Conflicts with kSocket3_VrefPinNumber
#define OUTPUT8_PWM 0 // Can't PWM anyway
#define OUTPUT9_PWM 0 // Unused
#define OUTPUT10_PWM 0 // Can't PWM anyway
#define OUTPUT11_PWM 0 // Can't PWM anyway
#define OUTPUT12_PWM 0 // Conflicts with kSocket2_VrefPinNumber
#define OUTPUT13_PWM 0 // Unused

namespace Motate {

    // Arduino pin name & function
    _MAKE_MOTATE_PIN(kUnassigned1                       , A, 'A',  0);
    _MAKE_MOTATE_PIN(kUnassigned2                       , A, 'A',  1);
    _MAKE_MOTATE_PIN(kSocket4_VrefPinNumber             , A, 'A',  2);
    _MAKE_MOTATE_PIN(kUnassigned3                       , A, 'A',  3);
    _MAKE_MOTATE_PIN(kADC0_PinNumber                    , A, 'A',  4); // HeatBed ADC
    _MAKE_MOTATE_PIN(kOutput1_PinNumber                 , A, 'A',  5);
    _MAKE_MOTATE_PIN(kOutput4_PinNumber                 , A, 'A',  6);
    _MAKE_MOTATE_PIN(kOutputSAFE_PinNumber              , A, 'A',  7);
    _MAKE_MOTATE_PIN(kSerial_RX                         , A, 'A',  8);
    _MAKE_MOTATE_PIN(kSerial_TX                         , A, 'A',  9);
    _MAKE_MOTATE_PIN(kInput5_PinNumber                  , A, 'A', 10);
    _MAKE_MOTATE_PIN(kInput4_PinNumber                  , A, 'A', 11);
    _MAKE_MOTATE_PIN(kUnassigned4                       , A, 'A', 12); // USART_RX -- unused
    _MAKE_MOTATE_PIN(kUnassigned5                       , A, 'A', 13); // USART_TX -- unused
    _MAKE_MOTATE_PIN(kSerial_RTS                        , A, 'A', 14);
    _MAKE_MOTATE_PIN(kSerial_CTS                        , A, 'A', 15);
    _MAKE_MOTATE_PIN(kSocket1_DirPinNumber              , A, 'A', 16);
    _MAKE_MOTATE_PIN(kUnassigned6                       , A, 'A', 17);
    _MAKE_MOTATE_PIN(kOutputInterrupt_PinNumber         , A, 'A', 18);
    _MAKE_MOTATE_PIN(kSocket1_Microstep_2PinNumber      , A, 'A', 19);
    _MAKE_MOTATE_PIN(kSocket1_Microstep_0PinNumber      , A, 'A', 20);
    _MAKE_MOTATE_PIN(kSocket2_StepPinNumber             , A, 'A', 21);
    _MAKE_MOTATE_PIN(kADC2_PinNumber                    , A, 'A', 22); // FS ADC
    _MAKE_MOTATE_PIN(kADC1_PinNumber                    , A, 'A', 23); // Extruder ADC
    _MAKE_MOTATE_PIN(kUnassigned7                       , A, 'A', 24);
    _MAKE_MOTATE_PIN(kSocket2_EnablePinNumber           , A, 'A', 25);
    _MAKE_MOTATE_PIN(kSocket2_DirPinNumber              , A, 'A', 26);
    _MAKE_MOTATE_PIN(kSocket3_Microstep_2PinNumber      , A, 'A', 27);
    _MAKE_MOTATE_PIN(kSocket3_Microstep_0PinNumber      , A, 'A', 28);
    _MAKE_MOTATE_PIN(kSocket3_StepPinNumber             , A, 'A', 29);

    _MAKE_MOTATE_PIN(kSocket3_EnablePinNumber           , B, 'B',  0);
    _MAKE_MOTATE_PIN(kSocket3_DirPinNumber              , B, 'B',  1);
    _MAKE_MOTATE_PIN(kUnassigned8                       , B, 'B',  2);
    _MAKE_MOTATE_PIN(kLEDPWM_PinNumber                  , B, 'B',  3);
    _MAKE_MOTATE_PIN(kUnassigned9                       , B, 'B',  4);
    _MAKE_MOTATE_PIN(kUnassigned10                      , B, 'B',  5);
    _MAKE_MOTATE_PIN(kUnassigned11                      , B, 'B',  6);
    _MAKE_MOTATE_PIN(kUnassigned12                      , B, 'B',  7);
    _MAKE_MOTATE_PIN(kUnassigned13                      , B, 'B',  8);
    _MAKE_MOTATE_PIN(kUnassigned14                      , B, 'B',  9);
    _MAKE_MOTATE_PIN(kSocket4_Microstep_2PinNumber      , B, 'B', 10);
    _MAKE_MOTATE_PIN(kSocket4_Microstep_0PinNumber      , B, 'B', 11);
    _MAKE_MOTATE_PIN(kUnassigned15                      , B, 'B', 12); // FS I2C DAT
    _MAKE_MOTATE_PIN(kUnassigned16                      , B, 'B', 13); // FS I2C CLK
    _MAKE_MOTATE_PIN(kSocket4_StepPinNumber             , B, 'B', 14);
    _MAKE_MOTATE_PIN(kSocket1_StepPinNumber             , B, 'B', 15);
    _MAKE_MOTATE_PIN(kSocket1_EnablePinNumber           , B, 'B', 16);
    _MAKE_MOTATE_PIN(kSocket1_VrefPinNumber             , B, 'B', 17);
    _MAKE_MOTATE_PIN(kSocket2_VrefPinNumber             , B, 'B', 18);
    _MAKE_MOTATE_PIN(kSocket3_VrefPinNumber             , B, 'B', 19);
    _MAKE_MOTATE_PIN(kSocket2_Microstep_2PinNumber      , B, 'B', 20);
    _MAKE_MOTATE_PIN(kSocket2_Microstep_0PinNumber      , B, 'B', 21);
    _MAKE_MOTATE_PIN(kSocket4_EnablePinNumber           , B, 'B', 22);
    _MAKE_MOTATE_PIN(kSocket4_DirPinNumber              , B, 'B', 23);
    _MAKE_MOTATE_PIN(kOutput11_PinNumber                , B, 'B', 24);
    _MAKE_MOTATE_PIN(kUnassigned17                      , B, 'B', 25);
    _MAKE_MOTATE_PIN(kInput1_PinNumber                  , B, 'B', 26);
    _MAKE_MOTATE_PIN(kOutput3_PinNumber                 , B, 'B', 27);
    _MAKE_MOTATE_PIN(kUnassigned18                      , B, 'B', 28);
    _MAKE_MOTATE_PIN(kUnassigned19                      , B, 'B', 29);
    _MAKE_MOTATE_PIN(kUnassigned20                      , B, 'B', 30);
    _MAKE_MOTATE_PIN(kUnassigned21                      , B, 'B', 31);

} // namespace Motate

// We then allow each chip-type to have it's onw function definitions
// that will refer to these pin assignments.
#include "motate_chip_pin_functions.h"

#endif
// pboard_c_pinout_h
