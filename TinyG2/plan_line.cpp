/*
 * plan_line.c - acceleration managed line planning and motion execution
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2015 Alden S. Hart, Jr.
 * Copyright (c) 2012 - 2015 Rob Giseburt
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

#include "tinyg2.h"
#include "config.h"
#include "controller.h"
#include "canonical_machine.h"
#include "planner.h"
#include "stepper.h"
#include "report.h"
#include "util.h"
#include "spindle.h"

using namespace Motate;
OutputPin<kDebug1_PinNumber> plan_debug_pin1;
OutputPin<kDebug2_PinNumber> plan_debug_pin2;
//OutputPin<kDebug3_PinNumber> plan_debug_pin3;
OutputPin<kDebug4_PinNumber> plan_debug_pin4;

//OutputPin<-1> plan_debug_pin1;
//OutputPin<-1> plan_debug_pin2;
OutputPin<-1> plan_debug_pin3;
//OutputPin<-1> plan_debug_pin4;

// planner helper functions
static void _calculate_move_times(GCodeState_t *gms, const float axis_length[], const float axis_square[]);
static void _calculate_jerk(mpBuf_t *bf);
//static float _calculate_junction_vmax(const float a_unit[], const float b_unit[]);
static float _calculate_junction_vmax(const float vmax, const float a_unit[], const float b_unit[]);

//static void _reset_replannable_list(void);

/* Runtime-specific setters and getters
 *
 * mp_zero_segment_velocity()         - correct velocity in last segment for reporting purposes
 * mp_get_runtime_velocity()          - returns current velocity (aggregate)
 * mp_get_runtime_machine_position()  - returns current axis position in machine coordinates
 * mp_set_runtime_work_offset()       - set offsets in the MR struct
 * mp_get_runtime_work_position()     - returns current axis position in work coordinates
 *                                      that were in effect at move planning time
 */

void mp_zero_segment_velocity() { mr.segment_velocity = 0;}
float mp_get_runtime_velocity(void) { return (mr.segment_velocity);}
float mp_get_runtime_absolute_position(uint8_t axis) { return (mr.position[axis]);}
void mp_set_runtime_work_offset(float offset[]) { copy_vector(mr.gm.work_offset, offset);}
float mp_get_runtime_work_position(uint8_t axis) { return (mr.position[axis] - mr.gm.work_offset[axis]);}

/*
 * mp_get_runtime_busy() - return TRUE if motion control busy (i.e. robot is moving)
 *
 *	Use this function to sync to the queue. If you wait until it returns
 *	FALSE you know the queue is empty and the motors have stopped.
 */

bool mp_runtime_is_idle()
{
	return (!st_runtime_isbusy());
}

//bool mp_planner_is_empty()

uint8_t mp_get_runtime_busy()
{
    if ((st_runtime_isbusy() == true) || (mr.move_state == MOVE_RUN) || mb.needs_replanned)
    {
        return (true);
    }
	return (false);
}

/****************************************************************************************
 * mp_aline() - plan a line with acceleration / deceleration
 *
 *	This function uses constant jerk motion equations to plan acceleration and deceleration
 *	The jerk is the rate of change of acceleration; it's the 1st derivative of acceleration,
 *	and the 3rd derivative of position. Jerk is a measure of impact to the machine.
 *	Controlling jerk smooths transitions between moves and allows for faster feeds while
 *	controlling machine oscillations and other undesirable side-effects.
 *
 * 	Note All math is done in absolute coordinates using single precision floating point (float).
 *
 *	Note: Returning a status that is not STAT_OK means the endpoint is NOT advanced. So lines
 *	that are too short to move will accumulate and get executed once the accumulated error
 *	exceeds the minimums.
 */

