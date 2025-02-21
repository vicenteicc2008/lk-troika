/*
 * Copyright@ Samsung Electronics Co. LTD
 *
 * This software is proprietary of Samsung Electronics.
 * No part of this software, either material or conceptual may be copied or distributed, transmitted,
 * transcribed, stored in a retrieval system or translated into any human or computer language in any form by any means,
 * electronic, mechanical, manual or otherwise, or disclosed
 * to third parties without the express written permission of Samsung Electronics.
 */

#ifndef __BOARD_INFO_H__
#define __BOARD_INFO_H__

#define CONFIG_UNIVERSAL3830
#define CONFIG_BOARD_ID 0x0

#define CONFIG_GET_B_REV_FROM_ADC

#define VOLDOWN_GPIOCON EXYNOS3830_GPA1CON
#define VOLDOWN_BIT	0

#define CONFIG_USE_RPMB
#define USE_MMC0
#endif /*__BOARD_INFO_H__ */
