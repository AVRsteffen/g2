/*
 * stepper.cpp - stepper motor controls
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2016 Alden S. Hart, Jr.
 * Copyright (c) 2013 - 2016 Robert Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
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
 */
/* 	This module provides the low-level stepper drivers and some related functions.
 *	See stepper.h for a detailed explanation of this module.
 */

#include "tinyg2.h"
#include "config.h"
#include "stepper.h"
#include "encoder.h"
#include "planner.h"
#include "hardware.h"
#include "text_parser.h"
#include "util.h"
#include "controller.h"
#include "xio.h"


#include "MotateSPI.h"
#include "MotateBuffer.h"
#include "MotateUtilities.h" // for to/fromLittle/BigEndian

/**** Allocate structures ****/

stConfig_t st_cfg;
stPrepSingleton_t st_pre;
static stRunSingleton_t st_run;

/**** Static functions ****/

static void _load_move(void);
#ifdef __ARM
static void _set_motor_power_level(const uint8_t motor, const float power_level);
#endif

// handy macro
#define _f_to_period(f) (uint16_t)((float)F_CPU / (float)f)

#define __NEW_DDA_ISR

/**** Setup motate ****/

using namespace Motate;
extern OutputPin<kDebug1_PinNumber> debug_pin1;
extern OutputPin<kDebug2_PinNumber> debug_pin2;
extern OutputPin<kDebug3_PinNumber> debug_pin3;
//extern OutputPin<kDebug4_PinNumber> debug_pin4;




// ############ SPI TESTING ###########

SPIChipSelectPinMux<kSocket1_SPISlaveSelectPinNumber, kSocket2_SPISlaveSelectPinNumber, kSocket3_SPISlaveSelectPinNumber, -1> spiCSPinMux;
SPIBus<kSPI_MISOPinNumber, kSPI_MOSIPinNumber, kSPI_SCKPinNumber> spiBus;

// Mostly complete base class for Trinamic2130s. Only missing the Chip Select.
struct Trinamic2130Base {
    // SPI and message handling properties
    SPIBusDeviceBase *_device;
    SPIMessage msg_0;
//    SPIMessage msg_1;

    // Create the type of a buffer
    struct trinamic_buffer_t {
        volatile union {
            uint8_t addr;
            uint8_t status;
        };
//        union {
            volatile uint32_t value;
//            uint8_t bytes[4];
//        };
    } __attribute__ ((packed));

    // And make two statically allocated buffers
    volatile trinamic_buffer_t out_buffer;
    volatile trinamic_buffer_t in_buffer;

    // Record if we're transmitting to prevent altering the buffers while they
    // are being transmitted still.
    volatile bool transmitting = false;

    // Record what register we just requested, so we know what register the
    // the response is for (and to read the response.)
    volatile int16_t _register_thats_reading = -1;

    // We need to have a flag for when we are doing a read *just* to get the
    // data requested. Otherwise we'll loop forever.
    bool _reading_only = false;

    // Store a circular buffer of registers we need to read/write.
    Motate::Buffer<32> _registers_to_access;


    // Create/copy/delete/move handling:
    Trinamic2130Base(SPIBusDeviceBase *device) : _device{device} {
//        _init();
    };

    // Prevent copying, but support moving semantics
    Trinamic2130Base(const Trinamic2130Base &) = delete;
    Trinamic2130Base(Trinamic2130Base && other) : _device{other._device} {
//        _init();
    };

    Motate::Timeout check_timer;

    // ############
    // Actual Trinamic2130 protol functions follow

    // Request reading a register
    void readRegister(uint8_t reg) {
        _registers_to_access.write(reg);
        _startNextRead();
    };

    // Request writing to a register
    void writeRegister(uint8_t reg) {
        _registers_to_access.write(reg | 0x80);
        _startNextRead();
    };


    // ###########
    // From here on we store actual values from the trinamic, and marshall data
    // from the in_buffer buffer to them, or from the values to the out_buffer.

    // Note that this includes _startNextRead() and _doneReadingCallback(),
    // which are what calls the functions to put data into the out_buffer and
    // read data from the in_buffer, respectively.

    // Also, _init() is last, so it can setup a newly created Trinamic object.

    enum {
        GCONF_reg      = 0x00,
        GSTAT_reg      = 0x01,
        IOIN_reg       = 0x04,
        IHOLD_IRUN_reg = 0x10,
        TPOWERDOWN_reg = 0x11,
        TSTEP_reg      = 0x12,
        TPWMTHRS_reg   = 0x13,
        TCOOLTHRS_reg  = 0x14,
        THIGH_reg      = 0x15,
        XDIRECT_reg    = 0x2D,
        VDCMIN_reg     = 0x33,
        MSCNT_reg      = 0x6A,
        CHOPCONF_reg   = 0x6C,
        COOLCONF_reg   = 0x6D,
        PWMCONF_reg    = 0x70,
    };

    // IMPORTANT NOTE: The endianness of the ARM is little endian, but other processors
    //  may be different.