stat_t mp_aline(GCodeState_t *gm_in)
{
    plan_debug_pin1 = 1;
	mpBuf_t *bf; 						// current move pointer

	// compute some reused terms
	float axis_length[AXES];
	float axis_square[AXES];
	float length_square = 0;

	for (uint8_t axis=0; axis<AXES; axis++) {
		axis_length[axis] = gm_in->target[axis] - mm.position[axis];
		axis_square[axis] = square(axis_length[axis]);
		length_square += axis_square[axis];
	}
	float length = sqrt(length_square);

	// exit if the move has zero movement. At all.
	if (fp_ZERO(length)) {
//		sr_request_status_report(SR_REQUEST_IMMEDIATE_FULL);
		sr_request_status_report(SR_REQUEST_TIMED_FULL);
        plan_debug_pin1 = 0;
		return (STAT_MINIMUM_LENGTH_MOVE);
	}

    _calculate_move_times(gm_in, axis_length, axis_square);         // set move time and minimum time in the state

    // get a cleared buffer and setup move variables
    if ((bf = mp_get_write_buffer()) == NULL) {                     // never supposed to fail
        return(cm_panic(STAT_BUFFER_FULL_FATAL, "no write buffer in aline"));
    }
    bf->bf_func = mp_exec_aline;                                    // register the callback to the exec function
    bf->length = length;
    for (uint8_t axis=0; axis<AXES; axis++) {                       // generate the unit vector
        bf->unit[axis] = axis_length[axis] / length;
        if (fabs(bf->unit[axis]) > 0) {
            bf->unit_flags[axis] = true;
        }
    }
	memcpy(&bf->gm, gm_in, sizeof(GCodeState_t));                   // copy model state into planner buffer

    _calculate_jerk(bf);                                            // get initial value for bf->jerk
	bf->cruise_vmax = bf->length / bf->gm.move_time;                // target velocity requested
	bf->delta_vmax = mp_get_target_velocity(0, bf->length, bf);
	bf->braking_velocity = bf->delta_vmax;

    if (cm_get_path_control(MODEL) == PATH_EXACT_STOP) {
        bf->entry_vmax = 0;
        bf->exit_vmax = 0;
        bf->replannable = false;                                     // ++++ Possible problem here --- for reference. This is already set to zero by the clear.
    } else {
        bf->entry_vmax = _calculate_junction_vmax(bf->cruise_vmax, bf->pv->unit, bf->unit);
        bf->exit_vmax = min(bf->cruise_vmax, (bf->entry_vmax + bf->delta_vmax));
        bf->replannable = true;
	}
    bf->real_move_time = 0;

	// Note: these next lines must remain in exact order. Position must update before committing the buffer.
//	mp_plan_block_list(bf, false);				// replan block list
	copy_vector(mm.position, bf->gm.target);	// set the planner position
	mp_commit_write_buffer(MOVE_TYPE_ALINE); 	// commit current block (must follow the position update)
    plan_debug_pin1 = 0;
    return (STAT_OK);
}

/*
 * mp_plan_block_list() - plans the entire block list
 *
 *	The block list is the circular buffer of planner buffers (bf's). The block currently
 *	being planned is the "bf" block. The "first block" is the next block to execute;
 *	queued immediately behind the currently executing block, aka the "running" block.
 *	In some cases there is no first block because the list is empty or there is only
 *	one block and it is already running.
 *
 *	If blocks following the first block are already optimally planned (non replannable)
 *	the first block that is not optimally planned becomes the effective first block.
 *
 *	_plan_block_list() plans all blocks between and including the (effective) first block
 *	and the bf. It sets entry, exit and cruise v's from vmax's then calls trapezoid generation.
 *
 *	Variables that must be provided in the mpBuffers that will be processed:
 *
 *	  bf (function arg)		- end of block list (last block in time)
 *	  bf->replannable		- start of block list set by last FALSE value [Note 1]
 *	  bf->move_type			- typically MOVE_TYPE_ALINE. Other move_types should be set to
 *							  length=0, entry_vmax=0 and exit_vmax=0 and are treated
 *							  as a momentary stop (plan to zero and from zero).
 *
 *	  bf->length			- provides block length
 *	  bf->entry_vmax		- used during forward planning to set entry velocity
 *	  bf->cruise_vmax		- used during forward planning to set cruise velocity
 *	  bf->exit_vmax			- used during forward planning to set exit velocity
 *	  bf->delta_vmax		- used during forward planning to set exit velocity
 *
 *	  bf->recip_jerk		- used during trapezoid generation
 *	  bf->cbrt_jerk			- used during trapezoid generation
 *
 *	Variables that will be set during processing:
 *
 *	  bf->replannable		- set if the block becomes optimally planned
 *
 *	  bf->braking_velocity	- set during backward planning
 *	  bf->entry_velocity	- set during forward planning
 *	  bf->cruise_velocity	- set during forward planning
 *	  bf->exit_velocity		- set during forward planning
 *
 *	  bf->head_length		- set during trapezoid generation
 *	  bf->body_length		- set during trapezoid generation
 *	  bf->tail_length		- set during trapezoid generation
 *
 *	Variables that are ignored but here's what you would expect them to be:
 *	  bf->move_state		- NEW for all blocks but the earliest
 *	  bf->target[]			- block target position
 *	  bf->unit[]			- block unit vector
 *	  bf->time				- gets set later
 *	  bf->jerk				- source of the other jerk variables. Used in mr.
 */
