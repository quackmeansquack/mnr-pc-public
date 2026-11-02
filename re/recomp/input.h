/* Wheels, pedals and other game controllers via the Windows joystick API, mapped by MNR_INPUT_CONFIG (input.c). */
#pragma once

typedef struct {
    unsigned d1, d2;      /* PS3 digital button bits to OR in (CellPadData button[2], button[3]) */
    int lx, ly, rx, ry;   /* sticks 0..255, or -1 when not bound */
    int l2, r2;           /* analog L2/R2 pressure 0..255, or -1 */
} input_state_t;

int input_read(input_state_t *out);   /* 1 when a bound controller is connected */
int input_bound_present(void);   /* a bound controller other than an Xbox one is connected (it is player 1) */
