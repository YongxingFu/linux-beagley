.. SPDX-License-Identifier: GPL-2.0

BeagleY-AI OX03H10 legacy GMSL topology
=====================================

The camera path uses a fixed, one-input/one-output topology. It is implemented
with the existing MAX96717 V4L2 sub-device driver
(``CONFIG_VIDEO_MAX96717``) and the MAX96716A driver
(``CONFIG_VIDEO_MAX96716A``). It does not use a shared SerDes routing core,
an I2C address translator, or media-core internal pads.

Expected graph
--------------

::

  OX03H10 source
      -> MAX96717 CSI-2 sink/source
      -> MAX96716A GMSL sink/CSI-2 source
      -> AM67A CSI0 sink

The boot-time control path is:

::

  local CSI0 I2C
      -> MAX96716A i2c-gate
          -> MAX96717 i2c-gate
              -> OX03H10

Build selection
---------------

Select the legacy path in the kernel configuration fragment::

  CONFIG_VIDEO_MAX96716A=m
  CONFIG_VIDEO_MAX96717=m
  CONFIG_VIDEO_OX03H10=m

Sensor register profile and mode description
--------------------------------------------

The supplied ``beagle_ai_camera_registers.csv`` contains 1,621 ordered
OX03H10 initialisation writes, four IO-pad writes and stream on/off writes.
They are preserved in ``drivers/media/i2c/beagley_ox03h10_regs.h`` and consumed
by ``drivers/media/i2c/ox03h10.c``. The OX03H10 profile is applied at each
stream start; the separate IO-pad and stream-control stages follow it.

The fixed V4L2 mode is 1920x1536, BGGR RAW12, 60 fps, four data lanes
and a 750 MHz CSI-2 link frequency (6 Gbit/s aggregate lane rate). The
endpoint must declare ``link-frequencies = /bits/ 64 <750000000>``.

The driver checks chip-ID bytes ``0x300a..0x300c`` for ``58:03:48`` and logs
version/split bytes from ``0x302a`` and ``0x302b``. Sensor rails and reset are
board-managed; the driver only enables the serializer-provided ``xvclk`` while
active. Exposure and gain controls are not exposed because their register
semantics are not documented in the CSV.

One captured timing value needs verification before hardware streaming:
the CSV labels ``0x380e/0x380f = 0x032e`` as VTS 814 lines, which is less than
the declared 1536-line output. The driver preserves that register table value;
confirm the capture/profile and timing interpretation against the actual
sensor configuration before treating it as a validated 60 fps mode.

The board's 7-bit I2C addresses are:

* MAX96716A: ``0x28`` on the local control bus.
* MAX96717: ``0x40`` below the MAX96716A I2C gate.
* OX03H10: ``0x36`` below the MAX96717 I2C gate.

The drivers do not hardcode these addresses. The optional
``k3-am67a-beagley-ai-ox03h10-gmsl.dtso`` overlay places the remote nodes under
the nested I2C gates, links the sensor, serializer, deserializer and CSI0
endpoints, and enables CSI0. Its lane order still needs confirmation against
the camera HAT wiring. Sensor power rails and reset remain board-managed.

Serializer configuration insertion points
-----------------------------------------

Device-specific MAX96717 data belongs in ``drivers/media/i2c/max96717.c``.

* The power-up, PLL, CSI-2 receiver, lane mapping, polarity and SSC sequence
  from the supplied CSV are applied in ``max96717_hw_init()``.
* If the module uses pixel mode, extend the mode handling around
  ``max96717_s_ctrl()`` and ``max96717_hw_init()``. The baseline driver
  currently accepts tunnel mode only.
* Keep the verified transmit start and stop ordering in
  ``max96717_enable_streams()`` and ``max96717_disable_streams()``.
* Board DSL, port selection and lane-map values are parsed and programmed in
  ``max96717_parse_dt()`` and ``max96717_init_csi_lanes()``.
* ``max96717_get_frame_desc()`` forwards the Sensor CSI-2 descriptor. Do not
  add the OX03H10 register table here.

