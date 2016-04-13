// Tonokip RepRap firmware rewrite based off of Hydra-mmm firmware.
// Licence: GPL

#ifndef MARLIN_H
#define MARLIN_H

#define  FORCE_INLINE __attribute__((always_inline)) inline

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>

//#include "Arduino.h"
//#include "fastio.h"
#include "Configuration.h"
#include "Marlin_pins.h"

/* only support this frequency value for now */
#define F_CPU 16000000

#define ECHO_FLOAT(x) fprintf(stderr, "%lf", x)
#define ECHO_STRING(x) fprintf(stderr, "%s", x)
#define ECHONL() fprintf(stderr, "\n")
#define ECHO_DECIMAL(x) fprintf(stderr, "%d", x)
#define ECHOPAIR_L(name,value) fprintf(stderr, "%s: %lu", name, value)
#define ECHOPAIR_F(name,value) fprintf(stderr, "%s: %lf", name, value)

#define errExit(msg) 	do { perror(msg); exit(EXIT_FAILURE); \
										 	} while (0)

#ifdef DEBUG
#define DEBUG_PRINT(...) do{ fprintf( stderr, __VA_ARGS__ ); } while( false )
#else
#define DEBUG_PRINT(...) do{ } while ( false )
#endif

enum AxisEnum {X_AXIS=0, Y_AXIS=1, Z_AXIS=2, E_AXIS=3};

void loop();
bool get_command();
void process_commands();
void get_coordinates();
void prepare_move();
void clamp_to_software_endstops(float target[3]);
void ikill();
void manage_inactivity();
bool IsStopped();
void Stop();

#if defined(X_ENABLE_PIN) && X_ENABLE_PIN > -1
  #define  enable_x() WRITE(X_ENABLE_PIN, X_ENABLE_ON)
  #define disable_x() { WRITE(X_ENABLE_PIN,!X_ENABLE_ON); axis_known_position[X_AXIS] = false; }
#else
  #define enable_x() ;
  #define disable_x() ;
#endif

#if defined(Y_ENABLE_PIN) && Y_ENABLE_PIN > -1
  #define  enable_y() WRITE(Y_ENABLE_PIN, Y_ENABLE_ON)
  #define disable_y() { WRITE(Y_ENABLE_PIN,!Y_ENABLE_ON); axis_known_position[Y_AXIS] = false; }
#else
  #define enable_y() ;
  #define disable_y() ;
#endif

#if defined(Z_ENABLE_PIN) && Z_ENABLE_PIN > -1
 #define  enable_z() WRITE(Z_ENABLE_PIN, Z_ENABLE_ON)
 #define disable_z() { WRITE(Z_ENABLE_PIN,!Z_ENABLE_ON); axis_known_position[Z_AXIS] = false; }
#else
  #define enable_z() ;
  #define disable_z() ;
#endif

#if defined(E0_ENABLE_PIN) && (E0_ENABLE_PIN > -1)
  #define enable_e0() WRITE(E0_ENABLE_PIN, E_ENABLE_ON)
  #define disable_e0() WRITE(E0_ENABLE_PIN,!E_ENABLE_ON)
#else
  #define enable_e0()  /* nothing */
  #define disable_e0() /* nothing */
#endif

#if (EXTRUDERS > 1) && defined(E1_ENABLE_PIN) && (E1_ENABLE_PIN > -1)
  #define enable_e1() ;//WRITE(E1_ENABLE_PIN, E_ENABLE_ON)
  #define disable_e1() ;//WRITE(E1_ENABLE_PIN,!E_ENABLE_ON)
#else
  #define enable_e1()  /* nothing */
  #define disable_e1() /* nothing */
#endif

#if (EXTRUDERS > 2) && defined(E2_ENABLE_PIN) && (E2_ENABLE_PIN > -1)
  #define enable_e2() ;//WRITE(E2_ENABLE_PIN, E_ENABLE_ON)
  #define disable_e2() ;//WRITE(E2_ENABLE_PIN,!E_ENABLE_ON)
#else
  #define enable_e2()  /* nothing */
  #define disable_e2() /* nothing */
#endif

extern int extrudemultiply;
extern int fanSpeed;
extern char active_extruder;
extern float add_homeing[3];
#ifdef ENABLE_AUTO_BED_LEVELING
extern float bed_level_probe_offset[3];
#endif
extern float base_min_pos[3];
extern float base_max_pos[3];
extern bool axis_known_position[3];

#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
      __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
      __typeof__ (b) _b = (b); \
     _a > _b ? _b : _a; })

#define square(x) (x*x)

#endif

/* vi: set et sw=2 sts=2: */
