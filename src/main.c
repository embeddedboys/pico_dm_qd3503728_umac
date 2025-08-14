// Copyright (c) 2024 embeddedboys developers

// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>

#include "pico/time.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "pico/stdio_uart.h"

#include "hardware/pll.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"

#include "kbd.h"

#include "bsp/rp2040/board.h"
#include "tusb.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "ft6236.h"
#include "ili9488.h"
#include "backlight.h"

#include "debug.h"

#include "umac.h"

QueueHandle_t xToFlushQueue = NULL;

extern const unsigned char __rom_begin[], __rom_end[];
extern const unsigned char __disc_begin[], __disc_end[];

static uint8_t umac_ram[RAM_SIZE];
static uint16_t framebuffer[DISP_WIDTH * DISP_HEIGHT];

static int cursor_x;
static int cursor_y;
static int cursor_button;

static int umac_cursor_x = 0;
static int umac_cursor_y = 0;
static int umac_cursor_button = 0;

void vApplicationTickHook()
{

}

static void video_init(uint32_t *fb)
{
    printf("%s, %p, %p\n", __func__, umac_ram, fb);
}

/* Blit a 1bpp FB to a 32BPP RGBA output.  SDL2 doesn't appear to support
 * bitmap/1bpp textures, so expand.
 */
static void copy_fb(uint16_t *fb_out, uint8_t *fb_in)
{
    // Output L-R, big-endian shorts, with bits in MSB-LSB order:
    for (int y = 0; y < DISP_HEIGHT; y++) {
        for (int x = 0; x < DISP_WIDTH; x += 16) {
            uint8_t plo = fb_in[x/8 + (y * DISP_WIDTH/8) + 0];
            uint8_t phi = fb_in[x/8 + (y * DISP_WIDTH/8) + 1];
            for (int i = 0; i < 8; i++) {
                    *fb_out++ = (plo & (0x80 >> i)) ? 0 : 0xffff;
            }
            for (int i = 0; i < 8; i++) {
                    *fb_out++ = (phi & (0x80 >> i)) ? 0 : 0xffff;
            }
        }
    }
}

static void poll_umac()
{
    static absolute_time_t last_1hz = 0;
    static absolute_time_t last_vsync = 0;
    absolute_time_t now = get_absolute_time();

    umac_loop();

    int64_t p_1hz = absolute_time_diff_us(last_1hz, now);
    int64_t p_vsync = absolute_time_diff_us(last_vsync, now);
    if (p_vsync >= 16667) {
            /* FIXME: Trigger this off actual vsync */
            umac_vsync_event();
            last_vsync = now;
    }
    if (p_1hz >= 1000000) {
            umac_1hz_event();
            last_1hz = now;
    }

    int update = 0;
    int dx = 0;
    int dy = 0;
    int b = umac_cursor_button;

    if (cursor_x != umac_cursor_x) {
        dx = cursor_x - umac_cursor_x;
        umac_cursor_x = cursor_x;
        update = 1;
    }
    if (cursor_y != umac_cursor_y) {
        dy = cursor_y - umac_cursor_y;
        umac_cursor_y = cursor_y;
        update = 1;
    }
    if (cursor_button != umac_cursor_button) {
        b = cursor_button;
        umac_cursor_button = cursor_button;
        update = 1;
    }
    if (update) {
        umac_mouse(dx, -dy, b);
    }
}

static portTASK_FUNCTION(input_task, pvParameters)
{
    for (;;) {
        cursor_x = ft6236_read_x();
        cursor_y = ft6236_read_y();
        cursor_button = ft6236_is_pressed();
    }

    vTaskDelete(NULL);
}

