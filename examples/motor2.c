//////////////////////////////////////////////////////////////////
// motor.c
// david austin
// http://www.embedded.com/design/mcus-processors-and-socs/4006438/Generate-stepper-motor-speed-profiles-in-real-time
// DECEMBER 30, 2004

// Demo program for stepper motor control with linear ramps
// Hardware: PIC18F252, L6219

//////////////////////////////////////////////////////////////////
// Note:
// changes to original source code to permit desktop compile, test
// and debug while remaining compatible with pic compiler.  These
// changes to the original code are marked with RR below

// Robert Ramey, 2015

// note changes to original source code
//
// signed int16 <- signed_int16 note '-' added
// commented out the #byte and #bit statements
// commented out the #INT_CCP1

// ***********
// RR factor out motor.c so motor code can be separated from tests
// and other user code.

// a) changed step_no and move to signed 16 bit integer types
// b) replaced bit shifts by multiplications.  bit shifts are not
//    guaranteed portable.  Decent compilers should implements
//    const multiplications by powers of 2 as bit shifts anyway
// c) changed c32 to 32 bit signed integer type
// d) changed phase_inc to 8 bit signed type
// e) factored phase index into phase_bump in order to have
//    standard conforming code

#include "motor.h"

#if !defined(DESKTOP)
    #include "picsfr.h"
    #define mod16 int16
    // ramp state-machine states
    #define ramp_idle 0
    #define ramp_up   1
    #define ramp_max  2
    #define ramp_down 3
    #define ramp_last 4
    #define ramp_state_t int8
#else
    // define a memory map to emulate the on in the pic.  This permits all
    // same pic code to work on the desktop with no alteration
    std::uint8_t base[0xfff];
    // now we can render
    //#bit  TMR1ON  = T1CON.0 as
    bit<std::uint8_t, 0> TMR1ON(T1CON);
    // and use expressions such as TMR1ON = 0
    enum ramp_state_t {
      ramp_idle,
      ramp_up,
      ramp_max,
      ramp_down,
      ramp_last
    };
#endif

// 1st step=50ms; max speed=120rpm (based on 1MHz timer, 1.8deg steps)
#define C0    50000
#define C_MIN  2500

// Types: int8,int16,int32=8,16,32bit integers, unsigned by default
ramp_state_t ramp_sts = ramp_idle;
signed_int16 motor_pos = 0; // absolute step number
signed_int16 pos_inc=0;     // motor_pos increment
int16 phase=0;              // ccpPhase[phase_ix]
int8  phase_ix=0;           // index to ccpPhase[]
signed_int8  phase_inc;     // phase_ix increment
int8  run_flg;              // true while motor is running
mod16 ccpr;                 // copy of CCPR1&2
mod16 c;                    // integer delay count
signed_int16 step_no;       // progress of move
int16 step_down;            // start of down-ramp
signed_int16 move;          // total steps to move
int16 midpt;                // midpoint of move
signed_int32 c32;           // 24.8 fixed point delay count
signed_int16 denom;         // 4.n+1 in ramp algo

// Config data to make CCP1&2 generate quadrature sequence on PHASE pins
// Action on CCP match: 8=set+irq; 9=clear+irq
int16 const ccpPhase[] = {0x909, 0x908, 0x808, 0x809}; // 00,01,11,10

void current_on(){/* code as needed */}  // motor drive current
void current_off(){/* code as needed */} // reduce to holding value

void phase_bump()
{
    if(phase_inc > 0)
        if(phase_ix == 3)
            phase_ix = 0;
        else
            ++phase_ix;
    else
    // phase_inc < 0
        if(phase_ix == 0)
            phase_ix = 3;
        else
            --phase_ix;
    phase = ccpPhase[phase_ix];
    CCP1CON = phase & 0xff; // set CCP action on next match
    CCP2CON = phase >> 8;
}
// compiler-specific ISR declaration
// #INT_CCP1
void isr_motor_step() 
{ // CCP1 match -> step pulse + IRQ
  ccpr += c; // next comparator value: add step delay count
  switch (ramp_sts)
  {
  case ramp_up:   // accel
    if (step_no==midpt)
    { // midpoint: decel
      ramp_sts = ramp_down;
      denom = ((step_no - move) * 4) + 1;
      if (!(move & 1))
      { // even move: repeat last delay before decel
        denom +=4;
        break;
      }
    }
    // no break: share code for ramp algo
  case ramp_down: // decel
    if (step_no == move-1)
    { // next irq is cleanup (no step)
      ramp_sts = ramp_last;
      break;
    }
    denom+=4;
    c32 -= (c32 * 2) / denom; // ramp algorithm
    // beware confict with foreground code if long div not reentrant
    c = (c32 + 128) / 256; // round 24.8format->int16
    if (c <= C_MIN)
    { // go to constant speed
      ramp_sts = ramp_max;
      step_down = move - step_no;
      c = C_MIN;
      break;
    }
    break;
  case ramp_max: // constant speed
    if (step_no == step_down)
    { // start decel
      ramp_sts = ramp_down;
      denom = ((step_no - move) * 4) + 5;
    }
    break;
  default: // last step: cleanup
    ramp_sts = ramp_idle;
    current_off(); // reduce motor current to holding value
    disable_interrupts(INT_CCP1);
    run_flg = FALSE; // move complete
    break;
  } // switch (ramp_sts)
  if (ramp_sts!=ramp_idle)
  {
    motor_pos += pos_inc;
    ++step_no;
    CCPR2H = CCPR1H = (ccpr >> 8); // timer value at next CCP match
    CCPR2L = CCPR1L = (ccpr & 0xff);

    if (ramp_sts!=ramp_last) // else repeat last action: no step
      phase_bump();
  } // if (ramp_sts != ramp_idle)
} // isr_motor_step()

void motor_run(int16 pos_new)
{ // set up to drive motor to pos_new (absolute step#)
  if (pos_new < motor_pos) // get direction & #steps
  {
    move = motor_pos-pos_new;
    pos_inc   = -1;
    phase_inc = -1;
  } 
  else if (pos_new != motor_pos)
  { 
    move = pos_new-motor_pos;
    pos_inc   = 1;
    phase_inc = 1;
  }
  else return; // already there
  midpt = (move-1)>>1;
  c   = C0;
  c32 = c * 256; // keep c in 24.8 fixed-point format for ramp calcs
  step_no  = 0; // step counter
  denom    = 1; // 4.n+1, n=0
  ramp_sts = ramp_up; // start ramp state-machine
  run_flg  = TRUE;
  TMR1ON   = 0; // stop timer1;
  ccpr = make16(TMR1H,TMR1L);  // 16bit value of Timer1
  ccpr += 1000; // 1st step + irq 1ms after timer1 restart
  CCPR2H = CCPR1H = (ccpr >> 8);
  CCPR2L = CCPR1L = (ccpr & 0xff);
  phase_bump();
  current_on(); // current in motor windings
  enable_interrupts(INT_CCP1); 
  TMR1ON=1; // restart timer1;
} // motor_run()

void initialize()
{
  disable_interrupts(GLOBAL);
  disable_interrupts(INT_CCP1);
  disable_interrupts(INT_CCP2);
  output_c(0);
  set_tris_c(0);
  T3CON = 0;
  T1CON = 0x35;
  enable_interrupts(GLOBAL);
} // initialize()