/* Notes:
 *	[1]	Whether or not a block is planned is controlled by the bf->replannable
 *		setting (set TRUE if it should be). Replan flags are checked during the
 *		backwards pass and prune the replan list to include only the the latest
 *		blocks that require planning
 *
 *		In normal operation the first block (currently running block) is not
 *		replanned, but may be for feedholds and feed overrides. In these cases
 *		the prep routines modify the contents of the mr buffer and re-shuffle
 *		the block list, re-enlisting the current bf buffer with new parameters.
 *		These routines also set all blocks in the list to be replannable so the
 *		list can be recomputed regardless of exact stops and previous replanning
 *		optimizations.
 */
void mp_plan_block_list(mpBuf_t *bf)
{
    plan_debug_pin2 = 1;

#ifdef DEBUG
    volatile uint32_t start_time = SysTickTimer.getValue();
#endif

    // the the exec not to change the moves out from under us
    mb.planning = true;

    mpBuf_t *bp = bf;

	// Backward planning pass. Find first block and update the braking velocities.
	// At the end *bp points to the buffer before the first block.
	while ((bp = mp_get_prev_buffer(bp)) != bf) {
		if (bp->replannable == false || bp->locked == true) {
            break;
        }
		bp->braking_velocity = min(bp->nx->entry_vmax, bp->nx->braking_velocity) + bp->delta_vmax;
	}

	// forward planning pass - recomputes trapezoids in the list from the first block to the bf block.
	while ((bp = mp_get_next_buffer(bp)) != bf) {

        // plan dwells, commands and other move types
        if (bp->move_type != MOVE_TYPE_ALINE) {
            bp->replannable = false;
            if (bp->buffer_state == MP_BUFFER_PLANNING) {
                bp->buffer_state = MP_BUFFER_QUEUED;
            } else if (bp->buffer_state == MP_BUFFER_EMPTY) {
                rpt_exception(STAT_PLANNER_ASSERTION_FAILURE, "buffer empty1 in mp_plan_block_list");
                _debug_trap();
            }
            // TODO: Add support for non-plan-to-zero commands by caching the correct pv value
            continue;
        }

        // plan lines
		if (bp->pv == bf)  {
			bp->entry_velocity = bp->entry_vmax;		// first block in the list
		} else {
			bp->entry_velocity = bp->pv->exit_velocity;	// other blocks in the list
		}
		bp->cruise_velocity = bp->cruise_vmax;
		bp->exit_velocity = min4( bp->exit_vmax, bp->nx->entry_vmax, bp->nx->braking_velocity,
								 (bp->entry_velocity + bp->delta_vmax) );

        plan_debug_pin3 = 1;
        mp_calculate_trapezoid(bp);
        plan_debug_pin3 = 0;

        if (fp_ZERO(bp->cruise_velocity)) { // ++++ Diagnostic - can be removed
            rpt_exception(STAT_PLANNER_ASSERTION_FAILURE, "zero velocity in mp_plan_block_list");
            _debug_trap();
        }

        // Force a calculation of this here
        bp->real_move_time = ((bp->head_length*2)/(bp->entry_velocity + bp->cruise_velocity)) + (bp->body_length/bp->cruise_velocity) + ((bp->tail_length*2)/(bp->exit_velocity + bp->cruise_velocity));

		// Test for optimally planned trapezoids - only need to check various exit conditions
        if  ( ((fp_EQ(bp->exit_velocity, bp->exit_vmax)) ||
               (fp_EQ(bp->exit_velocity, bp->nx->entry_vmax)) ) ||
               ((bp->pv->replannable == false) && fp_EQ(bp->exit_velocity, (bp->entry_velocity + bp->delta_vmax))) )
            {
            bp->replannable = false;
        }
        if (bp->buffer_state == MP_BUFFER_PLANNING) {
            bp->buffer_state = MP_BUFFER_QUEUED;
        } else if (bp->buffer_state == MP_BUFFER_EMPTY) {
            rpt_exception(STAT_PLANNER_ASSERTION_FAILURE, "buffer empty2 in mp_plan_block_list");
            _debug_trap();
        } else if (bp->buffer_state == MP_BUFFER_RUNNING) {
            rpt_exception(STAT_PLANNER_ASSERTION_FAILURE, "we just replanned a running buffer!");
            _debug_trap();
        }
	}

    if (bp->move_type == MOVE_TYPE_ALINE) {
        // finish up the last block move
        bp->entry_velocity = bp->pv->exit_velocity; // WARNING: bp->pv might not be initied
        bp->cruise_velocity = bp->cruise_vmax;
        bp->exit_velocity = 0;

        plan_debug_pin3 = 1;
        mp_calculate_trapezoid(bp);
        plan_debug_pin3 = 0;

        if (fp_ZERO(bp->cruise_velocity)) { // +++ diagnostic +++ remove later
            rpt_exception(STAT_PLANNER_ASSERTION_FAILURE, "min time move in mp_plan_block_list");
            _debug_trap();
        }

        // Force a calculation of this here
        bp->real_move_time = ((bp->head_length*2)/(bp->entry_velocity + bp->cruise_velocity)) + (bp->body_length/bp->cruise_velocity) + ((bp->tail_length*2)/(bp->exit_velocity + bp->cruise_velocity));

        if (bp->buffer_state == MP_BUFFER_PLANNING) {
            bp->buffer_state = MP_BUFFER_QUEUED;
        } else if (bp->buffer_state == MP_BUFFER_EMPTY) {
            rpt_exception(STAT_PLANNER_ASSERTION_FAILURE, "buffer empty3 in mp_plan_block_list");
            _debug_trap();
        }
    }

#ifdef DEBUG
    volatile uint32_t end_time = SysTickTimer.getValue();
    if ((end_time - start_time) > (MIN_PLANNED_USEC / 1000)) {
        rpt_exception(STAT_PLANNER_ASSERTION_FAILURE, "time mis-match in mp_plan_block_list");
        _debug_trap();
    }
#endif

    // let the exec know we're done planning, and that the times are likely wrong
    mb.planning = false;
    mb.needs_time_accounting = true;

    plan_debug_pin2 = 0;
}

