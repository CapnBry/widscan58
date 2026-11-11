/* Lap times said through the CYD's speaker (GPIO26, DAC channel 2, into
 * the board's amplifier). GPL-3.0-or-later. */
#pragma once
#include <stdint.h>

void audio_init(void);
/* Say the lap time to 0.1 s ("twenty three point nine"), at g_set.volume
 * (0: nothing). Replaces one still waiting; one being said finishes.
 * Never blocks: from any task, under model_lock() too. */
void audio_say_lap(uint32_t lap_ms);
