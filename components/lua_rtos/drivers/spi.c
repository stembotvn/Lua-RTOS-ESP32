/*
 * Lua RTOS, SPI driver
 *
 * Copyright (C) 2015 - 2017
 * IBEROXARXA SERVICIOS INTEGRALES, S.L.
 * 
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 * 
 * All rights reserved.  
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

/*
 * This driver is inspired and takes code from the following projects:
 *
 * arduino-esp32 (https://github.com/espressif/arduino-esp32)
 * esp32-nesemu (https://github.com/espressif/esp32-nesemu
 * esp-open-rtos (https://github.com/SuperHouse/esp-open-rtos)
 *
 * By default low level access uses Lua RTOS implementation, instead of spi_master from esp-idf that uses DMA transfers.
 * You can turn on esp-idf use setting SPI_USE_IDF_DRIVER to 1. Actually seems that spi_master from esp-idf have some
 * performance issues (although uses DMA) and issues related to read operations using DMA.
 *
 * Initial work in this driver was made by the Lua RTOS team in the espi driver (see):
 *
 * https://github.com/whitecatboard/Lua-RTOS-ESP32/commit/e4cfeccf60ddd2301c137537b3f8e039d3762869#diff-03afb94387bf851f6050a3066103cc67
 *
 * Work in espi driver was continued by Boris Lovošević (see):
 *
 * https://github.com/loboris/Lua-RTOS-ESP32-lobo/commit/1da097fd4e4c5bca61c28ea2f03bee17c84942f5#diff-03afb94387bf851f6050a3066103cc67
 *
 * Finally the Lua RTOS team have integrated all the ideas on the espi driver in the same driver.
 *
 */

#include "luartos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"

#include "soc/io_mux_reg.h"
#include "soc/spi_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_reg.h"

#include "driver/periph_ctrl.h"
#include "driver/spi_master.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <sys/macros.h>
#include <sys/syslog.h>
#include <sys/driver.h>

#include <drivers/spi.h>
#include <drivers/gpio.h>
#include <drivers/cpu.h>

#define PIN_FUNC_SPI 1
#define SPI_MAX_SIZE (4096 - 4)

extern uint32_t _rodata_start;
extern uint32_t _lit4_end;

extern void spi_flash_disable_interrupts_caches_and_other_cpu();
extern void spi_flash_enable_interrupts_caches_and_other_cpu();

// Register driver and messages
static void _spi_init();

DRIVER_REGISTER_BEGIN(SPI,spi,spi_locks,_spi_init,NULL);
	DRIVER_REGISTER_ERROR(SPI, spi, InvalidMode, "invalid mode", SPI_ERR_INVALID_MODE);
	DRIVER_REGISTER_ERROR(SPI, spi, InvalidUnit, "invalid unit", SPI_ERR_INVALID_UNIT);
	DRIVER_REGISTER_ERROR(SPI, spi, SlaveNotAllowed, "slave mode not allowed", SPI_ERR_SLAVE_NOT_ALLOWED);
	DRIVER_REGISTER_ERROR(SPI, spi, NotEnoughtMemory, "not enough memory", SPI_ERR_NOT_ENOUGH_MEMORY);
	DRIVER_REGISTER_ERROR(SPI, spi, PinNowAllowed, "pin not allowed", SPI_ERR_PIN_NOT_ALLOWED);
	DRIVER_REGISTER_ERROR(SPI, spi, NoMoreDevicesAllowed, "no more devices allowed", SPI_ERR_NO_MORE_DEVICES_ALLOWED);
	DRIVER_REGISTER_ERROR(SPI, spi, InvalidDevice, "invalid device", SPI_ERR_INVALID_DEVICE);
	DRIVER_REGISTER_ERROR(SPI, spi, DeviceNotSetup, "invalid device", SPI_ERR_DEVICE_NOT_SETUP);
	DRIVER_REGISTER_ERROR(SPI, spi, DeviceNotSelected, "device is not selected", SPI_ERR_DEVICE_IS_NOT_SELECTED);
	DRIVER_REGISTER_ERROR(SPI, spi, CannotChangePinMap, "cannot change pin map once the SPI unit has an attached device", SPI_ERR_CANNOT_CHANGE_PINMAP);
DRIVER_REGISTER_END(SPI,spi,NULL,_spi_init,NULL);

// SPI bus information
spi_bus_t spi_bus[CPU_LAST_SPI - CPU_FIRST_SPI + 1];

/*
 * Helper functions
 */
static void _spi_init() {
	memset(spi_bus, 0, sizeof(spi_bus_t) * (CPU_LAST_SPI - CPU_FIRST_SPI + 1));

	spi_bus[spi_idx(2)].last_device = -1;
	spi_bus[spi_idx(2)].selected_device = -1;

	spi_bus[spi_idx(3)].last_device = -1;
	spi_bus[spi_idx(3)].selected_device = -1;

	// SPI2
	spi_bus[spi_idx(2)].miso  = CONFIG_LUA_RTOS_SPI2_MISO;
	spi_bus[spi_idx(2)].mosi  = CONFIG_LUA_RTOS_SPI2_MOSI;
	spi_bus[spi_idx(2)].clk   = CONFIG_LUA_RTOS_SPI2_CLK;

	// SPI3
	spi_bus[spi_idx(3)].miso  = CONFIG_LUA_RTOS_SPI3_MISO;
	spi_bus[spi_idx(3)].mosi  = CONFIG_LUA_RTOS_SPI3_MOSI;
	spi_bus[spi_idx(3)].clk   = CONFIG_LUA_RTOS_SPI3_CLK;

	spi_bus[spi_idx(2)].mtx = xSemaphoreCreateRecursiveMutex();
	spi_bus[spi_idx(3)].mtx = xSemaphoreCreateRecursiveMutex();
}

