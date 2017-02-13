/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2011 Diogo Ferreira <defer@cyanogenmod.com>
 * Copyright (C) 2012 Alin Jerpelea <jerpelea@gmail.com>
 * Copyright (C) 2012 The CyanogenMod Project <http://www.cyanogenmod.com>
 * Copyright (C) 2017 Caio Oliveira <caiooliveirafarias0@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
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

#define LOG_TAG "lights.falconss"

#include <cutils/log.h>
#include <cutils/properties.h>
#include <errno.h>
#include <fcntl.h>
#include <hardware/lights.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

/* Synchronization primities */
static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Mini-led state machine */
static struct light_state_t g_notification;
static struct light_state_t g_battery;
static int g_attention = 0;

// Backlight
char const *const LCD_FILE = "/sys/class/leds/lcd-backlight/brightness";

// SNS/Bar Led
char const *const SNS_LED_FILE =
    "/sys/class/leds/lm3533-light-sns/rgb_brightness";

// Notification Led
char const *const RED_LED_FILE = "/sys/class/leds/red/brightness";
char const *const GREEN_LED_FILE = "/sys/class/leds/green/brightness";
char const *const BLUE_LED_FILE = "/sys/class/leds/notification/brightness";
char const *const RED_BLINK_FILE = "/sys/class/leds/red/blink";

int lights_property_get_int(const char *key, int default_value)
{
	if (!key) {
		return default_value;
	}

	int result = default_value;
	char buf[PROP_VALUE_MAX] = {
	    '\0',
	};

	int len = property_get(key, buf, "");
	if (len == 1) {
		char ch = buf[0];
		if (ch == '0' || ch == 'n') {
			result = 0;
		} else if (ch == '1' || ch == 'y') {
			result = 1;
		} else if (ch == '2' || ch == 'o') {
			result = 2;
		} else {
			result = 1;
		}
	} else if (len > 1) {
		if (!strcmp(buf, "no") || !strcmp(buf, "false") ||
		    !strcmp(buf, "off") || !strcmp(buf, "disable")) {
			result = 0;
		} else if (!strcmp(buf, "yes") || !strcmp(buf, "true") ||
			   !strcmp(buf, "on") || !strcmp(buf, "enable")) {
			result = 1;
		} else if (!strcmp(buf, "only")) {
			result = 2;
		} else {
			result = 1;
		}
	}

	return result;
}

static int lights_write_int(char const *path, int value)
{
	int fd;
	static int already_warned = 0;

	fd = open(path, O_RDWR);
	if (fd >= 0) {
		char buffer[20];
		int bytes = snprintf(buffer, sizeof(buffer), "%d\n", value);
		ssize_t written = write(fd, buffer, (size_t)bytes);
		close(fd);
		return written == -1 ? -errno : 0;
	} else {
		if (already_warned == 0) {
			ALOGE("%s: failed to open %s\n", __func__, path);
			already_warned = 1;
		}
		return -errno;
	}
}

/* Color tools */
static int lights_is_lit(struct light_state_t const *state)
{
	return state->color & 0x00FFFFFF;
}

static int lights_rgb_to_brightness(struct light_state_t const *state)
{
	int color = state->color & 0x00FFFFFF;

	return ((77 * ((color >> 16) & 0x00FF)) +
		(150 * ((color >> 8) & 0x00FF)) + (29 * (color & 0x00FF))) >>
	       8;
}

/* The actual lights controlling section */
static int lights_set_backlight(struct light_state_t const *state)
{
	int err = 0;
	int brightness = lights_rgb_to_brightness(state);
	pthread_mutex_lock(&g_lock);
	err = lights_write_int(LCD_FILE, brightness);
	pthread_mutex_unlock(&g_lock);

	return err;
}

