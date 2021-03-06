/** \file
	\brief Digital differential analyser - this is where we figure out which steppers need to move, and when they need to move
*/

#include	<string.h>
#include	<stdlib.h>
#include	<math.h>
#include	<avr/interrupt.h>

#include	"dda.h"

#include	"timer.h"
#include	"serial.h"
#include	"sermsg.h"
#include	"gcode_parse.h"
#include	"dda_queue.h"
#include	"debug.h"
#include	"sersendf.h"
#include	"pinio.h"
#include	"config.h"
#ifdef	DC_EXTRUDER
	#include	"heater.h"
#endif
#include	"dda_util.h"

/// step timeout
volatile uint8_t	steptimeout = 0;

/*
	position tracking
*/

/// \var startpoint
/// \brief target position of last move in queue
TARGET startpoint __attribute__ ((__section__ (".bss")));

/// \var current_position
/// \brief actual position of extruder head
/// \todo make current_position = real_position (from endstops) + offset from G28 and friends
TARGET current_position __attribute__ ((__section__ (".bss")));

/// \var move_state
/// \brief numbers for tracking the current state of movement
MOVE_STATE move_state __attribute__ ((__section__ (".bss")));

/*! Inititalise DDA movement structures
*/
void dda_init(void) {
	// set up default feedrate
	current_position.F = startpoint.F = next_target.target.F = SEARCH_FEEDRATE_Z;

	#ifdef ACCELERATION_RAMPING
		move_state.n = 1;
#ifdef NEW_DDA_CALCULATIONS
		move_state.c = 2500;	// recognizable value for debugging, actual value will be set later on!
#else
		// Calculate the initial step period, corrected by a factor 1/sqrt(2)
		// to overcome the error in the first step. (See Austin)
		move_state.c = ((uint32_t)((double)F_CPU / sqrt((double)(STEPS_PER_MM_X * ACCELERATION)))) << 8;
		if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
			sersendf_P(PSTR("\n{DDA_INIT: [c:%ld]\n"), move_state.c >> 8);
		}
#endif
	#endif
}