static void spi_lock(uint8_t unit) {
	xSemaphoreTakeRecursive(spi_bus[spi_idx(unit)].mtx, portMAX_DELAY);
}

static void spi_unlock(uint8_t unit) {
	while (xSemaphoreGiveRecursive(spi_bus[spi_idx(unit)].mtx) == pdTRUE);
}

static int spi_get_device_by_cs(int unit, int8_t cs) {
	int i;

	for(i=0;i < SPI_BUS_DEVICES;i++) {
		if (spi_bus[spi_idx(unit)].device[i].setup && (spi_bus[spi_idx(unit)].device[i].cs == cs)) return i;
	}

	return -1;
}

static int spi_get_free_device(int unit) {
	int i;

	for(i=0;i < SPI_BUS_DEVICES;i++) {
		if (!spi_bus[spi_idx(unit)].device[i].setup) return i;
	}

	return -1;
}

typedef union {
    uint32_t regValue;
    struct {
        unsigned regL :6;
        unsigned regH :6;
        unsigned regN :6;
        unsigned regPre :13;
        unsigned regEQU :1;
    };
} spiClk_t;

static int spi_freq_for_pre_n(int fapb, int pre, int n) {
    return (fapb / (pre * n));
}

static uint32_t spi_set_clock(int fapb, int hz, int duty_cycle, uint32_t *eff_clk) {
	spiClk_t clock;

    int pre, n, h, l;

    //In hw, n, h and l are 1-64, pre is 1-8K. Value written to register is one lower than used value.
    if (hz>((fapb/4)*3)) {
        //Using Fapb directly will give us the best result here.
    	clock.regL = 0;
    	clock.regH = 0;
    	clock.regN = 0;
    	clock.regPre = 0;
    	clock.regEQU = 1;

    	if (eff_clk) *eff_clk=fapb;
    } else {
        //For best duty cycle resolution, we want n to be as close to 32 as possible, but
        //we also need a pre/n combo that gets us as close as possible to the intended freq.
        //To do this, we bruteforce n and calculate the best pre to go along with that.
        //If there's a choice between pre/n combos that give the same result, use the one
        //with the higher n.
        int bestn=-1;
        int bestpre=-1;
        int besterr=0;
        int errval;
        for (n=1; n<=64; n++) {
            //Effectively, this does pre=round((fapb/n)/hz).
            pre=((fapb/n)+(hz/2))/hz;
            if (pre<=0) pre=1;
            if (pre>8192) pre=8192;
            errval=abs(spi_freq_for_pre_n(fapb, pre, n)-hz);
            if (bestn==-1 || errval<=besterr) {
                besterr=errval;
                bestn=n;
                bestpre=pre;
            }
        }

        n=bestn;
        pre=bestpre;
        l=n;
        //This effectively does round((duty_cycle*n)/256)
        h=(duty_cycle*n+127)/256;
        if (h<=0) h=1;

    	clock.regL = l-1;
    	clock.regH = h-1;
    	clock.regN = n-1;
    	clock.regPre = pre-1;
    	clock.regEQU = 0;

    	if (eff_clk) *eff_clk=spi_freq_for_pre_n(fapb, pre, n);
    }

    return clock.regValue;
}

static void IRAM_ATTR spi_master_op(int deviceid, uint32_t word_size, uint32_t len, uint8_t *in, uint8_t *out) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	if (!spi_bus[spi_idx(unit)].device[device].dma) {
		// SPI hardware registers index
		uint32_t idx = 0;

		// TX / RX buffer
		uint8_t buffer[64];

		// This is the number of bytes / bits to transfer for current chunk
		uint32_t cbytes;
		uint16_t cbits;

		// Number of bytes to transmit
		uint32_t bytes = word_size * len;

		while (bytes) {
			// Fill TX buffer
			cbytes = ((bytes > 64)?64:bytes);
			if (in) {
				memcpy(buffer, in, cbytes);
				in = in + cbytes;
			} else {
				memset(buffer, 0xff, cbytes);
			}
			cbits = cbytes << 3;

			// Decrement bytes
			bytes = bytes - cbytes;

			// Wait for SPI bus ready
			while (READ_PERI_REG(SPI_CMD_REG(unit))&SPI_USR);

			// Set MOSI / MISO bit length
			SET_PERI_REG_BITS(SPI_MOSI_DLEN_REG(unit), SPI_USR_MOSI_DBITLEN, cbits - 1, SPI_USR_MOSI_DBITLEN_S);
			SET_PERI_REG_BITS(SPI_MISO_DLEN_REG(unit), SPI_USR_MISO_DBITLEN, cbits - 1, SPI_USR_MISO_DBITLEN_S);

			// Populate HW buffers with TX buffer contents
			idx = 0;
			while ((idx << 5) < cbits) {
				WRITE_PERI_REG((SPI_W0_REG(unit) + (idx << 2)), ((uint32_t *)buffer)[idx]);
				idx++;
			}

			// Start transfer
			SET_PERI_REG_MASK(SPI_CMD_REG(unit), SPI_USR);

			// Wait for SPI bus ready
			while (READ_PERI_REG(SPI_CMD_REG(unit))&SPI_USR);

			if (out) {
				// Populate RX buffer from HW buffers
				idx = 0;
				while ((idx << 5) < cbits) {
					((uint32_t *)buffer)[idx] = READ_PERI_REG((SPI_W0_REG(unit) + (idx << 2)));
					idx++;
				}

				memcpy((void *)out, (void *)buffer, cbytes);
				out = out + cbytes;
			}
		}
	} else {
		int device = (deviceid & 0x00ff);
		esp_err_t ret;
		spi_transaction_t t;
		uint8_t *bin = NULL;
		uint8_t *nbin = NULL;
		uint8_t ro = 0;

		// esp-idf driver used DMA, but data in FLASH can't be transferred by DMA, so in this case
		// we copy to RAM
		if (in) {
			if (((&_rodata_start) <= (uint32_t *)in) && ((uint32_t *) in <= (&_lit4_end))) {
				ro = 1;
				bin = malloc(word_size * len);
				assert(bin != NULL);

				nbin = bin;
				memcpy(bin, in, word_size * len);
			} else {
				bin = in;
			}
		}

		// Lua RTOS SPI driver don't specify max_transfer_sz for spi bus
		// config, so max tranfer size is limited to 4 Kb. If we need to
		// transfer more than 4 Kb the transfer is split into 4 Kb chunks
		int size;

		while (len > 0) {
			if (len >  SPI_MAX_SIZE / word_size) {
				size = SPI_MAX_SIZE / word_size;
			} else {
				size = len;
			}

			memset(&t, 0, sizeof(t));
			t.length = size * word_size * 8;
			t.tx_buffer = (bin == NULL)?NULL:bin;
			t.rx_buffer = (out == NULL)?NULL:out;

			ret = spi_device_transmit(spi_bus[spi_idx(unit)].device[device].h, &t);
			assert(ret==ESP_OK);

			len = len - size;

			if (bin) {
				bin = bin + size * word_size;
			}

			if (out) {
				out = out + size * word_size;
			}
		}

		if (ro) {
			free(nbin);
		}
	}
}

