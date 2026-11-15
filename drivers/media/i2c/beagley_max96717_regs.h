/* SPDX-License-Identifier: GPL-2.0 */
/* Register sequences exported from beagle_ai_camera_registers.csv. */

static const struct reg_sequence max96717_init_regs[] = {
	{ .reg = 0x0001, .def = 0x08 },
	{ .reg = 0x0383, .def = 0x80 }, /* MIPI_RX_EXT11: tunnel mode bit7=1 */
	{ .reg = 0x0331, .def = 0x33 },
	{ .reg = 0x0570, .def = 0x10 },
	{ .reg = 0x03F1, .def = 0x09 },
	{ .reg = 0x0332, .def = 0xE0 },
	{ .reg = 0x0333, .def = 0xE4 },
	{ .reg = 0x0308, .def = 0x64 }, /* FRONTOP0: START_PORT bit5 */
	{ .reg = 0x0011, .def = 0x43 },
	{ .reg = 0x03F0, .def = 0x59 },
	{ .reg = 0x0003, .def = 0x03 },
	{ .reg = 0x0006, .def = 0xB1 },
	{ .reg = 0x0002, .def = 0x03 },
	{ .reg = 0x02BE, .def = 0x98 }, /* MFP0 output high = sensor reset released */
	{ .reg = 0x02D3, .def = 0x81 }, /* MFP7 GPIO input (PMIC PG) */
	{ .reg = 0x02D4, .def = 0x07 }, /* MFP7 TX GPIO id 0x07 */
	{ .reg = 0x02D6, .def = 0x83 }, /* MFP8 = ERRB */
	{ .reg = 0x02C7, .def = 0x84 }, /* MFP3 PWM TX */
	{ .reg = 0x02C8, .def = 0xB0 },
	{ .reg = 0x02C9, .def = 0x48 }, /* RX GPIO id 8 */
};

static const struct reg_sequence max96717_ssc_regs[] = {
	{ .reg = 0x1471, .def = 0x04 },
	{ .reg = 0x1470, .def = 0x06 },
	{ .reg = 0x1472, .def = 0xAB },
	{ .reg = 0x1473, .def = 0x00 },
	{ .reg = 0x1474, .def = 0x63 },
	{ .reg = 0x1475, .def = 0x07 },
	{ .reg = 0x1476, .def = 0x00 },
	{ .reg = 0x1464, .def = 0x03 },
	{ .reg = 0x1471, .def = 0x05 },
};