/*! CREATE a dda given current_position and a target, save to passed location so we can write directly into the queue
	\param *dda pointer to a dda_queue entry to overwrite
	\param *target the target position of this move

	\ref startpoint the beginning position of this move

	This function does a /lot/ of math. It works out directions for each axis, distance travelled, the time between the first and second step

	It also pre-fills any data that the selected accleration algorithm needs, and can be pre-computed for the whole move.

	This algorithm is probably the main limiting factor to print speed in terms of firmware limitations
*/
void dda_create(DDA *dda, TARGET *target) {
#ifndef NEW_DDA_CALCULATIONS
	uint32_t	distance, c_limit, c_limit_calc;
#else
	uint32_t	distance, c_limit;
#endif

	// initialise DDA to a known state
	dda->allflags = 0;

	if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
		serial_writestr_P(PSTR("\n{DDA_CREATE: ["));
	}
	// we end at the passed target
	memcpy(&(dda->endpoint), target, sizeof(TARGET));

	dda->x_delta = labs(target->X - startpoint.X);
	dda->y_delta = labs(target->Y - startpoint.Y);
	dda->z_delta = labs(target->Z - startpoint.Z);
	dda->e_delta = labs(target->E - startpoint.E);

	dda->x_direction = (target->X >= startpoint.X)?1:0;
	dda->y_direction = (target->Y >= startpoint.Y)?1:0;
	dda->z_direction = (target->Z >= startpoint.Z)?1:0;
	dda->e_direction = (target->E >= startpoint.E)?1:0;

	if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
		sersendf_P(PSTR("%c%ld,%c%ld,%c%ld,%c%ld] ["),
			(dda->x_direction)? '+' : '-', dda->x_delta,
			(dda->y_direction)? '+' : '-', dda->y_delta,
			(dda->z_direction)? '+' : '-', dda->z_delta,
			(dda->e_direction)? '+' : '-', dda->e_delta );

	// Determine the largest stepcount from all the axes.
	dda->total_steps = dda->x_delta;
	if (dda->y_delta > dda->total_steps)
		dda->total_steps = dda->y_delta;
	if (dda->z_delta > dda->total_steps)
		dda->total_steps = dda->z_delta;
	if (dda->e_delta > dda->total_steps)
		dda->total_steps = dda->e_delta;

	if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
		sersendf_P(PSTR("ts:%lu"), dda->total_steps);

	if (dda->total_steps == 0) {
		dda->nullmove = 1;
	}
	else {
		// get steppers ready to go
		steptimeout = 0;
		power_on();
		x_enable();
		y_enable();
		// Z is enabled in dda_start()
		e_enable();

		// since it's unusual to combine X, Y and Z changes in a single move on reprap, check if we can use simpler approximations before trying the full 3d approximation.
		if (dda->z_delta == 0) {
			if (dda->x_delta == 0) {
				distance = STEPS_TO_UM( Y, dda->y_delta);
			} else if (dda->y_delta == 0) {
				distance = STEPS_TO_UM( X, dda->x_delta);
			} else {
				distance = approx_distance_2d( STEPS_TO_UM( X, dda->x_delta), STEPS_TO_UM( Y, dda->y_delta));
			}
		} else if (dda->x_delta == 0 && dda->y_delta == 0) {
			distance = STEPS_TO_UM( Z, dda->z_delta);
		} else {
			distance = approx_distance_3d( STEPS_TO_UM( X, dda->x_delta), STEPS_TO_UM( Y, dda->y_delta), STEPS_TO_UM( Z, dda->z_delta));
		}
		// Handle E feed if specified.
		// Most of the times, E is very smal. In that case ignore it completely,
		// if E is significant correct distance to include E.
		uint32_t e_feed = STEPS_TO_UM( E, dda->e_delta);
		// if e_feed is more than 1.5% (1/64) of distance, don't ignore it.
		if (distance < (e_feed << 3)) {
			distance = approx_distance_2d( distance, e_feed);
		}
		if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
			sersendf_P(PSTR(",ef:%lu,ds:%lu"), e_feed, distance);
		}

		#ifdef	ACCELERATION_TEMPORAL
			// bracket part of this equation in an attempt to avoid overflow: 60 * 16MHz * 5mm is >32 bits
			uint32_t move_duration = distance * (60 * F_CPU / startpoint.F);
		#else
			// pre-calculate move speed in millimeter microseconds per step minute for less math in interrupt context
			// mm (distance) * 60000000 us/min / step (total_steps) = mm.us per step.min
			//   note: um (distance) * 60000 == mm * 60000000
			// so in the interrupt we must simply calculate
			// mm.us per step.min / mm per min (F) = us per step

			// break this calculation up a bit and lose some precision because 300,000um * 60000 is too big for a uint32
			// calculate this with a uint64 if you need the precision, but it'll take longer so routines with lots of short moves may suffer
			// 2^32/6000 is about 715mm which should be plenty

			// changed * 10 to * (F_CPU / 100000) so we can work in cpu_ticks rather than microseconds.
			// timer.c setTimer() routine altered for same reason

			// changed distance * 6000 .. * F_CPU / 100000 to
			//         distance * 2400 .. * F_CPU / 40000 so we can move a distance of up to 1800mm without overflowing
#ifndef NEW_DDA_CALCULATIONS
			uint32_t move_duration = ((distance * 2400) / dda->total_steps) * (F_CPU / 40000);
#else
			#define TIME_SCALING	(F_CPU / 1000)		// -> IOclocks / ms
			// The compiler won't optimize this correctly, so do it manually:
			//    move_duration = (uint32_t) (distance * 60 * 1000 * (F_CPU / 1000000.0) / TIME_SCALING)
			uint32_t move_duration = distance * 60;