static void IRAM_ATTR spi_ll_save_registers(int unit, int device) {
	spi_bus[spi_idx(unit)].device[device].regs[0]  = READ_PERI_REG(SPI_USER_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[1]  = READ_PERI_REG(SPI_USER1_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[2]  = READ_PERI_REG(SPI_USER2_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[3]  = READ_PERI_REG(SPI_CTRL_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[4]  = READ_PERI_REG(SPI_CTRL2_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[5]  = READ_PERI_REG(SPI_SLAVE_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[6]  = READ_PERI_REG(SPI_PIN_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[7]  = READ_PERI_REG(SPI_CLOCK_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[8]  = READ_PERI_REG(SPI_DMA_CONF_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[9]  = READ_PERI_REG(SPI_DMA_OUT_LINK_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[10] = READ_PERI_REG(SPI_DMA_IN_LINK_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[11] = READ_PERI_REG(SPI_CMD_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[12] = READ_PERI_REG(SPI_ADDR_REG(unit));
	spi_bus[spi_idx(unit)].device[device].regs[13] = READ_PERI_REG(SPI_SLV_WR_STATUS_REG(unit));
}

static void IRAM_ATTR spi_ll_restore_registers(int unit, int device) {
	WRITE_PERI_REG(SPI_USER_REG(unit),  spi_bus[spi_idx(unit)].device[device].regs[0]);
	WRITE_PERI_REG(SPI_USER1_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[1]);
	WRITE_PERI_REG(SPI_USER2_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[2]);
	WRITE_PERI_REG(SPI_CTRL_REG(unit),  spi_bus[spi_idx(unit)].device[device].regs[3]);
	WRITE_PERI_REG(SPI_CTRL2_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[4]);
	WRITE_PERI_REG(SPI_SLAVE_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[5]);
	WRITE_PERI_REG(SPI_PIN_REG(unit),   spi_bus[spi_idx(unit)].device[device].regs[6]);
	WRITE_PERI_REG(SPI_CLOCK_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[7]);

	WRITE_PERI_REG(SPI_DMA_CONF_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[8]);
	WRITE_PERI_REG(SPI_DMA_OUT_LINK_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[9]);
	WRITE_PERI_REG(SPI_DMA_IN_LINK_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[10]);

	WRITE_PERI_REG(SPI_CMD_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[11]);
	WRITE_PERI_REG(SPI_ADDR_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[12]);
	WRITE_PERI_REG(SPI_SLV_WR_STATUS_REG(unit), spi_bus[spi_idx(unit)].device[device].regs[13]);
}

/*
 * End of extracted code from arduino-esp32
 */

static void spi_setup_bus(uint8_t unit, uint8_t flags) {
	// Enable SPI unit
	switch (unit) {
		case 2: periph_module_enable(PERIPH_HSPI_MODULE);break;
		case 3: periph_module_enable(PERIPH_VSPI_MODULE);break;
	}

	if (flags & SPI_FLAG_NO_DMA) {
		if (flags & SPI_FLAG_READ) {
		    if (spi_bus[spi_idx(unit)].miso == SPI_DEFAULT_MISO(unit)) {
		    	PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[spi_bus[spi_idx(unit)].miso], PIN_FUNC_SPI);
		    } else {
		        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[spi_bus[spi_idx(unit)].miso], PIN_FUNC_GPIO);
		        gpio_set_direction(spi_bus[spi_idx(unit)].miso, GPIO_MODE_INPUT);

		        switch(unit) {
		        	case 2:
		                gpio_matrix_out(spi_bus[spi_idx(unit)].miso, HSPIQ_OUT_IDX, 0, 0);
		                gpio_matrix_in(spi_bus[spi_idx(unit)].miso, HSPIQ_IN_IDX, 0);
		                break;

		        	case 3:
		                gpio_matrix_out(spi_bus[spi_idx(unit)].miso, VSPIQ_OUT_IDX, 0, 0);
		                gpio_matrix_in(spi_bus[spi_idx(unit)].miso, VSPIQ_IN_IDX, 0);
		                break;
		        }
		    }
		}

		if (flags & SPI_FLAG_WRITE) {
		    if (spi_bus[spi_idx(unit)].mosi == SPI_DEFAULT_MOSI(unit)) {
		    	PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[spi_bus[spi_idx(unit)].mosi], PIN_FUNC_SPI);
		    } else {
		        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[spi_bus[spi_idx(unit)].mosi], PIN_FUNC_GPIO);
		        gpio_set_direction(spi_bus[spi_idx(unit)].mosi, GPIO_MODE_OUTPUT);

		        switch(unit) {
		        	case 2:
		                gpio_matrix_out(spi_bus[spi_idx(unit)].mosi, HSPID_OUT_IDX, 0, 0);
		                gpio_matrix_in(spi_bus[spi_idx(unit)].mosi, HSPID_IN_IDX, 0);
		                break;

		        	case 3:
		                gpio_matrix_out(spi_bus[spi_idx(unit)].mosi, VSPID_OUT_IDX, 0, 0);
		                gpio_matrix_in(spi_bus[spi_idx(unit)].mosi, VSPID_IN_IDX, 0);
		                break;
		        }
		    }
		}

	    if (spi_bus[spi_idx(unit)].clk == SPI_DEFAULT_CLK(unit)) {
	    	PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[spi_bus[spi_idx(unit)].clk], PIN_FUNC_SPI);
	    } else {
	        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[spi_bus[spi_idx(unit)].clk], PIN_FUNC_GPIO);
	        gpio_set_direction(spi_bus[spi_idx(unit)].clk, GPIO_MODE_OUTPUT);

	        switch(unit) {
	        	case 2:
	                gpio_matrix_out(spi_bus[spi_idx(unit)].clk, HSPICLK_OUT_IDX, 0, 0);
	                break;

	        	case 3:
	                gpio_matrix_out(spi_bus[spi_idx(unit)].clk, VSPICLK_OUT_IDX, 0, 0);
	                break;
	        }
	    }
	} else {
	    esp_err_t ret;

	    spi_bus_config_t buscfg={
	        .miso_io_num=spi_bus[spi_idx(unit)].miso,
	        .mosi_io_num=spi_bus[spi_idx(unit)].mosi,
	        .sclk_io_num=spi_bus[spi_idx(unit)].clk,
	        .quadwp_io_num=-1,
	        .quadhd_io_num=-1
	    };

	    ret = spi_bus_initialize(unit - 1, &buscfg, unit - 1);
	    assert(ret==ESP_OK);
	}
}

/*
 * Low-level functions
 *
 */

int spi_ll_setup(uint8_t unit, uint8_t master, int8_t cs, uint8_t mode, uint32_t speed, uint8_t flags, int *deviceid) {
	// If SPI unit PIN map are not the native pins the max speed must be 26 Mhz
	if (!spi_use_native_pins(unit)) {
		speed = 26000000;
	}

	// Check if there's some device un bus with the same cs
	// If there's one, we want to reconfigure device
	int device = spi_get_device_by_cs(unit, cs);
	if (device < 0) {
		// No device present with the same cs
		// Get a free device
		device = spi_get_free_device(unit);
		if (device < 0) {
	    	// No more devices
			return -1;
		}
	} else {
		// Device present with the same cs
		if ((spi_bus[spi_idx(unit)].last_device & 0x0f) == device) {
			spi_bus[spi_idx(unit)].last_device = -1;
		}

		if (spi_bus[spi_idx(unit)].device[device].dma) {
			// Remove device first
			spi_bus_remove_device(spi_bus[spi_idx(unit)].device[device].h);
		}
	}

	// Setup bus, if not done yet
	if (!(spi_bus[spi_idx(unit)].setup & SPI_DMA_SETUP)) {
		spi_setup_bus(unit, flags);
	}

	if (!spi_bus[spi_idx(unit)].setup) {
		if ((flags & (SPI_FLAG_READ | SPI_FLAG_WRITE)) == (SPI_FLAG_READ | SPI_FLAG_WRITE)) {
			syslog(LOG_INFO,
				"spi%u at pins miso=%s%d/mosi=%s%d/clk=%s%d", unit,
				gpio_portname(spi_bus[spi_idx(unit)].miso), gpio_name(spi_bus[spi_idx(unit)].miso),
				gpio_portname(spi_bus[spi_idx(unit)].mosi), gpio_name(spi_bus[spi_idx(unit)].mosi),
				gpio_portname(spi_bus[spi_idx(unit)].clk) , gpio_name(spi_bus[spi_idx(unit)].clk)
			);
		} else if ((flags & (SPI_FLAG_READ | SPI_FLAG_WRITE)) == SPI_FLAG_WRITE) {
			syslog(LOG_INFO,
				"spi%u at pins mosi=%s%d/clk=%s%d", unit,
				gpio_portname(spi_bus[spi_idx(unit)].mosi), gpio_name(spi_bus[spi_idx(unit)].mosi),
				gpio_portname(spi_bus[spi_idx(unit)].clk) , gpio_name(spi_bus[spi_idx(unit)].clk)
			);
		} else if ((flags & (SPI_FLAG_READ | SPI_FLAG_WRITE)) == SPI_FLAG_READ) {
			syslog(LOG_INFO,
				"spi%u at pins miso=%s%d/clk=%s%d", unit,
				gpio_portname(spi_bus[spi_idx(unit)].miso), gpio_name(spi_bus[spi_idx(unit)].miso),
				gpio_portname(spi_bus[spi_idx(unit)].clk) , gpio_name(spi_bus[spi_idx(unit)].clk)
			);
		}
	}

	// Setup CS
	gpio_pin_output(cs);
	gpio_ll_pin_set(cs);

    spi_bus[spi_idx(unit)].device[device].cs = cs;

    if (flags & SPI_FLAG_NO_DMA) {
        // Complete operations, if pending
        CLEAR_PERI_REG_MASK(SPI_SLAVE_REG(unit), SPI_TRANS_DONE << 5);
        SET_PERI_REG_MASK(SPI_USER_REG(unit), SPI_CS_SETUP);

        // Set mode
    	switch (mode) {
    		case 0: // CKP=0, CPHA = 0
    		    CLEAR_PERI_REG_MASK(SPI_PIN_REG(unit) , SPI_CK_IDLE_EDGE);
    		    CLEAR_PERI_REG_MASK(SPI_USER_REG(unit), SPI_CK_OUT_EDGE);
                break;

    		case 1: // CKP=0, CPHA = 1
    		    CLEAR_PERI_REG_MASK(SPI_PIN_REG(unit),  SPI_CK_IDLE_EDGE);
    		    SET_PERI_REG_MASK(SPI_USER_REG(unit) ,  SPI_CK_OUT_EDGE);
    		    break;

    		case 2: // CKP=1, CPHA = 0
    		    SET_PERI_REG_MASK(SPI_PIN_REG(unit)   , SPI_CK_IDLE_EDGE);
    		    CLEAR_PERI_REG_MASK(SPI_USER_REG(unit), SPI_CK_OUT_EDGE);
    		    break;

    		case 3: // CKP=1, CPHA = 1
    		    SET_PERI_REG_MASK(SPI_PIN_REG(unit) ,   SPI_CK_IDLE_EDGE);
    		    SET_PERI_REG_MASK(SPI_USER_REG(unit),   SPI_CK_OUT_EDGE);
    	}

    	// Set bit order to MSB
        CLEAR_PERI_REG_MASK(SPI_CTRL_REG(unit), SPI_WR_BIT_ORDER | SPI_RD_BIT_ORDER);

        // Full-Duplex
        SET_PERI_REG_MASK(SPI_USER_REG(unit), SPI_DOUTDIN);

        // Enable 3-wire / 4-wire
        if ((flags & SPI_FLAG_3WIRE)?1:0) {
	        SET_PERI_REG_MASK(SPI_USER_REG(unit), SPI_SIO);
        } else {
        	CLEAR_PERI_REG_MASK(SPI_USER_REG(unit), SPI_SIO);
        }

        // Configure as master
        WRITE_PERI_REG(SPI_USER1_REG(unit), 0);
    	SET_PERI_REG_BITS(SPI_CTRL2_REG(unit), SPI_MISO_DELAY_MODE, 0, SPI_MISO_DELAY_MODE_S);
    	CLEAR_PERI_REG_MASK(SPI_SLAVE_REG(unit), SPI_SLAVE_MODE);

        // Set clock
        WRITE_PERI_REG(SPI_CLOCK_REG(unit), spi_set_clock(APB_CLK_FREQ, speed, 128, NULL));

        // Enable MOSI / MISO / CS
        SET_PERI_REG_MASK(SPI_USER_REG(unit), SPI_CS_SETUP | SPI_CS_HOLD | SPI_USR_MOSI | SPI_USR_MISO);
        SET_PERI_REG_MASK(SPI_CTRL2_REG(unit), ((0x4 & SPI_MISO_DELAY_NUM) << SPI_MISO_DELAY_NUM_S));

        // Don't use command phase
        CLEAR_PERI_REG_MASK(SPI_USER_REG(unit), SPI_USR_COMMAND);
        SET_PERI_REG_BITS(SPI_USER2_REG(unit), SPI_USR_COMMAND_BITLEN, 0, SPI_USR_COMMAND_BITLEN_S);

        // Don't use address phase
        CLEAR_PERI_REG_MASK(SPI_USER_REG(unit), SPI_USR_ADDR);
        SET_PERI_REG_BITS(SPI_USER1_REG(unit), SPI_USR_ADDR_BITLEN, 0, SPI_USR_ADDR_BITLEN_S);

        spi_bus[spi_idx(unit)].setup |= SPI_NO_DMA_SETUP;
    } else {
    	esp_err_t ret;

        spi_device_interface_config_t devcfg={
            .clock_speed_hz=speed,
            .mode=mode,
            .spics_io_num=-1,
            .queue_size=7,
			.flags=((flags & SPI_FLAG_3WIRE)?SPI_DEVICE_3WIRE:0)
        };

        ret = spi_bus_add_device(unit - 1, &devcfg, &spi_bus[spi_idx(unit)].device[device].h);
        assert(ret==ESP_OK);

        spi_bus[spi_idx(unit)].setup |= SPI_DMA_SETUP;
	}

    spi_bus[spi_idx(unit)].device[device].mode = mode;
    spi_bus[spi_idx(unit)].device[device].dma = !(flags & SPI_FLAG_NO_DMA);

    spi_ll_save_registers(unit, device);

	*deviceid = (unit << 8) | device;

	return 0;
}

void spi_ll_get_speed(int deviceid, uint32_t *speed) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	//*speed = spi_bus[spi_idx(unit)].device[device].speed;
}

void spi_ll_set_speed(int deviceid, uint32_t speed) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// If SPI unit PIN map are not the native pins the max speed must be 26 Mhz
	if (!spi_use_native_pins(unit)) {
		speed = 26000000;
	}

	spi_bus[spi_idx(unit)].last_device = -1;

	if (!spi_bus[spi_idx(unit)].device[device].dma) {
        WRITE_PERI_REG(SPI_CLOCK_REG(unit), spi_set_clock(APB_CLK_FREQ, speed, 128, NULL));
	} else {
		esp_err_t ret;

		spi_bus_remove_device(spi_bus[spi_idx(unit)].device[device].h);

	    spi_device_interface_config_t devcfg={
	        .clock_speed_hz=speed,
	        .mode = spi_bus[spi_idx(unit)].device[device].mode,
	        .spics_io_num = -1,
	        .queue_size=7,
	    };

	    ret = spi_bus_add_device(unit - 1, &devcfg, &spi_bus[spi_idx(unit)].device[device].h);
	    assert(ret==ESP_OK);
	}

	spi_ll_save_registers(unit, device);
}

void IRAM_ATTR spi_ll_transfer(int deviceid, uint8_t data, uint8_t *read) {
    spi_master_op(deviceid, 1, 1, (uint8_t *)(&data), read);
}

void IRAM_ATTR spi_ll_bulk_write(int deviceid, uint32_t nbytes, uint8_t *data) {
    spi_master_op(deviceid, 1, nbytes, data, NULL);
}

void IRAM_ATTR spi_ll_bulk_read(int deviceid, uint32_t nbytes, uint8_t *data) {
    spi_master_op(deviceid, 1, nbytes, NULL, data);
}

int IRAM_ATTR spi_ll_bulk_rw(int deviceid, uint32_t nbytes, uint8_t *data) {
	uint8_t *read = (uint8_t *)malloc(nbytes);
	if (read) {
	    spi_master_op(deviceid, 1, nbytes, data, read);

	    memcpy(data, read, nbytes);
		free(read);
	} else {
		return -1;
	}

	return 0;
}

void IRAM_ATTR spi_ll_bulk_write16(int deviceid, uint32_t nelements, uint16_t *data) {
    spi_master_op(deviceid, 2, nelements, (uint8_t *)data, NULL);
}

void IRAM_ATTR spi_ll_bulk_read16(int deviceid, uint32_t nelements, uint16_t *data) {
    spi_master_op(deviceid, 2, nelements, NULL, (uint8_t *)data);
}

int IRAM_ATTR spi_ll_bulk_rw16(int deviceid, uint32_t nelements, uint16_t *data) {
	uint16_t *read = (uint16_t *)malloc(nelements * sizeof(uint16_t));
	if (read) {
	    spi_master_op(deviceid, 2, nelements, (uint8_t *)data, (uint8_t *)read);

	    memcpy(data, read, nelements);
		free(read);
	} else {
		return -1;
	}

	return 0;
}

void IRAM_ATTR spi_ll_bulk_write32(int deviceid, uint32_t nelements, uint32_t *data) {
    spi_master_op(deviceid, 4, nelements, (uint8_t *)data, NULL);
}

void IRAM_ATTR spi_ll_bulk_read32(int deviceid, uint32_t nelements, uint32_t *data) {
    spi_master_op(deviceid, 4, nelements, NULL, (uint8_t *)data);
}

int IRAM_ATTR spi_ll_bulk_rw32(int deviceid, uint32_t nelements, uint32_t *data) {
	uint32_t *read = (uint32_t *)malloc(nelements * sizeof(uint32_t));
	if (read) {
	    spi_master_op(deviceid, 4, nelements, (uint8_t *)data, (uint8_t *)read);

	    memcpy(data, read, nelements);
		free(read);
	} else {
		return -1;
	}

	return 0;
}

void IRAM_ATTR spi_ll_select(int deviceid) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	spi_lock(unit);

	if (spi_bus[spi_idx(unit)].last_device != deviceid) {
		spi_ll_restore_registers(unit, device);
    }

    spi_bus[spi_idx(unit)].last_device = deviceid;
    spi_bus[spi_idx(unit)].selected_device = deviceid;

	// Select device
    gpio_ll_pin_clr(spi_bus[spi_idx(unit)].device[device].cs);
}

void IRAM_ATTR spi_ll_deselect(int deviceid) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Deselect device
    gpio_ll_pin_set(spi_bus[spi_idx(unit)].device[device].cs);

    spi_bus[spi_idx(unit)].selected_device = -1;

	spi_unlock(unit);
}

/*
 * Operation functions
 *
 */
driver_error_t *spi_pin_map(int unit, int miso, int mosi, int clk) {
    // Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
    }

	spi_lock(unit);

    if (spi_bus[spi_idx(unit)].setup) {
    	spi_unlock(unit);
		return driver_error(SPI_DRIVER, SPI_ERR_CANNOT_CHANGE_PINMAP, NULL);
    }

    if ((!(GPIO_ALL_IN & (GPIO_BIT_MASK << spi_bus[spi_idx(unit)].miso))) && (miso >= 0)) {
    	spi_unlock(unit);
		return driver_error(SPI_DRIVER, SPI_ERR_PIN_NOT_ALLOWED, "miso, selected pin cannot be input");
    }

    if ((!(GPIO_ALL_OUT & (GPIO_BIT_MASK << spi_bus[spi_idx(unit)].mosi))) && (mosi >= 0)) {
    	spi_unlock(unit);
		return driver_error(SPI_DRIVER, SPI_ERR_PIN_NOT_ALLOWED, "mosi, selected pin cannot be output");
    }

    if ((!(GPIO_ALL_IN & (GPIO_BIT_MASK << spi_bus[spi_idx(unit)].clk))) && (clk >= 0)) {
    	spi_unlock(unit);
		return driver_error(SPI_DRIVER, SPI_ERR_PIN_NOT_ALLOWED, "clk, selected pin cannot be output");
    }

    if (!TEST_UNIQUE3(spi_bus[spi_idx(unit)].mosi, spi_bus[spi_idx(unit)].miso, spi_bus[spi_idx(unit)].clk)) {
    	spi_unlock(unit);
		return driver_error(SPI_DRIVER, SPI_ERR_PIN_NOT_ALLOWED, "miso, mosi and clk must be different");
    }

    // Update miso
    if (miso >= 0) {
    	spi_bus[spi_idx(unit)].miso  = miso;
    }

    // Update mosi
    if (mosi >= 0) {
    	spi_bus[spi_idx(unit)].mosi  = mosi;
    }

    // Update clk
    if (clk >= 0) {
    	spi_bus[spi_idx(unit)].clk   = clk;
    }

    spi_unlock(unit);

	return NULL;
}

driver_error_t *spi_setup(uint8_t unit, uint8_t master, int8_t cs, uint8_t mode, uint32_t speed, uint8_t flags, int *deviceid) {
	driver_error_t *error = NULL;

    // Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
    }

    if (master != 1) {
		return driver_error(SPI_DRIVER, SPI_ERR_SLAVE_NOT_ALLOWED, NULL);
    }

    if (mode > 3) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_MODE, NULL);
    }

    if ((!(GPIO_ALL_IN & (GPIO_BIT_MASK << spi_bus[spi_idx(unit)].miso))) && (flags & SPI_FLAG_READ)) {
		return driver_error(SPI_DRIVER, SPI_ERR_PIN_NOT_ALLOWED, "miso, selected pin cannot be input");
    }

    if ((!(GPIO_ALL_OUT & (GPIO_BIT_MASK << spi_bus[spi_idx(unit)].mosi))) && (flags & SPI_FLAG_WRITE)) {
		return driver_error(SPI_DRIVER, SPI_ERR_PIN_NOT_ALLOWED, "mosi, selected pin cannot be output");
    }

    if (!(GPIO_ALL_IN & (GPIO_BIT_MASK << spi_bus[spi_idx(unit)].clk))) {
		return driver_error(SPI_DRIVER, SPI_ERR_PIN_NOT_ALLOWED, "clk, selected pin cannot be output");
    }

	if (cs == -1) {
		if (unit == 2) {
			cs = CONFIG_LUA_RTOS_SPI2_CS;
		} else if (unit == 3) {
			cs = CONFIG_LUA_RTOS_SPI3_CS;
		}

		if (cs == -1) {
			return driver_error(SPI_DRIVER, SPI_ERR_PIN_NOT_ALLOWED, "default cs is not set");
		}
	}


    if (!(GPIO_ALL_OUT & (GPIO_BIT_MASK << cs))) {
		return driver_error(SPI_DRIVER, SPI_ERR_PIN_NOT_ALLOWED, "cs, selected pin cannot be output");
    }

    if (!TEST_UNIQUE4(spi_bus[spi_idx(unit)].mosi, spi_bus[spi_idx(unit)].miso, spi_bus[spi_idx(unit)].clk, cs)) {
		return driver_error(SPI_DRIVER, SPI_ERR_PIN_NOT_ALLOWED, "miso, mosi, clk and cs must be different");
    }

    // Lock resources
    if (!spi_bus[spi_idx(unit)].setup) {
        if ((error = spi_lock_bus_resources(unit, flags))) {
    		return error;
    	}
    }

    driver_unit_lock_error_t *lock_error = NULL;
    if ((lock_error = driver_lock(SPI_DRIVER, unit, GPIO_DRIVER, cs, flags, "CS"))) {
    	return driver_lock_error(SPI_DRIVER, lock_error);
    }

    // Low-level setup
	spi_lock(unit);
    if (spi_ll_setup(unit, master, cs, mode, speed, flags, deviceid) != 0) {
    	spi_unlock(unit);
		return driver_error(SPI_DRIVER, SPI_ERR_NO_MORE_DEVICES_ALLOWED, NULL);
    }
	spi_unlock(unit);

	return NULL;
}

