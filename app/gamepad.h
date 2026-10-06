/*
 * TrimUI Smart Pro gamepad ("TRIMUI Player1", reported by SDL as an Xbox 360 controller:
 * 11 buttons, 6 axes, 1 hat). Triggers L2/R2 are analog axes, not buttons.
 */
#pragma once

#define BTN_B      0
#define BTN_A      1
#define BTN_Y      2
#define BTN_X      3
#define BTN_L1     4
#define BTN_R1     5
#define BTN_SELECT 6
#define BTN_START  7
#define BTN_MENU   8
/* Buttons 9/10 are reported but the sticks don't click: no L3/R3 on this device. */

#define AXIS_LX 0
#define AXIS_LY 1
#define AXIS_L2 2
#define AXIS_RX 3
#define AXIS_RY 4
#define AXIS_R2 5
#define NUM_AXES 6

/* Trigger axes count as "pressed" above this value. */
#define TRIGGER_THRESHOLD 16000