/***** ALINE HELPERS *****
 * _calc_move_times()
 * _calculate_jerk()
 * _calculate_junction_vmax()
 * mp_reset_replannable_list()
 */

/*
 * _calculate_move_times() - compute optimal and minimum move times into the gcode_state
 *
 *	"Minimum time" is the fastest the move can be performed given the velocity constraints on each
 *	participating axis - regardless of the feed rate requested. The minimum time is the time limited
 *	by the rate-limiting axis. The minimum time is needed to compute the optimal time and is
 *	recorded for possible feed override computation..
 *
 *	"Optimal time" is either the time resulting from the requested feed rate or the minimum time if
 *	the requested feed rate is not achievable. Optimal times for traverses are always the minimum time.
 *
 *	The gcode state must have targets set prior by having cm_set_target(). Axis modes are taken into
 *	account by this.
 *
 *	The following times are compared and the longest is returned:
 *	  -	G93 inverse time (if G93 is active)
 *	  -	time for coordinated move at requested feed rate
 *	  -	time that the slowest axis would require for the move
 *
 *	Sets the following variables in the gcode_state struct
 *	  - move_time is set to optimal time
 *	  - minimum_time is set to minimum time
 */
/* --- NIST RS274NGC_v3 Guidance ---
 *
 *	The following is verbatim text from NIST RS274NGC_v3. As I interpret A for moves that
 *	combine both linear and rotational movement, the feed rate should apply to the XYZ
 *	movement, with the rotational axis (or axes) timed to start and end at the same time
 *	the linear move is performed. It is possible under this case for the rotational move
 *	to rate-limit the linear move.
 *
 * 	2.1.2.5 Feed Rate
 *
 *	The rate at which the controlled point or the axes move is nominally a steady rate
 *	which may be set by the user. In the Interpreter, the interpretation of the feed
 *	rate is as follows unless inverse time feed rate mode is being used in the
 *	RS274/NGC view (see Section 3.5.19). The canonical machining functions view of feed
 *	rate, as described in Section 4.3.5.1, has conditions under which the set feed rate
 *	is applied differently, but none of these is used in the Interpreter.
 *
 *	A. 	For motion involving one or more of the X, Y, and Z axes (with or without
 *		simultaneous rotational axis motion), the feed rate means length units per
 *		minute along the programmed XYZ path, as if the rotational axes were not moving.
 *
 *	B.	For motion of one rotational axis with X, Y, and Z axes not moving, the
 *		feed rate means degrees per minute rotation of the rotational axis.
 *
 *	C.	For motion of two or three rotational axes with X, Y, and Z axes not moving,
 *		the rate is applied as follows. Let dA, dB, and dC be the angles in degrees
 *		through which the A, B, and C axes, respectively, must move.
 *		Let D = sqrt(dA^2 + dB^2 + dC^2). Conceptually, D is a measure of total
 *		angular motion, using the usual Euclidean metric. Let T be the amount of
 *		time required to move through D degrees at the current feed rate in degrees
 *		per minute. The rotational axes should be moved in coordinated linear motion
 *		so that the elapsed time from the start to the end of the motion is T plus
 *		any time required for acceleration or deceleration.
 */