#endif
		#endif

		if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
			sersendf_P(PSTR(",md:%lu"), move_duration);

		// similarly, find out how fast we can run our axes.
		// do this for each axis individually, as the combined speed of two or more axes can be higher than the capabilities of a single one.

#ifndef NEW_DDA_CALCULATIONS
		c_limit = 0;
		// check X axis
		c_limit_calc = ( (dda->x_delta * (UM_PER_STEP_X * 2400L)) / dda->total_steps * (F_CPU / 40000) / MAXIMUM_FEEDRATE_X) << 8;
		if (c_limit_calc > c_limit)
			c_limit = c_limit_calc;
		// check Y axis
		c_limit_calc = ( (dda->y_delta * (UM_PER_STEP_Y * 2400L)) / dda->total_steps * (F_CPU / 40000) / MAXIMUM_FEEDRATE_Y) << 8;
		if (c_limit_calc > c_limit)
			c_limit = c_limit_calc;
		// check Z axis
		c_limit_calc = ( (dda->z_delta * (UM_PER_STEP_Z * 2400L)) / dda->total_steps * (F_CPU / 40000) / MAXIMUM_FEEDRATE_Z) << 8;
		if (c_limit_calc > c_limit)
			c_limit = c_limit_calc;
		// check E axis
		c_limit_calc = ( (dda->e_delta * (UM_PER_STEP_E * 2400L)) / dda->total_steps * (F_CPU / 40000) / MAXIMUM_FEEDRATE_E) << 8;
		if (c_limit_calc > c_limit)
			c_limit = c_limit_calc;

#else

#ifndef ACCELERATION_REPRAP
		// Calculate the duration of the complete move as run at the specified speed.
		// Adjust that duration for any one of the axes running above it's rated speed.
		// This will scale all axes simultanously, resulting in the same move at reduced feed.

		// Start with (an estimate of) the duration of the vectored move at the new feed.
		// Scale back to IOclock ticks after the division by the feed.
		uint32_t limiting_total_clock_ticks = TIME_SCALING * (move_duration / target->F);		// [IOclocks]
		uint32_t min_total_clock_ticks;
#else
		// Less optimized, allows use with ACCELERATION_REPRAP
		// In this case move_duration is not used in the calculation of c_limit.
		uint32_t limiting_total_clock_ticks = 0;
		uint32_t min_total_clock_ticks;
#endif
		// For each axis, determine the minimum number of IOclocks needed to run at the maximum
		// speed. Take into account that the axis runs at a fraction of the speed of the vectored move.
		// The maximum of these numbers and the time it takes to make the vectored move at the
		// specified speed, determines the (limiting) speed we'll run at.
		//
		min_total_clock_ticks			= dda->x_delta * MIN_CLOCKS_PER_STEP_X;
		if (min_total_clock_ticks > limiting_total_clock_ticks) {
			limiting_total_clock_ticks	= min_total_clock_ticks;
		}
		min_total_clock_ticks			= dda->y_delta * MIN_CLOCKS_PER_STEP_Y;
		if (min_total_clock_ticks > limiting_total_clock_ticks) {
			limiting_total_clock_ticks	= min_total_clock_ticks;
		}
		min_total_clock_ticks			= dda->z_delta * MIN_CLOCKS_PER_STEP_Z;
		if (min_total_clock_ticks > limiting_total_clock_ticks) {
			limiting_total_clock_ticks	= min_total_clock_ticks;
		}
		min_total_clock_ticks			= dda->e_delta * MIN_CLOCKS_PER_STEP_E;
		if (min_total_clock_ticks > limiting_total_clock_ticks) {
			limiting_total_clock_ticks	= min_total_clock_ticks;
		}
		c_limit = limiting_total_clock_ticks / dda->total_steps;

		// THIS SOLVES THE BIGGEST PROBLEM: LOW SPEED
		// 20110821 modmaker - Finally found the cause of the too low speeds:
		//                     The single timer start value (c0) calculated in dda_init
		// is not sufficient as it's only valid for a single axis move along the x (and y by accident).
		// We need to calculate c0 for each move, and that implies a division and a square root operation.

		dda->c0 = (F_CPU / int_sqrt( (1000 * ACCELERATION * dda->total_steps) / distance)) << 8;
		if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
			sersendf_P(PSTR(",c0:%lu"), dda->c0 >> 8);
		}
		if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
			sersendf_P(PSTR(",cl:%lu"), c_limit);