static void lights_set_shared_locked(struct light_state_t const *state)
{
	int red, green, blue, rgb;
	int blink, onMS, offMS;
	int barled = lights_property_get_int("sys.lights.barled", 1);

	switch (state->flashMode) {
	case LIGHT_FLASH_TIMED:
		onMS = state->flashOnMS;
		offMS = state->flashOffMS;
		break;
	case LIGHT_FLASH_NONE:
	default:
		onMS = 0;
		offMS = 0;
		break;
	}

	if (barled == 1) {
		red = (state->color >> 16) & 0x00FF;
		green = (state->color >> 8) & 0x00FF;
		blue = state->color & 0x00FF;
		rgb = ((red & 0x00FF) << 16) | ((green & 0x00FF) << 8) |
		      (blue & 0x00FF);
	} elif (barled == 2) {
		red = 0
		green = 0
		blue = 0
		rgb = ((red & 0x00FF) << 16) | ((green & 0x00FF) << 8) |
		      (blue & 0x00FF);
	} else {
		red = (state->color >> 16) & 0x00FF;
		green = (state->color >> 8) & 0x00FF;
		blue = state->color & 0x00FF;
		rgb = 0;
	}

	if (onMS > 0 && offMS > 0) {
		blink = 1;
	} else {
		blink = 0;
	}

	if (blink) {
		if (red)
			lights_write_int(RED_BLINK_FILE, blink);
	} else {
		lights_write_int(RED_LED_FILE, red);
		lights_write_int(GREEN_LED_FILE, green);
		lights_write_int(BLUE_LED_FILE, blue);
		lights_write_int(SNS_LED_FILE, rgb);
	}
}

static void lights_handle_shared_locked(void)
{
	if (lights_is_lit(&g_battery)) {
		lights_set_shared_locked(&g_battery);
	} else {
		lights_set_shared_locked(&g_notification);
	}
}

static int lights_set_battery(struct light_state_t const *state)
{
	pthread_mutex_lock(&g_lock);
	g_battery = *state;
	lights_handle_shared_locked();
	pthread_mutex_unlock(&g_lock);

	return 0;
}

static int lights_set_notifications(struct light_state_t const *state)
{
	pthread_mutex_lock(&g_lock);
	g_notification = *state;
	lights_handle_shared_locked();
	pthread_mutex_unlock(&g_lock);

	return 0;
}

static int lights_set_attention(struct light_state_t const* state)
{
	pthread_mutex_lock(&g_lock);
	if (state->flashMode == LIGHT_FLASH_HARDWARE) {
		g_attention = state->flashOnMS;
	} else if (state->flashMode == LIGHT_FLASH_NONE) {
		g_attention = 0;
	}
	lights_handle_shared_locked();
	pthread_mutex_unlock(&g_lock);

	return 0;
}

/* Initializations */
void lights_init_globals(void) { pthread_mutex_init(&g_lock, NULL); }

/* Close the lights device */
static int lights_close(struct light_device_t *dev)
{
	if (dev) {
		free(dev);
	}

	return 0;
}

/* Open a new instance of a lights device using name */
static int lights_open(const struct hw_module_t *module, char const *name,
		       struct hw_device_t **device)
{
	int (*set_light)(struct light_state_t const *state);

	if (0 == strcmp(LIGHT_ID_BACKLIGHT, name)) {
		set_light = lights_set_backlight;
	} else if (0 == strcmp(LIGHT_ID_BATTERY, name)) {
		set_light = lights_set_battery;
	} else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name)) {
		set_light = lights_set_notifications;
	} else if (0 == strcmp(LIGHT_ID_ATTENTION, name)) {
		set_light = lights_set_attention;
	} else {
		return -EINVAL;
	}

	pthread_once(&g_init, lights_init_globals);

	struct light_device_t *dev = malloc(sizeof(struct light_device_t));
	if(!dev)
		return -ENOMEM;

	memset(dev, 0, sizeof(*dev));

	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t *)module;
	dev->common.close = (int (*)(struct hw_device_t *))lights_close;
	dev->set_light = set_light;

	*device = (struct hw_device_t *)dev;

	return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open = lights_open,
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "Sony Lights Module",
    .author = "Diogo Ferreira <defer@cyanogenmod.com>, Alin Jerpelea "
	      "<jerpelea@gmail.com>, Caio Oliveira "
	      "<caiooliveirafarias0@gmail.com>",
    .methods = &lights_module_methods,
};
