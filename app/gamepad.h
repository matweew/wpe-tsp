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
/* Buttons 9/10 are reported on the Smart Pro but its sticks don't click. Brick: 9/10 are its F1/F2
 * keys (no sticks). Brick Pro: 9/10 are L3/R3, 11/12 F1/F2, 15 HOME (the firmware's overlay). */
#define BTN_9      9
#define BTN_10     10
#define BTN_11     11
#define BTN_12     12

#define AXIS_LX 0
#define AXIS_LY 1
#define AXIS_L2 2
#define AXIS_RX 3
#define AXIS_RY 4
#define AXIS_R2 5
#define NUM_AXES 6

/* Trigger axes count as "pressed" above this value. */
#define TRIGGER_THRESHOLD 16000
