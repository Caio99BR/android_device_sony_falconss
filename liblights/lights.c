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

#define LOG_TAG "lights.sony"

#define BARLED "sys.lights.barled"

#include <cutils/log.h>
#include <cutils/properties.h>
#include <errno.h>
#include <fcntl.h>
#include <hardware/lights.h>
#include <malloc.h>
#include <pthread.h>
#include <stdbool.h>
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

// Backlight
char const *const LCD_FILE = "/sys/class/leds/lcd-backlight/brightness";

// SNS/Bar Led
char const *const SNS_LED_FILE =
    "/sys/class/leds/lm3533-light-sns/rgb_brightness";

// Notification Led
char const *const RED_LED_FILE = "/sys/class/leds/red/brightness";
char const *const GREEN_LED_FILE = "/sys/class/leds/green/brightness";
char const *const BLUE_LED_FILE = "/sys/class/leds/notification/brightness";

bool property_get_bool(const char *key, bool default_value)
{
	if (!key) {
		return default_value;
	}

	bool result = default_value;
	char buf[PROP_VALUE_MAX] = {
	    '\0',
	};

	int len = property_get(key, buf, "");
	if (len == 1) {
		char ch = buf[0];
		if (ch == '0' || ch == 'n') {
			result = false;
		} else if (ch == '1' || ch == 'y') {
			result = true;
		}
	} else if (len > 1) {
		if (!strcmp(buf, "no") || !strcmp(buf, "false") ||
		    !strcmp(buf, "off")) {
			result = false;
		} else if (!strcmp(buf, "yes") || !strcmp(buf, "true") ||
			   !strcmp(buf, "on")) {
			result = true;
		}
	}

	return result;
}

static int write_int(char const *path, int value)
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
static int is_lit(struct light_state_t const *state)
{
	return state->color & 0x00FFFFFF;
}

static int rgb_to_brightness(struct light_state_t const *state)
{
	int color = state->color & 0x00FFFFFF;

	return ((77 * ((color >> 16) & 0x00FF)) +
		(150 * ((color >> 8) & 0x00FF)) + (29 * (color & 0x00FF))) >>
	       8;
}

/* The actual lights controlling section */
static int set_light_backlight(struct light_device_t *dev,
			       struct light_state_t const *state)
{
	int err = 0;
	int brightness = rgb_to_brightness(state);
	pthread_mutex_lock(&g_lock);
	err = write_int(LCD_FILE, brightness);
	pthread_mutex_unlock(&g_lock);

	return err;
}

static void set_shared_light_locked(struct light_device_t *dev,
				    struct light_state_t const *state)
{
	int red, green, blue, rgb;

	red = (state->color >> 16) & 0x00FF;
	green = (state->color >> 8) & 0x00FF;
	blue = state->color & 0x00FF;

	write_int(RED_LED_FILE, red);
	write_int(GREEN_LED_FILE, green);
	write_int(BLUE_LED_FILE, blue);

	bool barled = property_get_bool(BARLED, true);
	if (barled) {
		rgb = ((red & 0x00FF) << 16) | ((green & 0x00FF) << 8) |
		      (blue & 0x00FF);
		write_int(SNS_LED_FILE, rgb);
	} else {
		write_int(SNS_LED_FILE, 0);
	}

	return 0;
}

static void handle_shared_locked(struct light_device_t *dev)
{
	if (is_lit(&g_battery)) {
		set_shared_light_locked(dev, &g_battery);
	} else {
		set_shared_light_locked(dev, &g_notification);
	}
}

static int set_light_battery(struct light_device_t *dev,
			     struct light_state_t const *state)
{
	pthread_mutex_lock(&g_lock);
	g_battery = *state;
	handle_shared_locked(dev);
	pthread_mutex_unlock(&g_lock);

	return 0;
}

static int set_light_notifications(struct light_device_t *dev,
				   struct light_state_t const *state)
{
	pthread_mutex_lock(&g_lock);
	g_notification = *state;
	handle_shared_locked(dev);
	pthread_mutex_unlock(&g_lock);

	return 0;
}

/* Initializations */
void init_globals(void) { pthread_mutex_init(&g_lock, NULL); }

/* Close the lights device */
static int close_lights(struct light_device_t *dev)
{
	if (dev) {
		free(dev);
	}

	return 0;
}

/* Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t *module, char const *name,
		       struct hw_device_t **device)
{
	int (*set_light)(struct light_device_t * dev,
			 struct light_state_t const *state);

	if (0 == strcmp(LIGHT_ID_BACKLIGHT, name)) {
		set_light = set_light_backlight;
	} else if (0 == strcmp(LIGHT_ID_BATTERY, name)) {
		set_light = set_light_battery;
	} else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name)) {
		set_light = set_light_notifications;
	} else {
		return -EINVAL;
	}

	pthread_once(&g_init, init_globals);

	struct light_device_t *dev = malloc(sizeof(struct light_device_t));
	if(!dev)
		return -ENOMEM;

	memset(dev, 0, sizeof(*dev));

	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t *)module;
	dev->common.close = (int (*)(struct hw_device_t *))close_lights;
	dev->set_light = set_light;

	*device = (struct hw_device_t *)dev;

	return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open = open_lights,
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