driver_error_t *spi_select(int deviceid) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	spi_ll_select(deviceid);

    return NULL;
}

driver_error_t *spi_deselect(int deviceid) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

    spi_ll_deselect(deviceid);

    return NULL;
}

driver_error_t *spi_get_speed(int deviceid, uint32_t *speed) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	spi_lock(unit);
	spi_ll_get_speed(deviceid, speed);
	spi_unlock(unit);

	return NULL;
}

driver_error_t *spi_set_speed(int deviceid, uint32_t speed) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	spi_lock(unit);
	spi_ll_set_speed(deviceid, speed);
	spi_unlock(unit);

	return NULL;
}

driver_error_t *spi_transfer(int deviceid, uint8_t data, uint8_t *read) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	if (spi_bus[spi_idx(unit)].selected_device != deviceid) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_IS_NOT_SELECTED, NULL);
	}

	spi_ll_transfer(deviceid, data, read);

    return NULL;
}

driver_error_t *spi_bulk_write(int deviceid, uint32_t nbytes, uint8_t *data) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	if (spi_bus[spi_idx(unit)].selected_device != deviceid) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_IS_NOT_SELECTED, NULL);
	}

	spi_ll_bulk_write(deviceid, nbytes, data);

    return NULL;
}

driver_error_t *spi_bulk_read(int deviceid, uint32_t nbytes, uint8_t *data) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	if (spi_bus[spi_idx(unit)].selected_device != deviceid) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_IS_NOT_SELECTED, NULL);
	}

	spi_ll_bulk_read(deviceid, nbytes, data);

	return NULL;
}

