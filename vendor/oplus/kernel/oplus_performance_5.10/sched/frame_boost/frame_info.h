/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */

#ifndef _OPLUS_FRAME_INFO_H
#define _OPLUS_FRAME_INFO_H

#define FRAME_START	(1 << 0)
#define FRAME_END	(1 << 1)

extern int frame_info_init(void);

extern void set_frame_state(unsigned int state);
extern int set_frame_util_min(int min_util, bool clear);

extern bool set_frame_rate(unsigned int frame_rate);
extern void set_max_frame_rate(unsigned int frame_rate);
extern bool is_high_frame_rate(void);

extern unsigned long get_frame_vutil(u64 delta);
extern unsigned long get_frame_putil(u64 delta, unsigned int frame_zone);

extern unsigned long frame_uclamp(unsigned long util);
#endif /* _OPLUS_FRAME_INFO_H */