    uint8_t status = 0;
    union {
        volatile uint32_t value;
//        uint8_t bytes[4];
        volatile struct {
            uint32_t i_scale_analog      : 1; // 0
            uint32_t internal_Rsense     : 1; // 1
            uint32_t en_pwm_mode         : 1; // 2
            uint32_t enc_commutation     : 1; // 3
            uint32_t shaft               : 1; // 4
            uint32_t diag0_error         : 1; // 5
            uint32_t diag0_otpw          : 1; // 6
            uint32_t diag0_stall         : 1; // 7

            uint32_t diag1_stall         : 1; // 8
            uint32_t diag1_index         : 1; // 9
            uint32_t diag1_onstate       : 1; // 10
            uint32_t diag1_steps_skipped : 1; // 11
            uint32_t diag0_int_pushpull  : 1; // 12
            uint32_t diag1_pushpull      : 1; // 13
            uint32_t small_hysteresis    : 1; // 14
        }  __attribute__ ((packed));
    } GCONF; // 0x00 - READ/WRITE
    void _postReadGConf() {
        GCONF.value = fromBigEndian(in_buffer.value);
    };
    void _prepWriteGConf() {
        out_buffer.value = toBigEndian(GCONF.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
        volatile struct {
            uint32_t reset      : 1; // 0
            uint32_t drv_err    : 1; // 1
            uint32_t uv_cp      : 1; // 2
        }  __attribute__ ((packed));
    } GSTAT; // 0x01 - CLEARS ON READ
    void _postReadGStat() {
        GSTAT.value = fromBigEndian(in_buffer.value);
    };

    union {
        volatile uint32_t value;
//        uint8_t bytes[4];
        volatile struct {
            uint32_t STEP         : 1; // 0
            uint32_t DIR          : 1; // 1
            uint32_t DCEN_CFG4    : 1; // 2
            uint32_t DCIN_CFG5    : 1; // 3
            uint32_t DRV_ENN_CFG6 : 1; // 4
            uint32_t DCO          : 1; // 5
            uint32_t _always_1    : 1; // 6
            uint32_t _dont_care   : 1; // 7

            uint32_t _unused_0    : 8; //  8-15
            uint32_t _unused_1    : 8; // 16-23
            uint32_t CHIP_VERSION : 8; // 24-31 - should always read 0x11
        }  __attribute__ ((packed));
    } IOIN; // 0x04 - READ ONLY
    void _postReadIOIN() {
        IOIN.value = fromBigEndian(in_buffer.value);
    };

    union {
        volatile uint32_t value;
//        uint8_t bytes[4];
        volatile struct {
            uint32_t IHOLD      : 5; //  0- 4
            uint32_t _unused_0  : 3; //  5- 7
            uint32_t IRUN       : 5; //  8-12
            uint32_t _unused_1  : 3; // 13-15
            uint32_t IHOLDDELAY : 4; // 16-19
        }  __attribute__ ((packed));
    } IHOLD_IRUN; // 0x10 - WRITE ONLY
    void _prepWriteIHoldIRun() {
        out_buffer.value = toBigEndian(IHOLD_IRUN.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
    } TPOWERDOWN; // 0x11 - WRITE ONLY
    void _prepWriteTPowerDown() {
        out_buffer.value = toBigEndian(TPOWERDOWN.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
    } TSTEP; // 0x12 - READ ONLY
    void _postReadTSep() {
        TSTEP.value = fromBigEndian(in_buffer.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
    } TPWMTHRS; // 0x13 - WRITE ONLY
    void _prepWriteTPWMTHRS() {
        out_buffer.value = toBigEndian(TPWMTHRS.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
    } TCOOLTHRS; // 0x14 - WRITE ONLY
    void _prepWriteTCOOLTHRS() {
        out_buffer.value = toBigEndian(TCOOLTHRS.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
    } THIGH; // 0x15 - WRITE ONLY
    void _prepWriteTHIGH() {
        out_buffer.value = toBigEndian(THIGH.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
    } XDIRECT; // 0x2D - READ/WRITE
    void _postReadXDirect() {
        XDIRECT.value = fromBigEndian(in_buffer.value);
    };
    void _prepWriteXDirect() {
        out_buffer.value = toBigEndian(XDIRECT.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
    } VDCMIN; // 0x33 - WRITE ONLY
    void _prepWriteVDCMIN() {
        out_buffer.value = toBigEndian(VDCMIN.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
    } MSCNT; // 0x6A - READ ONLY
    void _postReadMSCount() {
        MSCNT.value = fromBigEndian(in_buffer.value);
    };

    union {
        volatile uint32_t value;
//        uint8_t bytes[4];
        volatile struct {
            uint32_t TOFF         : 4; //  0- 3
            uint32_t HSTRT_TFD012 : 3; //  4- 6 - HSTRT when chm==0, TFD012 when chm==1
            uint32_t HEND_OFFSET  : 4; //  7-10 - HEND when chm==0, OFFSET when chm==1
            uint32_t TFD3         : 1; // 11
            uint32_t disfdcc      : 1; // 12 -- when chm==1
            uint32_t rndtf        : 1; // 13
            uint32_t chm          : 1; // 14
            uint32_t TBL          : 2; // 15-16
            uint32_t vsense       : 1; // 17
            uint32_t vhighfs      : 1; // 18
            uint32_t vhighchm     : 1; // 19
            uint32_t SYNC         : 4; // 20-23
            uint32_t MRES         : 4; // 24-27
            uint32_t intpol       : 1; // 28
            uint32_t dedge        : 1; // 29
            uint32_t diss2g       : 1; // 30
        }  __attribute__ ((packed));
    } CHOPCONF; // 0x6C- READ/WRITE
    void _postReadChopConf() {
        CHOPCONF.value = fromBigEndian(in_buffer.value);
    };
    void _prepWriteChopConf() {
        out_buffer.value = toBigEndian(CHOPCONF.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
    } COOLCONF; // 0x6D - READ ONLY
    void _postReadCoolConf() {
        COOLCONF.value = fromBigEndian(in_buffer.value);
    };

    struct {
        volatile uint32_t value;
//        uint8_t bytes[4];
    } PWMCONF; // 0x70 - READ ONLY
    void _prepWritePWMConf() {
        out_buffer.value = toBigEndian(PWMCONF.value);
    };

    void _startNextRead() {
        if (transmitting || (_registers_to_access.isEmpty() && (_register_thats_reading == -1))) {
            return;
        }
        transmitting = true;

        // We request the next register, or re-request that we're reading (and already requested) in order to get the response.
        int16_t next_reg;

        if (!_registers_to_access.isEmpty()) {
            next_reg = _registers_to_access.read();
            // If we requested a write, we need to setup the out_buffer
            if (next_reg & 0x80) {
                switch(next_reg & ~0x80) {
                    case GCONF_reg:      _prepWriteGConf(); break;
                    case IHOLD_IRUN_reg: _prepWriteIHoldIRun(); break;
                    case TPOWERDOWN_reg: _prepWriteTPowerDown(); break;
                    case TPWMTHRS_reg:   _prepWriteTPWMTHRS(); break;
                    case TCOOLTHRS_reg:  _prepWriteTCOOLTHRS(); break;
                    case THIGH_reg:      _prepWriteTHIGH(); break;
                    case XDIRECT_reg:    _prepWriteXDirect(); break;
                    case VDCMIN_reg:     _prepWriteVDCMIN(); break;
                    case CHOPCONF_reg:   _prepWriteChopConf(); break;
                    case PWMCONF_reg:    _prepWritePWMConf(); break;

                    default:
                        break;
                }
            }
        } else {
            next_reg = _register_thats_reading;
            _reading_only = true;
        }

        out_buffer.addr = (uint8_t) next_reg;
        _device->queueMessage(msg_0.setup((uint8_t *)&out_buffer, (uint8_t *)&in_buffer, 5, SPIMessageDeassertAfter, SPIMessageKeepTransaction));
    };

    void _doneReadingCallback() {
//        for (uint16_t i = 10; i>0; i--) { __NOP(); }
        status = in_buffer.status;
        if (_register_thats_reading != -1) {
            switch(_register_thats_reading) {
                case GCONF_reg:    _postReadGConf(); break;
                case GSTAT_reg:    _postReadGStat(); break;
                case IOIN_reg:     _postReadIOIN(); break;
                case TSTEP_reg:    _postReadTSep(); break;
                case XDIRECT_reg:  _postReadXDirect(); break;
                case MSCNT_reg:    _postReadMSCount(); break;
                case CHOPCONF_reg: _postReadChopConf(); break;
                case COOLCONF_reg: _postReadCoolConf(); break;

                default:
                    break;
            }

            _register_thats_reading = -1;
        }

        // if we just requested a read, we should record it so we know to clock
        // in the response
        if (!_reading_only && (out_buffer.addr & 0x80) == 0) {
            _register_thats_reading = out_buffer.addr;
        } else {
            // we're not waiting for a read, let another device have a transaction
            msg_0.immediate_ends_transaction = true;
        }
        _reading_only = false;

        transmitting = false;
        _startNextRead();
    };

    void init() {
        msg_0.message_done_callback = [&] { this->_doneReadingCallback(); };

        // Establish default values, and then prepare to read the registers we can to establish starting values
        //        TPWMTHRS   = {0x000001F4};   writeRegister(TPWMTHRS_reg);
        //        PWMCONF    = {0x000401C8};   writeRegister(PWMCONF_reg);
        //        XDIRECT    = {0x00000000};   writeRegister(XDIRECT_reg);
        //        TPOWERDOWN = {0x0000000A};   writeRegister(TPOWERDOWN_reg);

        IHOLD_IRUN.IHOLD = 0x10;
        IHOLD_IRUN.IRUN = 0x10;
        writeRegister(IHOLD_IRUN_reg);

        GCONF      = {0x00000000};
        GCONF.en_pwm_mode = 1;
        writeRegister(GCONF_reg);

        CHOPCONF   = {0x040100C5};
//        {
//            TOFF = 0x5,
//            HSTRT_TFD012 = 0x4,
//            HEND_OFFSET = 0x1,
//            TFD3 = 0x0,
//            disfdcc = 0x0,
//            rndtf = 0x0,
//            chm = 0x0,
//            TBL = 0x2,
//            vsense = 0x0,
//            vhighfs = 0x0,
//            vhighchm = 0x0,
//            SYNC = 0x0, 
//            MRES = 0x4, 
//            intpol = 0x0, 
//            dedge = 0x0, 
//            diss2g = 0x0
//        }
        writeRegister(CHOPCONF_reg);

        readRegister(IOIN_reg);
        readRegister(MSCNT_reg);

        check_timer.set(100);
    };

    void check() {
        if (check_timer.isPast()) {
            check_timer.set(100);
            readRegister(IOIN_reg);
            readRegister(MSCNT_reg);
        }
    };
};

template <typename device_t>
struct Trinamic2130 : Trinamic2130Base {
    device_t _raw_device;

    template <typename SPIBus_t, typename chipSelect_t>
    Trinamic2130(SPIBus_t &spi_bus, const chipSelect_t &_cs) :
        Trinamic2130Base {&_raw_device},
        _raw_device{spi_bus.getDevice(_cs,
                                      4000000, //1MHz
                                      kSPIMode2 | kSPI8Bit,
                                      0, // min_between_cs_delay_ns
                                      10, // cs_to_sck_delay_ns
                                      0  // between_word_delay_ns
                                      )}
    {};

    // Prevent copying, and prevent moving (so we know if it happens)
    Trinamic2130(const Trinamic2130 &) = delete;
    Trinamic2130(Trinamic2130 &&other) : Trinamic2130Base{_raw_device}, _raw_device{std::move(other._raw_device)} {};
};

Trinamic2130<decltype(spiBus)::SPIBusDevice> trinamics[] = {
    {spiBus, spiCSPinMux.getCS(0)},
    {spiBus, spiCSPinMux.getCS(1)},
    {spiBus, spiCSPinMux.getCS(2)},
    {spiBus, spiCSPinMux.getCS(3)},
    {spiBus, spiCSPinMux.getCS(4)},
};

// ############ SPI TESTING ###########




#ifdef __ARM

OutputPin<kGRBL_CommonEnablePinNumber> common_enable;

// Example with prefixed name::
//Motate::Timer<dda_timer_num> dda_timer(kTimerUpToMatch, FREQUENCY_DDA);// stepper pulse generation
dda_timer_type dda_timer(kTimerUpToMatch, FREQUENCY_DDA);			// stepper pulse generation
dwell_timer_type dwell_timer(kTimerUpToMatch, FREQUENCY_DWELL);	// dwell timer
load_timer_type load_timer;		// triggers load of next stepper segment
exec_timer_type exec_timer;		// triggers calculation of next+1 stepper segment
fwd_plan_timer_type fwd_plan_timer;		// triggers plannning of next block

// Motor structures
template<const uint8_t motor,
         pin_number step_num,			// Setup a stepper template to hold our pins
		 pin_number dir_num,
		 pin_number enable_num,
		 pin_number ms0_num,
		 pin_number ms1_num,
		 pin_number ms2_num,
		 pin_number vref_num>

struct Stepper {
	/* stepper pin assignments */

	OutputPin<step_num> _step;
    uint8_t _step_downcount;
	OutputPin<dir_num> _dir;
    OutputPin<enable_num> _enable {kStartHigh};
	OutputPin<ms0_num> ms0;
	OutputPin<ms1_num> ms1;
	OutputPin<ms2_num> ms2;
	PWMOutputPin<vref_num> _vref;

	/* stepper default values */

	// sets default pwm freq for all motor vrefs (commented line below also sets HiZ)
    Stepper(const uint32_t frequency = 500000) : _step_downcount{0}, _vref {kPWMOn, frequency} {
        setDirection(STEP_INITIAL_DIRECTION);
    };

	/* functions bound to stepper structures */

    constexpr bool canStep()
    {
        return !_step.isNull();
    }

	void setMicrosteps(const uint8_t microsteps)
	{
        if (!_enable.isNull()) {
            switch (microsteps) {
                case ( 1): { ms2=0; ms1=0; ms0=0; break; }
                case ( 2): { ms2=0; ms1=0; ms0=1; break; }
                case ( 4): { ms2=0; ms1=1; ms0=0; break; }
                case ( 8): { ms2=0; ms1=1; ms0=1; break; }
                case (16): { ms2=1; ms1=0; ms0=0; break; }
                case (32): { ms2=1; ms1=0; ms0=1; break; }
            }
        }
	};

	void enable()
	{
        if (!_enable.isNull()) {
            if (st_cfg.mot[motor].power_mode != MOTOR_DISABLED) {
                _enable.clear();
                st_run.mot[motor].power_state = MOTOR_POWER_TIMEOUT_START;
                common_enable.clear();  // if we have a common enable, this is the time to use it...
            }
        }
	};

    void disable()
    {
        if (!_enable.isNull()) {
            _enable.set();
            st_run.mot[motor].power_state = MOTOR_IDLE;
        }
    };

    void step_start()
    {
        _step.set();
        _step_downcount = 5;
    };

    void step_end()
    {
        if (_step_downcount) {
            _step_downcount--;
            if (!_step_downcount) {
                _step.clear();
            }
        }
    };

    void setDirection(uint8_t new_direction) {
        if (!_dir.isNull()) {
            if (new_direction == DIRECTION_CW) {
                _dir.clear();
            } else {
                _dir.set(); // set the bit for CCW motion
            }
        }
    };

    void setVref(float new_vref) {
        if (!_vref.isNull()) {
            _vref = new_vref;
        }
    };

};

Stepper<MOTOR_1,
        kSocket1_StepPinNumber,
		kSocket1_DirPinNumber,
		kSocket1_EnablePinNumber,
		kSocket1_Microstep_0PinNumber,
		kSocket1_Microstep_1PinNumber,
		kSocket1_Microstep_2PinNumber,
		kSocket1_VrefPinNumber> motor_1;

Stepper<MOTOR_2,
        kSocket2_StepPinNumber,
		kSocket2_DirPinNumber,
		kSocket2_EnablePinNumber,
		kSocket2_Microstep_0PinNumber,
		kSocket2_Microstep_1PinNumber,
		kSocket2_Microstep_2PinNumber,
		kSocket2_VrefPinNumber> motor_2;

Stepper<MOTOR_3,
        kSocket3_StepPinNumber,
		kSocket3_DirPinNumber,
		kSocket3_EnablePinNumber,
		kSocket3_Microstep_0PinNumber,
		kSocket3_Microstep_1PinNumber,
		kSocket3_Microstep_2PinNumber,
		kSocket3_VrefPinNumber> motor_3;

Stepper<MOTOR_4,
        kSocket4_StepPinNumber,
		kSocket4_DirPinNumber,
		kSocket4_EnablePinNumber,
		kSocket4_Microstep_0PinNumber,
		kSocket4_Microstep_1PinNumber,
		kSocket4_Microstep_2PinNumber,
		kSocket4_VrefPinNumber> motor_4;

Stepper<MOTOR_5,
        kSocket5_StepPinNumber,
		kSocket5_DirPinNumber,
		kSocket5_EnablePinNumber,
		kSocket5_Microstep_0PinNumber,
		kSocket5_Microstep_1PinNumber,
		kSocket5_Microstep_2PinNumber,
		kSocket5_VrefPinNumber> motor_5;

Stepper<MOTOR_6,
        kSocket6_StepPinNumber,
		kSocket6_DirPinNumber,
		kSocket6_EnablePinNumber,
		kSocket6_Microstep_0PinNumber,
		kSocket6_Microstep_1PinNumber,
		kSocket6_Microstep_2PinNumber,
		kSocket6_VrefPinNumber> motor_6;

#endif // __ARM

/************************************************************************************
 **** CODE **************************************************************************
 ************************************************************************************/
/*
 * stepper_init() - initialize stepper motor subsystem
 * stepper_reset() - reset stepper motor subsystem
 *
 *	Notes:
 *	  - This init requires sys_init() to be run beforehand
 * 	  - microsteps are setup during config_init()
 *	  - motor polarity is setup during config_init()
 *	  - high level interrupts must be enabled in main() once all inits are complete
 */
/*	NOTE: This is the bare code that the Motate timer calls replace.
 *	NB: requires: #include <component_tc.h>
 *
 *	REG_TC1_WPMR = 0x54494D00;			// enable write to registers
 *	TC_Configure(TC_BLOCK_DDA, TC_CHANNEL_DDA, TC_CMR_DDA);
 *	REG_RC_DDA = TC_RC_DDA;				// set frequency
 *	REG_IER_DDA = TC_IER_DDA;			// enable interrupts
 *	NVIC_EnableIRQ(TC_IRQn_DDA);
 *	pmc_enable_periph_clk(TC_ID_DDA);
 *	TC_Start(TC_BLOCK_DDA, TC_CHANNEL_DDA);
 */
void stepper_init()
{
	memset(&st_run, 0, sizeof(st_run));			// clear all values, pointers and status
	memset(&st_pre, 0, sizeof(st_pre));			// clear all values, pointers and status
	stepper_init_assertions();

#ifdef __AVR
	// Configure virtual ports
	PORTCFG.VPCTRLA = PORTCFG_VP0MAP_PORT_MOTOR_1_gc | PORTCFG_VP1MAP_PORT_MOTOR_2_gc;
	PORTCFG.VPCTRLB = PORTCFG_VP2MAP_PORT_MOTOR_3_gc | PORTCFG_VP3MAP_PORT_MOTOR_4_gc;

	// setup ports and data structures
	for (uint8_t i=0; i<MOTORS; i++) {
		hw.st_port[i]->DIR = MOTOR_PORT_DIR_gm;  // sets outputs for motors & GPIO1, and GPIO2 inputs
		hw.st_port[i]->OUT = MOTOR_ENABLE_BIT_bm;// zero port bits AND disable motor
	}
	// setup DDA timer
	TIMER_DDA.CTRLA = STEP_TIMER_DISABLE;		// turn timer off
	TIMER_DDA.CTRLB = STEP_TIMER_WGMODE;		// waveform mode
	TIMER_DDA.INTCTRLA = TIMER_DDA_INTLVL;		// interrupt mode

	// setup DWELL timer
	TIMER_DWELL.CTRLA = STEP_TIMER_DISABLE;		// turn timer off
	TIMER_DWELL.CTRLB = STEP_TIMER_WGMODE;		// waveform mode
	TIMER_DWELL.INTCTRLA = TIMER_DWELL_INTLVL;	// interrupt mode

	// setup software interrupt load timer
	TIMER_LOAD.CTRLA = LOAD_TIMER_DISABLE;		// turn timer off
	TIMER_LOAD.CTRLB = LOAD_TIMER_WGMODE;		// waveform mode
	TIMER_LOAD.INTCTRLA = TIMER_LOAD_INTLVL;	// interrupt mode
	TIMER_LOAD.PER = LOAD_TIMER_PERIOD;			// set period

	// setup software interrupt exec timer
	TIMER_EXEC.CTRLA = EXEC_TIMER_DISABLE;		// turn timer off
	TIMER_EXEC.CTRLB = EXEC_TIMER_WGMODE;		// waveform mode
	TIMER_EXEC.INTCTRLA = TIMER_EXEC_INTLVL;	// interrupt mode
	TIMER_EXEC.PER = EXEC_TIMER_PERIOD;			// set period
#endif // __AVR

#ifdef __ARM
	// setup DDA timer
	// Longer duty cycles stretch ON pulses but 75% is about the upper limit and about
	// optimal for 200 KHz DDA clock before the time in the OFF cycle is too short.
	// If you need more pulse width you need to drop the DDA clock rate
#ifdef __NEW_DDA_ISR
	dda_timer.setInterrupts(kInterruptOnOverflow | kInterruptPriorityHighest);
#else
	dda_timer.setInterrupts(kInterruptOnOverflow | kInterruptOnMatch | kInterruptPriorityHighest);
	dda_timer.setDutyCycle(1.0 - 0.75);		// This is a 75% duty cycle on the ON step part
#endif

	// setup DWELL timer
	dwell_timer.setInterrupts(kInterruptOnOverflow | kInterruptPriorityHighest);

	// setup software interrupt load timer
	load_timer.setInterrupts(kInterruptOnSoftwareTrigger | kInterruptPriorityMedium);

	// setup software interrupt exec timer & initial condition
	exec_timer.setInterrupts(kInterruptOnSoftwareTrigger | kInterruptPriorityLow);
	st_pre.buffer_state = PREP_BUFFER_OWNED_BY_EXEC;

    // setup software interrupt forward plan timer & initial condition
    fwd_plan_timer.setInterrupts(kInterruptOnSoftwareTrigger | kInterruptPriorityLowest);

	// setup motor power levels and apply power level to stepper drivers
	for (uint8_t motor=0; motor<MOTORS; motor++) {
		_set_motor_power_level(motor, st_cfg.mot[motor].power_level_scaled);
		st_run.mot[motor].power_level_dynamic = st_cfg.mot[motor].power_level_scaled;
	}
#endif // __ARM



    // ############ SPI TESTING ###########

    spiBus.init();
//    spiBus._TMP_setChannel(spiCSTrinamic5.csValue);

//    spiBus._TMP_setMsgDone([] {
//
//        // clear msg done
//        spiBus._TMP_setMsgDone(nullptr);
//
//        
//        spiBus._TMP_setChannel(spiCSTrinamic5.csValue);
//        spiBus._TMP_startTransfer(
//                                  out_buffer,
//                                  in_buffer,
//                                  5
//                                  );
//    });
//
//    spiBus._TMP_startTransfer(
//                              out_buffer,
//                              nullptr,
//                              5
//                              );

//    int16_t toss;
//    spiBus._TMP_write(0x6F, toss, false);
//    spiBus._TMP_write(0, toss, false);
//    spiBus._TMP_write(0, toss, false);
//    spiBus._TMP_write(0, toss, false);
//    spiBus._TMP_write(0, toss, true);
//
//    spiBus._TMP_write(0x6F, toss, false); in_buffer[0] = toss;
//    spiBus._TMP_write(0, toss, false); in_buffer[0] = toss;
//    spiBus._TMP_write(0, toss, false); in_buffer[0] = toss;
//    spiBus._TMP_write(0, toss, false); in_buffer[0] = toss;
//    spiBus._TMP_write(0, toss, true); in_buffer[0] = toss;

//    trinamic1.queueMessage(read_message_pt1.setup(out_buffer, in_buffer, 5, SPIMessageDeassertAfter, SPIMessageKeepTransaction));
//    trinamic1.queueMessage(read_message_pt2.setup(out_buffer, in_buffer, 5, SPIMessageDeassertAfter, SPIMessageEndTransaction));

    trinamics[0].init();
    trinamics[1].init();
    trinamics[2].init();
    trinamics[3].init();
    trinamics[4].init();

//    trinamics[0].readRegister(0x00);
//    trinamics[0].readRegister(0x01);
//    trinamics[0].readRegister(0x04);
//    trinamics[0].readRegister(0x12);
//    trinamics[0].readRegister(0x2D);
//    trinamics[0].readRegister(0x6C);
//    trinamics[0].readRegister(0x6D);
//    trinamics[1].readRegister(0x10);
//    trinamics[2].readRegister(0x10);
//    trinamics[3].readRegister(0x10);
//    trinamics[4].readRegister(0x10);

    // ############ SPI TESTING ###########



    stepper_reset();                            // reset steppers to known state
}

/*
 * stepper_reset() - reset stepper internals
 *
 * Used to initialize stepper and also to halt movement
 */

void stepper_reset()
{
    dda_timer.stop();                                   // stop all movement
    dwell_timer.stop();
    st_run.dda_ticks_downcount = 0;                     // signal the runtime is not busy
    st_pre.buffer_state = PREP_BUFFER_OWNED_BY_EXEC;    // set to EXEC or it won't restart

	for (uint8_t motor=0; motor<MOTORS; motor++) {
		st_pre.mot[motor].prev_direction = STEP_INITIAL_DIRECTION;
        st_pre.mot[motor].direction = STEP_INITIAL_DIRECTION;
		st_run.mot[motor].substep_accumulator = 0;      // will become max negative during per-motor setup;
		st_pre.mot[motor].corrected_steps = 0;          // diagnostic only - no action effect
	}
 	mp_set_steps_to_runtime_position();                 // reset encoder to agree with the above
}

/*
 * stepper_init_assertions() - test assertions, return error code if violation exists
 * stepper_test_assertions() - test assertions, return error code if violation exists
 */

void stepper_init_assertions()
{
	st_run.magic_end = MAGICNUM;
	st_run.magic_start = MAGICNUM;
	st_pre.magic_end = MAGICNUM;
	st_pre.magic_start = MAGICNUM;
}

stat_t stepper_test_assertions()
{
    if ((BAD_MAGIC(st_run.magic_start)) || (BAD_MAGIC(st_run.magic_end)) ||
        (BAD_MAGIC(st_pre.magic_start)) || (BAD_MAGIC(st_pre.magic_end))) {
        return(cm_panic(STAT_STEPPER_ASSERTION_FAILURE, "stepper_test_assertions()"));
    }
    return (STAT_OK);
}

/*
 * st_runtime_isbusy() - return TRUE if runtime is busy:
 *
 *	Busy conditions:
 *	- motors are running
 *	- dwell is running
 */

bool st_runtime_isbusy()
{
    return (st_run.dda_ticks_downcount);    // returns false if down count is zero
}

/*
 * st_clc() - clear counters
 */

stat_t st_clc(nvObj_t *nv)	// clear diagnostic counters, reset stepper prep
{
	stepper_reset();
	return(STAT_OK);
}

/*
 * Motor power management functions
 *
 * _deenergize_motor()		 - remove power from a motor
 * _energize_motor()		 - apply power to a motor and start motor timeout
 * _set_motor_power_level()	 - set the actual Vref to a specified power level
 *
 * st_energize_motors()		 - apply power to all motors
 * st_deenergize_motors()	 - remove power from all motors
 * st_motor_power_callback() - callback to manage motor power sequencing
 */

static void _deenergize_motor(const uint8_t motor)
{
#ifdef __AVR
	switch (motor) {
		case (MOTOR_1): { PORT_MOTOR_1_VPORT.OUT |= MOTOR_ENABLE_BIT_bm; break; }
		case (MOTOR_2): { PORT_MOTOR_2_VPORT.OUT |= MOTOR_ENABLE_BIT_bm; break; }
		case (MOTOR_3): { PORT_MOTOR_3_VPORT.OUT |= MOTOR_ENABLE_BIT_bm; break; }
		case (MOTOR_4): { PORT_MOTOR_4_VPORT.OUT |= MOTOR_ENABLE_BIT_bm; break; }
	}
	st_run.mot[motor].power_state = MOTOR_OFF;
#endif
#ifdef __ARM
	// Motors that are not defined are not compiled. Saves some ugly #ifdef code
    if (motor == MOTOR_1) motor_1.disable();	// set disables the motor
	if (motor == MOTOR_2) motor_2.disable();
    if (motor == MOTOR_3) motor_3.disable();
    if (motor == MOTOR_4) motor_4.disable();
    if (motor == MOTOR_5) motor_5.disable();
    if (motor == MOTOR_6) motor_6.disable();

    st_run.mot[motor].power_state = MOTOR_OFF;

    if (!common_enable.isNull()) {
        bool do_disable = true;
        for (uint8_t i = MOTOR_1; i < MOTORS; i++) {
            if (st_run.mot[i].power_state != MOTOR_OFF) {
                do_disable = false;
                break;
            }
        }
        if (do_disable) {
            common_enable.set(); // enables are inverted
        }
    }
#endif
}

static void _energize_motor(const uint8_t motor, float timeout_seconds)
{
	if (st_cfg.mot[motor].power_mode == MOTOR_DISABLED) {
		_deenergize_motor(motor);
		return;
	}
#ifdef __AVR
	switch(motor) {
		case (MOTOR_1): { PORT_MOTOR_1_VPORT.OUT &= ~MOTOR_ENABLE_BIT_bm; break; }
		case (MOTOR_2): { PORT_MOTOR_2_VPORT.OUT &= ~MOTOR_ENABLE_BIT_bm; break; }
		case (MOTOR_3): { PORT_MOTOR_3_VPORT.OUT &= ~MOTOR_ENABLE_BIT_bm; break; }
		case (MOTOR_4): { PORT_MOTOR_4_VPORT.OUT &= ~MOTOR_ENABLE_BIT_bm; break; }
	}
#endif
#ifdef __ARM
	// Motors that are not defined are not compiled. Saves some ugly #ifdef code
	if (motor == MOTOR_1) motor_1.enable();
	if (motor == MOTOR_2) motor_2.enable();
	if (motor == MOTOR_3) motor_3.enable();
	if (motor == MOTOR_4) motor_4.enable();
	if (motor == MOTOR_5) motor_5.enable();
	if (motor == MOTOR_6) motor_6.enable();

    common_enable.clear(); // enables are inverted
#endif

	st_run.mot[motor].power_systick = SysTickTimer_getValue() + (timeout_seconds * 1000);
	st_run.mot[motor].power_state = MOTOR_POWER_TIMEOUT_COUNTDOWN;
}

/*
 * _set_motor_power_level()	- applies the power level to the requested motor.
 *
 *	The power_level must be a compensated PWM value - presumably one of:
 *		st_cfg.mot[motor].power_level_scaled
 *		st_run.mot[motor].power_level_dynamic
 */
static void _set_motor_power_level(const uint8_t motor, const float power_level)
{
#ifdef __ARM
	// power_level must be scaled properly for the driver's Vref voltage requirements
	if (motor == MOTOR_1) motor_1.setVref(power_level);
	if (motor == MOTOR_2) motor_2.setVref(power_level);
	if (motor == MOTOR_3) motor_3.setVref(power_level);
	if (motor == MOTOR_4) motor_4.setVref(power_level);
	if (motor == MOTOR_5) motor_5.setVref(power_level);
	if (motor == MOTOR_6) motor_6.setVref(power_level);
#endif
}

void st_energize_motors(float timeout_seconds)
{
	for (uint8_t motor = MOTOR_1; motor < MOTORS; motor++) {
		_energize_motor(motor, timeout_seconds);
	}
#ifdef __ARM
	common_enable.clear();			// enable gShield common enable
#endif
}

void st_deenergize_motors()
{
	for (uint8_t motor = MOTOR_1; motor < MOTORS; motor++) {
		_deenergize_motor(motor);
	}
#ifdef __ARM
	common_enable.set();			// disable gShield common enable
#endif
}

/*
 * st_motor_power_callback() - callback to manage motor power sequencing
 *
 *	Handles motor power-down timing, low-power idle, and adaptive motor power
 */
stat_t st_motor_power_callback() 	// called by controller
{
    if (!mp_is_phat_city_time()) {   // don't process this if you are time constrained in the planner
        return (STAT_NOOP);
    }

    bool have_actually_stopped = false;
    if ((!st_runtime_isbusy()) && (st_pre.buffer_state != PREP_BUFFER_OWNED_BY_LOADER)) {	// if there are no moves to load...
        have_actually_stopped = true;
    }

	// manage power for each motor individually
	for (uint8_t motor = MOTOR_1; motor < MOTORS; motor++) {

        if (have_actually_stopped && st_run.mot[motor].power_state == MOTOR_RUNNING)
            st_run.mot[motor].power_state = MOTOR_POWER_TIMEOUT_START;	// ...start motor power timeouts

		// start timeouts initiated during a load so the loader does not need to burn these cycles
		if (st_run.mot[motor].power_state == MOTOR_POWER_TIMEOUT_START && st_cfg.mot[motor].power_mode != MOTOR_ALWAYS_POWERED) {
			st_run.mot[motor].power_state = MOTOR_POWER_TIMEOUT_COUNTDOWN;
			if (st_cfg.mot[motor].power_mode == MOTOR_POWERED_IN_CYCLE) {
				st_run.mot[motor].power_systick = SysTickTimer_getValue() + (uint32_t)(st_cfg.motor_power_timeout * 1000);
			} else if (st_cfg.mot[motor].power_mode == MOTOR_POWERED_ONLY_WHEN_MOVING) {
				st_run.mot[motor].power_systick = SysTickTimer_getValue() + (uint32_t)(MOTOR_TIMEOUT_SECONDS * 1000);
			}
		}

		// count down and time out the motor
		if (st_run.mot[motor].power_state == MOTOR_POWER_TIMEOUT_COUNTDOWN) {
			if (SysTickTimer_getValue() > st_run.mot[motor].power_systick ) {
				st_run.mot[motor].power_state = MOTOR_IDLE;
				_deenergize_motor(motor);
			}
		}
	}

    for (uint8_t motor = MOTOR_1; motor < MOTORS; motor++) {
        if (motor < 5) {
            trinamics[motor].check();
        }
    }
	return (STAT_OK);
}

/******************************
 * Interrupt Service Routines *
 ******************************/

/***** Stepper Interrupt Service Routine ************************************************
 * ISR - DDA timer interrupt routine - service ticks from DDA timer
 */

#ifdef __AVR
/*
 *	Uses direct struct addresses and literal values for hardware devices - it's faster than
 *	using indexed timer and port accesses. I checked. Even when -0s or -03 is used.
 */
ISR(TIMER_DDA_ISR_vect)
{
	if ((st_run.mot[MOTOR_1].substep_accumulator += st_run.mot[MOTOR_1].substep_increment) > 0) {
		PORT_MOTOR_1_VPORT.OUT |= STEP_BIT_bm;		// turn step bit on
		st_run.mot[MOTOR_1].substep_accumulator -= st_run.dda_ticks_X_substeps;
		INCREMENT_ENCODER(MOTOR_1);
	}
	if ((st_run.mot[MOTOR_2].substep_accumulator += st_run.mot[MOTOR_2].substep_increment) > 0) {
		PORT_MOTOR_2_VPORT.OUT |= STEP_BIT_bm;
		st_run.mot[MOTOR_2].substep_accumulator -= st_run.dda_ticks_X_substeps;
		INCREMENT_ENCODER(MOTOR_2);
	}
	if ((st_run.mot[MOTOR_3].substep_accumulator += st_run.mot[MOTOR_3].substep_increment) > 0) {
		PORT_MOTOR_3_VPORT.OUT |= STEP_BIT_bm;
		st_run.mot[MOTOR_3].substep_accumulator -= st_run.dda_ticks_X_substeps;
		INCREMENT_ENCODER(MOTOR_3);
	}
	if ((st_run.mot[MOTOR_4].substep_accumulator += st_run.mot[MOTOR_4].substep_increment) > 0) {
		PORT_MOTOR_4_VPORT.OUT |= STEP_BIT_bm;
		st_run.mot[MOTOR_4].substep_accumulator -= st_run.dda_ticks_X_substeps;
		INCREMENT_ENCODER(MOTOR_4);
	}

	// pulse stretching for using external drivers.- turn step bits off
	PORT_MOTOR_1_VPORT.OUT &= ~STEP_BIT_bm;				// ~ 5 uSec pulse width
	PORT_MOTOR_2_VPORT.OUT &= ~STEP_BIT_bm;				// ~ 4 uSec
	PORT_MOTOR_3_VPORT.OUT &= ~STEP_BIT_bm;				// ~ 3 uSec
	PORT_MOTOR_4_VPORT.OUT &= ~STEP_BIT_bm;				// ~ 2 uSec

	if (--st_run.dda_ticks_downcount != 0) return;

	TIMER_DDA.CTRLA = STEP_TIMER_DISABLE;				// disable DDA timer
	_load_move();										// load the next move
}
#endif // __AVR

#ifdef __ARM
/*
 *	The DDA timer interrupt does this:
 *    - fire on overflow
 *    - clear interrupt condition
 *    - clear all step pins - this clears those the were set during the previous interrupt
 *    - if downcount == 0  and stop the timer and exit
 *    - run the DDA for each channel
 *    - decrement the downcount - if it reaches zero load the next segment
 *
 *	Note that the motor_N.step.isNull() tests are compile-time tests, not run-time tests.
 *	If motor_N is not defined that if{} clause (i.e. that motor) drops out of the complied code.
 */
#ifdef __NEW_DDA_ISR

namespace Motate {			// Must define timer interrupts inside the Motate namespace
template<>
void dda_timer_type::interrupt()
{
	dda_timer.getInterruptCause();	    // clear interrupt condition

    // clear all steps
	if (motor_1.canStep()) { motor_1.step_end(); }
	if (motor_2.canStep()) { motor_2.step_end(); }
	if (motor_3.canStep()) { motor_3.step_end(); }
	if (motor_4.canStep()) { motor_4.step_end(); }
	if (motor_5.canStep()) { motor_5.step_end(); }
	if (motor_6.canStep()) { motor_6.step_end(); }

    // process last DDA tick after end of segment
    if (st_run.dda_ticks_downcount == 0) {
    	dda_timer.stop(); // turn it off or it will keep stepping out the last segment
        return;
	}

    // process DDAs for each motor
	if (motor_1.canStep()) {
        if  ((st_run.mot[MOTOR_1].substep_accumulator += st_run.mot[MOTOR_1].substep_increment) > 0) {
			motor_1.step_start();		// turn step bit on
			st_run.mot[MOTOR_1].substep_accumulator -= st_run.dda_ticks_X_substeps;
			INCREMENT_ENCODER(MOTOR_1);
        }
	}
	if (motor_2.canStep()) {
        if ((st_run.mot[MOTOR_2].substep_accumulator += st_run.mot[MOTOR_2].substep_increment) > 0) {
    		motor_2.step_start();
    		st_run.mot[MOTOR_2].substep_accumulator -= st_run.dda_ticks_X_substeps;
    		INCREMENT_ENCODER(MOTOR_2);
        }
	}
	if (motor_3.canStep()) {
        if ((st_run.mot[MOTOR_3].substep_accumulator += st_run.mot[MOTOR_3].substep_increment) > 0) {
    		motor_3.step_start();
    		st_run.mot[MOTOR_3].substep_accumulator -= st_run.dda_ticks_X_substeps;
    		INCREMENT_ENCODER(MOTOR_3);
        }
	}
	if (motor_4.canStep()) {
        if ((st_run.mot[MOTOR_4].substep_accumulator += st_run.mot[MOTOR_4].substep_increment) > 0) {
    		motor_4.step_start();
    		st_run.mot[MOTOR_4].substep_accumulator -= st_run.dda_ticks_X_substeps;
    		INCREMENT_ENCODER(MOTOR_4);
        }
	}
	if (motor_5.canStep()) {
        if ((st_run.mot[MOTOR_5].substep_accumulator += st_run.mot[MOTOR_5].substep_increment) > 0) {
    		motor_5.step_start();
    		st_run.mot[MOTOR_5].substep_accumulator -= st_run.dda_ticks_X_substeps;
    		INCREMENT_ENCODER(MOTOR_5);
        }
	}
	if (motor_6.canStep()) {
        if ((st_run.mot[MOTOR_6].substep_accumulator += st_run.mot[MOTOR_6].substep_increment) > 0) {
    		motor_6.step_start();
    		st_run.mot[MOTOR_6].substep_accumulator -= st_run.dda_ticks_X_substeps;
    		INCREMENT_ENCODER(MOTOR_6);
        }
	}

    // process end of segment
    if (--st_run.dda_ticks_downcount == 0) {
	    _load_move();							// load the next move at the current interrupt level
    }
} // MOTATE_TIMER_INTERRUPT
} // namespace Motate

#else // __NEW_DDA_ISR

/*
 *	This interrupt is really 2 interrupts. It fires on timer overflow and also on match.
 *	Match interrupts are used to set step pins, and overflow interrupts clear step pins.
 *  When the timer starts (at 0), it does *not* fire an interrupt, but it will on match,
 *  and then again on overflow.
 *	This way the length of the stepper pulse can be controlled by setting the match value.
 *  Note that this makes the pulse timing the inverted duty cycle.
 *
 *	Note that the motor_N.step.isNull() tests are compile-time tests, not run-time tests.
 *	If motor_N is not defined that if{} clause (i.e. that motor) drops out of the complied code.
 */
namespace Motate {			// Must define timer interrupts inside the Motate namespace
template<>
void dda_timer_type::interrupt()
{
    uint32_t interrupt_cause = dda_timer.getInterruptCause();	// also clears interrupt condition

    //  debug_pin2=1;           // example of use of debug pin for profiling with a logic analyser or scope

    if (interrupt_cause == kInterruptOnMatch) {

        if (!motor_1.step.isNull() && (st_run.mot[MOTOR_1].substep_accumulator += st_run.mot[MOTOR_1].substep_increment) > 0) {
            motor_1.step.set();		// turn step bit on
            st_run.mot[MOTOR_1].substep_accumulator -= st_run.dda_ticks_X_substeps;
            INCREMENT_ENCODER(MOTOR_1);
        }
        if (!motor_2.step.isNull() && (st_run.mot[MOTOR_2].substep_accumulator += st_run.mot[MOTOR_2].substep_increment) > 0) {
            motor_2.step.set();
            st_run.mot[MOTOR_2].substep_accumulator -= st_run.dda_ticks_X_substeps;
            INCREMENT_ENCODER(MOTOR_2);
        }
        if (!motor_3.step.isNull() && (st_run.mot[MOTOR_3].substep_accumulator += st_run.mot[MOTOR_3].substep_increment) > 0) {
            motor_3.step.set();
            st_run.mot[MOTOR_3].substep_accumulator -= st_run.dda_ticks_X_substeps;
            INCREMENT_ENCODER(MOTOR_3);
        }
        if (!motor_4.step.isNull() && (st_run.mot[MOTOR_4].substep_accumulator += st_run.mot[MOTOR_4].substep_increment) > 0) {
            motor_4.step.set();
            st_run.mot[MOTOR_4].substep_accumulator -= st_run.dda_ticks_X_substeps;
            INCREMENT_ENCODER(MOTOR_4);
        }
        if (!motor_5.step.isNull() && (st_run.mot[MOTOR_5].substep_accumulator += st_run.mot[MOTOR_5].substep_increment) > 0) {
            motor_5.step.set();
            st_run.mot[MOTOR_5].substep_accumulator -= st_run.dda_ticks_X_substeps;
            INCREMENT_ENCODER(MOTOR_5);
        }
        if (!motor_6.step.isNull() && (st_run.mot[MOTOR_6].substep_accumulator += st_run.mot[MOTOR_6].substep_increment) > 0) {
            motor_6.step.set();
            st_run.mot[MOTOR_6].substep_accumulator -= st_run.dda_ticks_X_substeps;
            INCREMENT_ENCODER(MOTOR_6);
        }

    } else if (interrupt_cause == kInterruptOnOverflow) {
        motor_1.step.clear();							// turn step bits off
        motor_2.step.clear();
        motor_3.step.clear();
        motor_4.step.clear();
        motor_5.step.clear();
        motor_6.step.clear();

        if (--st_run.dda_ticks_downcount != 0) return;

        // process end of segment
        dda_timer.stop();								// turn it off or it will keep stepping out the last segment
        _load_move();									// load the next move at the current interrupt level
    }
    //    debug_pin2=0;
} // MOTATE_TIMER_INTERRUPT
} // namespace Motate

#endif // __NEW_DDA_ISR


#endif // __ARM

/***** Dwell Interrupt Service Routine **************************************************
 * ISR - DDA timer interrupt routine - service ticks from DDA timer
 */

#ifdef __AVR
ISR(TIMER_DWELL_ISR_vect) {								// DWELL timer interrupt
	if (--st_run.dda_ticks_downcount == 0) {
		TIMER_DWELL.CTRLA = STEP_TIMER_DISABLE;			// disable DWELL timer
		_load_move();
	}
}
#endif
#ifdef __ARM
namespace Motate {			// Must define timer interrupts inside the Motate namespace
    template<>
    void dwell_timer_type::interrupt()
{
	dwell_timer.getInterruptCause(); // read SR to clear interrupt condition
	if (--st_run.dda_ticks_downcount == 0) {
		dwell_timer.stop();
		_load_move();
	}
}
} // namespace Motate
#endif

/****************************************************************************************
 * Exec sequencing code		- computes and prepares next load segment
 * st_request_exec_move()	- SW interrupt to request to execute a move
 * exec_timer interrupt		- interrupt handler for calling exec function
 */

#ifdef __AVR
void st_request_exec_move()
{
	if (st_pre.buffer_state == PREP_BUFFER_OWNED_BY_EXEC) {// bother interrupting
		TIMER_EXEC.PER = EXEC_TIMER_PERIOD;
		TIMER_EXEC.CTRLA = EXEC_TIMER_ENABLE;           // trigger a LO interrupt
	}
}

ISR(TIMER_EXEC_ISR_vect) {								// exec move SW interrupt
	TIMER_EXEC.CTRLA = EXEC_TIMER_DISABLE;				// disable SW interrupt timer

	// exec_move
	if (st_pre.buffer_state == PREP_BUFFER_OWNED_BY_EXEC) {
		if (mp_exec_move() != STAT_NOOP) {
			st_pre.buffer_state = PREP_BUFFER_OWNED_BY_LOADER; // flip it back
			st_request_load_move();
		}
	}
}
#endif // __AVR

#ifdef __ARM
void st_request_exec_move()
{
	if (st_pre.buffer_state == PREP_BUFFER_OWNED_BY_EXEC) {// bother interrupting
		exec_timer.setInterruptPending();
	}
}

namespace Motate {	// Define timer inside Motate namespace
    template<>
    void exec_timer_type::interrupt()
	{
		exec_timer.getInterruptCause();					// clears the interrupt condition
		if (st_pre.buffer_state == PREP_BUFFER_OWNED_BY_EXEC) {
			if (mp_exec_move() != STAT_NOOP) {
				st_pre.buffer_state = PREP_BUFFER_OWNED_BY_LOADER; // flip it back
				st_request_load_move();
			}
		}
	}
} // namespace Motate

void st_request_plan_move()
{
    fwd_plan_timer.setInterruptPending();
}

namespace Motate {	// Define timer inside Motate namespace
    template<>
    void fwd_plan_timer_type::interrupt()
    {
        fwd_plan_timer.getInterruptCause();	   // clears the interrupt condition
        if (mp_plan_move() != STAT_NOOP) { // We now have a move to exec.
            st_request_exec_move();
        }
    }
} // namespace Motate

#endif // __ARM

/****************************************************************************************
 * Loader sequencing code
 * st_request_load_move() - fires a software interrupt (timer) to request to load a move
 * load_move interrupt	  - interrupt handler for running the loader
 *
 *	_load_move() can only be called be called from an ISR at the same or higher level as
 *	the DDA or dwell ISR. A software interrupt has been provided to allow a non-ISR to
 *	request a load (see st_request_load_move())
 */

#ifdef __AVR
void st_request_load_move()
{
	if (st_runtime_isbusy()) {
		return;													// don't request a load if the runtime is busy
	}
	if (st_pre.buffer_state == PREP_BUFFER_OWNED_BY_LOADER) {	// bother interrupting
		TIMER_LOAD.PER = LOAD_TIMER_PERIOD;
		TIMER_LOAD.CTRLA = LOAD_TIMER_ENABLE;					// trigger a HI interrupt
	}
}

ISR(TIMER_LOAD_ISR_vect) {										// load steppers SW interrupt
	TIMER_LOAD.CTRLA = LOAD_TIMER_DISABLE;						// disable SW interrupt timer
	_load_move();
}
#endif // __AVR

#ifdef __ARM
void st_request_load_move()
{
	if (st_runtime_isbusy()) {                                  // don't request a load if the runtime is busy
		return;
	}
	if (st_pre.buffer_state == PREP_BUFFER_OWNED_BY_LOADER) {	// bother interrupting
		load_timer.setInterruptPending();
	}
}

namespace Motate {	// Define timer inside Motate namespace
    template<>
    void load_timer_type::interrupt()
	{
		load_timer.getInterruptCause();							// read SR to clear interrupt condition
		_load_move();
	}
} // namespace Motate
#endif // __ARM

/****************************************************************************************
 * _load_move() - Dequeue move and load into stepper runtime structure
 *
 *  This routine can only be called be called from an ISR at the same or
 *  higher level as the DDA or dwell ISR. A software interrupt has been
 *  provided to allow a non-ISR to request a load (st_request_load_move())
 *
 *  In aline() code:
 *   - All axes must set steps and compensate for out-of-range pulse phasing.
 *   - If axis has 0 steps the direction setting can be omitted
 *   - If axis has 0 steps the motor must not be enabled to support power mode = 1
 */
/****** WARNING - THIS CODE IS SPECIFIC TO ARM. SEE TINYG FOR AVR CODE ******/

#ifdef __ARM
static void _load_move()
{
	// Be aware that dda_ticks_downcount must equal zero for the loader to run.
	// So the initial load must also have this set to zero as part of initialization
	if (st_runtime_isbusy()) {
		return;													// exit if the runtime is busy
	}
	if (st_pre.buffer_state != PREP_BUFFER_OWNED_BY_LOADER) {	// if there are no moves to load...
		for (uint8_t motor = MOTOR_1; motor < MOTORS; motor++) {
			st_run.mot[motor].power_state = MOTOR_POWER_TIMEOUT_START;	// ...start motor power timeouts
		}
		return;
	}

	// handle aline loads first (most common case)  NB: there are no more lines, only alines
	if (st_pre.block_type == BLOCK_TYPE_ALINE) {

		//**** setup the new segment ****

		st_run.dda_ticks_downcount = st_pre.dda_ticks;
		st_run.dda_ticks_X_substeps = st_pre.dda_ticks_X_substeps;

		//**** MOTOR_1 LOAD ****

		// These sections are somewhat optimized for execution speed. The whole load operation
		// is supposed to take < 10 uSec (Xmega). Be careful if you mess with this.

		// the following if() statement sets the runtime substep increment value or zeroes it
		if ((st_run.mot[MOTOR_1].substep_increment = st_pre.mot[MOTOR_1].substep_increment) != 0) {

			// NB: If motor has 0 steps the following is all skipped. This ensures that state comparisons
			//	   always operate on the last segment actually run by this motor, regardless of how many
			//	   segments it may have been inactive in between.

			// Apply accumulator correction if the time base has changed since previous segment
			if (st_pre.mot[MOTOR_1].accumulator_correction_flag == true) {
				st_pre.mot[MOTOR_1].accumulator_correction_flag = false;
				st_run.mot[MOTOR_1].substep_accumulator *= st_pre.mot[MOTOR_1].accumulator_correction;
			}

			// Detect direction change and if so:
			//	- Set the direction bit in hardware.
			//	- Compensate for direction change by flipping substep accumulator value about its midpoint.

			if (st_pre.mot[MOTOR_1].direction != st_pre.mot[MOTOR_1].prev_direction) {
				st_pre.mot[MOTOR_1].prev_direction = st_pre.mot[MOTOR_1].direction;
				st_run.mot[MOTOR_1].substep_accumulator = -(st_run.dda_ticks_X_substeps + st_run.mot[MOTOR_1].substep_accumulator);
                motor_1.setDirection(st_pre.mot[MOTOR_1].direction);
			}

			// Enable the stepper and start motor power management
			motor_1.enable();								// enable the motor (clear the ~Enable line)
			st_run.mot[MOTOR_1].power_state = MOTOR_RUNNING;
			SET_ENCODER_STEP_SIGN(MOTOR_1, st_pre.mot[MOTOR_1].step_sign);

		} else {  // Motor has 0 steps; might need to energize motor for power mode processing
			if (st_cfg.mot[MOTOR_1].power_mode == MOTOR_POWERED_ONLY_WHEN_MOVING) {
				motor_1.enable();									// energize motor
				st_run.mot[MOTOR_1].power_state = MOTOR_POWER_TIMEOUT_START;
			}
		}
		// accumulate counted steps to the step position and zero out counted steps for the segment currently being loaded
		ACCUMULATE_ENCODER(MOTOR_1);

#if (MOTORS >= 2)
		if ((st_run.mot[MOTOR_2].substep_increment = st_pre.mot[MOTOR_2].substep_increment) != 0) {
			if (st_pre.mot[MOTOR_2].accumulator_correction_flag == true) {
				st_pre.mot[MOTOR_2].accumulator_correction_flag = false;
				st_run.mot[MOTOR_2].substep_accumulator *= st_pre.mot[MOTOR_2].accumulator_correction;
			}
			if (st_pre.mot[MOTOR_2].direction != st_pre.mot[MOTOR_2].prev_direction) {
				st_pre.mot[MOTOR_2].prev_direction = st_pre.mot[MOTOR_2].direction;
				st_run.mot[MOTOR_2].substep_accumulator = -(st_run.dda_ticks_X_substeps + st_run.mot[MOTOR_2].substep_accumulator);
                motor_2.setDirection(st_pre.mot[MOTOR_2].direction);
			}
			motor_2.enable(); st_run.mot[MOTOR_2].power_state = MOTOR_RUNNING;
			SET_ENCODER_STEP_SIGN(MOTOR_2, st_pre.mot[MOTOR_2].step_sign);
		} else if (st_cfg.mot[MOTOR_2].power_mode == MOTOR_POWERED_ONLY_WHEN_MOVING) {
			motor_2.enable(); st_run.mot[MOTOR_2].power_state = MOTOR_POWER_TIMEOUT_START;
		}
		ACCUMULATE_ENCODER(MOTOR_2);
#endif
#if (MOTORS >= 3)
		if ((st_run.mot[MOTOR_3].substep_increment = st_pre.mot[MOTOR_3].substep_increment) != 0) {
			if (st_pre.mot[MOTOR_3].accumulator_correction_flag == true) {
				st_pre.mot[MOTOR_3].accumulator_correction_flag = false;
				st_run.mot[MOTOR_3].substep_accumulator *= st_pre.mot[MOTOR_3].accumulator_correction;
			}
			if (st_pre.mot[MOTOR_3].direction != st_pre.mot[MOTOR_3].prev_direction) {
				st_pre.mot[MOTOR_3].prev_direction = st_pre.mot[MOTOR_3].direction;
				st_run.mot[MOTOR_3].substep_accumulator = -(st_run.dda_ticks_X_substeps + st_run.mot[MOTOR_3].substep_accumulator);
                motor_3.setDirection(st_pre.mot[MOTOR_3].direction);
			}
			motor_3.enable(); st_run.mot[MOTOR_3].power_state = MOTOR_RUNNING;
			SET_ENCODER_STEP_SIGN(MOTOR_3, st_pre.mot[MOTOR_3].step_sign);
		} else if (st_cfg.mot[MOTOR_3].power_mode == MOTOR_POWERED_ONLY_WHEN_MOVING) {
			motor_3.enable(); st_run.mot[MOTOR_3].power_state = MOTOR_POWER_TIMEOUT_START;
		}
		ACCUMULATE_ENCODER(MOTOR_3);
#endif
#if (MOTORS >= 4)
		if ((st_run.mot[MOTOR_4].substep_increment = st_pre.mot[MOTOR_4].substep_increment) != 0) {
			if (st_pre.mot[MOTOR_4].accumulator_correction_flag == true) {
				st_pre.mot[MOTOR_4].accumulator_correction_flag = false;
				st_run.mot[MOTOR_4].substep_accumulator *= st_pre.mot[MOTOR_4].accumulator_correction;
			}
			if (st_pre.mot[MOTOR_4].direction != st_pre.mot[MOTOR_4].prev_direction) {
				st_pre.mot[MOTOR_4].prev_direction = st_pre.mot[MOTOR_4].direction;
				st_run.mot[MOTOR_4].substep_accumulator = -(st_run.dda_ticks_X_substeps + st_run.mot[MOTOR_4].substep_accumulator);
                motor_4.setDirection(st_pre.mot[MOTOR_4].direction);
			}
			motor_4.enable(); st_run.mot[MOTOR_4].power_state = MOTOR_RUNNING;
			SET_ENCODER_STEP_SIGN(MOTOR_4, st_pre.mot[MOTOR_4].step_sign);
		} else if (st_cfg.mot[MOTOR_4].power_mode == MOTOR_POWERED_ONLY_WHEN_MOVING) {
			motor_4.enable(); st_run.mot[MOTOR_4].power_state = MOTOR_POWER_TIMEOUT_START;
		}
		ACCUMULATE_ENCODER(MOTOR_4);
#endif
#if (MOTORS >= 5)
		if ((st_run.mot[MOTOR_5].substep_increment = st_pre.mot[MOTOR_5].substep_increment) != 0) {
			if (st_pre.mot[MOTOR_5].accumulator_correction_flag == true) {
				st_pre.mot[MOTOR_5].accumulator_correction_flag = false;
				st_run.mot[MOTOR_5].substep_accumulator *= st_pre.mot[MOTOR_5].accumulator_correction;
			}
			if (st_pre.mot[MOTOR_5].direction != st_pre.mot[MOTOR_5].prev_direction) {
				st_pre.mot[MOTOR_5].prev_direction = st_pre.mot[MOTOR_5].direction;
				st_run.mot[MOTOR_5].substep_accumulator = -(st_run.dda_ticks_X_substeps + st_run.mot[MOTOR_5].substep_accumulator);
                motor_5.setDirection(st_pre.mot[MOTOR_5].direction);
			}
			motor_5.enable(); st_run.mot[MOTOR_5].power_state = MOTOR_RUNNING;
			SET_ENCODER_STEP_SIGN(MOTOR_5, st_pre.mot[MOTOR_5].step_sign);
		} else if (st_cfg.mot[MOTOR_5].power_mode == MOTOR_POWERED_ONLY_WHEN_MOVING) {
			motor_5.enable(); st_run.mot[MOTOR_5].power_state = MOTOR_POWER_TIMEOUT_START;
		}
		ACCUMULATE_ENCODER(MOTOR_5);
#endif
#if (MOTORS >= 6)
		if ((st_run.mot[MOTOR_6].substep_increment = st_pre.mot[MOTOR_6].substep_increment) != 0) {
			if (st_pre.mot[MOTOR_6].accumulator_correction_flag == true) {
				st_pre.mot[MOTOR_6].accumulator_correction_flag = false;
				st_run.mot[MOTOR_6].substep_accumulator *= st_pre.mot[MOTOR_6].accumulator_correction;
			}
			if (st_pre.mot[MOTOR_6].direction != st_pre.mot[MOTOR_6].prev_direction) {
				st_pre.mot[MOTOR_6].prev_direction = st_pre.mot[MOTOR_6].direction;
				st_run.mot[MOTOR_6].substep_accumulator = -(st_run.dda_ticks_X_substeps + st_run.mot[MOTOR_6].substep_accumulator);
                motor_6.setDirection(st_pre.mot[MOTOR_6].direction);
			}
			motor_6.enable(); st_run.mot[MOTOR_6].power_state = MOTOR_RUNNING;
			SET_ENCODER_STEP_SIGN(MOTOR_6, st_pre.mot[MOTOR_6].step_sign);
		} else if (st_cfg.mot[MOTOR_6].power_mode == MOTOR_POWERED_ONLY_WHEN_MOVING) {
			motor_6.enable(); st_run.mot[MOTOR_6].power_state = MOTOR_POWER_TIMEOUT_START;
		}
		ACCUMULATE_ENCODER(MOTOR_6);
#endif

		//**** do this last ****

		dda_timer.start();									// start the DDA timer if not already running

	// handle dwells
	} else if (st_pre.block_type == BLOCK_TYPE_DWELL) {
		st_run.dda_ticks_downcount = st_pre.dda_ticks;
		dwell_timer.start();

	// handle synchronous commands
	} else if (st_pre.block_type == BLOCK_TYPE_COMMAND) {
		mp_runtime_command(st_pre.bf);

	} // else null - WARNING - We cannot printf from here!! Causes crashes.

	// all other cases drop to here (e.g. Null moves after Mcodes skip to here)
	st_pre.block_type = BLOCK_TYPE_NULL;
	st_pre.buffer_state = PREP_BUFFER_OWNED_BY_EXEC;	// we are done with the prep buffer - flip the flag back
	st_request_exec_move();								// exec and prep next move
}
#endif // __ARM

/***********************************************************************************
 * st_prep_line() - Prepare the next move for the loader
 *
 *	This function does the math on the next pulse segment and gets it ready for
 *	the loader. It deals with all the DDA optimizations and timer setups so that
 *	loading can be performed as rapidly as possible. It works in joint space
 *	(motors) and it works in steps, not length units. All args are provided as
 *	floats and converted to their appropriate integer types for the loader.
 *
 * Args:
 *	  - travel_steps[] are signed relative motion in steps for each motor. Steps are
 *		floats that typically have fractional values (fractional steps). The sign
 *		indicates direction. Motors that are not in the move should be 0 steps on input.
 *
 *	  - following_error[] is a vector of measured errors to the step count. Used for correction.
 *
 *	  - segment_time - how many minutes the segment should run. If timing is not
 *		100% accurate this will affect the move velocity, but not the distance traveled.
 *
 * NOTE:  Many of the expressions are sensitive to casting and execution order to avoid long-term
 *		  accuracy errors due to floating point round off. One earlier failed attempt was:
 *		    dda_ticks_X_substeps = (int32_t)((microseconds/1000000) * f_dda * dda_substeps);
 */

stat_t st_prep_line(float travel_steps[], float following_error[], float segment_time)
{
	// trap assertion failures and other conditions that would prevent queuing the line
	if (st_pre.buffer_state != PREP_BUFFER_OWNED_BY_EXEC) {     // never supposed to happen
        return (cm_panic(STAT_INTERNAL_ERROR, "st_prep_line() prep sync error"));
	} else if (isinf(segment_time)) {                           // never supposed to happen
        return (cm_panic(STAT_PREP_LINE_MOVE_TIME_IS_INFINITE, "st_prep_line()"));
	} else if (isnan(segment_time)) {                           // never supposed to happen
        return (cm_panic(STAT_PREP_LINE_MOVE_TIME_IS_NAN, "st_prep_line()"));
	} else if (segment_time < EPSILON) {
        return (STAT_MINIMUM_TIME_MOVE);
	}
	// setup segment parameters
	// - dda_ticks is the integer number of DDA clock ticks needed to play out the segment
	// - ticks_X_substeps is the maximum depth of the DDA accumulator (as a negative number)

	st_pre.dda_period = _f_to_period(FREQUENCY_DDA);                // FYI: this is a constant
	st_pre.dda_ticks = (int32_t)(segment_time * 60 * FREQUENCY_DDA);// NB: converts minutes to seconds
	st_pre.dda_ticks_X_substeps = st_pre.dda_ticks * DDA_SUBSTEPS;

	// setup motor parameters

	float correction_steps;
	for (uint8_t motor=0; motor<MOTORS; motor++) {	    // remind us that this is motors, not axes

		// Skip this motor if there are no new steps. Leave all other values intact.
        if (fp_ZERO(travel_steps[motor])) {
            st_pre.mot[motor].substep_increment = 0;    // substep increment also acts as a motor flag
            continue;
        }

		// Setup the direction, compensating for polarity.
		// Set the step_sign which is used by the stepper ISR to accumulate step position

		if (travel_steps[motor] >= 0) {					// positive direction
			st_pre.mot[motor].direction = DIRECTION_CW ^ st_cfg.mot[motor].polarity;
			st_pre.mot[motor].step_sign = 1;
		} else {
			st_pre.mot[motor].direction = DIRECTION_CCW ^ st_cfg.mot[motor].polarity;
			st_pre.mot[motor].step_sign = -1;
		}

		// Detect segment time changes and setup the accumulator correction factor and flag.
		// Putting this here computes the correct factor even if the motor was dormant for some
		// number of previous moves. Correction is computed based on the last segment time actually used.

		if (fabs(segment_time - st_pre.mot[motor].prev_segment_time) > 0.0000001) { // highly tuned FP != compare
			if (fp_NOT_ZERO(st_pre.mot[motor].prev_segment_time)) {					// special case to skip first move
				st_pre.mot[motor].accumulator_correction_flag = true;
				st_pre.mot[motor].accumulator_correction = segment_time / st_pre.mot[motor].prev_segment_time;
			}
			st_pre.mot[motor].prev_segment_time = segment_time;
		}

		// 'Nudge' correction strategy. Inject a single, scaled correction value then hold off
        // NOTE: This clause can be commented out to test for numerical accuracy and accumulating errors
		if ((--st_pre.mot[motor].correction_holdoff < 0) &&
			(fabs(following_error[motor]) > STEP_CORRECTION_THRESHOLD)) {

			st_pre.mot[motor].correction_holdoff = STEP_CORRECTION_HOLDOFF;
			correction_steps = following_error[motor] * STEP_CORRECTION_FACTOR;

			if (correction_steps > 0) {
				correction_steps = min3(correction_steps, fabs(travel_steps[motor]), STEP_CORRECTION_MAX);
			} else {
				correction_steps = max3(correction_steps, -fabs(travel_steps[motor]), -STEP_CORRECTION_MAX);
			}
			st_pre.mot[motor].corrected_steps += correction_steps;
			travel_steps[motor] -= correction_steps;
		}

		// Compute substeb increment. The accumulator must be *exactly* the incoming
		// fractional steps times the substep multiplier or positional drift will occur.
		// Rounding is performed to eliminate a negative bias in the uint32 conversion
		// that results in long-term negative drift. (fabs/round order doesn't matter)

		st_pre.mot[motor].substep_increment = round(fabs(travel_steps[motor] * DDA_SUBSTEPS));
	}
	st_pre.block_type = BLOCK_TYPE_ALINE;
	st_pre.buffer_state = PREP_BUFFER_OWNED_BY_LOADER;	// signal that prep buffer is ready
	return (STAT_OK);
}

/*
 * st_prep_null() - Keeps the loader happy. Otherwise performs no action
 */

void st_prep_null()
{
	st_pre.block_type = BLOCK_TYPE_NULL;
	st_pre.buffer_state = PREP_BUFFER_OWNED_BY_EXEC;	// signal that prep buffer is empty
}

/*
 * st_prep_command() - Stage command to execution
 */

void st_prep_command(void *bf)
{
	st_pre.block_type = BLOCK_TYPE_COMMAND;
	st_pre.bf = (mpBuf_t *)bf;
	st_pre.buffer_state = PREP_BUFFER_OWNED_BY_LOADER;	// signal that prep buffer is ready
}

/*
 * st_prep_dwell() 	 - Add a dwell to the move buffer
 */

void st_prep_dwell(float microseconds)
{
	st_pre.block_type = BLOCK_TYPE_DWELL;
	st_pre.dda_period = _f_to_period(FREQUENCY_DWELL);
	st_pre.dda_ticks = (uint32_t)((microseconds/1000000) * FREQUENCY_DWELL);
	st_pre.buffer_state = PREP_BUFFER_OWNED_BY_LOADER;	// signal that prep buffer is ready
}

/*
 * st_request_out_of_band_dwell()
 * (only usable while exec isn't running, e.g. in feedhold or stopped states...)
 * add a dwell to the loader without going through the planner buffers
 */
void st_request_out_of_band_dwell(float microseconds)
{
	st_prep_dwell(microseconds);
	st_pre.buffer_state = PREP_BUFFER_OWNED_BY_LOADER;	// signal that prep buffer is ready
	st_request_load_move();
}

/*
 * _set_hw_microsteps() - set microsteps in hardware
 */
static void _set_hw_microsteps(const uint8_t motor, const uint8_t microsteps)
{
#ifdef __ARM
	switch (motor) {
		case (MOTOR_1): { motor_1.setMicrosteps(microsteps); break; }
		case (MOTOR_2): { motor_2.setMicrosteps(microsteps); break; }
		case (MOTOR_3): { motor_3.setMicrosteps(microsteps); break; }
		case (MOTOR_4): { motor_4.setMicrosteps(microsteps); break; }
		case (MOTOR_5): { motor_5.setMicrosteps(microsteps); break; }
		case (MOTOR_6): { motor_6.setMicrosteps(microsteps); break; }
	}
#endif //__ARM
#ifdef __AVR
	if (microsteps == 8) {
		hw.st_port[motor]->OUTSET = MICROSTEP_BIT_0_bm;
		hw.st_port[motor]->OUTSET = MICROSTEP_BIT_1_bm;
	} else if (microsteps == 4) {
		hw.st_port[motor]->OUTCLR = MICROSTEP_BIT_0_bm;
		hw.st_port[motor]->OUTSET = MICROSTEP_BIT_1_bm;
	} else if (microsteps == 2) {
		hw.st_port[motor]->OUTSET = MICROSTEP_BIT_0_bm;
		hw.st_port[motor]->OUTCLR = MICROSTEP_BIT_1_bm;
	} else if (microsteps == 1) {
		hw.st_port[motor]->OUTCLR = MICROSTEP_BIT_0_bm;
		hw.st_port[motor]->OUTCLR = MICROSTEP_BIT_1_bm;
	}
#endif // __AVR
}


/***********************************************************************************
 * CONFIGURATION AND INTERFACE FUNCTIONS
 * Functions to get and set variables from the cfgArray table
 ***********************************************************************************/

/* HELPERS
 * _get_motor() - helper to return motor number as an index or -1 if na
 */

static int8_t _get_motor(const index_t index)
{
	char *ptr;
	char motors[] = {"123456"};
	char tmp[TOKEN_LEN+1];

	strcpy_P(tmp, cfgArray[index].group);
	if ((ptr = strchr(motors, tmp[0])) == NULL) {
		return (-1);
	}
	return (ptr - motors);
}

/*
 * _set_motor_steps_per_unit() - what it says
 * This function will need to be rethought if microstep morphing is implemented
 */

static void _set_motor_steps_per_unit(nvObj_t *nv)
{
	uint8_t m = _get_motor(nv->index);
	st_cfg.mot[m].units_per_step = (st_cfg.mot[m].travel_rev * st_cfg.mot[m].step_angle) / (360 * st_cfg.mot[m].microsteps);
	st_cfg.mot[m].steps_per_unit = 1/st_cfg.mot[m].units_per_step;
}

/* PER-MOTOR FUNCTIONS
 * st_set_sa() - set motor step angle
 * st_set_tr() - set travel per motor revolution
 * st_set_mi() - set motor microsteps
 * st_set_pm() - set motor power mode
 * st_set_pl() - set motor power level
 */

stat_t st_set_sa(nvObj_t *nv)			// motor step angle
{
	set_flt(nv);
	_set_motor_steps_per_unit(nv);
	return(STAT_OK);
}

stat_t st_set_tr(nvObj_t *nv)			// motor travel per revolution
{
	set_flu(nv);
	_set_motor_steps_per_unit(nv);
	return(STAT_OK);
}

stat_t st_set_mi(nvObj_t *nv)			// motor microsteps
{
	uint8_t mi = (uint8_t)nv->value;

#ifdef __ARM
	if ((mi != 1) && (mi != 2) && (mi != 4) && (mi != 8) && (mi != 16) && (mi != 32)) {
#else
	if ((mi != 1) && (mi != 2) && (mi != 4) && (mi != 8)) {
#endif
		nv_add_conditional_message((const char *)"*** WARNING *** Setting non-standard microstep value");
	}
	set_ui8(nv);						// set it anyway, even if it's unsupported
	_set_motor_steps_per_unit(nv);
	_set_hw_microsteps(_get_motor(nv->index), (uint8_t)nv->value);
	return (STAT_OK);
}

stat_t st_set_pm(nvObj_t *nv)			// motor power mode
{
	if (nv->value >= MOTOR_POWER_MODE_MAX_VALUE) return (STAT_INPUT_VALUE_UNSUPPORTED);
	set_ui8(nv);

    // We do this *here* in order for this to take effect immediately.
    _deenergize_motor(_get_motor(nv->index));
	return (STAT_OK);
}

/*
 * st_set_pl() - set motor power level
 *
 *	Input value may vary from 0.000 to 1.000 The setting is scaled to allowable PWM range.
 *	This function sets both the scaled and dynamic power levels, and applies the
 *	scaled value to the vref.
 */
stat_t st_set_pl(nvObj_t *nv)	// motor power level
{
#ifdef __ARM
	if ((nv->value < (float)0.0) || (nv->value > (float)1.0)) {
        return (STAT_INPUT_VALUE_RANGE_ERROR);
	}
	set_flt(nv);	// set power_setting value in the motor config struct (st)

	uint8_t motor = _get_motor(nv->index);
	st_cfg.mot[motor].power_level_scaled = (nv->value * POWER_LEVEL_SCALE_FACTOR);
	st_run.mot[motor].power_level_dynamic = (st_cfg.mot[motor].power_level_scaled);
	_set_motor_power_level(motor, st_cfg.mot[motor].power_level_scaled);
#endif
	return(STAT_OK);
}

/* GLOBAL FUNCTIONS (SYSTEM LEVEL)
 *
 * st_set_mt() - set motor timeout in seconds
 * st_set_md() - disable motor power
 * st_set_me() - enable motor power
 *
 * Calling me or md with NULL will enable or disable all motors
 * Setting a value of 0 will enable or disable all motors
 * Setting a value from 1 to MOTORS will enable or disable that motor only
 */

stat_t st_set_mt(nvObj_t *nv)
{
	st_cfg.motor_power_timeout = min(MOTOR_TIMEOUT_SECONDS_MAX, max(nv->value, MOTOR_TIMEOUT_SECONDS_MIN));
	return (STAT_OK);
}

stat_t st_set_md(nvObj_t *nv)	// Make sure this function is not part of initialization --> f00
{
	st_deenergize_motors();
	return (STAT_OK);
}

stat_t st_set_me(nvObj_t *nv)	// Make sure this function is not part of initialization --> f00
{
	if (((uint8_t)nv->value == 0) || (nv->valuetype == TYPE_NULL)) {
		st_energize_motors(st_cfg.motor_power_timeout);
	} else {
		st_energize_motors(nv->value);
	}
	return (STAT_OK);
}

/***********************************************************************************
 * TEXT MODE SUPPORT
 * Functions to print variables from the cfgArray table
 ***********************************************************************************/

#ifdef __TEXT_MODE

static const char msg_units0[] PROGMEM = " in";	// used by generic print functions
static const char msg_units1[] PROGMEM = " mm";
static const char msg_units2[] PROGMEM = " deg";
static const char *const msg_units[] PROGMEM = { msg_units0, msg_units1, msg_units2 };
#define DEGREE_INDEX 2

static const char fmt_me[] PROGMEM = "motors energized\n";
static const char fmt_md[] PROGMEM = "motors de-energized\n";
static const char fmt_mt[] PROGMEM = "[mt]  motor idle timeout%14.2f seconds\n";
static const char fmt_0ma[] PROGMEM = "[%s%s] m%s map to axis%15d [0=X,1=Y,2=Z...]\n";
static const char fmt_0sa[] PROGMEM = "[%s%s] m%s step angle%20.3f%s\n";
static const char fmt_0tr[] PROGMEM = "[%s%s] m%s travel per revolution%10.4f%s\n";
static const char fmt_0po[] PROGMEM = "[%s%s] m%s polarity%18d [0=normal,1=reverse]\n";
static const char fmt_0pm[] PROGMEM = "[%s%s] m%s power management%10d [0=disabled,1=always on,2=in cycle,3=when moving]\n";
static const char fmt_0pl[] PROGMEM = "[%s%s] m%s motor power level%13.3f [0.000=minimum, 1.000=maximum]\n";
#ifdef __AVR
    static const char fmt_0mi[] PROGMEM = "[%s%s] m%s microsteps%16d [1,2,4,8]\n";
#else
    static const char fmt_0mi[] PROGMEM = "[%s%s] m%s microsteps%16d [1,2,4,8,16,32]\n";
#endif

void st_print_me(nvObj_t *nv) { text_print(nv, fmt_me);}    // TYPE_NULL - message only
void st_print_md(nvObj_t *nv) { text_print(nv, fmt_md);}    // TYPE_NULL - message only
void st_print_mt(nvObj_t *nv) { text_print(nv, fmt_mt);}    // TYPE_FLOAT

static void _print_motor_int(nvObj_t *nv, const char *format)
{
	sprintf_P(cs.out_buf, format, nv->group, nv->token, nv->group, (int)nv->value);
    xio_writeline(cs.out_buf);
}

static void _print_motor_flt(nvObj_t *nv, const char *format)
{
	sprintf_P(cs.out_buf, format, nv->group, nv->token, nv->group, nv->value);
    xio_writeline(cs.out_buf);
}

static void _print_motor_flt_units(nvObj_t *nv, const char *format, uint8_t units)
{
    sprintf_P(cs.out_buf, format, nv->group, nv->token, nv->group, nv->value, GET_TEXT_ITEM(msg_units, units));
    xio_writeline(cs.out_buf);
}

void st_print_ma(nvObj_t *nv) { _print_motor_int(nv, fmt_0ma);}
void st_print_sa(nvObj_t *nv) { _print_motor_flt_units(nv, fmt_0sa, DEGREE_INDEX);}
void st_print_tr(nvObj_t *nv) { _print_motor_flt_units(nv, fmt_0tr, cm_get_units_mode(MODEL));}
void st_print_mi(nvObj_t *nv) { _print_motor_int(nv, fmt_0mi);}
void st_print_po(nvObj_t *nv) { _print_motor_int(nv, fmt_0po);}
void st_print_pm(nvObj_t *nv) { _print_motor_int(nv, fmt_0pm);}
void st_print_pl(nvObj_t *nv) { _print_motor_flt(nv, fmt_0pl);}

#endif // __TEXT_MODE
