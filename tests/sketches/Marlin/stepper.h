/*
  stepper.h - stepper motor driver: executes motion plans of planner.c using the stepper motors
  Part of Grbl

  Copyright (c) 2009-2011 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef stepper_h
#define stepper_h 

#include "planner.h"

// Initialize and start the stepper motor subsystem
extern void st_init();

// Block until all buffered steps are executed
extern void st_synchronize();

// Set current position in steps
extern void st_set_position(const long x, const long y, const long z, const long e);
extern void st_set_e_position(const long e);

// Get current position in steps
extern long st_get_position(uint8_t axis);

#ifdef ENABLE_AUTO_BED_LEVELING
// Get current position in mm
extern float st_get_position_mm(uint8_t axis);
#endif  //ENABLE_AUTO_BED_LEVELING

// The stepper subsystem goes to sleep when it runs out of things to execute. Call this
// to notify the subsystem that it is time to go to work.
// --TOM--: we don't really need st_wake_up for now coz the stepper subsystem
// will never be put to sleep even if the printer is inactive for a long time
// check the code in manage_inactivity()
void st_wake_up();
  
void checkHitEndstops(); //call from somwhere to create an serial error message with the locations the endstops where hit, in case they were triggered
void endstops_hit_on_purpose(); //avoid creation of the message, i.e. after homeing and before a routine call of checkHitEndstops();

void enable_endstops(bool check); // Enable/disable endstop checking

void checkStepperErrors(); //Print errors detected by the stepper

void finishAndDisableSteppers();

//extern block_t *current_block;  // A pointer to the block currently being traced

#endif

/* vi: set et sw=2 sts=2: */