#endif

		#ifdef ACCELERATION_REPRAP
		// c is initial step time in IOclk ticks
		dda->c = (TIME_SCALING * (move_duration / startpoint.F)) << 8;
		if (dda->c < c_limit)
			dda->c = c_limit;
		dda->end_c = (TIME_SCALING * (move_duration / target->F)) << 8;
		if (dda->end_c < c_limit)
			dda->end_c = c_limit;

		if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
			sersendf_P(PSTR(",md:%lu,c:%lu"), move_duration, dda->c >> 8);

		if (dda->c != dda->end_c) {
			uint32_t stF = startpoint.F / 4;
			uint32_t enF = target->F / 4;
			// now some constant acceleration stuff, courtesy of http://www.embedded.com/columns/technicalinsights/56800129?printable=true
			uint32_t ssq = (stF * stF);
			uint32_t esq = (enF * enF);
			int32_t dsq = (int32_t) (esq - ssq) / 4;

			uint8_t msb_ssq = msbloc(ssq);
			uint8_t msb_tot = msbloc(dda->total_steps);

			// the raw equation WILL overflow at high step rates, but 64 bit math routines take waay too much space
			// at 65536 mm/min (1092mm/s), ssq/esq overflows, and dsq is also close to overflowing if esq/ssq is small
			// but if ssq-esq is small, ssq/dsq is only a few bits
			// we'll have to do it a few different ways depending on the msb locations of each
			if ((msb_tot + msb_ssq) <= 30) {
				// we have room to do all the multiplies first
				if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
					serial_writechar('A');
				dda->n = ((int32_t) (dda->total_steps * ssq) / dsq) + 1;
			}
			else if (msb_tot >= msb_ssq) {
				// total steps has more precision
				if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
					serial_writechar('B');
				dda->n = (((int32_t) dda->total_steps / dsq) * (int32_t) ssq) + 1;
			}
			else {
				// otherwise
				if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
					serial_writechar('C');
				dda->n = (((int32_t) ssq / dsq) * (int32_t) dda->total_steps) + 1;
			}

			if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
				sersendf_P(PSTR("\n{DDA:CA end_c:%lu, n:%ld, md:%lu, ssq:%lu, esq:%lu, dsq:%lu, msbssq:%u, msbtot:%u}\n"), dda->end_c >> 8, dda->n, move_duration, ssq, esq, dsq, msb_ssq, msb_tot);

			dda->accel = 1;
		}
		else
			dda->accel = 0;
		#elif defined ACCELERATION_RAMPING
// remove this when people have swallowed the new config item
#ifdef ACCELERATION_STEEPNESS
#error ACCELERATION_STEEPNESS is gone, review your config.h and use ACCELERATION
#endif
			// yes, this assumes always the x axis as the critical one regarding acceleration. If we want to implement per-axis acceleration, things get tricky ...
#ifndef NEW_DDA_CALCULATIONS
			dda->c_min = (move_duration / target->F) << 8;
			if (dda->c_min < c_limit << 8)
				dda->c_min = c_limit << 8;
#else
			// TODO: (remove later) The new code started with the move_duration, that's why we're done here!
			dda->c_min = c_limit << 8;