driver_error_t *spi_bulk_rw(int deviceid, uint32_t nbytes, uint8_t *data) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	if (spi_bus[spi_idx(unit)].selected_device != deviceid) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_IS_NOT_SELECTED, NULL);
	}

	if (spi_ll_bulk_rw(deviceid, nbytes, data) < 0) {
		return driver_error(SPI_DRIVER, SPI_ERR_NOT_ENOUGH_MEMORY, NULL);
	}

	return NULL;
}

driver_error_t *spi_bulk_write16(int deviceid, uint32_t nelements, uint16_t *data) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	if (spi_bus[spi_idx(unit)].selected_device != deviceid) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_IS_NOT_SELECTED, NULL);
	}

	spi_ll_bulk_write16(deviceid, nelements, data);

    return NULL;
}

driver_error_t *spi_bulk_read16(int deviceid, uint32_t nelements, uint16_t *data) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	if (spi_bus[spi_idx(unit)].selected_device != deviceid) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_IS_NOT_SELECTED, NULL);
	}

	spi_ll_bulk_read16(deviceid, nelements, data);

    return NULL;
}

driver_error_t *spi_bulk_rw16(int deviceid, uint32_t nelements, uint16_t *data) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	if (spi_bus[spi_idx(unit)].selected_device != deviceid) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_IS_NOT_SELECTED, NULL);
	}

	if (spi_ll_bulk_rw16(deviceid, nelements, data) < 0) {
		return driver_error(SPI_DRIVER, SPI_ERR_NOT_ENOUGH_MEMORY, NULL);
	}

	return NULL;
}

