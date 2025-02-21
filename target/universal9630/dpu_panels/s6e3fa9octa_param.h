/* Copyright (c) 2019 Samsung Electronics Co, Ltd.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 * Copyright@ Samsung Electronics Co. LTD
 * Manseok Kim <manseoks.kim@samsung.com>

 * Alternatively, Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __S6E3FA9OCTA_PARAM_H__
#define __S6E3FA9OCTA_PARAM_H__

#define S6E3FA9OCTA_ID_REG			0x04
#define S6E3FA9OCTA_RD_LEN			3
#define S6E3FA9OCTA_RDDPM_ADDR		0x0A
#define S6E3FA9OCTA_RDDSM_ADDR		0x0E

static const unsigned char SEQ_TEST_KEY_ON_F0[] = {
	0xf0, 0x5a, 0x5a
};

static const unsigned char SEQ_TEST_KEY_OFF_F0[] = {
	0xf0, 0xa5, 0xa5
};

static const unsigned char SEQ_TEST_KEY_ON_FC[] = {
	0xfc, 0x5a, 0x5a
};

static const unsigned char SEQ_TEST_KEY_OFF_FC[] = {
	0xfc, 0xa5, 0xa5
};

static const unsigned char SEQ_SLEEP_OUT[] = {
	0x11
};

static const unsigned char SEQ_SLEEP_IN[] = {
	0x10
};

static const unsigned char SEQ_HBM_OFF[] = {
	0x53,
	0x20
};

static const unsigned char SEQ_BRIGHTNESS[] = {
	0x51,
	0x01, 0xBD
};

static const unsigned char SEQ_ACL_OFF[] = {
	0x55,
	0x00
};

static const unsigned char SEQ_PAGE_ADDR_SET_2A[] = {
	0x2A,
	0x00, 0x00, 0x04, 0x37
};

static const unsigned char SEQ_PAGE_ADDR_SET_2B[] = {
	0x2B,
	0x00, 0x00, 0x09, 0x5F
};

static const unsigned char SEQ_TSP_VSYNC_ON[] = {
	0xB9,
	0x00, 0x00, 0x14, 0x18, 0x00, 0x00, 0x00, 0x10
};

static const unsigned char SEQ_ELVSS_SET[] = {
	0xB5,
	0x19, 0x8D, 0x16		/* ELVSS Return */
};

static const unsigned char SEQ_TE_ON[] = {
	0x35,
	0x00, 0x00
};

static const unsigned char SEQ_DISPLAY_ON[] = {
	0x29
};

static const unsigned char SEQ_DISPLAY_OFF[] = {
	0x28
};
#endif /* __S6E3FA9OCTA_PARAM_H__ */
