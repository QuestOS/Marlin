/*
  temperature.c - temperature control
  Part of Marlin
  
 Copyright (C) 2011 Camiel Gubbels / Erik van der Zalm
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 This firmware is a mashup between Sprinter and grbl.
  (https://github.com/kliment/Sprinter)
  (https://github.com/simen/grbl/tree)
 
 It has preliminary support for Matthew Roberts advance algorithm 
    http://reprap.org/pipermail/reprap-dev/2011-May/003323.html

 */

#include <inttypes.h>
#include "pthread.h"
#include "Marlin.h"
#include "temperature.h"
#include "thermistortables.h"
#include "fastio.h"
#include "ardutime.h"
#include "arduthread.h"

//===========================================================================
//=============================public variables============================
//===========================================================================
int target_temperature[EXTRUDERS] = { 0 };
int current_temperature_raw[EXTRUDERS] = { 0 };
float current_temperature[EXTRUDERS] = { 0.0 };

//pthread_t temp_thread;

#ifdef PIDTEMP
  float Kp=DEFAULT_Kp;
  float Ki=(DEFAULT_Ki*PID_dT);
  float Kd=(DEFAULT_Kd/PID_dT);
  #ifdef PID_ADD_EXTRUSION_RATE
    float Kc=DEFAULT_Kc;
  #endif
#endif //PIDTEMP

//===========================================================================
//=============================private variables============================
//===========================================================================
//static int timerid;
static volatile bool temp_meas_ready = false;

#ifdef PIDTEMP
  //static cannot be external:
  static float temp_iState[EXTRUDERS] = { 0 };
  static float temp_dState[EXTRUDERS] = { 0 };
  static float pTerm[EXTRUDERS];
  static float iTerm[EXTRUDERS];
  static float dTerm[EXTRUDERS];
  //int output;
  static float pid_error[EXTRUDERS];
  static float temp_iState_min[EXTRUDERS];
  static float temp_iState_max[EXTRUDERS];
  // static float pid_input[EXTRUDERS];
  // static float pid_output[EXTRUDERS];
  static bool pid_reset[EXTRUDERS];
#endif //PIDTEMP

static unsigned char soft_pwm[EXTRUDERS];

# define ARRAY_BY_EXTRUDERS(v1, v2, v3) { v1 }

// Init min and max temp with extreme values to prevent false errors during startup
static int minttemp_raw[EXTRUDERS] = ARRAY_BY_EXTRUDERS( HEATER_0_RAW_LO_TEMP , HEATER_1_RAW_LO_TEMP , HEATER_2_RAW_LO_TEMP );
static int maxttemp_raw[EXTRUDERS] = ARRAY_BY_EXTRUDERS( HEATER_0_RAW_HI_TEMP , HEATER_1_RAW_HI_TEMP , HEATER_2_RAW_HI_TEMP );
static int minttemp[EXTRUDERS] = ARRAY_BY_EXTRUDERS( 0, 0, 0 );
static int maxttemp[EXTRUDERS] = ARRAY_BY_EXTRUDERS( 16383, 16383, 16383 );

static void *heater_ttbl_map[EXTRUDERS] = ARRAY_BY_EXTRUDERS( (void *)HEATER_0_TEMPTABLE, (void *)HEATER_1_TEMPTABLE, (void *)HEATER_2_TEMPTABLE );
static uint8_t heater_ttbllen_map[EXTRUDERS] = ARRAY_BY_EXTRUDERS( HEATER_0_TEMPTABLE_LEN, HEATER_1_TEMPTABLE_LEN, HEATER_2_TEMPTABLE_LEN );

static float analog2temp(int raw, uint8_t e);
static void updateTemperaturesFromRawValues();

#ifndef SOFT_PWM_SCALE
#define SOFT_PWM_SCALE 0
#endif

//===========================================================================
//=============================   functions      ============================
//===========================================================================
//static void handler(int sig, siginfo_t *si, void *uc);
//static void * handler(void * arg);

float constrain(float x, float a, float b)
{
  if (x >= a && x <= b) return x;
  else if (x < a) return a;
  else return b;
}   