driver_error_t *spi_bulk_write32(int deviceid, uint32_t nelements, uint32_t *data) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	if (spi_bus[spi_idx(unit)].selected_device != deviceid) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_IS_NOT_SELECTED, NULL);
	}

	spi_ll_bulk_write32(deviceid, nelements, data);

    return NULL;
}

driver_error_t *spi_bulk_read32(int deviceid, uint32_t nelements, uint32_t *data) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	if (spi_bus[spi_idx(unit)].selected_device != deviceid) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_IS_NOT_SELECTED, NULL);
	}

	spi_ll_bulk_read32(deviceid, nelements, data);

    return NULL;
}

driver_error_t *spi_bulk_rw32(int deviceid, uint32_t nelements, uint32_t *data) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	if (spi_bus[spi_idx(unit)].selected_device != deviceid) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_IS_NOT_SELECTED, NULL);
	}

	if (spi_ll_bulk_rw32(deviceid, nelements, data) < 0) {
		return driver_error(SPI_DRIVER, SPI_ERR_NOT_ENOUGH_MEMORY, NULL);
	}

	return NULL;
}

driver_error_t *spi_lock_bus_resources(int unit, uint8_t flags) {
    driver_unit_lock_error_t *lock_error = NULL;

    // Lock pins
    if ((flags & SPI_FLAG_READ) && (spi_bus[spi_idx(unit)].miso >= 0)) {
        if ((lock_error = driver_lock(SPI_DRIVER, unit, GPIO_DRIVER, spi_bus[spi_idx(unit)].miso, flags, "MISO"))) {
        	// Revoked lock on pin
        	return driver_lock_error(SPI_DRIVER, lock_error);
        }
    }

    if ((flags & SPI_FLAG_WRITE)  && (spi_bus[spi_idx(unit)].mosi >= 0)) {
        if ((lock_error = driver_lock(SPI_DRIVER, unit, GPIO_DRIVER, spi_bus[spi_idx(unit)].mosi, flags, "MOSI"))) {
        	// Revoked lock on pin
        	return driver_lock_error(SPI_DRIVER, lock_error);
        }
    }

    if (spi_bus[spi_idx(unit)].clk >= 0){
        if ((lock_error = driver_lock(SPI_DRIVER, unit, GPIO_DRIVER, spi_bus[spi_idx(unit)].clk, flags, "CLK"))) {
        	// Revoked lock on pin
        	return driver_lock_error(SPI_DRIVER, lock_error);
        }
    }

    return NULL;
}