#endif
			if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
				sersendf_P(PSTR(",c-:%lu"), dda->c_min);
			}
			// 20110819 modmaker - Calculation of the length of the ramps.
			// 
			// The profile is always symetrical (i.e. ramp up and ramp down have
			// the same slope, defined by ACCELERATION).
			//
			// This is a very tricky calculation to do in 32 bits as all precision is
			// needed to get the correct number of steps. If bits are lost and the
			// number is small, the reached feed will be too low.
			// 
			// Do this calculation in several steps so it's easy to detect overflow
			// when debug is on.
			uint32_t x = (F_CPU / c_limit);						// (24 - 11..12) -> 12..13 bits
			if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
				sersendf_P(PSTR(",(x:%lu"), x);
			}
			x *= x;												// + (24 - 11..12) -> 24..26 bits
			if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
				sersendf_P(PSTR("->%lu"), x);
			}
			x >>= 12;	/* scale down but keep precision! */	// - 12 -> 12..14 bits
			if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
				sersendf_P(PSTR("->%lu"), x);
			}
			x *= distance;										// + 5..18 -> 17..32 bits
			if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
				sersendf_P(PSTR("->%lu"), x);
			}
			// total_steps has a fixed relation to distance (um/step) !
			x /= (((uint32_t)(2000 * ACCELERATION) >> 6) * dda->total_steps) >> 6; //    -> 0..14 bits
			if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
				sersendf_P(PSTR("->%lu"), x);
			}
			dda->rampup_steps = x;
			// If move is too short for full ramp-up and ramp-down, clip ramping.
			if (2 * dda->rampup_steps > dda->total_steps) {
				dda->rampup_steps = dda->total_steps / 2;
			}
			// rampdown_steps is not actually the number of rampdown steps, but the
			// step number at which the rampdown starts!
			dda->rampdown_steps = dda->total_steps - dda->rampup_steps;
			if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
				sersendf_P(PSTR(",ru:%lu,rd:%lu"), dda->rampup_steps, dda->rampdown_steps);
			}
		#else
#ifndef NEW_DDA_CALCULATIONS
			dda->c = (move_duration / target->F) << 8;
			if (dda->c < c_limit)
				dda->c = c_limit;
#else
			dda->c = c_limit << 8;
#endif
		#endif
	}

	if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
		serial_writestr_P(PSTR("] }\n"));

	// next dda starts where we finish
	memcpy(&startpoint, target, sizeof(TARGET));
	// if E is relative, reset it here
	#ifndef E_ABSOLUTE
		startpoint.E = 0;
	#endif
}

/*! Start a prepared DDA
	\param *dda pointer to entry in dda_queue to start

	This function actually begins the move described by the passed DDA entry.

	We set direction and enable outputs, and set the timer for the first step from the precalculated value.

	We also mark this DDA as running, so other parts of the firmware know that something is happening

	Called both inside and outside of interrupts.
*/
void dda_start(DDA *dda) {
	// called from interrupt context: keep it simple!
	if (dda->nullmove) {
		// just change speed?
		current_position.F = dda->endpoint.F;
		// keep dda->live = 0
	}
	else {
		// get ready to go
		steptimeout = 0;
		if (dda->z_delta)
			z_enable();

		// set direction outputs
		x_direction(dda->x_direction);
		y_direction(dda->y_direction);
		z_direction(dda->z_direction);
		e_direction(dda->e_direction);

		#ifdef	DC_EXTRUDER
		if (dda->e_delta)
			heater_set(DC_EXTRUDER, DC_EXTRUDER_PWM);
		#endif

		// initialise state variable
		move_state.x_counter = move_state.y_counter = move_state.z_counter = \
			move_state.e_counter = -(dda->total_steps >> 1);
		memcpy(&move_state.x_steps, &dda->x_delta, sizeof(uint32_t) * 4);
		#ifdef ACCELERATION_RAMPING
			move_state.step_no = 0;
		#endif

		# ifdef NEW_DDA_CALCULATIONS
			move_state.c = dda->c0;
		# endif

		// ensure this dda starts
		dda->live = 1;

		// set timeout for first step
		#ifdef ACCELERATION_RAMPING
		if (dda->c_min > move_state.c) // can be true when look-ahead removed all deceleration steps
			setTimer(dda->c_min >> 8);
		else
			setTimer(move_state.c >> 8);
		#else
		setTimer(dda->c >> 8);
		#endif
	}
}