void updatePID()
{
#ifdef PIDTEMP
	int e;
  for(e = 0; e < EXTRUDERS; e++) { 
     temp_iState_max[e] = PID_INTEGRAL_DRIVE_MAX / Ki;  
  }
#endif
}
  
int getHeaterPower(int heater) {
  return soft_pwm[heater];
}

void manage_heater()
{
  float pid_input;
  float pid_output;

  if(temp_meas_ready != true)   //better readability
    return; 

  updateTemperaturesFromRawValues();

	int e;
  for(e = 0; e < EXTRUDERS; e++) 
  {

  #ifdef PIDTEMP
    pid_input = current_temperature[e];

    #ifndef PID_OPENLOOP
        pid_error[e] = target_temperature[e] - pid_input;
        if(pid_error[e] > PID_FUNCTIONAL_RANGE) {
          pid_output = BANG_MAX;
          pid_reset[e] = true;
        }
        else if(pid_error[e] < -PID_FUNCTIONAL_RANGE || target_temperature[e] == 0) {
          pid_output = 0;
          pid_reset[e] = true;
        }
        else {
          if(pid_reset[e] == true) {
            temp_iState[e] = 0.0;
            pid_reset[e] = false;
          }
          pTerm[e] = Kp * pid_error[e];
          temp_iState[e] += pid_error[e];
          temp_iState[e] = constrain(temp_iState[e], temp_iState_min[e], temp_iState_max[e]);
          iTerm[e] = Ki * temp_iState[e];

          //K1 defined in Configuration.h in the PID settings
          #define K2 (1.0-K1)
          dTerm[e] = (Kd * (pid_input - temp_dState[e]))*K2 + (K1 * dTerm[e]);
          pid_output = constrain(pTerm[e] + iTerm[e] - dTerm[e], 0, PID_MAX);
        }
        temp_dState[e] = pid_input;
    #else 
          pid_output = constrain(target_temperature[e], 0, PID_MAX);
    #endif //PID_OPENLOOP
    #ifdef PID_DEBUG
    ECHO_STRING(" PIDDEBUG ");
    ECHO_DECIMAL(e);
    ECHO_STRING(": Input ");
    ECHO_FLOAT(pid_input);
    ECHO_STRING(" Output ");
    ECHO_FLOAT(pid_output);
    ECHO_STRING(" pTerm ");
    ECHO_FLOAT(pTerm[e]);
    ECHO_STRING(" iTerm ");
    ECHO_FLOAT(iTerm[e]);
    ECHO_STRING(" dTerm ");
    ECHO_FLOAT(dTerm[e]);  
    #endif //PID_DEBUG
  #else /* PID off */
    pid_output = 0;
    if(current_temperature[e] < target_temperature[e]) {
      pid_output = PID_MAX;
    }
  #endif

    //DEBUG_PRINT("current_temperature is %d, minttemp is %d, maxttemp is %d\n",
     //   current_temperature[e], minttemp[e], maxttemp[0]);
    // Check if temperature is within the correct range

    /* --RW-- Commented out the following min temp check. */
    //    if((current_temperature[e] > minttemp[e]) && (current_temperature[e] < maxttemp[e])) 
    if(current_temperature[e] < maxttemp[e])
    {
      soft_pwm[e] = (int)pid_output >> 1;
    }
    else {
      soft_pwm[e] = 0;
    }
  } // End extruder for loop
}

// Derived from RepRap FiveD extruder::getTemperature()
// For hot end temperature measurement.
static float analog2temp(int raw, uint8_t e) {
  if(e >= EXTRUDERS)
  {
      ECHO_DECIMAL((int)e);
      ECHO_STRING(" - Invalid extruder number !");
      ikill();
  } 

  if(heater_ttbl_map[e] != NULL)
  {
    float celsius = 0;
    uint8_t i;
    short (*tt)[][2] = (short (*)[][2])(heater_ttbl_map[e]);

    for (i=1; i<heater_ttbllen_map[e]; i++)
    {
      if ((*tt)[i][0] > raw)
      {
        celsius = (*tt)[i-1][1] + 
          (raw - (*tt)[i-1][0]) * 
          (float)((*tt)[i][1] - (*tt)[i-1][1]) /
          (float)((*tt)[i][0] - (*tt)[i-1][0]);
        break;
      }
    }

    // Overflow: Set to last value in the table
    if (i == heater_ttbllen_map[e]) celsius = (*tt)[i-1][1];

    return celsius;
  }
  return ((raw * ((5.0 * 100.0) / 1024.0) / OVERSAMPLENR) * TEMP_SENSOR_AD595_GAIN) + TEMP_SENSOR_AD595_OFFSET;
}