static void _calculate_move_times(GCodeState_t *gms, const float axis_length[], const float axis_square[])
										// gms = Gcode model state
{
	float inv_time=0;				// inverse time if doing a feed in G93 mode
	float xyz_time=0;				// coordinated move linear part at requested feed rate
	float abc_time=0;				// coordinated move rotary part at requested feed rate
	float max_time=0;				// time required for the rate-limiting axis
	float tmp_time=0;				// used in computation
	gms->minimum_time = 8675309;	// arbitrarily large number

	// compute times for feed motion
	if (gms->motion_mode != MOTION_MODE_STRAIGHT_TRAVERSE) {
		if (gms->feed_rate_mode == INVERSE_TIME_MODE) {
			inv_time = gms->feed_rate;	// NB: feed rate was un-inverted to minutes by cm_set_feed_rate()
			gms->feed_rate_mode = UNITS_PER_MINUTE_MODE;
		} else {
			// compute length of linear move in millimeters. Feed rate is provided as mm/min
			xyz_time = sqrt(axis_square[AXIS_X] + axis_square[AXIS_Y] + axis_square[AXIS_Z]) / gms->feed_rate;

			// if no linear axes, compute length of multi-axis rotary move in degrees. Feed rate is provided as degrees/min
			if (fp_ZERO(xyz_time)) {
				abc_time = sqrt(axis_square[AXIS_A] + axis_square[AXIS_B] + axis_square[AXIS_C]) / gms->feed_rate;
			}
		}
	}
	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		if (gms->motion_mode == MOTION_MODE_STRAIGHT_TRAVERSE) {
			tmp_time = fabs(axis_length[axis]) / cm.a[axis].velocity_max;
		} else { // MOTION_MODE_STRAIGHT_FEED
			tmp_time = fabs(axis_length[axis]) / cm.a[axis].feedrate_max;
		}
		max_time = max(max_time, tmp_time);

		if (tmp_time > 0) { 	// collect minimum time if this axis is not zero
			gms->minimum_time = min(gms->minimum_time, tmp_time);
		}
	}
	gms->move_time = max4(inv_time, max_time, xyz_time, abc_time);
}

/*
 * _calculate_jerk() - calculate jerk given the dynamic state
 *

 Set the jerk scaling to the lowest axis with a non-zero unit vector.
 Go through the axes one by one and compute the scaled jerk, then pick
 the highest jerk that does not violate any of the axes in the move.

*/

