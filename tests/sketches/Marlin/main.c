#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "Configuration.h"
#include "planner.h"
#include "ardutime.h"

#define errExit(msg) 	do { perror(msg); exit(EXIT_FAILURE); \
										 	} while (0)

extern char _binary_test_gcode_start;

//===========================================================================
//=============================public variables=============================
//===========================================================================
// none of variables here need to be protected by lock coz they won't be
// accessed from except the main thread
int extrudemultiply=100; //100->1 200->2
float add_homeing[3]={0,0,0};
bool axis_known_position[3] = {false, false, false};
//--TOM--: the active extruder is always #0 coz that's the only one we've got
char active_extruder = 0;
int fanSpeed=0;

float base_min_pos[3] = { X_MIN_POS_DEFAULT, Y_MIN_POS_DEFAULT, Z_MIN_POS_DEFAULT };
float base_max_pos[3] = { X_MAX_POS_DEFAULT, Y_MAX_POS_DEFAULT, Z_MAX_POS_DEFAULT };
#ifdef ENABLE_AUTO_BED_LEVELING
float bed_level_probe_offset[3] = {X_PROBE_OFFSET_FROM_EXTRUDER_DEFAULT,
	Y_PROBE_OFFSET_FROM_EXTRUDER_DEFAULT, -Z_PROBE_OFFSET_FROM_EXTRUDER_DEFAULT};
#endif

//===========================================================================
//=============================private variables=============================
//===========================================================================
static float homing_feedrate[] = HOMING_FEEDRATE;
static bool axis_relative_modes[] = AXIS_RELATIVE_MODES;
static int feedmultiply=100; //100->1 200->2
static int saved_feedmultiply;
static float min_pos[3] = { X_MIN_POS_DEFAULT, Y_MIN_POS_DEFAULT, Z_MIN_POS_DEFAULT };
static float max_pos[3] = { X_MAX_POS_DEFAULT, Y_MAX_POS_DEFAULT, Z_MAX_POS_DEFAULT };
static float current_position[NUM_AXIS] = { 0.0, 0.0, 0.0, 0.0 };
static float destination[NUM_AXIS] = {  0.0, 0.0, 0.0, 0.0};
static const char axis_codes[NUM_AXIS] = {'X', 'Y', 'Z', 'E'};
static float offset[3] = {0.0, 0.0, 0.0};
static bool home_all_axis = true;
static float feedrate = 1500.0, next_feedrate, saved_feedrate;
static long gcode_N, gcode_LastN, Stopped_gcode_LastN = 0;
static bool relative_mode = false;  //Determines Absolute or Relative Coordinates

static char cmdbuffer[MAX_CMD_SIZE];
static char *strchr_pointer; // just a pointer to find chars in the cmd string like X, Y, Z, E, etc

//Inactivity shutdown variables
static unsigned long previous_millis_cmd = 0;
static unsigned long max_inactive_time = 0;
static unsigned long stepper_inactive_time = DEFAULT_STEPPER_DEACTIVE_TIME*1000l;

static unsigned long starttime=0;
static unsigned long stoptime=0;

static uint8_t tmp_extruder;

static bool target_direction;
static bool Stopped = false;

static char *file_buf = NULL;
static int file_size;