/*! STEP
	\param *dda the current move

	This is called from our timer interrupt every time a step needs to occur. Keep it as simple as possible!
	We first work out which axes need to step, and generate step pulses for them
	Then we re-enable global interrupts so serial data reception and other important things can occur while we do some math.
	Next, we work out how long until our next step using the selected acceleration algorithm and set the timer.
	Then we decide if this was the last step for this move, and if so mark this dda as dead so next timer interrupt we can start a new one.
	Finally we de-assert any asserted step pins.

	\todo take into account the time that interrupt takes to run
*/
void dda_step(DDA *dda) {
	uint8_t	did_step = 0;

	if ((move_state.x_steps) /* &&
			(x_max() != dda->x_direction) && (x_min() == dda->x_direction) */) {
		move_state.x_counter -= dda->x_delta;
		if (move_state.x_counter < 0) {
			x_step();
			did_step = 1;
			move_state.x_steps--;
			move_state.x_counter += dda->total_steps;
		}
	}

	if ((move_state.y_steps) /* &&
			(y_max() != dda->y_direction) && (y_min() == dda->y_direction) */) {
		move_state.y_counter -= dda->y_delta;
		if (move_state.y_counter < 0) {
			y_step();
			did_step = 1;
			move_state.y_steps--;
			move_state.y_counter += dda->total_steps;
		}
	}

	if ((move_state.z_steps) /* &&
			(z_max() != dda->z_direction) && (z_min() == dda->z_direction) */) {
		move_state.z_counter -= dda->z_delta;
		if (move_state.z_counter < 0) {
			z_step();
			did_step = 1;
			move_state.z_steps--;
			move_state.z_counter += dda->total_steps;
		}
	}

	if (move_state.e_steps) {
		move_state.e_counter -= dda->e_delta;
		if (move_state.e_counter < 0) {
			e_step();
			did_step = 1;
			move_state.e_steps--;
			move_state.e_counter += dda->total_steps;
		}
	}

	#if STEP_INTERRUPT_INTERRUPTIBLE
		// since we have sent steps to all the motors that will be stepping and the rest of this function isn't so time critical,
		// this interrupt can now be interruptible
		// however we must ensure that we don't step again while computing the below, so disable *this* interrupt but allow others to fire
// 		disableTimerInterrupt();
		sei();
	#endif

	#ifdef ACCELERATION_REPRAP
		// linear acceleration magic, courtesy of http://www.embedded.com/columns/technicalinsights/56800129?printable=true
		if (dda->accel) {
			if ((dda->c > dda->end_c) && (dda->n > 0)) {
				uint32_t new_c = dda->c - (dda->c * 2) / dda->n;
				if (new_c <= dda->c && new_c > dda->end_c) {
					dda->c = new_c;
					dda->n += 4;
				}
				else
					dda->c = dda->end_c;
			}
			else if ((dda->c < dda->end_c) && (dda->n < 0)) {
				uint32_t new_c = dda->c + ((dda->c * 2) / -dda->n);
				if (new_c >= dda->c && new_c < dda->end_c) {
					dda->c = new_c;
					dda->n += 4;
				}
				else
					dda->c = dda->end_c;
			}
			else if (dda->c != dda->end_c) {
				dda->c = dda->end_c;
			}
			// else we are already at target speed
		}
	#endif
	#ifdef ACCELERATION_RAMPING
		// - algorithm courtesy of http://www.embedded.com/columns/technicalinsights/56800129?printable=true
		// - precalculate ramp lengths instead of counting them, see AVR446 tech note
		uint8_t recalc_speed;

		// debug ramping algorithm
		//if (move_state.step_no == 0) {
		//	sersendf_P(PSTR("\r\nc %lu  c_min %lu  n %d"), dda->c, dda->c_min, move_state.n);
		//}

		recalc_speed = 0;
		if (move_state.step_no < dda->rampup_steps) {
			if (move_state.n < 0) // wrong ramp direction
				move_state.n = -((int32_t)2) - move_state.n;
			recalc_speed = 1;
		}
		else if (move_state.step_no > dda->rampdown_steps) {
			if (move_state.n > 0) // wrong ramp direction
				move_state.n = -((int32_t)2) - move_state.n;
			recalc_speed = 1;
		}
		if (recalc_speed) {
			move_state.n += 4;
			// be careful of signedness!
			move_state.c = (int32_t)move_state.c - ((int32_t)(move_state.c * 2) / (int32_t)move_state.n);
		}
		move_state.step_no++;

		// debug ramping algorithm
		// for very low speeds like 10 mm/min, only
		//if (move_state.step_no % 10 /* 10, 100, ...*/ == 0)
		//	sersendf_P(PSTR("\r\nc %lu  c_min %lu  n %d"), dda->c, dda->c_min, move_state.n);
	#endif

	// TODO: did_step is obsolete ...
	if (did_step) {
		// we stepped, reset timeout
		steptimeout = 0;

		// if we could do anything at all, we're still running
		// otherwise, must have finished
	}
	else if (move_state.x_steps == 0 && move_state.y_steps == 0 && move_state.z_steps == 0 && move_state.e_steps == 0) {
		dda->live = 0;
		// if E is relative reset it
		#ifndef E_ABSOLUTE
			current_position.E = 0;
		#endif
		// linear acceleration code doesn't alter F during a move, so we must update it here
		// in theory, we *could* update F every step, but that would require a divide in interrupt context which should be avoided if at all possible
		current_position.F = dda->endpoint.F;
		#ifdef	DC_EXTRUDER
			heater_set(DC_EXTRUDER, 0);
		#endif
		// z stepper is only enabled while moving
		z_disable();
	}

	cli();

	#ifdef ACCELERATION_RAMPING
		// we don't hit maximum speed exactly with acceleration calculation, so limit it here
		// the nice thing about _not_ setting dda->c to dda->c_min is, the move stops at the exact same c as it started, so we have to calculate c only once for the time being
		// TODO: set timer only if dda->c has changed
		if (dda->c_min > move_state.c)
			setTimer(dda->c_min >> 8);
		else
			setTimer(move_state.c >> 8);
	#else
		setTimer(dda->c >> 8);
	#endif

	// turn off step outputs, hopefully they've been on long enough by now to register with the drivers
	// if not, too bad. or insert a (very!) small delay here, or fire up a spare timer or something.
	// we also hope that we don't step before the drivers register the low- limit maximum speed if you think this is a problem.
	unstep();
}

/// update global current_position struct
void update_position() {
	DDA *dda = &movebuffer[mb_tail];

	if (dda->live == 0)
		return;

	if (dda->x_direction)
		current_position.X = dda->endpoint.X - move_state.x_steps;
	else
		current_position.X = dda->endpoint.X + move_state.x_steps;

	if (dda->y_direction)
		current_position.Y = dda->endpoint.Y - move_state.y_steps;
	else
		current_position.Y = dda->endpoint.Y + move_state.y_steps;

	if (dda->z_direction)
		current_position.Z = dda->endpoint.Z - move_state.z_steps;
	else
		current_position.Z = dda->endpoint.Z + move_state.z_steps;

	#ifndef E_ABSOLUTE
		current_position.E = move_state.e_steps;
	#else
		if (dda->e_direction)
			current_position.E = dda->endpoint.E - move_state.e_steps;
		else
			current_position.E = dda->endpoint.E + move_state.e_steps;
	#endif
}