/* Called to get the raw values into the the actual temperatures. The raw values are created in interrupt context,
    and this function is called from normal context as it is too slow to run in interrupts and will block the stepper routine otherwise */
static void updateTemperaturesFromRawValues()
{
		uint8_t e;
    for(e=0;e<EXTRUDERS;e++)
    {
        current_temperature[e] = analog2temp(current_temperature_raw[e], e);
    }

    //--TOM--: i don't see why temp_meas_ready needs to be protected
    //CRITICAL_SECTION_START;
    temp_meas_ready = false;
    //CRITICAL_SECTION_END;
}

void tp_init()
{
  int e;
  // Finish init of mult extruder arrays 
  for(e = 0; e < EXTRUDERS; e++) {
    // populate with the first value 
    maxttemp[e] = maxttemp[0];
#ifdef PIDTEMP
    temp_iState_min[e] = 0.0;
    temp_iState_max[e] = PID_INTEGRAL_DRIVE_MAX / Ki;
#endif //PIDTEMP
  }

  //heater disabled after initialization
  SET_OUTPUT(HEATER_0_PIN);
  disable_heater();

  // --TOM--: we are reading temperature using I2C-ADC instead of analog pin.
  // Legitimately, i2c should be initialized here. But it is actually initialized 
  // in setup() because we want to cluster all board specific initializations
  
  /* establish handler for timer signal */
  //timerid = create_timer(handler);

  /* start the periodic timer */
  //set_time(timerid, 1, 1000000);
  //enable_timer(timerid);
  //if (pthread_create(&temp_thread, NULL, &handler, NULL)) {
  //  errExit("pthread_create");
  //}

  // Wait for temperature measurement to settle
  delay(250);

#ifdef HEATER_0_MINTEMP
  minttemp[0] = HEATER_0_MINTEMP;
  while(analog2temp(minttemp_raw[0], 0) < HEATER_0_MINTEMP) {
#if HEATER_0_RAW_LO_TEMP < HEATER_0_RAW_HI_TEMP
    minttemp_raw[0] += OVERSAMPLENR;
#else
    minttemp_raw[0] -= OVERSAMPLENR;
#endif
  }
#endif //MINTEMP
#ifdef HEATER_0_MAXTEMP
  maxttemp[0] = HEATER_0_MAXTEMP;
  while(analog2temp(maxttemp_raw[0], 0) > HEATER_0_MAXTEMP) {
#if HEATER_0_RAW_LO_TEMP < HEATER_0_RAW_HI_TEMP
    maxttemp_raw[0] -= OVERSAMPLENR;
#else
    maxttemp_raw[0] += OVERSAMPLENR;
#endif
  }
#endif //MAXTEMP
}

void setWatch() 
{  
#ifdef WATCH_TEMP_PERIOD
  for (int e = 0; e < EXTRUDERS; e++)
  {
    if(degHotend(e) < degTargetHotend(e) - (WATCH_TEMP_INCREASE * 2))
    {
      watch_start_temp[e] = degHotend(e);
      watchmillis[e] = millis();
    } 
  }
#endif 
}

void write_heater(int val)
{
  //DEBUG_PRINT("write_heater: %d\n", val);
  WRITE(HEATER_0_PIN, val);
}

void disable_heater()
{
	int i;

  DEBUG_PRINT("disabling heater...\n");
  for(i=0;i<EXTRUDERS;i++)
    setTargetHotend(0,i);
  soft_pwm[0]=0;
  write_heater(0);
}

