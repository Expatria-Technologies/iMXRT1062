/*
  ioports.c - driver code for IMXRT1062 processor (on Teensy 4.1 board)

  Part of grblHAL

  Board by Phil Barrett: https://github.com/phil-barrett/grblHAL-teensy-4.x

  Copyright (c) 2020-2022 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "driver.h"
#include "mcp3221.h"

#ifdef HAS_IOPORTS

//#include "Arduino.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "grbl/protocol.h"

static char *pnum = NULL;
static uint_fast8_t aux_n_in, aux_n_out, analog_n_in, *in_map = NULL, *out_map = NULL;;
static volatile uint32_t event_bits;
static volatile bool spin_lock = false;
static input_signal_t *aux_in;
static output_signal_t *aux_out;
static ioport_bus_t out = {0};
static char input_ports[50] = "", output_ports[50] = "";
#if MCP3221_ENABLE
static xbar_t analog_in;
static enumerate_pins_ptr on_enumerate_pins;
#endif

static void aux_settings_load (void);
static status_code_t aux_set_invert_out (setting_id_t id, uint_fast16_t int_value);
static uint32_t aux_get_invert_out (setting_id_t setting);
static bool is_setting_available (const setting_detail_t *setting);

static const setting_group_detail_t aux_groups[] = {
    { Group_Root, Group_AuxPorts, "Aux ports"}
};

static const setting_detail_t aux_settings[] = {
    { Settings_IoPort_InvertIn, Group_AuxPorts, "Invert I/O Port inputs", NULL, Format_Bitfield, input_ports, NULL, NULL, Setting_NonCore, &settings.ioport.invert_in.mask, NULL, is_setting_available },
    { Settings_IoPort_InvertOut, Group_AuxPorts, "Invert I/O Port outputs", NULL, Format_Bitfield, output_ports, NULL, NULL, Setting_NonCoreFn, aux_set_invert_out, aux_get_invert_out, is_setting_available },
};

#ifndef NO_SETTINGS_DESCRIPTIONS

static const setting_descr_t aux_settings_descr[] = {
    { Settings_IoPort_InvertIn, "Invert IOPort inputs." },
    { Settings_IoPort_InvertOut, "Invert IOPort output." },
};

#endif

static setting_details_t setting_details = {
    .groups = aux_groups,
    .n_groups = sizeof(aux_groups) / sizeof(setting_group_detail_t),
    .settings = aux_settings,
    .n_settings = sizeof(aux_settings) / sizeof(setting_detail_t),
#ifndef NO_SETTINGS_DESCRIPTIONS
    .descriptions = aux_settings_descr,
    .n_descriptions = sizeof(aux_settings_descr) / sizeof(setting_descr_t),
#endif
    .load = aux_settings_load,
    .save = settings_write_global
};

static bool is_setting_available (const setting_detail_t *setting)
{
    bool available = false;

    switch(setting->id) {

        case Settings_IoPort_InvertIn:
        case Settings_IoPort_Pullup_Disable:
            available = aux_n_in > 0;
            break;

        case Settings_IoPort_InvertOut:
        case Settings_IoPort_OD_Enable:
            available = aux_n_out > 0;
            break;

        default:
            break;
    }

    return available;
}

static void aux_settings_load (void)
{
    uint_fast8_t idx = aux_n_out;

    if(aux_n_out) do {
        idx--;
        pinModeOutput(aux_out[idx].port, aux_out[idx].pin);
        DIGITAL_OUT((*(aux_out[idx].port)), (settings.ioport.invert_out.mask >> idx) & 0x01);
    } while(idx);
}

static status_code_t aux_set_invert_out (setting_id_t id, uint_fast16_t value)
{
    ioport_bus_t invert;
    invert.mask = (uint8_t)value & out.mask;

    if(invert.mask != settings.ioport.invert_out.mask) {
        uint_fast8_t idx = aux_n_out;
        do {
            idx--;
            if(((settings.ioport.invert_out.mask >> idx) & 0x01) != ((invert.mask >> idx) & 0x01))
                DIGITAL_OUT((*(aux_out[idx].port)), !DIGITAL_IN((*(aux_out[idx].port))));
        } while(idx);

        settings.ioport.invert_out.mask = invert.mask;
    }

    return Status_OK;
}

static uint32_t aux_get_invert_out (setting_id_t setting)
{
    return settings.ioport.invert_out.mask;
}

static void digital_out (uint8_t port, bool on)
{
    if(port < aux_n_out) {
        if(out_map)
            port = out_map[port];
        DIGITAL_OUT((*(aux_out[port].port)), ((settings.ioport.invert_out.mask >> port) & 0x01) ? !on : on);
    }
}

inline static __attribute__((always_inline)) int32_t get_input (const input_signal_t *input, bool invert, wait_mode_t wait_mode, float timeout)
{
    if(wait_mode == WaitMode_Immediate)
        return DIGITAL_IN(input->gpio) ^ invert;

    int32_t value = -1;
    uint_fast16_t delay = (uint_fast16_t)ceilf((1000.0f / 50.0f) * timeout) + 1;

    if(wait_mode == WaitMode_Rise || wait_mode == WaitMode_Fall) {

        pin_irq_mode_t irq_mode = wait_mode == WaitMode_Rise ? IRQ_Mode_Rising : IRQ_Mode_Falling;

        if(input->cap.irq_mode & irq_mode) {

            event_bits &= ~input->gpio.bit;
            pinEnableIRQ(input, irq_mode);

            do {
                if(event_bits & input->gpio.bit) {
                    value = DIGITAL_IN(input->gpio) ^ invert;
                    break;
                }
                if(delay) {
                    protocol_execute_realtime();
                    hal.delay_ms(50, NULL);
                } else
                    break;
            } while(--delay && !sys.abort);

            pinEnableIRQ(input, IRQ_Mode_None);    // Restore pin interrupt status
        }

    } else {

        bool wait_for = wait_mode != WaitMode_Low;

        do {
            if((DIGITAL_IN(input->gpio) ^ invert) == wait_for) {
                value = DIGITAL_IN(input->gpio);
                break;
            }
            if(delay) {
                protocol_execute_realtime();
                hal.delay_ms(50, NULL);
            } else
                break;
        } while(--delay && !sys.abort);
    }

    return value;
}

inline static __attribute__((always_inline)) uint8_t in_map_rev (uint8_t port)
{
    if(in_map) {
        uint_fast8_t idx = aux_n_in;
        do {
            if(in_map[--idx] == port) {
                port = idx;
                break;
            }
        } while(idx);
    }

    return port;
}

inline static __attribute__((always_inline)) uint8_t out_map_rev (uint8_t port)
{
    if(out_map) {
        uint_fast8_t idx = aux_n_out;
        do {
            if(out_map[--idx] == port) {
                port = idx;
                break;
            }
        } while(idx);
    }

    return port;
}

void ioports_event (input_signal_t *input)
{
    spin_lock = true;
    event_bits |= input->gpio.bit;

    if(input->interrupt_callback)
        input->interrupt_callback(in_map_rev(input->id - Output_Aux0), !!(input->port->reg->DR & input->port->bit));

    spin_lock = false;
}

static int32_t wait_on_input (io_port_type_t type, uint8_t port, wait_mode_t wait_mode, float timeout)
{
    int32_t value = -1;

    if(type == Port_Digital) {
        if(port < aux_n_in) {
            if(in_map)
                port = in_map[port];
            value = get_input(&aux_in[port], (settings.ioport.invert_in.mask << port) & 0x01, wait_mode, timeout);
        }
    }
#if MCP3221_ENABLE
    else if(port < analog_n_in)
        value = (int32_t)MCP3221_read();
#endif

    return value;
}

static bool register_interrupt_handler (uint8_t port, pin_irq_mode_t irq_mode, ioport_interrupt_callback_ptr interrupt_callback)
{
    bool ok;

    if(in_map)
        port = in_map[port];

    if((ok = port < aux_n_in && aux_in[port].cap.irq_mode != IRQ_Mode_None)) {

        input_signal_t *input = &aux_in[port];

        if((ok = (irq_mode & aux_in[port].cap.irq_mode) == irq_mode && interrupt_callback != NULL)) {
            input->irq_mode = irq_mode;
            input->interrupt_callback = interrupt_callback;
            pinEnableIRQ(input, irq_mode);
        }

        if(irq_mode == IRQ_Mode_None || !ok) {
            while(spin_lock);
            pinEnableIRQ(input, IRQ_Mode_None);
            input->irq_mode = IRQ_Mode_None;
            input->interrupt_callback = NULL;
        }
    }

    return ok;
}

static xbar_t *get_pin_info (io_port_type_t type, io_port_direction_t dir, uint8_t port)
{
    static xbar_t pin;
    xbar_t *info = NULL;

    memset(&pin, 0, sizeof(xbar_t));

    if(type == Port_Digital) {

        memset(&pin, 0, sizeof(xbar_t));

        if(dir == Port_Input && port < aux_n_in) {
            if(in_map)
                port = in_map[port];
            pin.mode.input = On;
            pin.mode.irq_mode = aux_in[port].irq_mode;
            pin.mode.can_remap = !aux_in[port].cap.remapped;
            pin.cap = aux_in[port].cap;
            pin.function = aux_in[port].id;
            pin.group = aux_in[port].group;
            pin.pin = aux_in[port].pin;
            pin.port = (void *)aux_in[port].port;
            pin.description = aux_in[port].description;
            info = &pin;
        }

        if(dir == Port_Output && port < aux_n_out) {
            if(out_map)
                port = out_map[port];
            pin.mode = aux_out[port].mode;
            pin.mode.output = On;
            pin.function = aux_out[port].id;
            pin.group = aux_out[port].group;
            pin.pin = aux_out[port].pin;
            pin.bit = 1 << aux_out[port].pin;
            pin.port = (void *)aux_out[port].port;
            pin.description = aux_out[port].description;
            info = &pin;
        }
    }
#if MCP3221_ENABLE
    else if(dir == Port_Input && port == 0)
        info = &analog_in;
#endif

    return info;
}

static void set_pin_description (io_port_type_t type, io_port_direction_t dir, uint8_t port, const char *s)
{
    if(type == Port_Digital) {
        if(dir == Port_Input && port < aux_n_in)
            aux_in[in_map ? in_map[port] : port].description = s;

        if(dir == Port_Output && port < aux_n_out)
            aux_out[out_map ? out_map[port] : port].description = s;
    }
}

static char *get_pnum (uint8_t port)
{
    return pnum ? (pnum + (port * 3) + (port > 9 ? port - 10 : 0)) : NULL;
}

static bool claim (io_port_type_t type, io_port_direction_t dir, uint8_t *port, const char *description)
{
    bool ok = false;

    if(type == Port_Digital) {

        if(dir == Port_Input) {

            if((ok = in_map && *port < aux_n_in && !aux_in[*port].cap.claimed)) {

                uint8_t i;

                hal.port.num_digital_in--;

                for(i = in_map_rev(*port); i < hal.port.num_digital_in ; i++) {
                    in_map[i] = in_map[i + 1];
                    aux_in[in_map[i]].description = get_pnum(i);
                }

                aux_in[*port].cap.claimed = On;
                aux_in[*port].description = description;

                in_map[hal.port.num_digital_in] = *port;
                *port = hal.port.num_digital_in;
            }

        } else if((ok = out_map && *port < aux_n_out && !aux_out[*port].mode.claimed)) {

            uint8_t i;

            hal.port.num_digital_out--;

            for(i = out_map_rev(*port); i < hal.port.num_digital_out; i++) {
                out_map[i] = out_map[i + 1];
                aux_out[out_map[i]].description = get_pnum(i);
            }

            aux_out[*port].mode.claimed = On;
            aux_out[*port].description = description;

            out_map[hal.port.num_digital_out] = *port;
            *port = hal.port.num_digital_out;
        }
    }
#if MCP3221_ENABLE
    else if(dir == Port_Input && (ok = *port == 0 && analog_in.mode.analog && !analog_in.mode.claimed)) {
        analog_in.mode.claimed = On;
        analog_in.description = description;
    }
#endif

    return ok;
}

bool swap_pins (io_port_type_t type, io_port_direction_t dir, uint8_t port_a, uint8_t port_b)
{
    bool ok = port_a == port_b;

    if(!ok && type == Port_Digital) {

        if((ok = dir == Port_Input && port_a < aux_n_in && port_b < aux_n_in &&
                   aux_in[port_a].interrupt_callback == NULL &&
                    aux_in[port_b].interrupt_callback == NULL)) {

            input_signal_t tmp;

            memcpy(&tmp, &aux_in[port_a], sizeof(input_signal_t));
            memcpy(&aux_in[port_a], &aux_in[port_b], sizeof(input_signal_t));
            aux_in[port_a].description = tmp.description;
            tmp.description = aux_in[port_b].description;
            memcpy(&aux_in[port_b], &tmp, sizeof(input_signal_t));
        }

        if((ok = dir == Port_Output && port_a < aux_n_out && port_b < aux_n_out)) {

            output_signal_t tmp;

            memcpy(&tmp, &aux_out[port_a], sizeof(output_signal_t));
            memcpy(&aux_out[port_a], &aux_out[port_b], sizeof(output_signal_t));
            aux_out[port_a].description = tmp.description;
            tmp.description = aux_out[port_b].description;
            memcpy(&aux_out[port_b], &tmp, sizeof(output_signal_t));
        }
    }

    return ok;
}

#if MCP3221_ENABLE

static void enumerate_pins (bool low_level, pin_info_ptr pin_info, void *data)
{
    on_enumerate_pins(low_level, pin_info, data);

    pin_info(&analog_in, data);
}

#endif

void ioports_init (pin_group_pins_t *aux_inputs, pin_group_pins_t *aux_outputs)
{
    uint_fast8_t i, ports;

    aux_in = aux_inputs->pins.inputs;
    aux_out = aux_outputs->pins.outputs;

    if((hal.port.num_digital_in = aux_n_in = aux_inputs->n_pins)) {
        hal.port.wait_on_input = wait_on_input;
        hal.port.register_interrupt_handler = register_interrupt_handler;
        in_map = malloc(aux_n_in * sizeof(aux_n_in));
    }

    if((hal.port.num_digital_out = aux_n_out = aux_outputs->n_pins)) {
        hal.port.digital_out = digital_out;
        out_map = malloc(aux_n_out * sizeof(aux_n_out));
    }

    if((ports = max(aux_n_in, aux_n_out)) > 0)  {

        char *pn;

        hal.port.claim = claim;
        hal.port.swap_pins = swap_pins;
        hal.port.get_pin_info = get_pin_info;
        hal.port.set_pin_description = set_pin_description;

        settings_register(&setting_details);

        // Add M62-M65 port number mappings (P<n>) to description
        pnum = pn = malloc((3 * ports + (ports > 9 ? ports - 10 : 0)) + 1);

        for(i = 0; i < ports; i++) {

            if(pn) {
                *pn = 'P';
                strcpy(pn + 1, uitoa(i));
            }

            if(hal.port.num_digital_in && i < hal.port.num_digital_in) {
                if(in_map)
                    in_map[i] = i;
                if(pn)
                    aux_in[i].description = pn;
            }

            if(hal.port.num_digital_out && i < hal.port.num_digital_out) {
                if(out_map)
                    out_map[i] = i;
                if(pn)
                    aux_out[i].description = pn;
            }

            if(pn)
                pn += i > 9 ? 4 : 3;
        }

        // Add port names for ports up to 8 for $-setting flags

        for(i = 0; i < min(hal.port.num_digital_in, 8); i++) {
            strcat(input_ports, i == 0 ? "Aux " : ",Aux ");
            strcat(input_ports, uitoa(i));
        }

        for(i = 0; i < min(hal.port.num_digital_out, 8) ; i++) {
            out.mask = (out.mask << 1) + 1;
            strcat(output_ports, i == 0 ? "Aux " : ",Aux ");
            strcat(output_ports, uitoa(i));
        }
    }

#if MCP3221_ENABLE

    analog_in.function = Input_Aux0;
    analog_in.group = PinGroup_AuxInput;
    analog_in.pin = 0;
    analog_in.port = "MCP3221:";

    if(MCP3221_init()) {
        analog_in.mode.analog = On;
        hal.port.num_analog_in = analog_n_in = 1;
    };

    analog_in.description = analog_in.mode.analog ? "E0" : "No power";

    on_enumerate_pins = hal.enumerate_pins;
    hal.enumerate_pins = enumerate_pins;

#else
    UNUSED(analog_n_in);
#endif

}

#endif