static void _calculate_jerk(mpBuf_t *bf)
{
    // compute the jerk as the largest jerk that still meets axis constraints
    bf->jerk = 8675309;                                        // a ridiculously large number
    float jerk=0;

    for (uint8_t axis=0; axis<AXES; axis++) {
        if (fabs(bf->unit[axis]) > 0) {                        // if this axis is participating in the move
            jerk = cm.a[axis].jerk_max / fabs(bf->unit[axis]);
            if (jerk < bf->jerk) {
                bf->jerk = jerk;
                bf->jerk_axis = axis;                        // +++ diagnostic
            }
        }
    }
    bf->jerk *= JERK_MULTIPLIER;                            // goose it!

    // set up and pre-compute the jerk terms needed for this round of planning
    if (fabs(bf->jerk - mm.jerk) > JERK_MATCH_TOLERANCE) {    // specialized comparison for tolerance of delta
        mm.jerk = bf->jerk;
        mm.recip_jerk = 1/bf->jerk;                            // compute cached jerk terms used by planning
        mm.cbrt_jerk = cbrt(bf->jerk);
    }
    bf->recip_jerk = mm.recip_jerk;
    bf->cbrt_jerk = mm.cbrt_jerk;

/*    // use this form if you don't want the caching
    bf->recip_jerk = 1/bf->jerk;
    bf->cbrt_jerk = cbrt(bf->jerk);
*/
}

// The amount of time that we expect a change in velocity to actually take effect in reality.
// Used it to determine jerk from an "instantaneous" velocity change.

//#define __CENTRIPETAL_JERK

#ifdef __CENTRIPETAL_JERK
/*
 * _calculate_junction_vmax() - Sonny's algorithm - simple
 *
 *  Computes the maximum allowable junction speed by finding the velocity that will yield
 *	the centripetal acceleration in the corner_acceleration value. The value of delta sets
 *	the effective radius of curvature. Here's Sonny's (Sungeun K. Jeon's) explanation
 *	of what's going on:
 *
 *	"First let's assume that at a junction we only look a centripetal acceleration to simply
 *	things. At a junction of two lines, let's place a circle such that both lines are tangent
 *	to the circle. The circular segment joining the lines represents the path for constant
 *	centripetal acceleration. This creates a deviation from the path (let's call this delta),
 *	which is the distance from the junction to the edge of the circular segment. Delta needs
 *	to be defined, so let's replace the term max_jerk (see note 1) with max_junction_deviation,
 *	or "delta". This indirectly sets the radius of the circle, and hence limits the velocity
 *	by the centripetal acceleration. Think of the this as widening the race track. If a race
 *	car is driving on a track only as wide as a car, it'll have to slow down a lot to turn
 *	corners. If we widen the track a bit, the car can start to use the track to go into the
 *	turn. The wider it is, the faster through the corner it can go.
 *
 * (Note 1: "max_jerk" refers to the old grbl/marlin max_jerk" approximation term, not the
 *	tinyG jerk terms)
 *
 *	If you do the geometry in terms of the known variables, you get:
 *		sin(theta/2) = R/(R+delta)  Re-arranging in terms of circle radius (R)
 *		R = delta*sin(theta/2)/(1-sin(theta/2).
 *
 *	Theta is the angle between line segments given by:
 *		cos(theta) = dot(a,b)/(norm(a)*norm(b)).
 *
 *	Most of these calculations are already done in the planner. To remove the acos()
 *	and sin() computations, use the trig half angle identity:
 *		sin(theta/2) = +/- sqrt((1-cos(theta))/2).
 *
 *	For our applications, this should always be positive. Now just plug the equations into
 *	the centripetal acceleration equation: v_c = sqrt(a_max*R). You'll see that there are
 *	only two sqrt computations and no sine/cosines."
 *
 *	How to compute the radius using brute-force trig:
 *		float theta = acos(costheta);
 *		float radius = delta * sin(theta/2)/(1-sin(theta/2));
 */
/*  This version extends Chamnit's algorithm by computing a value for delta that takes
 *	the contributions of the individual axes in the move into account. This allows the
 *	control radius to vary by axis. This is necessary to support axes that have
 *	different dynamics; such as a Z axis that doesn't move as fast as X and Y (such as a
 *	screw driven Z axis on machine with a belt driven XY - like a Shapeoko), or rotary
 *	axes ABC that have completely different dynamics than their linear counterparts.
 *
 *	The function takes the absolute values of the sum of the unit vector components as
 *	a measure of contribution to the move, then scales the delta values from the non-zero
 *	axes into a composite delta to be used for the move. Shown for an XY vector:
 *
 *	 	U[i]	Unit sum of i'th axis	fabs(unit_a[i]) + fabs(unit_b[i])
 *	 	Usum	Length of sums			Ux + Uy
 *	 	d		Delta of sums			(Dx*Ux+DY*UY)/Usum
 */

