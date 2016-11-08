/*
 * output.cpp
 * Output related routines
 *
 * Copyright (c) 2015-2016 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <math.h>
#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>
#include <shout/shout.h>
#include <lame/lame.h>
#include <syslog.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include "rtl_airband.h"

void shout_setup(icecast_data *icecast) {
	int ret;
	shout_t * shouttemp = shout_new();
	if (shouttemp == NULL) {
		printf("cannot allocate\n");
	}
	if (shout_set_host(shouttemp, icecast->hostname) != SHOUTERR_SUCCESS) {
		shout_free(shouttemp); return;
	}
	if (shout_set_protocol(shouttemp, SHOUT_PROTOCOL_HTTP) != SHOUTERR_SUCCESS) {
		shout_free(shouttemp); return;
	}
	if (shout_set_port(shouttemp, icecast->port) != SHOUTERR_SUCCESS) {
		shout_free(shouttemp); return;
	}
	char mp[100];
	sprintf(mp, "/%s", icecast->mountpoint);
	if (shout_set_mount(shouttemp, mp) != SHOUTERR_SUCCESS) {
		shout_free(shouttemp); return;
	}
	if (shout_set_user(shouttemp, icecast->username) != SHOUTERR_SUCCESS) {
		shout_free(shouttemp); return;
	}
	if (shout_set_password(shouttemp, icecast->password) != SHOUTERR_SUCCESS) {
		shout_free(shouttemp); return;
	}
	if (shout_set_format(shouttemp, SHOUT_FORMAT_MP3) != SHOUTERR_SUCCESS){
		shout_free(shouttemp); return;
	}
	if(icecast->name && shout_set_name(shouttemp, icecast->name) != SHOUTERR_SUCCESS) {
		shout_free(shouttemp); return;
	}
	if(icecast->genre && shout_set_genre(shouttemp, icecast->genre) != SHOUTERR_SUCCESS) {
		shout_free(shouttemp); return;
	}
	char samplerates[20];
	sprintf(samplerates, "%d", MP3_RATE);
	shout_set_audio_info(shouttemp, SHOUT_AI_SAMPLERATE, samplerates);
	shout_set_audio_info(shouttemp, SHOUT_AI_CHANNELS, "1");

	if (shout_set_nonblocking(shouttemp, 1) != SHOUTERR_SUCCESS) {
		log(LOG_ERR, "Error setting non-blocking mode: %s\n", shout_get_error(shouttemp));
		return;
	}
	ret = shout_open(shouttemp);
	if (ret == SHOUTERR_SUCCESS)
		ret = SHOUTERR_CONNECTED;

	if (ret == SHOUTERR_BUSY)
		log(LOG_NOTICE, "Connecting to %s:%d/%s...\n",
			icecast->hostname, icecast->port, icecast->mountpoint);

	while (ret == SHOUTERR_BUSY) {
		usleep(10000);
		ret = shout_get_connected(shouttemp);
	}

	if (ret == SHOUTERR_CONNECTED) {
		log(LOG_NOTICE, "Connected to %s:%d/%s\n",
			icecast->hostname, icecast->port, icecast->mountpoint);
		SLEEP(100);
		icecast->shout = shouttemp;
	} else {
		log(LOG_WARNING, "Could not connect to %s:%d/%s\n",
			icecast->hostname, icecast->port, icecast->mountpoint);
		shout_free(shouttemp);
		return;
	}
}

lame_t airlame_init() {
	lame_t lame = lame_init();
	if (!lame) {
		log(LOG_WARNING, "lame_init failed\n");
		return NULL;
	}

	lame_set_in_samplerate(lame, WAVE_RATE);
	lame_set_VBR(lame, vbr_mtrh);
	lame_set_brate(lame, 16);
	lame_set_quality(lame, 7);
	lame_set_out_samplerate(lame, MP3_RATE);
	lame_set_num_channels(lame, 1);
	lame_set_mode(lame, MONO);
	lame_init_params(lame);
	return lame;
}

class LameTone
{
	unsigned char* _data;
	int _bytes;

public:
	LameTone(int msec, unsigned int hz = 0) : _data(NULL), _bytes(0) {
		_data = (unsigned char *)malloc(LAMEBUF_SIZE);
		if (!_data) {
			log(LOG_WARNING, "LameTone: can't alloc %u bytes\n", LAMEBUF_SIZE);
			return;
		}

		int samples = (msec * WAVE_RATE) / 1000;
		float *buf = (float *)malloc(samples * sizeof(float));
		if (!buf) {
			log(LOG_WARNING, "LameTone: can't alloc %u samples\n", samples);
			return;
		}

		if (hz > 0) {
			const float period = 1.0 / (float)hz;
			const float sample_time = 1.0 / (float)WAVE_RATE;
			float t = 0;
			for (int i = 0; i < samples; ++i, t+= sample_time) {
				buf[i] = 0.9 * sinf(t * 2.0 * M_PI / period);
			}
		} else
			memset(buf, 0, samples * sizeof(float));

		lame_t lame = airlame_init();
		if (lame) {
			_bytes = lame_encode_buffer_ieee_float(lame, buf, NULL, samples, _data, LAMEBUF_SIZE);
			if (_bytes > 0) {
				int flush_ofs = _bytes;
				if (flush_ofs&0x1f)
					flush_ofs+= 0x20 - (flush_ofs&0x1f);
				if (flush_ofs < LAMEBUF_SIZE) {
					int flush_bytes = lame_encode_flush(lame, _data + flush_ofs, LAMEBUF_SIZE - flush_ofs);
					if (flush_bytes > 0) {
						memmove(_data + _bytes, _data + flush_ofs, flush_bytes);
						_bytes+= flush_bytes;
					}
				}
			}
			else
				log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", _bytes);
			lame_close(lame);
		}
		free(buf);
	}

	~LameTone() {
		if (_data)
			free(_data);
	}

	int write(FILE *f) {
		if (!_data || _bytes<=0)
			return 1;

		if(fwrite(_data, 1, _bytes, f) != (unsigned int)_bytes) {
			log(LOG_WARNING, "LameTone: failed to write %d bytes\n", _bytes);
			return -1;
		}

		return 0;
	}
};

static int fdata_open(file_data *fdata, const char *filename) {
	fdata->f = fopen(filename, fdata->append ? "a+" : "w");
	if(fdata->f == NULL)
		return -1;

	struct stat st = {0};
	if (!fdata->append || fstat(fileno(fdata->f), &st)!=0 || st.st_size == 0) {
		log(LOG_INFO, "Writing to %s\n", filename);
		return 0;
	}
	log(LOG_INFO, "Appending from pos %llu to %s\n", (unsigned long long)st.st_size, filename);

	//fill missing space with marker tones
	LameTone lt_a(120, 2222);
	LameTone lt_b(120, 1111);
	LameTone lt_c(120, 555);

	int r = lt_a.write(fdata->f);
	if (r==0) r = lt_b.write(fdata->f);
	if (r==0) r = lt_c.write(fdata->f);
	if (fdata->continuous) {
		time_t now = time(NULL);
		if (now > st.st_mtime ) {
			time_t delta = now - st.st_mtime;
			if (delta > 3600) {
				log(LOG_WARNING, "Too big time difference: %llu sec, limiting to one hour\n", (unsigned long long)delta);
				delta = 3600;
			}
			LameTone lt_silence(1000);
			for (; (r==0 && delta > 1); --delta)
				r = lt_silence.write(fdata->f);
		}
	}
	if (r==0) r = lt_c.write(fdata->f);
	if (r==0) r = lt_b.write(fdata->f);
	if (r==0) r = lt_a.write(fdata->f);

	if (r<0) fseek(fdata->f, st.st_size, SEEK_SET);
	return 0;
}

unsigned char lamebuf[LAMEBUF_SIZE];
void process_outputs(channel_t *channel, int cur_scan_freq) {
	int mp3_bytes;
	if(channel->need_mp3) {
		mp3_bytes = lame_encode_buffer_ieee_float(channel->lame, channel->waveout, NULL, WAVE_BATCH, lamebuf, LAMEBUF_SIZE);
		if (mp3_bytes < 0) {
			log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", mp3_bytes);
			return;
		} else if (mp3_bytes == 0)
			return;
	}
	for (int k = 0; k < channel->output_count; k++) {
		if(channel->outputs[k].enabled == false) continue;
		if(channel->outputs[k].type == O_ICECAST) {
			icecast_data *icecast = (icecast_data *)(channel->outputs[k].data);
			if(icecast->shout == NULL) continue;
			int ret = shout_send(icecast->shout, lamebuf, mp3_bytes);
			if (ret != SHOUTERR_SUCCESS || shout_queuelen(icecast->shout) > MAX_SHOUT_QUEUELEN) {
				if (shout_queuelen(icecast->shout) > MAX_SHOUT_QUEUELEN)
					log(LOG_WARNING, "Exceeded max backlog for %s:%d/%s, disconnecting\n",
						icecast->hostname, icecast->port, icecast->mountpoint);
				// reset connection
				log(LOG_WARNING, "Lost connection to %s:%d/%s\n",
					icecast->hostname, icecast->port, icecast->mountpoint);
				shout_close(icecast->shout);
				shout_free(icecast->shout);
				icecast->shout = NULL;
			} else if(icecast->send_scan_freq_tags && cur_scan_freq >= 0) {
				shout_metadata_t *meta = shout_metadata_new();
				char description[32];
				if(channel->labels[cur_scan_freq] != NULL)
					shout_metadata_add(meta, "song", channel->labels[cur_scan_freq]);
				else {
					snprintf(description, sizeof(description), "%.3f MHz", channel->freqlist[cur_scan_freq] / 1000000.0);
					shout_metadata_add(meta, "song", description);
				}
				shout_set_metadata(icecast->shout, meta);
				shout_metadata_free(meta);
			}
		} else if(channel->outputs[k].type == O_FILE) {
			file_data *fdata = (file_data *)(channel->outputs[k].data);
			if(fdata->continuous == false && channel->axcindicate == ' ' && channel->outputs[k].active == false) continue;
			time_t t = time(NULL);
			struct tm *tmp;
			if(use_localtime)
				tmp = localtime(&t);
			else
				tmp = gmtime(&t);

			char suffix[32];
			if(strftime(suffix, sizeof(suffix), "_%Y%m%d_%H.mp3", tmp) == 0) {
				log(LOG_NOTICE, "strftime returned 0\n");
				continue;
			}
			if(fdata->suffix == NULL || strcmp(suffix, fdata->suffix)) {	// need to open new file
				fdata->suffix = strdup(suffix);
				char *filename = (char *)malloc(strlen(fdata->dir) + strlen(fdata->prefix) + strlen(fdata->suffix) + 2);
				if(filename == NULL) {
					log(LOG_WARNING, "process_outputs: cannot allocate memory, output disabled\n");
					channel->outputs[k].enabled = false;
					continue;
				}
				sprintf(filename, "%s/%s%s", fdata->dir, fdata->prefix, fdata->suffix);
				if(fdata->f != NULL) {
					//todo: finalize file stream with lame_encode_flush_nogap
					fclose(fdata->f);
					fdata->f = NULL;
				}
				int r = fdata_open(fdata, filename);
				free(filename);
				if (r<0) {
					log(LOG_WARNING, "Cannot open output file %s (%s), output disabled\n", filename, strerror(errno));
					channel->outputs[k].enabled = false;
					continue;
				}
			}
// mp3_bytes is signed, but we've checked for negative values earlier
// so it's save to ignore the warning here
#pragma GCC diagnostic ignored "-Wsign-compare"
			if(fwrite(lamebuf, 1, mp3_bytes, fdata->f) < mp3_bytes) {
#pragma GCC diagnostic warning "-Wsign-compare"
				if(ferror(fdata->f))
					log(LOG_WARNING, "Cannot write to %s/%s%s (%s), output disabled\n",
						fdata->dir, fdata->prefix, fdata->suffix, strerror(errno));
				else
					log(LOG_WARNING, "Short write on %s/%s%s, output disabled\n",
						fdata->dir, fdata->prefix, fdata->suffix);
				fclose(fdata->f);
				fdata->f = NULL;
				channel->outputs[k].enabled = false;
			}
			channel->outputs[k].active = (channel->axcindicate != ' ');
		} else if(channel->outputs[k].type == O_MIXER) {
			mixer_data *mdata = (mixer_data *)(channel->outputs[k].data);
			mixer_put_samples(mdata->mixer, mdata->input, channel->waveout, WAVE_BATCH);
		}
	}
}

void disable_channel_outputs(channel_t *channel) {
	for (int k = 0; k < channel->output_count; k++) {
		output_t *output = channel->outputs + k;
		output->enabled = false;
		if(output->type == O_ICECAST) {
			icecast_data *icecast = (icecast_data *)(channel->outputs[k].data);
			if(icecast->shout == NULL) continue;
			log(LOG_WARNING, "Closing connection to %s:%d/%s\n",
				icecast->hostname, icecast->port, icecast->mountpoint);
			shout_close(icecast->shout);
			shout_free(icecast->shout);
			icecast->shout = NULL;
		} else if(output->type == O_MIXER) {
			mixer_data *mdata = (mixer_data *)(output->data);
			mixer_disable_input(mdata->mixer, mdata->input);
		}
	}
}

void disable_device_outputs(device_t *dev) {
	for(int j = 0; j < dev->channel_count; j++) {
		disable_channel_outputs(dev->channels + j);
	}
}

void* output_thread(void* params) {
	struct freq_tag tag;
	struct timeval tv;
	int new_freq = -1;
	struct timeval ts, te;

	if(DEBUG) gettimeofday(&ts, NULL);
	while (!do_exit) {
		pthread_cond_wait(&mp3_cond, &mp3_mutex);
		for (int i = 0; i < mixer_count; i++) {
			if(mixers[i].enabled == false) continue;
			channel_t *channel = &mixers[i].channel;
			if(channel->state == CH_READY) {
				process_outputs(channel, -1);
				channel->state = CH_DIRTY;
			}
		}
		if(DEBUG) {
			gettimeofday(&te, NULL);
			debug_bulk_print("mixeroutput: %lu.%lu %lu\n", te.tv_sec, te.tv_usec, (te.tv_sec - ts.tv_sec) * 1000000UL + te.tv_usec - ts.tv_usec);
			ts.tv_sec = te.tv_sec;
			ts.tv_usec = te.tv_usec;
		}
		for (int i = 0; i < device_count; i++) {
			device_t* dev = devices + i;
			if (!dev->failed && dev->waveavail) {
				dev->waveavail = 0;
				if(dev->mode == R_SCAN) {
					tag_queue_get(dev, &tag);
					if(tag.freq >= 0) {
						tag.tv.tv_sec += shout_metadata_delay;
						gettimeofday(&tv, NULL);
						if(tag.tv.tv_sec < tv.tv_sec || (tag.tv.tv_sec == tv.tv_sec && tag.tv.tv_usec <= tv.tv_usec)) {
							new_freq = tag.freq;
							tag_queue_advance(dev);
						}
					}
				}
				for (int j = 0; j < dev->channel_count; j++) {
					channel_t* channel = devices[i].channels + j;
					process_outputs(channel, new_freq);
					memcpy(channel->waveout, channel->waveout + WAVE_BATCH, AGC_EXTRA * 4);
				}
			}
// make sure we don't carry new_freq value to the next receiver which might be working
// in multichannel mode
			new_freq = -1;
		}
	}
	return 0;
}

// reconnect as required
void* icecast_check(void* params) {
	while (!do_exit) {
		SLEEP(10000);
		for (int i = 0; i < device_count; i++) {
			device_t* dev = devices + i;
			for (int j = 0; j < dev->channel_count; j++) {
				for (int k = 0; k < dev->channels[j].output_count; k++) {
					if(dev->channels[j].outputs[k].type != O_ICECAST)
						continue;
					icecast_data *icecast = (icecast_data *)(dev->channels[j].outputs[k].data);
					if(dev->failed) {
						if(icecast->shout) {
							log(LOG_WARNING, "Device #%d failed, disconnecting stream %s:%d/%s\n",
								i, icecast->hostname, icecast->port, icecast->mountpoint);
							shout_close(icecast->shout);
							shout_free(icecast->shout);
							icecast->shout = NULL;
						}
					} else {
						if (icecast->shout == NULL){
							log(LOG_NOTICE, "Trying to reconnect to %s:%d/%s...\n",
								icecast->hostname, icecast->port, icecast->mountpoint);
							shout_setup(icecast);
						}
					}
				}
			}
		}
		for (int i = 0; i < mixer_count; i++) {
			if(mixers[i].enabled == false) continue;
			for (int k = 0; k < mixers[i].channel.output_count; k++) {
				if(mixers[i].channel.outputs[k].enabled == false)
					continue;
				if(mixers[i].channel.outputs[k].type != O_ICECAST)
					continue;
				icecast_data *icecast = (icecast_data *)(mixers[i].channel.outputs[k].data);
				if(icecast->shout == NULL) {
					log(LOG_NOTICE, "Trying to reconnect to %s:%d/%s...\n",
						icecast->hostname, icecast->port, icecast->mountpoint);
					shout_setup(icecast);
				}
			}
		}
	}
	return 0;
}

// vim: ts=4