#define XYZ_CONSTS_FROM_CONFIG(type, array, CONFIG) \
static const type array##_P[3] =        \
    { X_##CONFIG, Y_##CONFIG, Z_##CONFIG };     \
static inline type array(int axis)          \
    { return array##_P[axis]; }

#define XYZ_DYN_FROM_CONFIG(type, array, CONFIG)	\
static inline type array(int axis)			\
    { type temp[3] = { X_##CONFIG, Y_##CONFIG, Z_##CONFIG };\
      return temp[axis];}

XYZ_DYN_FROM_CONFIG(float, base_home_pos,   HOME_POS);
XYZ_DYN_FROM_CONFIG(float, max_length, MAX_LENGTH);
XYZ_CONSTS_FROM_CONFIG(float, home_retract_mm, HOME_RETRACT_MM);
XYZ_CONSTS_FROM_CONFIG(signed char, home_dir,  HOME_DIR);

//===========================================================================
//=============================ROUTINES=============================
//===========================================================================

void setup() {
 	file_buf = &_binary_test_gcode_start;
	DEBUG_PRINT("%s", file_buf);
}

float code_value()
{
  return (strtod(&cmdbuffer[strchr_pointer - cmdbuffer + 1], NULL));
}

long code_value_long()
{
  return (strtol(&cmdbuffer[strchr_pointer - cmdbuffer + 1], NULL, 10));
}

bool code_seen(char code)
{
  strchr_pointer = strchr(cmdbuffer, code);
  return (strchr_pointer != NULL);  //Return True if a character was found
}

//get-command() processes one line at a time
//if it is a command, return true with the command copied into cmdbuffer
//if it is an empty or comment line, return false
bool get_command()
{
  static int file_pos = 0;
  char cur_char;
  bool comment_mode = false;
  int read_count = 0;

  while (file_pos < file_size) {
    cur_char = file_buf[file_pos++];

    //if cur_char is ';', turn comment_mode on.
    //if cur_char is '\n', end of line. turn comment_mode off. Check read_count,
    ////if 0, this is an empty or comment line. 
    ////if not 0, there must be something in the cmdbuffer, return.
    //if not ';' or '\n', check comment_mode.
    ////if on, do nothing.
    ////if off, copy current char into cmdbuffer and increment read_count

    if (cur_char == ';') {
      comment_mode = true;
    }
    else if (cur_char == '\n') {
      if (read_count == 0) {
        //this is an empty or comment line 
        return false;
      } else {
        cmdbuffer[read_count] = '\0';
        if(strchr(cmdbuffer, 'N') != NULL) {
          errExit("line number support needed\n");
        }
        if((strchr(cmdbuffer, '*') != NULL)) {
          errExit("checksum support needed\n");
        }
        if((strchr(cmdbuffer, 'G') != NULL)){
          strchr_pointer = strchr(cmdbuffer, 'G');
          switch((int)((strtod(&cmdbuffer[strchr_pointer - cmdbuffer + 1], NULL)))){
          case 0:
          case 1:
          case 2:
          case 3:
            if(Stopped == true) { // If printer is stopped by an error the G[0-3] codes are ignored.
              fprintf(stderr, "G[0-3] codes will be ignored because printer is stopped\n");
            }
            break;
          default:
            break;
          }
        }
        return true;
      }
    }
    else {
      if (!comment_mode) {
        cmdbuffer[read_count++] = cur_char; 
      }
    }
  }
  return false;
}

void get_coordinates()
{
  int8_t i;
  bool seen[4]={false,false,false,false};
  for(i=0; i < NUM_AXIS; i++) {
    if(code_seen(axis_codes[i]))
    {
      destination[i] = (float)code_value() + (axis_relative_modes[i] || relative_mode)*current_position[i];
      seen[i]=true;
    }
    else destination[i] = current_position[i]; //Are these else lines really needed?
  }
  if(code_seen('F')) {
    next_feedrate = code_value();
    if(next_feedrate > 0.0) feedrate = next_feedrate;
  }
}

void clamp_to_software_endstops(float target[3])
{
  if (min_software_endstops) {
    if (target[X_AXIS] < min_pos[X_AXIS]) target[X_AXIS] = min_pos[X_AXIS];
    if (target[Y_AXIS] < min_pos[Y_AXIS]) target[Y_AXIS] = min_pos[Y_AXIS];
    if (target[Z_AXIS] < min_pos[Z_AXIS]) target[Z_AXIS] = min_pos[Z_AXIS];
  }

  if (max_software_endstops) {
    if (target[X_AXIS] > max_pos[X_AXIS]) target[X_AXIS] = max_pos[X_AXIS];
    if (target[Y_AXIS] > max_pos[Y_AXIS]) target[Y_AXIS] = max_pos[Y_AXIS];
    if (target[Z_AXIS] > max_pos[Z_AXIS]) target[Z_AXIS] = max_pos[Z_AXIS];
  }
}

void prepare_move()
{
  clamp_to_software_endstops(destination);

  previous_millis_cmd = millis();

  DEBUG_PRINT("MAIN current: (%lf, %lf, %lf, %lf)\n",
      current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS],
      current_position[E_AXIS]);
  DEBUG_PRINT("MAIN target: (%lf, %lf, %lf, %lf)\n",
      destination[X_AXIS], destination[Y_AXIS], destination[Z_AXIS], destination[E_AXIS]);
  // Do not use feedmultiply for E or Z only moves
  if( (current_position[X_AXIS] == destination [X_AXIS]) && (current_position[Y_AXIS] == destination [Y_AXIS])) {
      plan_buffer_line(destination[X_AXIS], destination[Y_AXIS], destination[Z_AXIS], destination[E_AXIS], feedrate/60, active_extruder);
  }
  else {
    plan_buffer_line(destination[X_AXIS], destination[Y_AXIS], destination[Z_AXIS], destination[E_AXIS], feedrate*feedmultiply/60/100.0, active_extruder);
  }
  int8_t i;
  for(i=0; i < NUM_AXIS; i++) {
    current_position[i] = destination[i];
  }
}

void process_commands()
{
  int8_t i;

  if(code_seen('G')) {
    switch((int)code_value()) {
    case 0: // G0 -> G1
    case 1: // G1
      if(Stopped == false) {
        get_coordinates(); // For X Y Z E F
        prepare_move();
      }
      return;
#if 0
    case 28: //G28 Home all Axis one at a time
      do_home();
      return;
    #ifdef ENABLE_AUTO_BED_LEVELING
    case 29: // G29 Detailed Z-Probe, probes the bed at 3 points.
      do_auto_bed_leveling();
      return;
    #endif
    case 90: // G90
      relative_mode = false;
      break;
    case 91: // G91
      relative_mode = true;
      break;
    case 92: // G92
      do_set_position();
     break;
    default:
      fprintf(stderr, "Unsupported G command found!\n");
      break;
    }
  } else if(code_seen('M')) {
    switch((int)code_value()) {
    case 82:
      axis_relative_modes[3] = false;
      break;
    case 83:
      axis_relative_modes[3] = true;
      break;
    case 18: //compatibility
    case 84: // M84
      stop_idle_hold();
      break;
    case 104: // M104
      if(setTargetedHotend(104)){
        break;
      }
      if (code_seen('S')) setTargetHotend(code_value(), tmp_extruder);
      setWatch();
      break;
    case 106: //M106 Fan On
      if (code_seen('S')){
         fanSpeed=constrain(code_value(),0,255);
      }
      else {
        fanSpeed=255;
      }
      break;
    case 107: //M107 Fan Off
      fanSpeed = 0;
      break;
    case 109:
      set_temp_and_wait();
      break;
    #ifdef ENABLE_AUTO_BED_LEVELING
    case 212: //M212 - Set probe offset for bed leveling
    {
      for(i=0; i < 3; i++)
      {
        if(code_seen(axis_codes[i]))
        {
          bed_level_probe_offset[i] = code_value();
        }
      }
    }
    break;
    #endif
    default:
      fprintf(stderr, "Unsupported M command found!\n");
      break;
#endif
    }
  }
}

void loop() {
	if (get_command()) {
    DEBUG_PRINT("==========================================\n");
    DEBUG_PRINT("%s\n", cmdbuffer);
    process_commands();
	}
	while(1);
}