//static float _calculate_junction_vmax(const float a_unit[], const float b_unit[])
static float _calculate_junction_vmax(const float vmax, const float a_unit[], const float b_unit[])
{
	float costheta = - (a_unit[AXIS_X] * b_unit[AXIS_X])
					 - (a_unit[AXIS_Y] * b_unit[AXIS_Y])
					 - (a_unit[AXIS_Z] * b_unit[AXIS_Z])
					 - (a_unit[AXIS_A] * b_unit[AXIS_A])
					 - (a_unit[AXIS_B] * b_unit[AXIS_B])
					 - (a_unit[AXIS_C] * b_unit[AXIS_C]);

	if (costheta < -0.99) { return (vmax); } 		// straight line cases
	if (costheta > 0.99)  { return (0); } 				// reversal cases

	// Fuse the junction deviations into a vector sum
	float a_delta = square(a_unit[AXIS_X] * cm.a[AXIS_X].junction_dev);
	a_delta += square(a_unit[AXIS_Y] * cm.a[AXIS_Y].junction_dev);
	a_delta += square(a_unit[AXIS_Z] * cm.a[AXIS_Z].junction_dev);
	a_delta += square(a_unit[AXIS_A] * cm.a[AXIS_A].junction_dev);
	a_delta += square(a_unit[AXIS_B] * cm.a[AXIS_B].junction_dev);
	a_delta += square(a_unit[AXIS_C] * cm.a[AXIS_C].junction_dev);

	float b_delta = square(b_unit[AXIS_X] * cm.a[AXIS_X].junction_dev);
	b_delta += square(b_unit[AXIS_Y] * cm.a[AXIS_Y].junction_dev);
	b_delta += square(b_unit[AXIS_Z] * cm.a[AXIS_Z].junction_dev);
	b_delta += square(b_unit[AXIS_A] * cm.a[AXIS_A].junction_dev);
	b_delta += square(b_unit[AXIS_B] * cm.a[AXIS_B].junction_dev);
	b_delta += square(b_unit[AXIS_C] * cm.a[AXIS_C].junction_dev);

    float delta = (sqrt(a_delta) + sqrt(b_delta))/2;
    float sintheta_over2 = sqrt((1 - costheta)/2);
    float radius = delta * sintheta_over2 / (1-sintheta_over2);

    return(min(vmax, (float)sqrt(radius * cm.junction_acceleration)));

/* DIAGNOSTIC
    float velocity = min(vmax, (float)sqrt(radius * cm.junction_acceleration));
    printf("v:%f\n", velocity);
    return (velocity);
*/

//    return(sqrt(radius * cm.junction_acceleration));

// New junction code - untested
//    float delta = (sqrt(a_delta) + sqrt(b_delta))/2;
//    float delta_over_costheta = delta / costheta;
//    float radius = sqrt(delta_over_costheta * delta_over_costheta - delta * delta);
//    return((radius * cm.junction_acceleration) / delta);
}

#else // __CENTRIPETAL_JERK

/*
 * _calculate_junction_vmax() - Giseburt's Algorithm ;-)
 *
 *  Computes the maximum allowable junction speed by finding the velocity that
 *  will not violate the jerk value of any axis.
 *
 *  In order to achieve this, we take the unit vector of the difference of the
 *  unit vectors of the two moves of the corner, at the point from vector a to
 *  vector b. The unit vectors of those two moves are a_unit and b_unit.
 *
 *      Delta[i]       = (b_unit[i] - a_unit[i])                   (1)
 *      UnitMagnitude  = sqrt(Delta[i]^2 + Delta[i+1]^2 + Delta[i+2]^2 + ...)
 *                                                                 (2)
 *      UnitAccel[i]   = Delta[i] / UnitMagnitude                  (3)
 *
 *  We take, axis by axis, the difference in "unit velocity" to get a vector
 *  that represents the direction of acceleration - which may be the opposite
 *  direction as that of the a vector to achieve deceleration. To get the actual
 *  acceleration, we use the corner velocity (what we intend to calculate) as
 *  the magnitude.
 *
 *      Acceleration[i] = UnitAccel[i] * Velocity                  (4)
 *
 *  Since we need the jerk value, which is defined as the "rate of change of
 *  acceleration. That is, the derivative of acceleration with respect to time",
 *  (Wikipedia) we need to have a quantum of time where the change in
 *  acceleration is actually carried out by the physics. That will give us the
 *  time over which to "apply" the change of acceleration in order to get a
 *  physically realistic jerk. The yields a fairly simple formula:
 *
 *      Jerk[i] = Acceleration[i] / Time                           (5)
 *
 *  Now that we can compute the jerk for a given corner, we need to know the
 *  maximum Velocity that we can take the corner without violating that jerk for
 *  any axis. Let's incorporate formula (4) above into formula (5) above, and
 *  solve for Velocity, using the known max Jerk and UnitAccel for this corner:
 *
 *      Velocity[i] = (Jerk[i] * Time) / UnitAccel[i]              (6)
 *
 *  We then compute (6) for each axis, and use the smallest (most limited)
 *  result.
 */
