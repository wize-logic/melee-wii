/* The launcher's controls (input.c) */
#ifndef LAUNCHER_INPUT_H
#define LAUNCHER_INPUT_H

enum {
    IN_UP = 1 << 0,
    IN_DOWN = 1 << 1,
    IN_LEFT = 1 << 2,
    IN_RIGHT = 1 << 3,
    IN_A = 1 << 4,
    IN_B = 1 << 5,
    IN_X = 1 << 6,     /* GameCube X, Wii Remote + */
    IN_Z = 1 << 7,     /* GameCube Z, Wii Remote - */
    IN_START = 1 << 8,
    IN_HOME = 1 << 9,
    IN_Y = 1 << 10,    /* GameCube Y, Wii Classic Y (the launcher's Help) */
};

void input_init(void);
void input_shutdown(void);
/* The game starts (the Wii Remotes stay connected) */
void input_hand_over(void);
/* What was pressed since the last call (directions repeat); call once a frame */
unsigned int input_poll(void);
unsigned int input_held(void);
/* The Wii Remote's pointer (640x480), when one points at the screen */
int input_pointer(int* x, int* y);

#endif