void do_manage_heater()
{
  static unsigned char pwm_count = (1 << SOFT_PWM_SCALE);
  static unsigned char soft_pwm_0;
  
  if(pwm_count == 0){
    soft_pwm_0 = soft_pwm[0];
    if(soft_pwm_0 > 0) 
      write_heater(1);
    else
      write_heater(0);
  }
  if(soft_pwm_0 < pwm_count) {
    //DEBUG_PRINT("soft_pwm[0] is %d, soft_pwm_0 is %d, pwm_count is %d\n",
        //soft_pwm[0], soft_pwm_0, pwm_count);
    write_heater(0);
  }
  
  pwm_count += (1 << SOFT_PWM_SCALE);
  pwm_count &= 0x7f;
}

void max_temp_error(uint8_t e) {
  disable_heater();
  if(IsStopped() == false) {
    fprintf(stderr, "Extruder %d switched off. MAXTEMP triggered !\n", e);
  }
  #ifndef BOGUS_TEMPERATURE_FAILSAFE_OVERRIDE
  Stop();
  #endif
}

void min_temp_error(uint8_t e) {
  disable_heater();
  if(IsStopped() == false) {
    fprintf(stderr, "Extruder %d switched off. MINTEMP triggered !\n", e);
  }
  #ifndef BOGUS_TEMPERATURE_FAILSAFE_OVERRIDE
  Stop();
  #endif
}

// Timer 0 is shared with millis
//ISR(TIMER0_COMPB_vect)
//static void
//handler(int sig, siginfo_t *si, void *uc)
//static void * handler(void * arg)
const struct timespec t = {.tv_sec = 0, .tv_nsec = 1000000};
const unsigned msec = 1000;

void loop(2, 20, 100)
{
  //these variables are only accesible from the ISR, but static, so they don't lose their value
  static unsigned char temp_count = 0;
  static unsigned long raw_temp_0_value = 0;
  static unsigned char temp_state = 0;
  uint16_t temp;
  //nanosleep args

  //while (1) {
    //DEBUG_PRINT("temperature\n");

    do_manage_heater();

    //--TOM-- modified based on Marlin firmware
    //read temperature from TEMP_0_PIN every 8 interrupts
    if (++temp_state % 8 == 0) {
      temp = ads7828_read_temp();
      DEBUG_PRINT("read word: %u\n", temp);
      raw_temp_0_value += temp >> 2;
      temp_state = 0;
      temp_count++;
    }
    
    if(temp_count >= 16) { // 8 ms * 16 = 128ms.
      //Only update the raw values if they have been read. Else we could be updating them during reading.
      if (!temp_meas_ready) {
        current_temperature_raw[0] = raw_temp_0_value;
      }
      //XXX
      current_temperature_raw[0] = 3529;
      
      temp_meas_ready = true;
      temp_count = 0;
      raw_temp_0_value = 0;

  #if HEATER_0_RAW_LO_TEMP > HEATER_0_RAW_HI_TEMP
      if(current_temperature_raw[0] <= maxttemp_raw[0]) {
  #else
      if(current_temperature_raw[0] >= maxttemp_raw[0]) {
  #endif
          DEBUG_PRINT("max_temp_error: current_temperature_raw is %d, maxttemp_raw is %d, mintemp_raw is %d\n",
              current_temperature_raw[0], maxttemp_raw[0], minttemp_raw[0]);
          max_temp_error(0);
      }
  //--TOM--: disable min_temp checking
  #if 0
  #if HEATER_0_RAW_LO_TEMP > HEATER_0_RAW_HI_TEMP
      if(current_temperature_raw[0] >= minttemp_raw[0]) {
  #else
      if(current_temperature_raw[0] <= minttemp_raw[0]) {
  #endif
          DEBUG_PRINT("min_temp_error: current_temperature_raw is %d, maxttemp_raw is %d, mintemp_raw is %d\n",
              current_temperature_raw[0], maxttemp_raw[0], minttemp_raw[0]);
          min_temp_error(0);
      }
    #endif
    }
    //nanosleep(&t, NULL);
    usleep(msec);
  //}
}

#ifdef PIDTEMP
// Apply the scale factors to the PID values


float scalePID_i(float i)
{
	return i*PID_dT;
}

float unscalePID_i(float i)
{
	return i/PID_dT;
}

float scalePID_d(float d)
{
    return d/PID_dT;
}

float unscalePID_d(float d)
{
	return d*PID_dT;
}

#endif //PIDTEMP


/* vi: set et sw=2 sts=2: */