Deserializer configuration insertion points
--------------------------------------------

Device-specific MAX96716A data belongs in
``drivers/media/i2c/max96716a.c``.

* The power-up, link mode, GMSL pipe, CSI-2 output PHY, DPLL, lane-map, SSC
  and AEQ sequence from the supplied CSV are applied in
  ``max96716a_hw_init()``. The table does not include a deserializer chip-ID
  read or a status/lock check.
* The V4L2 stream callbacks use the CSV's ``0x0313`` stream-control values.
* Output endpoint lane order, polarity and link frequency are parsed in
  ``max96716a_parse_output_endpoint()``. They are also checked by the generic
  V4L2 link validation.
* Keep the one-to-one stream-zero route. Adding multiple links or dynamic
  routes requires a separate design and is outside this legacy driver.

Device-tree insertion points
----------------------------

Use an optional CSI0 overlay rather than modifying the common SoC DTSI. The
board nodes and labels already used by BeagleY-AI are:

* ``main_i2c2`` / ``csi0_i2c`` for local camera control;
* ``cdns_csi2rx0`` / ``csi0_port0`` for the CSI-2 receiver;
* ``dphy0`` and ``ti_csi2rx0`` for the active CSI0 PHY and capture path.

The nesting must express the physical control path. The CSV fixes the I2C
addresses and four-lane count. The local I2C controller and CSI0 routing must
match the hardware:

::

  &main_i2c2 {
  deserializer: gmsl-deserializer@28 {
          compatible = "maxim,max96716a";
          reg = <0x28>;

          ports {
              #address-cells = <1>;
              #size-cells = <0>;

              port@0 {
                  reg = <0>;
                  deserializer_in: endpoint {
                      remote-endpoint = <&serializer_out>;
                  };
              };

              port@1 {
                  reg = <1>;
                  deserializer_out: endpoint {
                      remote-endpoint = <&csi0_in>;
                      data-lanes = <1 2 3 4>;
                      link-frequencies = /bits/ 64 <750000000>;
                  };
              };
          };

          i2c-gate {
              #address-cells = <1>;
              #size-cells = <0>;

              serializer: gmsl-serializer@40 {
                  compatible = "maxim,max96717";
                  reg = <0x40>;
                  gpio-controller;
                  #gpio-cells = <2>;
                  #clock-cells = <0>;

                  ports {
                      #address-cells = <1>;
                      #size-cells = <0>;

                      port@0 {
                          reg = <0>;
                          serializer_in: endpoint {
                              remote-endpoint = <&sensor_out>;
                              data-lanes = <1 2 3 4>;
                          };
                      };

                      port@1 {
                          reg = <1>;
                          serializer_out: endpoint {
                              remote-endpoint = <&deserializer_in>;
                          };
                      };
                  };

                  i2c-gate {
                      #address-cells = <1>;
                      #size-cells = <0>;

                      sensor: camera@36 {
                          compatible = "ovti,ox03h10";
                          reg = <0x36>;
                          clocks = <&serializer>;
                          clock-names = "xvclk";

                          port {
                              sensor_out: endpoint {
                                  remote-endpoint = <&serializer_in>;
                                  data-lanes = <1 2 3 4>;
                                  link-frequencies = /bits/ 64 <750000000>;
                              };
                          };
                      };
                  };
              };
          };
      };
  };

  &cdns_csi2rx0 {
      ports {
          csi0_port0: port@0 {
              status = "okay";
              csi0_in: endpoint {
                  remote-endpoint = <&deserializer_out>;
                  bus-type = <4>;
                  clock-lanes = <0>;
                  data-lanes = <1 2 3 4>;
              };
          };
      };
  };

  &dphy0 {
      status = "okay";
  };

  &ti_csi2rx0 {
      status = "okay";
  };

The symbolic values above are deliberately not valid device-tree cells. This
prevents copying another board's addresses, power rails, lanes or clock
frequencies without checking the target module.
