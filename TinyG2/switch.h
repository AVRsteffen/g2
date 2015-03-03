/*
 * switch.h - switch handling functions
 * This file is part of the TinyG project
 *
 * Copyright (c) 2013 - 2015 Alden S. Hart, Jr.
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
/* Switch processing functions under Motate
 *
 *	Switch processing turns pin transitions into reliable switch states.
 *	There are 2 main operations:
 *
 *	  - read pin		get raw data from a pin
 *	  - read switch		return processed switch closures
 *
 *	Read pin may be a polled operation or an interrupt on pin change. If interrupts
 *	are used they must be provided for both leading and trailing edge transitions.
 *
 *	Read switch contains the results of read pin and manages edges and debouncing.
 */
#ifndef SWITCH_H_ONCE
#define SWITCH_H_ONCE

/*
 * new GPIO
 */

#define DI_CHANNELS	        9       // number of digital inputs supported
#define DO_CHANNELS	        4       // number of digital outputs supported
#define AI_CHANNELS	        0       // number of analog inputs supported
#define AO_CHANNELS	        0       // number of analog outputs supported

#define DI_LOCKOUT_MS       50      // milliseconds to go dead after input firing

enum gpioMode {
    GPIO_DISABLED = -1,             // gpio is disabled
    GPIO_ACTIVE_LOW = 0,            // gpio is active low (normally open)
    GPIO_ACTIVE_HIGH = 1            // gpio is active high (normally closed)
};
#define NORMALLY_OPEN GPIO_ACTIVE_LOW    // equivalent
#define NORMALLY_CLOSED GPIO_ACTIVE_HIGH // equivalent

enum diAction {                     // actions are initiated from within the input's ISR
    DI_ACTION_NONE = 0,
    DI_ACTION_STOP,                 // stop at normal jerk - preserves positional accuracy
    DI_ACTION_FAST_STOP,            // stop at high jerk - preserves positional accuracy
    DI_ACTION_HALT,                 // stop immediately - not guaranteed to preserve position
    DI_ACTION_RESET                 // reset system immediately
};

enum diFunc {                       // functions are requested from the ISR, run from the main loop
    DI_FUNCTION_NONE = 0,
    DI_FUNCTION_LIMIT,              // limit switch processing
    DI_FUNCTION_INTERLOCK,          // interlock processing
    DI_FUNCTION_SHUTDOWN,           // shutdown in support of external emergency stop
    DI_FUNCTION_SPINDLE_READY       // signal that spindle is ready (up to speed)
};

enum diState {
    DI_DISABLED = -1,               // value returned if input is disabled
    DI_INACTIVE = 0,				// aka switch open, also read as 'false'
    DI_ACTIVE = 1					// aka switch closed, also read as 'true'
};

enum diEdgeFlag {
    DI_EDGE_NONE = 0,               // no edge detected or edge flag reset
    DI_EDGE_LEADING,				// flag is set when leading edge is detected
    DI_EDGE_TRAILING				// flag is set when trailing edge is detected
};

/*
 * GPIO structures
 */
typedef struct gpioDigitalInput {   // one struct per digital input
	gpioMode mode;                  // -1=disabled, 0=active low (NO), 1= active high (NC)
	diAction action;                // 0=none, 1=stop, 2=halt, 3=stop_steps, 4=reset
	diFunc function;                // function to perform when activated / deactivated

    int8_t state;                   // input state 0=inactive, 1=active, -1=disabled
    diEdgeFlag edge;                // keeps a transient record of edges for immediate inquiry
    bool homing_mode;               // set true when input is in homing mode.

	uint16_t lockout_ms;            // number of milliseconds for debounce lockout
	uint32_t lockout_timer;         // time to expire current debounce lockout, or 0 if no lockout
} gpio_di_t;

typedef struct gpioDigitalOutput {  // one struct per digital output
    gpioMode mode;
} gpio_do_t;

typedef struct gpioAnalogInput {    // one struct per analog input
    gpioMode mode;
} gpio_ai_t;

typedef struct gpioAnalogOutput {   // one struct per analog output
    gpioMode mode;
} gpio_ao_t;

