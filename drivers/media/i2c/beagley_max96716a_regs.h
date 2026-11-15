/* SPDX-License-Identifier: GPL-2.0 */
/* Register sequences exported from beagle_ai_camera_registers.csv. */

static const struct reg_sequence max96716a_init_regs[] = {
	{ .reg = 0x0001, .def = 0x02 }, /* RX_RATE[1:0]=2 */
	{ .reg = 0x0010, .def = 0x01 }, /* LINK_CTRL bit0=1 */
	{ .reg = 0x040A, .def = 0xC0 },
	{ .reg = 0x044A, .def = 0xC0 }, /* MIPI lane count (0xC0 = 4 lanes) */
	{ .reg = 0x0004, .def = 0x01 },
	{ .reg = 0x0F00, .def = 0x01 }, /* LINK_EN: bit0=LinkA, bit1=LinkB; 0x01=only A */
	{ .reg = 0x0161, .def = 0x32 },
	{ .reg = 0x0319, .def = 0x2C },
	{ .reg = 0x031D, .def = 0x2F },
	{ .reg = 0x0320, .def = 0x2C },
	{ .reg = 0x044B, .def = 0x07 },
	{ .reg = 0x046D, .def = 0x15 },
	{ .reg = 0x044D, .def = 0xAC },
	{ .reg = 0x044E, .def = 0x2C },
	{ .reg = 0x044F, .def = 0x80 },
	{ .reg = 0x0450, .def = 0x00 },
	{ .reg = 0x0451, .def = 0x81 },
	{ .reg = 0x0452, .def = 0x01 },
	{ .reg = 0x0332, .def = 0xF0 }, /* MIPI TX standby bits5:4 */
	{ .reg = 0x0474, .def = 0x09 }, /* tunnel enable bit0 */
	{ .reg = 0x02C8, .def = 0x43 },
	{ .reg = 0x02C9, .def = 0x88 },
	{ .reg = 0x02CA, .def = 0x40 },
};

static const struct reg_sequence max96716a_ssc_regs[] = {
	{ .reg = 0x1471, .def = 0x04 },
	{ .reg = 0x1447, .def = 0x02 },
	{ .reg = 0x1472, .def = 0xEF },
	{ .reg = 0x1473, .def = 0x00 },
	{ .reg = 0x1470, .def = 0x06 },
	{ .reg = 0x1464, .def = 0x03 },
	{ .reg = 0x1474, .def = 0x00 },
	{ .reg = 0x1475, .def = 0x00 },
	{ .reg = 0x1471, .def = 0x05 },
	{ .reg = 0x1571, .def = 0x04 },
	{ .reg = 0x1547, .def = 0x02 },
	{ .reg = 0x1572, .def = 0xEF },
	{ .reg = 0x1573, .def = 0x00 },
	{ .reg = 0x1570, .def = 0x06 },
	{ .reg = 0x1564, .def = 0x03 },
	{ .reg = 0x1574, .def = 0x00 },
	{ .reg = 0x1575, .def = 0x00 },
	{ .reg = 0x1571, .def = 0x05 },
};

static const struct reg_sequence max96716a_aeq_regs[] = {
	{ .reg = 0x1403, .def = 0x8A },
	{ .reg = 0x1503, .def = 0x8A },
};