static portTASK_FUNCTION(video_push_task, pvParameters)
{
    struct video_frame vf = {
        .xs = 0,
        .xe = DISP_WIDTH - 1,
        .ys = 0,
        .ye = DISP_HEIGHT - 1,
        .vmem16 = framebuffer,
        .len = sizeof(framebuffer) / sizeof(framebuffer[0]),
    };

    for (;;) {
        tuh_task();
        copy_fb(framebuffer, umac_ram + umac_get_fb_offset());
        ili9488_async_video_flush(&vf);
    }

    vTaskDelete(NULL);
}

static void disc_setup(disc_descr_t discs[DISC_NUM_DRIVES])
{
    /* If we don't find (or look for) an SD-based image, attempt
     * to use in-flash disc image:
     */
    discs[0].base = (void *)__disc_begin;
    discs[0].read_only = 1;
    discs[0].size = (__disc_end - __disc_begin);
}

static portTASK_FUNCTION(umac_task_handler, pvParameters)
{
    disc_descr_t discs[DISC_NUM_DRIVES] = {0};

    printf("umac task started\n");
    disc_setup(discs);

    /* Rom has been patched previously */
    umac_init(umac_ram, (void *)__rom_begin, discs);

    video_init((uint32_t *)(umac_ram + umac_get_fb_offset()));

    printf("Enjoyable Mac times now begin:\n\n");

	for(;;)
        poll_umac();

	vTaskDelete(NULL);
}

int main(void)
{
    /* NOTE: DO NOT MODIFY THIS BLOCK */
#define CPU_SPEED_MHZ (DEFAULT_SYS_CLK_KHZ / 1000)
    if(CPU_SPEED_MHZ > 266 && CPU_SPEED_MHZ <= 360)
        vreg_set_voltage(VREG_VOLTAGE_1_20);
    else if (CPU_SPEED_MHZ > 360 && CPU_SPEED_MHZ <= 396)
        vreg_set_voltage(VREG_VOLTAGE_1_25);
    else if (CPU_SPEED_MHZ > 396)
        vreg_set_voltage(VREG_VOLTAGE_MAX);
    else
        vreg_set_voltage(VREG_VOLTAGE_DEFAULT);

    set_sys_clock_khz(CPU_SPEED_MHZ * 1000, true);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    CPU_SPEED_MHZ * MHZ,
                    CPU_SPEED_MHZ * MHZ);
    stdio_uart_init_full(uart0, 115200, 16, 17);

    printf("\n\n\nPICO DM QD3503728 UMAC Porting\n");

    xToFlushQueue = xQueueCreate(2, sizeof(struct video_frame));

    printf("Starting demo\n");

    printf("init hardware\n");
    tusb_init();
    ft6236_driver_init();
    ili9488_driver_init();

    TaskHandle_t umac_task_handle;
    xTaskCreate(umac_task_handler, "umac_task", 2048, NULL, (tskIDLE_PRIORITY + 3), &umac_task_handle);
    vTaskCoreAffinitySet(umac_task_handle, (1 << 0));

    TaskHandle_t input_task_handle;
    xTaskCreate(input_task, "input_task", configMINIMAL_STACK_SIZE, NULL, (tskIDLE_PRIORITY + 2), &input_task_handle);
    vTaskCoreAffinitySet(input_task_handle, (1 << 1));

    TaskHandle_t video_push_handle;
    xTaskCreate(video_push_task, "video_push", configMINIMAL_STACK_SIZE, NULL, (tskIDLE_PRIORITY + 2), &video_push_handle);
    vTaskCoreAffinitySet(video_push_handle, (1 << 1));

    TaskHandle_t video_flush_handle;
    xTaskCreate(video_flush_task, "video_flush", configMINIMAL_STACK_SIZE, NULL, (tskIDLE_PRIORITY + 2), &video_flush_handle);
    vTaskCoreAffinitySet(video_flush_handle, (1 << 1));

    backlight_driver_init();
    backlight_set_level(100);
    printf("backlight set to 100%%\n");

    printf("calling freertos scheduler, %lld\n", time_us_64());
    vTaskStartScheduler();
    for(;;);

    return 0;
}