void spi_unlock_bus_resources(int unit) {
	int num_devices = 0;
	int i;

	spi_lock(unit);

	// Count active devices
	for(i=0;i < SPI_BUS_DEVICES;i++) {
		if (spi_bus[spi_idx(unit)].device[i].setup) {
			num_devices++;
		}
	}

	if (num_devices == 0) {
		// There are not devices in bus
		// Remove bus locks
	    if (spi_bus[spi_idx(unit)].miso >= 0) {
			driver_unlock(SPI_DRIVER, unit, GPIO_DRIVER, spi_bus[spi_idx(unit)].miso);
	    }

	    if (spi_bus[spi_idx(unit)].mosi >= 0) {
			driver_unlock(SPI_DRIVER, unit, GPIO_DRIVER, spi_bus[spi_idx(unit)].mosi);
	    }

	    if (spi_bus[spi_idx(unit)].clk >= 0){
			driver_unlock(SPI_DRIVER, unit, GPIO_DRIVER, spi_bus[spi_idx(unit)].clk);
	    }
	}

	spi_unlock(unit);
}

void spi_ll_unsetup(int deviceid) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	spi_lock(unit);

	if (spi_bus[spi_idx(unit)].device[device].setup) {
		// Remove device fom bus
		if (spi_bus[spi_idx(unit)].device[device].dma) {
			spi_bus_remove_device(spi_bus[spi_idx(unit)].device[device].h);
		}

		// Unlock device CS
		driver_unlock(SPI_DRIVER, unit, GPIO_DRIVER, spi_bus[spi_idx(unit)].device[device].cs);

		spi_bus[spi_idx(unit)].device[unit].setup = 0;
	}

	spi_unlock_bus_resources(unit);

	spi_unlock(unit);
}

driver_error_t *spi_unsetup(int deviceid) {
	int unit = (deviceid & 0xff00) >> 8;
	int device = (deviceid & 0x00ff);

	// Sanity checks
	if ((unit > CPU_LAST_SPI) || (unit < CPU_FIRST_SPI)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((device < 0) || (device > SPI_BUS_DEVICES)) {
		return driver_error(SPI_DRIVER, SPI_ERR_INVALID_DEVICE, NULL);
	}

	if (!spi_bus[spi_idx(unit)].device[device].setup) {
		return driver_error(SPI_DRIVER, SPI_ERR_DEVICE_NOT_SETUP, NULL);
	}

	spi_ll_unsetup(deviceid);

	return NULL;
}