typedef struct gpioSingleton {      // collected gpio
	gpio_di_t in[DI_CHANNELS];
    gpio_do_t out[DO_CHANNELS];     // Note: 'do' is a reserved word
    gpio_ai_t an_in[AI_CHANNELS];
    gpio_ao_t an_out[AO_CHANNELS];
} gpio_t;
extern gpio_t gpio;

/*********************************************************
 * Generic variables and settings
 */

// switch array configuration / sizing
#define SW_PAIRS				HOMING_AXES	// number of axes that can have switches
#define SW_POSITIONS			2			// swPosition is either SW_MIN or SW_MAX

// switch modes
#define SW_HOMING_BIT			0x01
#define SW_LIMIT_BIT			0x02

#define SW_MODE_DISABLED 		0							 // disabled for all operations
#define SW_MODE_HOMING 			SW_HOMING_BIT				 // enable switch for homing only
#define SW_MODE_LIMIT 			SW_LIMIT_BIT				 // enable switch for limits only
#define SW_MODE_HOMING_LIMIT   (SW_HOMING_BIT | SW_LIMIT_BIT)// homing and limits
#define SW_MODE_CUSTOM          0x04
#define SW_MODE_MAX_VALUE 		SW_MODE_CUSTOM

enum swType {
	SW_TYPE_NORMALLY_OPEN = 0,
	SW_TYPE_NORMALLY_CLOSED
};

enum swState {
	SW_DISABLED = -1,
	SW_OPEN = 0,					// also read as 'false'
	SW_CLOSED = 1					// also read as 'true'
};

enum swPosition {
	SW_MIN = 0,
	SW_MAX
};

enum swEdge {
	SW_NO_EDGE = 0,
	SW_LEADING,
	SW_TRAILING,
};

#define SW_LOCKOUT_TICKS 50			// milliseconds to go dead after switch firing

/*
 * Switch control structures
 */
typedef struct swSwitch {						// one struct per switch
	// public
	uint8_t type;								// swType: 0=NO, 1=NC
	uint8_t mode;								// 0=disabled, 1=homing, 2=limit, 3=homing+limit
	int8_t state;								// 0=open, 1=closed, -1=disabled
	uint8_t limit_switch_thrown;    // if this is configured as a limit switch, 1 = limit switch has been triggered

	// private
	uint8_t edge;								// keeps a transient record of edges for immediate inquiry
	uint16_t debounce_ticks;					// number of millisecond ticks for debounce lockout
	uint32_t debounce_timeout;					// time to expire current debounce lockout, or 0 if no lockout
	void (*when_open)(struct swSwitch *s);		// callback to action function when sw is open - passes *s, returns void
	void (*when_closed)(struct swSwitch *s);	// callback to action function when closed
	void (*on_leading)(struct swSwitch *s);		// callback to action function for leading edge onset
	void (*on_trailing)(struct swSwitch *s);	// callback to action function for trailing edge
} switch_t;
typedef void (*sw_callback)(switch_t *s);		// typedef for switch action callback

typedef struct swSwitchArray {					// array of switches
	switch_t s[SW_PAIRS][SW_POSITIONS];
} switches_t;
extern switches_t sw;

/*
 * Function prototypes
 */

void gpio_print_mo(nvObj_t *nv);
void gpio_print_ac(nvObj_t *nv);
void gpio_print_fn(nvObj_t *nv);
void gpio_print_in(nvObj_t *nv);

stat_t gpio_get_in(nvObj_t *nv);


//***** old switch functions *****
void switch_init(void);
void switch_reset(void);
stat_t poll_switches(void);
int8_t poll_switch(switch_t *s, uint8_t pin_value);
uint8_t get_switch_mode(uint8_t axis, uint8_t position);
uint8_t get_switch_type(uint8_t axis, uint8_t position);
int8_t read_switch(uint8_t axis, uint8_t position);
uint8_t get_limit_switch_thrown(void);
void reset_limit_switches(void);

/*
 * Switch config accessors and text functions
 */
stat_t sw_set_st(nvObj_t *nv);
stat_t sw_set_sw(nvObj_t *nv);
stat_t sw_get_ss(nvObj_t *nv);
void sw_print_ss(nvObj_t *nv);

#ifdef __TEXT_MODE
	void sw_print_st(nvObj_t *nv);
#else
	#define sw_print_st tx_print_stub
#endif // __TEXT_MODE

#endif // End of include guard: SWITCH_H_ONCE