static float _calculate_junction_vmax(const float vmax, const float a_unit[], const float b_unit[])
{
    float best_velocity = (1000000000.0);    // an arbitrarily large number
    float test_velocity;
    
    for (uint8_t axis=0; axis<AXES; axis++) {
        float delta = fabs(b_unit[axis] - a_unit[axis]);
        if (delta > EPSILON) {                              // remove divide-by-zero for efficiency
            test_velocity = cm.a[axis].jerk_max / delta;
            printf ("%d: %f, %f\n", axis, test_velocity, best_velocity); //+++++ omit
            test_velocity = min(test_velocity, best_velocity);
//            if (test_velocity < best_velocity) {          // alternate form is best_axis is needed
//                best_velocity = test_velocity;
//                best_axis = axis;
//            }
        }
    }
    printf ("end: %f, %f\n", test_velocity, best_velocity); //+++++ omit
    best_velocity /= (60 * cm.junction_acceleration);       // convert to mm/min and adjust for quantum
    printf ("corner velocity: %f\n", min(vmax, best_velocity));  //+++++ omit after testing
    return(min(vmax, best_velocity));
}

/*
static float _calculate_junction_vmax(const float vmax, const float a_unit[], const float b_unit[])
{
    cm.junction_acceleration = (CORNER_TIME_QUANTUM);

    float delta[AXES]; // (1)
    delta[AXIS_X] = b_unit[AXIS_X] - a_unit[AXIS_X];
    delta[AXIS_Y] = b_unit[AXIS_Y] - a_unit[AXIS_Y];
    delta[AXIS_Z] = b_unit[AXIS_Z] - a_unit[AXIS_Z];
    delta[AXIS_A] = b_unit[AXIS_A] - a_unit[AXIS_A];
    delta[AXIS_B] = b_unit[AXIS_B] - a_unit[AXIS_B];
    delta[AXIS_C] = b_unit[AXIS_C] - a_unit[AXIS_C];

    uint8_t best_axis = 0;    // reserved for later
    float best_velocity = 8675309;

    for (uint8_t axis=0; axis<AXES; axis++) {
        float recip_delta = 1/delta[axis]; // (3a)
        float test_velocity = (cm.a[axis].jerk_max * CORNER_TIME_QUANTUM) * recip_delta; // (6a)

        if (test_velocity < best_velocity) {
            best_velocity = test_velocity;
//            best_axis = axis;
        }
    }
    cm.junction_acceleration = (min(vmax, best_velocity));
    printf ("%f\n", cm.junction_acceleration);
    return (cm.junction_acceleration);
//    return(min(vmax, best_velocity));
}
*/

#endif // __CENTRIPETAL_JERK

/*
 *  mp_reset_replannable_list() - resets all blocks in the planning list to be replannable
 */
void mp_reset_replannable_list()
{
    mpBuf_t *bf = mp_get_first_buffer();
    if (bf == NULL) return;
    mpBuf_t *bp = bf;
    do {
        bp->replannable = true;
        bp->locked = false;
        bp->buffer_state = MP_BUFFER_PLANNING;
    } while (((bp = mp_get_next_buffer(bp)) != bf) && (bp->move_state != MOVE_OFF));

    mb.needs_replanned = true;
    mb.needs_time_accounting = true;
}
