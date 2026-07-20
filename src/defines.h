#ifndef FWM_DEFINES_H
#define FWM_DEFINES_H

#define MAX_WINDOWS             32
#define DRAG_MARGIN             5
#define PHYSICS_MARGIN          3
#define MASS_DENSITY            0.0005
#define FRICTION                0.985
#define THROW_SPEED_MULTIPLIER  0.65
#define MAX_THROW_SPEED         1800.0
#define STOP_SPEED_THRESHOLD    1.0
#define RESTITUTION             0.3
#define PHYSICS_TICK_RATE       60.0
/* Approach speed (px/s) a collision must reach before it counts as an impact
 * worth reacting to. Above resting jitter, below a short drop. */
#define PHYSICS_HIT_MIN_SPEED   120.0
#define GRAVITY  981.0   // px/s² (earth ~9.8 m/s² at 100 px/m)

#endif /* FWM_DEFINES_H */