/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "AoOutputPlugin.hxx"
#include "OutputAPI.hxx"

#include <ao/ao.h>
#include <glib.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ao"

/* An ao_sample_format, with all fields set to zero: */
static ao_sample_format OUR_AO_FORMAT_INITIALIZER;

static unsigned ao_output_ref;

struct AoOutput {
	struct audio_output base;

	size_t write_size;
	int driver;
	ao_option *options;
	ao_device *device;

	bool Initialize(const config_param *param, GError **error_r) {
		return ao_base_init(&base, &ao_output_plugin, param,
				    error_r);
	}

	void Deinitialize() {
		ao_base_finish(&base);
	}

	bool Configure(const config_param *param, GError **error_r);
};

static inline GQuark
ao_output_quark(void)
{
	return g_quark_from_static_string("ao_output");
}

static void
ao_output_error(GError **error_r)
{
	const char *error;

	switch (errno) {
	case AO_ENODRIVER:
		error = "No such libao driver";
		break;

	case AO_ENOTLIVE:
		error = "This driver is not a libao live device";
		break;

	case AO_EBADOPTION:
		error = "Invalid libao option";
		break;

	case AO_EOPENDEVICE:
		error = "Cannot open the libao device";
		break;

	case AO_EFAIL:
		error = "Generic libao failure";
		break;

	default:
		error = g_strerror(errno);
	}

	g_set_error(error_r, ao_output_quark(), errno,
		    "%s", error);
}

inline bool
AoOutput::Configure(const config_param *param, GError **error_r)
{
	const char *value;

	options = nullptr;

	write_size = config_get_block_unsigned(param, "write_size", 1024);

	if (ao_output_ref == 0) {
		ao_initialize();
	}
	ao_output_ref++;

	value = config_get_block_string(param, "driver", "default");
	if (0 == strcmp(value, "default"))
		driver = ao_default_driver_id();
	else
		driver = ao_driver_id(value);

	if (driver < 0) {
		g_set_error(error_r, ao_output_quark(), 0,
			    "\"%s\" is not a valid ao driver",
			    value);
		return false;
	}

	ao_info *ai = ao_driver_info(driver);
	if (ai == nullptr) {
		g_set_error(error_r, ao_output_quark(), 0,
			    "problems getting driver info");
		return false;
	}

	g_debug("using ao driver \"%s\" for \"%s\"\n", ai->short_name,
		config_get_block_string(param, "name", nullptr));

	value = config_get_block_string(param, "options", nullptr);
	if (value != nullptr) {
		gchar **_options = g_strsplit(value, ";", 0);

		for (unsigned i = 0; _options[i] != nullptr; ++i) {
			gchar **key_value = g_strsplit(_options[i], "=", 2);

			if (key_value[0] == nullptr || key_value[1] == nullptr) {
				g_set_error(error_r, ao_output_quark(), 0,
					    "problems parsing options \"%s\"",
					    _options[i]);
				return false;
			}

			ao_append_option(&options, key_value[0],
					 key_value[1]);

			g_strfreev(key_value);
		}

		g_strfreev(_options);
	}

	return true;
}

static struct audio_output *
ao_output_init(const config_param *param, GError **error_r)
{
	AoOutput *ad = new AoOutput();

	if (!ad->Initialize(param, error_r)) {
		delete ad;
		return nullptr;
	}

	if (!ad->Configure(param, error_r)) {
		ad->Deinitialize();
		delete ad;
		return nullptr;
	}

	return &ad->base;
}

static void
ao_output_finish(struct audio_output *ao)
{
	AoOutput *ad = (AoOutput *)ao;

	ao_free_options(ad->options);
	ad->Deinitialize();
	delete ad;

	ao_output_ref--;

	if (ao_output_ref == 0)
		ao_shutdown();
}

static void
ao_output_close(struct audio_output *ao)
{
	AoOutput *ad = (AoOutput *)ao;

	ao_close(ad->device);
}

static bool
ao_output_open(struct audio_output *ao, struct audio_format *audio_format,
	       GError **error)
{
	ao_sample_format format = OUR_AO_FORMAT_INITIALIZER;
	AoOutput *ad = (AoOutput *)ao;

	switch (audio_format->format) {
	case SAMPLE_FORMAT_S8:
		format.bits = 8;
		break;

	case SAMPLE_FORMAT_S16:
		format.bits = 16;
		break;

	default:
		/* support for 24 bit samples in libao is currently
		   dubious, and until we have sorted that out,
		   convert everything to 16 bit */
		audio_format->format = SAMPLE_FORMAT_S16;
		format.bits = 16;
		break;
	}

	format.rate = audio_format->sample_rate;
	format.byte_format = AO_FMT_NATIVE;
	format.channels = audio_format->channels;

	ad->device = ao_open_live(ad->driver, &format, ad->options);

	if (ad->device == nullptr) {
		ao_output_error(error);
		return false;
	}

	return true;
}

/**
 * For whatever reason, libao wants a non-const pointer.  Let's hope
 * it does not write to the buffer, and use the union deconst hack to
 * work around this API misdesign.
 */
static int ao_play_deconst(ao_device *device, const void *output_samples,
			   uint_32 num_bytes)
{
	union {
		const void *in;
		char *out;
	} u;

	u.in = output_samples;
	return ao_play(device, u.out, num_bytes);
}

static size_t
ao_output_play(struct audio_output *ao, const void *chunk, size_t size,
	       GError **error)
{
	AoOutput *ad = (AoOutput *)ao;

	if (size > ad->write_size)
		size = ad->write_size;

	if (ao_play_deconst(ad->device, chunk, size) == 0) {
		ao_output_error(error);
		return 0;
	}

	return size;
}

const struct audio_output_plugin ao_output_plugin = {
	"ao",
	nullptr,
	ao_output_init,
	ao_output_finish,
	nullptr,
	nullptr,
	ao_output_open,
	ao_output_close,
	nullptr,
	nullptr,
	ao_output_play,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
};
