/*
 * Copyright (C) 2011 Andreas Auras
 *
 * This file is part of DFAtmo the driver for 'Atmolight' controllers for XBMC and xinelib based video players.
 *
 * DFAtmo is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * DFAtmo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *

 * This is the DFAtmo xinelib post plugin.
 */


#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <errno.h>
#include <sys/time.h>

#include <xine/post.h>


#ifndef HAVE_XINE_GRAB_VIDEO_FRAME
#error xine-lib does not have df-xine-lib-extensions patch!
#endif

#include "atmodriver.h"

#define GRAB_TIMEOUT            100     /* max. time waiting for next grab image [ms] */
#define THREAD_RESPONSE_TIMEOUT 500000  /* timeout for thread state change [us] */

static char *filter_enum[NUM_FILTERS+1] = { "off", "percentage", "combined" };

START_PARAM_DESCR(atmo_parameters_t)
PARAM_ITEM(POST_PARAM_TYPE_BOOL, enabled, NULL, 0, 1, 0,
  "enable plugin")
PARAM_ITEM(POST_PARAM_TYPE_CHAR, driver, NULL, 0, 0, 0,
  "output driver")
PARAM_ITEM(POST_PARAM_TYPE_CHAR, driver_param, NULL, 0, 0, 0,
  "parameters for output driver")
PARAM_ITEM(POST_PARAM_TYPE_INT, top, NULL, 0, 25, 0,
  "number of areas at top border")
PARAM_ITEM(POST_PARAM_TYPE_INT, bottom, NULL, 0, 25, 0,
  "number of areas at bottom border")
PARAM_ITEM(POST_PARAM_TYPE_INT, left, NULL, 0, 25, 0,
  "number of areas at left border")
PARAM_ITEM(POST_PARAM_TYPE_INT, right, NULL, 0, 25, 0,
  "number of areas at right border")
PARAM_ITEM(POST_PARAM_TYPE_INT, center, NULL, 0, 1, 0,
  "activate center area")
PARAM_ITEM(POST_PARAM_TYPE_INT, top_left, NULL, 0, 1, 0,
  "activate top_left area")
PARAM_ITEM(POST_PARAM_TYPE_INT, top_right, NULL, 0, 1, 0,
  "activate top_right area")
PARAM_ITEM(POST_PARAM_TYPE_INT, bottom_left, NULL, 0, 1, 0,
  "activate bottom_left area")
PARAM_ITEM(POST_PARAM_TYPE_INT, bottom_right, NULL, 0, 1, 0,
  "activate bottom right area")
PARAM_ITEM(POST_PARAM_TYPE_INT, analyze_rate, NULL, 10, 500, 0,
  "analyze rate [ms]")
PARAM_ITEM(POST_PARAM_TYPE_INT, analyze_size, NULL, 0, 3, 0,
  "size of analyze image")
PARAM_ITEM(POST_PARAM_TYPE_INT, overscan, NULL, 0, 200, 0,
  "ignored overscan border of grabbed image [%1000]")
PARAM_ITEM(POST_PARAM_TYPE_INT, darkness_limit, NULL, 0, 100, 0,
  "limit for black pixel")
PARAM_ITEM(POST_PARAM_TYPE_INT, edge_weighting, NULL, 10, 200, 0,
  "power of edge weighting")
PARAM_ITEM(POST_PARAM_TYPE_INT, hue_win_size, NULL, 0, 5, 0,
  "hue windowing size")
PARAM_ITEM(POST_PARAM_TYPE_INT, sat_win_size, NULL, 0, 5, 0,
  "saturation windowing size")
PARAM_ITEM(POST_PARAM_TYPE_INT, hue_threshold, NULL, 0, 100, 0,
  "hue threshold [%]")
PARAM_ITEM(POST_PARAM_TYPE_INT, brightness, NULL, 50, 300, 0,
  "brightness [%]")
PARAM_ITEM(POST_PARAM_TYPE_BOOL, uniform_brightness, NULL, 0, 1, 0,
    "calculate uniform brightness")
PARAM_ITEM(POST_PARAM_TYPE_INT, filter, filter_enum, 0, NUM_FILTERS, 0,
  "filter mode")
PARAM_ITEM(POST_PARAM_TYPE_INT, filter_smoothness, NULL, 1, 100, 0,
  "filter smoothness [%]")
PARAM_ITEM(POST_PARAM_TYPE_INT, filter_length, NULL, 300, 5000, 0,
  "filter length [ms]")
PARAM_ITEM(POST_PARAM_TYPE_INT, filter_threshold, NULL, 1, 100, 0,
  "filter threshold [%]")
PARAM_ITEM(POST_PARAM_TYPE_INT, filter_delay, NULL, 0, 1000, 0,
  "delay for output send to controller [ms]")
PARAM_ITEM(POST_PARAM_TYPE_INT, wc_red, NULL, 0, 255, 0,
  "white calibration correction factor of red color channel")
PARAM_ITEM(POST_PARAM_TYPE_INT, wc_green, NULL, 0, 255, 0,
  "white calibration correction factor of green color channel")
PARAM_ITEM(POST_PARAM_TYPE_INT, wc_blue, NULL, 0, 255, 0,
  "white calibration correction factor of blue color channel")
PARAM_ITEM(POST_PARAM_TYPE_INT, gamma, NULL, 0, 30, 0,
  "gamma correction factor")
PARAM_ITEM(POST_PARAM_TYPE_INT, start_delay, NULL, 0, 5000, 0,
  "delay after stream start before first output is send [ms]")
END_PARAM_DESCR(atmo_param_descr)


typedef struct {
  post_class_t post_class;
  xine_t *xine;
} atmo_post_class_t;

enum { TS_STOP, TS_RUNNING, TS_SUSPEND, TS_SUSPENDED, TS_TICKET_REVOKED };

typedef struct atmo_post_plugin_s
{
    /* xine related */
  post_plugin_t post_plugin;
  xine_post_in_t parameter_input;
  post_video_port_t *port;
  pthread_mutex_t port_lock;

  /* thread related */
  int *grab_thread_state, *output_thread_state;
  pthread_t grab_thread, output_thread;
  pthread_mutex_t lock;
  pthread_cond_t thread_state_change;

  atmo_driver_t ad;
  atmo_parameters_t default_parm;

} atmo_post_plugin_t;


static int act_log_level = LOG_ERROR;
dfatmo_log_level_t dfatmo_log_level = &act_log_level;

static xine_t *xine_instance;

static void driver_log(int level, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  xine_log(xine_instance, XINE_LOG_PLUGIN, "DFAtmo: %s\n", buf);
}
dfatmo_log_t dfatmo_log = &driver_log;


static int build_post_api_parameter_string(char *buf, int size, xine_post_api_descr_t *descr, void *values, void *defaults) {
  xine_post_api_parameter_t *p = descr->parameter;
  int sep = 0;
  char arg[512];

  while (p->type != POST_PARAM_TYPE_LAST) {
    if (!p->readonly) {
      char *v = (char *)values + p->offset;
      char *d = (char *)defaults + p->offset;
      arg[0] = 0;
      switch (p->type) {
      case POST_PARAM_TYPE_INT:
      case POST_PARAM_TYPE_BOOL:
        if (*((int *)v) != *((int *)d))
          snprintf(arg, sizeof(arg), "%s=%d", p->name, *((int *)v));
        break;
      case POST_PARAM_TYPE_DOUBLE:
        if (*((double *)v) != *((double *)d))
          snprintf(arg, sizeof(arg), "%s=%f", p->name, *((double *)v));
        break;
      case POST_PARAM_TYPE_CHAR:
        if (strncmp(v, d, p->size))
          snprintf(arg, sizeof(arg), "%s=%.*s", p->name, p->size, v);
        break;
      }
      if (arg[0]) {
        int n = strlen(arg) + sep;
        if (size < n)
          break;
        if (sep)
          *buf = ',';
        strcpy(buf + sep, arg);
        buf += n;
        size -= n;
        sep = 1;
      }
    }
    ++p;
  }
  *buf = 0;
  return (sep);
}


static int parse_post_api_parameter_string(xine_post_api_descr_t *descr, void *values, char *param) {
  xine_post_api_parameter_t *p = descr->parameter;
  int changed = 0;

  while (p->type != POST_PARAM_TYPE_LAST) {
    if (!p->readonly) {
      char *arg = strstr(param, p->name);
      if (arg && arg[strlen(p->name)] == '=' && (arg == param || arg[-1] == ',' || isspace(arg[-1]))) {
        arg += strlen(p->name) + 1;
        char *v = (char *)values + p->offset;
        int iv;
        double dv;
        switch (p->type) {
        case POST_PARAM_TYPE_INT:
        case POST_PARAM_TYPE_BOOL:
          iv = atoi(arg);
          if (iv != *((int *)v)) {
            *((int *)v) = iv;
            changed = 1;
          }
          break;
        case POST_PARAM_TYPE_DOUBLE:
          dv = atof(arg);
          if (dv != *((double *)v)) {
            *((double *)v) = dv;
            changed = 1;
          }
          break;
        case POST_PARAM_TYPE_CHAR:
          while (isspace(*arg))
            ++arg;
          char *e = strchr(arg, ',');
          if (!e)
            e = arg + strlen(arg);
          while (e > arg && isspace(e[-1]))
            --e;
          int l = e - arg;
          if (l < (p->size - 1)) {
            if (l != strlen(v) || memcmp(arg, v, l)) {
              memset(v, 0, p->size);
              memcpy(v, arg, l);
              changed = 1;
            }
          }
          break;
        }
      }
    }
    ++p;
  }
  return (changed);
}


static int join_post_api_parameters(xine_post_api_descr_t *descr, void *dst, void *src) {
  xine_post_api_parameter_t *p = descr->parameter;
  int changed = 0;

  while (p->type != POST_PARAM_TYPE_LAST) {
    if (!p->readonly) {
      char *s = (char *)src + p->offset;
      char *d = (char *)dst + p->offset;

      switch (p->type) {
      case POST_PARAM_TYPE_INT:
      case POST_PARAM_TYPE_BOOL:
        if (*((int *)d) != *((int *)s)) {
          *((int *)d) = *((int *)s);
          changed = 1;
        }
        break;
      case POST_PARAM_TYPE_DOUBLE:
        if (*((double *)d) != *((double *)s)) {
          *((double *)d) = *((double *)s);
          changed = 1;
        }
        break;
      case POST_PARAM_TYPE_CHAR:
        if (strncmp(d, s, p->size)) {
          memcpy(d, s, p->size);
          changed = 1;
        }
        break;
      }
    }
    ++p;
  }
  return (changed);
}


static void calc_hsv_image_from_rgb(hsv_color_t *hsv, uint8_t *img, int img_size) {
  while (img_size--) {
    rgb_to_hsv(hsv, img[0], img[1], img[2]);
    ++hsv;
    img += 3;
  }
}


static void *atmo_grab_loop (void *this_gen) {
  atmo_post_plugin_t *this = (atmo_post_plugin_t *) this_gen;
  atmo_driver_t *ad = &this->ad;
  xine_ticket_t *ticket = this->post_plugin.running_ticket;
  post_video_port_t *port = NULL;
  xine_video_port_t *video_port = NULL;
  xine_grab_video_frame_t *frame = NULL;
  int rc;
  struct timeval tvnow, tvlast, tvdiff, tvtimeout;
  struct timespec ts;
  int thread_state = TS_RUNNING;

  pthread_mutex_lock(&this->lock);
  this->grab_thread_state = &thread_state;
  pthread_cond_broadcast(&this->thread_state_change);
  pthread_mutex_unlock(&this->lock);

  DFATMO_LOG(LOG_INFO, "grab thread running");

  ticket->acquire(ticket, 0);

  pthread_mutex_lock(&this->lock);

  gettimeofday(&tvlast, NULL);

  for (;;) {

      /* loop with analyze rate duration */
    tvdiff.tv_sec = 0;
    tvdiff.tv_usec = ad->active_parm.analyze_rate * 1000;
    timeradd(&tvlast, &tvdiff, &tvtimeout);
    gettimeofday(&tvnow, NULL);
    if (timercmp(&tvtimeout, &tvnow, >)) {
      ts.tv_sec  = tvtimeout.tv_sec;
      ts.tv_nsec = tvtimeout.tv_usec;
      ts.tv_nsec *= 1000;
      pthread_cond_timedwait(&this->thread_state_change, &this->lock, &ts);
      gettimeofday(&tvnow, NULL);
    }
    tvlast = tvnow;

    if (thread_state == TS_STOP)
      break;

    if (ticket->ticket_revoked || thread_state == TS_SUSPEND) {
        /* free grab frame */
      if (frame) {
        frame->dispose(frame);
        frame = NULL;
      }

      if (ticket->ticket_revoked) {
        DFATMO_LOG(LOG_INFO, "grab thread waiting for new ticket");

        thread_state = TS_TICKET_REVOKED;
        pthread_cond_broadcast(&this->thread_state_change);
        pthread_mutex_unlock(&this->lock);

        ticket->renew(ticket, 0);

        pthread_mutex_lock(&this->lock);
        if (thread_state == TS_STOP)
          break;

        thread_state = TS_RUNNING;
        pthread_cond_broadcast(&this->thread_state_change);

        DFATMO_LOG(LOG_INFO, "grab thread got new ticket (revoke=%d)", ticket->ticket_revoked);

        gettimeofday(&tvlast, NULL);
        continue;
      }

      thread_state = TS_SUSPENDED;
      pthread_cond_broadcast(&this->thread_state_change);

      DFATMO_LOG(LOG_INFO, "grab thread suspended");
    }

    if (thread_state == TS_SUSPENDED || !this->port)
      continue;

    if (port && port != this->port) {
      _x_post_dec_usage(port);
      port = NULL;
    }
    if (!port) {
      port = this->port;
      video_port = port->original_port;
      _x_post_inc_usage(port);
    }

      /* allocate grab frame */
    if (!frame) {
      frame = xine_new_grab_video_frame(port->stream);
      if (!frame) {
        DFATMO_LOG(LOG_ERROR, "frame grabbing not supported!");
        break;
      }

      DFATMO_LOG(LOG_INFO, "grab thread resumed");
    }

    pthread_mutex_unlock(&this->lock);

      /* get actual displayed image size */
    int grab_width = video_port->get_property(video_port, VO_PROP_WINDOW_WIDTH);
    int grab_height = video_port->get_property(video_port, VO_PROP_WINDOW_HEIGHT);
    if (grab_width > 0 && grab_height > 0) {

        /* calculate size of analyze image */
      int analyze_width = (ad->active_parm.analyze_size + 1) * 64;
      int analyze_height = (analyze_width * grab_height) / grab_width;

        /* calculate size of grab (sub) window */
      int overscan = ad->active_parm.overscan;
      if (overscan) {
        frame->crop_left = frame->crop_right = grab_width * overscan / 1000;
        frame->crop_top = frame->crop_bottom = grab_height * overscan / 1000;
        grab_width = grab_width - frame->crop_left - frame->crop_right;
        grab_height = grab_height - frame->crop_top - frame->crop_bottom;
      } else {
        frame->crop_bottom = 0;
        frame->crop_top = 0;
        frame->crop_left =  0;
        frame->crop_right = 0;
      }

        /* grab displayed video frame */
      frame->timeout = GRAB_TIMEOUT;
      frame->width = analyze_width;
      frame->height = analyze_height;
      frame->flags = XINE_GRAB_VIDEO_FRAME_FLAGS_CONTINUOUS | XINE_GRAB_VIDEO_FRAME_FLAGS_WAIT_NEXT;
      if (!(rc = frame->grab(frame))) {
        if (frame->width == analyze_width && frame->height == analyze_height) {
          int img_size = analyze_width * analyze_height;

            /* allocate hsv and weight images */
          if (img_size > ad->alloc_img_size) {
            free(ad->hsv_img);
            free(ad->weight);
            ad->alloc_img_size = 0;
            ad->hsv_img = (hsv_color_t *) malloc(img_size * sizeof(hsv_color_t));
            ad->weight = (uint8_t *) malloc(img_size * ad->sum_channels * sizeof(uint8_t));
            if (ad->hsv_img == NULL || ad->weight == NULL) {
              DFATMO_LOG(LOG_ERROR, "allocating image memory failed!");
              pthread_mutex_lock(&this->lock);
              break;
            }
            ad->alloc_img_size = img_size;
            ad->analyze_width = 0;
            ad->analyze_height = 0;
            ad->edge_weighting = 0;
          }
          ad->img_size = img_size;

            /* calculate weight image */
          int edge_weighting = ad->active_parm.edge_weighting;
          if (analyze_width != ad->analyze_width || analyze_height != ad->analyze_height || edge_weighting != ad->edge_weighting) {
            ad->edge_weighting = edge_weighting;
            ad->analyze_width = analyze_width;
            ad->analyze_height = analyze_height;
            calc_weight(ad);
            DFATMO_LOG(LOG_INFO, "analyze size %dx%d, grab %dx%d@%d,%d", analyze_width, analyze_height, grab_width, grab_height, frame->crop_left, frame->crop_top);
          }

            /* analyze grabbed image */
          calc_hsv_image_from_rgb(ad->hsv_img, frame->img, img_size);
          calc_hue_hist(ad);
          calc_windowed_hue_hist(ad);
          calc_most_used_hue(ad);
          calc_sat_hist(ad);
          calc_windowed_sat_hist(ad);
          calc_most_used_sat(ad);
          if (ad->active_parm.uniform_brightness)
            calc_uniform_average_brightness(ad);
          else
            calc_average_brightness(ad);
          pthread_mutex_lock(&this->lock);
          calc_rgb_values(ad);
          DFATMO_LOG(LOG_DEBUG, "grab %ld.%03ld: vpts=%ld", tvlast.tv_sec, tvlast.tv_usec / 1000, frame->vpts);
          continue;
        }
      } else {
        if (rc < 0)
          DFATMO_LOG(LOG_INFO, "grab failed!");
        if (rc > 0)
          DFATMO_LOG(LOG_DEBUG, "grab timed out!");
      }
    }

    pthread_mutex_lock(&this->lock);

#if 0
    {
      gettimeofday(&tvnow, NULL);
      timersub(&tvnow, &tvlast, &tvdiff);
      static uint64_t sum_us, peak_us, call_cnt;
      uint64_t diff_us = tvdiff.tv_sec * 1000000 + tvdiff.tv_usec;
      if (diff_us > peak_us)
        peak_us = diff_us;
      if (call_cnt >= (50*60*3) || peak_us >= 100000) {
        if (call_cnt)
          sum_us /= call_cnt;
        printf("%s: (%s:%d) peak: %.3lf ms  avg: %.3lf ms  calls: %d\n", LOG_MODULE, __XINE_FUNCTION__, __LINE__, ((double)peak_us)/1000.0, ((double)sum_us)/1000.0, (int)call_cnt);
        call_cnt = sum_us = peak_us = 0;
      }
      sum_us += diff_us;
      ++call_cnt;
    }
#endif
  }

  DFATMO_LOG(LOG_INFO, "grab thread terminating");

    /* free grab frame */
  if (frame)
    frame->dispose(frame);

  if (this->grab_thread_state == &thread_state)
    this->grab_thread_state = NULL;
  pthread_cond_broadcast(&this->thread_state_change);
  pthread_mutex_unlock(&this->lock);

  if (port)
    _x_post_dec_usage(port);

  ticket->release(ticket, 0);

  DFATMO_LOG(LOG_INFO, "grab thread terminated");

  return NULL;
}


static void *atmo_output_loop (void *this_gen) {
  atmo_post_plugin_t *this = (atmo_post_plugin_t *) this_gen;
  atmo_driver_t *ad = &this->ad;
  xine_ticket_t *ticket = this->post_plugin.running_ticket;
  post_video_port_t *port = NULL;
  int colors_size = 0, init = 1;
  struct timeval tvnow, tvlast, tvdiff, tvtimeout, tvfirst;
  struct timespec ts;
  int thread_state = TS_RUNNING;

  pthread_mutex_lock(&this->lock);
  this->output_thread_state = &thread_state;
  pthread_cond_broadcast(&this->thread_state_change);
  pthread_mutex_unlock(&this->lock);

  DFATMO_LOG(LOG_INFO, "output thread running");

  ticket->acquire(ticket, 0);

  pthread_mutex_lock(&this->lock);

  gettimeofday(&tvlast, NULL);

  for (;;) {

      /* Loop with output rate duration */
    tvdiff.tv_sec = 0;
    tvdiff.tv_usec = ad->active_parm.output_rate * 1000;
    timeradd(&tvlast, &tvdiff, &tvtimeout);
    gettimeofday(&tvnow, NULL);
    if (timercmp(&tvtimeout, &tvnow, >)) {
      ts.tv_sec  = tvtimeout.tv_sec;
      ts.tv_nsec = tvtimeout.tv_usec;
      ts.tv_nsec *= 1000;
      pthread_cond_timedwait(&this->thread_state_change, &this->lock, &ts);
      gettimeofday(&tvnow, NULL);
    }
    tvlast = tvnow;

    if (thread_state == TS_STOP)
      break;

    if (ticket->ticket_revoked || thread_state == TS_SUSPEND) {
      if (turn_lights_off(ad))
        DFATMO_LOG(LOG_ERROR, "output driver error: %s", ad->output_driver->errmsg);

      init = 1;

      if (ticket->ticket_revoked) {
        DFATMO_LOG(LOG_INFO, "output thread waiting for new ticket");

        thread_state = TS_TICKET_REVOKED;
        pthread_cond_broadcast(&this->thread_state_change);
        pthread_mutex_unlock(&this->lock);

        ticket->renew(ticket, 0);

        pthread_mutex_lock(&this->lock);
        if (thread_state == TS_STOP)
          break;

        thread_state = TS_RUNNING;
        pthread_cond_broadcast(&this->thread_state_change);

        DFATMO_LOG(LOG_INFO, "output thread got new ticket (revoke=%d)", ticket->ticket_revoked);

        gettimeofday(&tvlast, NULL);
        continue;
      }

      thread_state = TS_SUSPENDED;
      pthread_cond_broadcast(&this->thread_state_change);

      DFATMO_LOG(LOG_INFO, "output thread suspended");
    }

    if (thread_state == TS_SUSPENDED || !this->port)
      continue;

    if (port && port != this->port) {
      _x_post_dec_usage(port);
      port = NULL;
    }
    if (!port) {
      port = this->port;
      _x_post_inc_usage(port);
    }

    if (init) {
      init = 0;

      colors_size = ad->sum_channels * sizeof(rgb_color_t);
      reset_filters(ad);

      gettimeofday(&tvfirst, NULL);

      DFATMO_LOG(LOG_INFO, "output thread resumed");
    }

      /* Transfer analyzed colors into filtered colors */
    switch (ad->active_parm.filter) {
    case FILTER_PERCENTAGE:
      percent_filter(ad, ad->analyzed_colors);
      break;
    case FILTER_COMBINED:
      mean_filter(ad, ad->analyzed_colors);
      break;
    default:
        /* no filtering */
      memcpy(ad->filtered_colors, ad->analyzed_colors, colors_size);
    }

    pthread_mutex_unlock(&this->lock);

    timersub(&tvlast, &tvfirst, &tvdiff);
    if ((tvdiff.tv_sec * 1000 + tvdiff.tv_usec / 1000) >= ad->active_parm.start_delay) {

        /* Initialize delay filter queue */
      int filter_delay = ad->active_parm.filter_delay;
      int output_rate = ad->active_parm.output_rate;
      if (ad->filter_delay != filter_delay || ad->output_rate != output_rate) {
        free(ad->delay_filter_queue);
        ad->filter_delay = -1;
        ad->delay_filter_queue_length = ((filter_delay >= output_rate) ? filter_delay / output_rate + 1: 0) * ad->sum_channels;
        if (ad->delay_filter_queue_length) {
          ad->delay_filter_queue = (rgb_color_t *) calloc(ad->delay_filter_queue_length, sizeof(rgb_color_t));
          if (ad->delay_filter_queue == NULL) {
            DFATMO_LOG(LOG_ERROR, "allocating delay filter queue failed!");
            pthread_mutex_lock(&this->lock);
            break;
          }
        }
        else
          ad->delay_filter_queue = NULL;
        ad->filter_delay = filter_delay;
        ad->output_rate = output_rate;
        ad->delay_filter_queue_pos = 0;
      }

        /* Transfer filtered colors to output colors */
      if (ad->delay_filter_queue) {
        int outp = ad->delay_filter_queue_pos + ad->sum_channels;
        if (outp >= ad->delay_filter_queue_length)
          outp = 0;

        memcpy(&ad->delay_filter_queue[ad->delay_filter_queue_pos], ad->filtered_colors, colors_size);
        memcpy(ad->filtered_output_colors, &ad->delay_filter_queue[outp], colors_size);

        ad->delay_filter_queue_pos = outp;
      }
      else
        memcpy(ad->filtered_output_colors, ad->filtered_colors, colors_size);


      apply_gamma_correction(ad);
      apply_white_calibration(ad);

        /* Output colors */
      if (memcmp(ad->filtered_output_colors, ad->last_output_colors, colors_size)) {
        if (ad->output_driver->output_colors(ad->output_driver, ad->filtered_output_colors, ad->last_output_colors))
          DFATMO_LOG(LOG_ERROR, "output driver error: %s", ad->output_driver->errmsg);
        memcpy(ad->last_output_colors, ad->filtered_output_colors, colors_size);
      }
    }

    pthread_mutex_lock(&this->lock);

#if 0
    {
      gettimeofday(&tvnow, NULL);
      timersub(&tvnow, &tvlast, &tvdiff);
      static uint64_t sum_us, peak_us, call_cnt;
      uint64_t diff_us = tvdiff.tv_sec * 1000000 + tvdiff.tv_usec;
      if (diff_us > peak_us)
        peak_us = diff_us;
      if (call_cnt >= (50*60*3) || peak_us >= 100000) {
        if (call_cnt)
          sum_us /= call_cnt;
        printf("%s: (%s:%d) peak: %.3lf ms  avg: %.3lf ms  calls: %d\n", LOG_MODULE, __XINE_FUNCTION__, __LINE__, ((double)peak_us)/1000.0, ((double)sum_us)/1000.0, (int)call_cnt);
        call_cnt = sum_us = peak_us = 0;
      }
      sum_us += diff_us;
      ++call_cnt;
    }
#endif
  }

  DFATMO_LOG(LOG_INFO, "output thread terminating");

  if (this->output_thread_state == &thread_state)
    this->output_thread_state = NULL;
  pthread_cond_broadcast(&this->thread_state_change);
  pthread_mutex_unlock(&this->lock);

  if (port)
    _x_post_dec_usage(port);

  ticket->release(ticket, 0);

  DFATMO_LOG(LOG_INFO, "output thread terminated");

  return NULL;
}


static int wait_for_thread_state_change(atmo_post_plugin_t *this) {
  struct timeval tvnow, tvdiff, tvtimeout;
  struct timespec ts;

  /* calculate absolute timeout time */
  tvdiff.tv_sec = 0;
  tvdiff.tv_usec = THREAD_RESPONSE_TIMEOUT;
  gettimeofday(&tvnow, NULL);
  timeradd(&tvnow, &tvdiff, &tvtimeout);
  ts.tv_sec  = tvtimeout.tv_sec;
  ts.tv_nsec = tvtimeout.tv_usec;
  ts.tv_nsec *= 1000;

  if (pthread_cond_timedwait(&this->thread_state_change, &this->lock, &ts) == ETIMEDOUT) {
    DFATMO_LOG(LOG_ERROR, "timeout while waiting for thread state change!");
    return 0;
  }
  return 1;
}


static void start_threads(atmo_post_plugin_t *this) {

  pthread_mutex_lock(&this->lock);

  int grab_running = 0, output_running = 0;
  if (this->grab_thread_state == NULL || this->output_thread_state == NULL) {
    pthread_attr_t pth_attrs;
    pthread_attr_init(&pth_attrs);
    pthread_attr_setscope(&pth_attrs, PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setdetachstate(&pth_attrs, PTHREAD_CREATE_DETACHED);

    int err = 0;
    if (this->grab_thread_state == NULL) {
      if ((err = pthread_create (&this->grab_thread, &pth_attrs, atmo_grab_loop, this))) {
        DFATMO_LOG(LOG_ERROR, "can't create grab thread: %s", strerror(err));
        grab_running = 1;
      }
    }
    if (!err && this->output_thread_state == NULL) {
      if ((err = pthread_create (&this->output_thread, &pth_attrs, atmo_output_loop, this))) {
        DFATMO_LOG(LOG_ERROR, "can't create output thread: %s", strerror(err));
        output_running = 1;
      }
    }

    pthread_attr_destroy(&pth_attrs);
  }

  do {
    int changed = 0;
    if (!grab_running) {
      if (this->grab_thread_state) {
        if (*this->grab_thread_state != TS_TICKET_REVOKED && *this->grab_thread_state != TS_RUNNING) {
          *this->grab_thread_state = TS_RUNNING;
          changed = 1;
        }
        grab_running = 1;
      }
    }
    if (!output_running) {
      if (this->output_thread_state) {
        if (*this->output_thread_state != TS_TICKET_REVOKED && *this->output_thread_state != TS_RUNNING) {
          *this->output_thread_state = TS_RUNNING;
          changed = 1;
        }
        output_running = 1;
      }
    }
    if (changed)
      pthread_cond_broadcast(&this->thread_state_change);
  } while ((!grab_running || !output_running) && wait_for_thread_state_change(this));

  pthread_mutex_unlock(&this->lock);
}


static void suspend_threads(atmo_post_plugin_t *this) {

  pthread_mutex_lock(&this->lock);

  int grab_suspended = 0, output_suspended = 0;
  do {
    int changed = 0;
    if (!grab_suspended) {
      if (this->grab_thread_state) {
        if (*this->grab_thread_state == TS_SUSPENDED || *this->grab_thread_state == TS_TICKET_REVOKED) {
          grab_suspended = 1;
        } else if (*this->grab_thread_state != TS_SUSPEND) {
          *this->grab_thread_state = TS_SUSPEND;
          changed = 1;
        }
      } else {
        grab_suspended = 1;
      }
    }
    if (!output_suspended) {
      if (this->output_thread_state) {
        if (*this->output_thread_state == TS_SUSPENDED || *this->output_thread_state == TS_TICKET_REVOKED) {
          output_suspended = 1;
        } else if (*this->output_thread_state != TS_SUSPEND) {
          *this->output_thread_state = TS_SUSPEND;
          changed = 1;
        }
      } else {
        output_suspended = 1;
      }
    }
    if (changed)
      pthread_cond_broadcast(&this->thread_state_change);
  } while ((!grab_suspended || !output_suspended) && wait_for_thread_state_change(this));

  pthread_mutex_unlock(&this->lock);
}


static void stop_threads(atmo_post_plugin_t *this) {

  pthread_mutex_lock(&this->lock);

  int grab_stopped = 0, output_stopped = 0;
  do {
    int changed = 0;
    if (!grab_stopped) {
      if (this->grab_thread_state) {
        if (*this->grab_thread_state == TS_TICKET_REVOKED) {
          *this->grab_thread_state = TS_STOP;
          grab_stopped = 1;
        } else if (*this->grab_thread_state != TS_STOP) {
          *this->grab_thread_state = TS_STOP;
          changed = 1;
        }
      } else
        grab_stopped = 1;
    }
    if (!output_stopped) {
      if (this->output_thread_state) {
        if (*this->output_thread_state == TS_TICKET_REVOKED) {
          *this->output_thread_state = TS_STOP;
          output_stopped = 1;
        } else if (*this->output_thread_state != TS_STOP) {
          *this->output_thread_state = TS_STOP;
          changed = 1;
        }
      } else {
        output_stopped = 1;
      }
    }
    if (changed)
      pthread_cond_broadcast(&this->thread_state_change);
  } while ((!grab_stopped || !output_stopped) && wait_for_thread_state_change(this));

  this->grab_thread_state = NULL;
  this->output_thread_state = NULL;
  pthread_mutex_unlock(&this->lock);
}


static void configure(atmo_post_plugin_t *this) {
  atmo_driver_t *ad = &this->ad;

  if (!ad->parm.enabled ||
        strcmp(ad->active_parm.driver, ad->parm.driver) ||
        strcmp(ad->active_parm.driver_path, ad->parm.driver_path) ||
        strcmp(ad->active_parm.driver_param, ad->parm.driver_param)) {
    stop_threads(this);

    if (close_output_driver(ad))
      DFATMO_LOG(LOG_ERROR, "output driver error: %s", ad->output_driver->errmsg);
    else
      DFATMO_LOG(LOG_INFO, "output driver closed");

    if (strcmp(ad->active_parm.driver, ad->parm.driver)) {
      unload_output_driver(ad);
      DFATMO_LOG(LOG_INFO, "output driver unloaded");
    }
  }

  if (ad->parm.enabled) {
    atmo_parameters_t parm = ad->parm;
    int start = 1, send = 0;

    if (!ad->driver_opened) {
      if (ad->output_driver == NULL) {
#ifndef OUTPUT_DRIVER_PATH
#include <xine/plugin_catalog.h>

          /* get plugin path */
        char *plugin_filename = NULL, *sep = NULL, path[256];
        int len = 0;
        plugin_node_t *plugin_node = (plugin_node_t*) this->post_plugin.node;
        if (plugin_node != NULL && plugin_node->file != NULL)
          plugin_filename = plugin_node->file->filename;
        if (plugin_filename != NULL)
          sep = strrchr(plugin_filename, '/');
        if (sep)
          len = sep - plugin_filename;
        if (len > (sizeof(ad->parm.driver_path) - 8))
          len = 0;
        if (len > 0) {
          memcpy(ad->parm.driver_path, plugin_filename, len);
          ad->parm.driver_path[len] = 0;
          strcat(ad->parm.driver_path, "/dfatmo");
          DFATMO_LOG(LOG_INFO, "output driver search path: '%s'", ad->parm.driver_path);
        } else {
          DFATMO_LOG(LOG_ERROR, "could not determine plugin installation path!");
          start = 0;
        }
#endif
        if (start) {
          if (load_output_driver(ad)) {
            DFATMO_LOG(LOG_ERROR, "loading output driver fails!");
            start = 0;
          } else
            DFATMO_LOG(LOG_INFO, "output driver loaded");
        }
      }
      if (start && ad->output_driver->open(ad->output_driver, &ad->parm)) {
        DFATMO_LOG(LOG_ERROR, "output driver error: %s", ad->output_driver->errmsg);
        start = 0;
      }
      if (start) {
        ad->driver_opened = 1;
        send = 1;
        DFATMO_LOG(LOG_INFO, "output driver opened");
      }
    } else if (ad->output_driver->configure(ad->output_driver, &ad->parm)) {
      DFATMO_LOG(LOG_ERROR, "output driver error: %s", ad->output_driver->errmsg);
      start = 0;
    }

    if (join_post_api_parameters(&atmo_param_descr, &parm, &ad->parm)) {
      char buf[512];
      build_post_api_parameter_string(buf, sizeof(buf), &atmo_param_descr, &ad->parm, &this->default_parm);
      this->post_plugin.xine->config->update_string(this->post_plugin.xine->config, "post.dfatmo.parameters", buf);
    }

    if (ad->active_parm.top != ad->parm.top ||
                    ad->active_parm.bottom != ad->parm.bottom ||
                    ad->active_parm.left != ad->parm.left ||
                    ad->active_parm.right != ad->parm.right ||
                    ad->active_parm.center != ad->parm.center ||
                    ad->active_parm.top_left != ad->parm.top_left ||
                    ad->active_parm.top_right != ad->parm.top_right ||
                    ad->active_parm.bottom_left != ad->parm.bottom_left ||
                    ad->active_parm.bottom_right != ad->parm.bottom_right) {
      free_channels(ad);
      if (config_channels(ad)) {
        DFATMO_LOG(LOG_ERROR, "channel configuration fails!");
        start = 0;
      }
      send = 1;
    }

    ad->active_parm = ad->parm;

    if (!ad->sum_channels) {
      DFATMO_LOG(LOG_ERROR, "no channels configured!");
      start = 0;
    }

    if (start) {
        /* send first initial color packet */
      if (send && ad->output_driver->output_colors(ad->output_driver, ad->output_colors, NULL))
        DFATMO_LOG(LOG_ERROR, "output driver error: %s", ad->output_driver->errmsg);

      start_threads(this);
    } else
      stop_threads(this);
  }
}


/*
 * Open/Close video port
 */

static void atmo_video_open(xine_video_port_t *port_gen, xine_stream_t *stream) {
  post_video_port_t *port = (post_video_port_t *)port_gen;
  atmo_post_plugin_t *this = (atmo_post_plugin_t *) port->post;

  DFATMO_LOG(LOG_INFO, "video open");
  _x_post_rewire(port->post);
  _x_post_inc_usage(port);

  pthread_mutex_lock(&this->port_lock);
  (port->original_port->open) (port->original_port, stream);
  port->stream = stream;
  this->port = port;

  configure(this);
  pthread_mutex_unlock(&this->port_lock);
  DFATMO_LOG(LOG_INFO, "video opened");
}


static void atmo_video_close(xine_video_port_t *port_gen, xine_stream_t *stream) {
  post_video_port_t *port = (post_video_port_t *)port_gen;
  atmo_post_plugin_t *this = (atmo_post_plugin_t *) port->post;

  DFATMO_LOG(LOG_INFO, "video close");
  pthread_mutex_lock(&this->port_lock);
  suspend_threads(this);

  this->port = NULL;
  port->original_port->close(port->original_port, stream);
  port->stream = NULL;
  pthread_mutex_unlock(&this->port_lock);

  _x_post_dec_usage(port);
  DFATMO_LOG(LOG_INFO, "video closed");
}


/*
 *    Parameter functions
 */

static xine_post_api_descr_t *atmo_get_param_descr(void)
{
  return &atmo_param_descr;
}


static int atmo_set_parameters(xine_post_t *this_gen, void *parm_gen)
{
  atmo_post_plugin_t *this = (atmo_post_plugin_t *)this_gen;
  atmo_driver_t *ad = &this->ad;

  if (join_post_api_parameters(&atmo_param_descr, &ad->parm, parm_gen)) {
    char buf[512];
    build_post_api_parameter_string(buf, sizeof(buf), &atmo_param_descr, &ad->parm, &this->default_parm);
    this->post_plugin.xine->config->update_string(this->post_plugin.xine->config, "post.dfatmo.parameters", buf);
    DFATMO_LOG(LOG_INFO, "set parameters");

    pthread_mutex_lock(&this->port_lock);
    if (this->port) {
      if (ad->parm.enabled) {
        if (!ad->active_parm.enabled)
          configure(this);
        else
          instant_configure(ad);
      } else {
        if (ad->active_parm.enabled) {
          stop_threads(this);
          if (close_output_driver(ad))
            DFATMO_LOG(LOG_ERROR, "output driver error: %s", ad->output_driver->errmsg);
        }
      }
      ad->active_parm.enabled = ad->parm.enabled;
    }
    pthread_mutex_unlock(&this->port_lock);
  }

  return 1;
}


static int atmo_get_parameters(xine_post_t *this_gen, void *parm_gen)
{
  atmo_post_plugin_t *this = (atmo_post_plugin_t *)this_gen;
  atmo_parameters_t *parm = (atmo_parameters_t *)parm_gen;

  *parm  = this->ad.parm;
  return 1;
}


static char *atmo_get_help(void) {
  return _("DFAtmo post plugin\n"
           "Analyze video picture and generate output data for atmolight controllers\n"
           "\n"
         );
}


/*
 *    open/close plugin
 */

static void atmo_dispose(post_plugin_t *this_gen)
{
  atmo_post_plugin_t *this = (atmo_post_plugin_t *) this_gen;
  atmo_driver_t *ad = &this->ad;

  DFATMO_LOG(LOG_INFO, "dispose plugin");
  stop_threads(this);

  if (_x_post_dispose(this_gen)) {
    if (close_output_driver(ad))
      DFATMO_LOG(LOG_ERROR, "output driver error: %s", ad->output_driver->errmsg);
    unload_output_driver(ad);
    free_channels(ad);
    free(ad->hsv_img);
    free(ad->weight);
    free(ad->delay_filter_queue);
    pthread_mutex_destroy(&this->lock);
    pthread_mutex_destroy(&this->port_lock);
    pthread_cond_destroy(&this->thread_state_change);
    free(this);
    DFATMO_LOG(LOG_INFO, "final dispose");
  }
  DFATMO_LOG(LOG_INFO, "disposed plugin");
}


static post_plugin_t *atmo_open_plugin(post_class_t *class_gen,
					    int inputs,
					    xine_audio_port_t **audio_target,
					    xine_video_port_t **video_target)
{
  atmo_post_class_t *class = (atmo_post_class_t *) class_gen;
  post_in_t *input;
  post_out_t *output;
  post_video_port_t *port;
  xine_post_in_t *input_param;
  static xine_post_api_t post_api =
      { atmo_set_parameters, atmo_get_parameters,
        atmo_get_param_descr, atmo_get_help };

  DFATMO_LOG(LOG_INFO, "open plugin");

  if (!video_target || !video_target[0])
    return NULL;

  atmo_post_plugin_t *this = (atmo_post_plugin_t *) calloc(1, sizeof(atmo_post_plugin_t));
  if (!this)
    return NULL;

  atmo_driver_t *ad = &this->ad;

  _x_post_init(&this->post_plugin, 0, 1);
  this->post_plugin.xine = class->xine;

  port = _x_post_intercept_video_port(&this->post_plugin, video_target[0], &input, &output);

  input->xine_in.name   = "video in";
  output->xine_out.name = "video out";

  this->post_plugin.dispose = atmo_dispose;

  port->new_port.open = atmo_video_open;
  port->new_port.close = atmo_video_close;
  //port->port_lock = &this->port_lock;

  this->post_plugin.xine_post.video_input[0] = &port->new_port;

  input_param       = &this->parameter_input;
  input_param->name = "parameters";
  input_param->type = XINE_POST_DATA_PARAMETERS;
  input_param->data = &post_api;
  xine_list_push_back(this->post_plugin.input, input_param);

  pthread_mutex_init(&this->lock, NULL);
  pthread_mutex_init(&this->port_lock, NULL);
  pthread_cond_init(&this->thread_state_change, NULL);

  init_configuration(ad);
  reset_filters(ad);
  this->default_parm = ad->parm;

    /* Read parameters from xine configuration file */
  config_values_t *config = this->post_plugin.xine->config;
  char *param = config->register_string (config, "post.dfatmo.parameters", "",
                                                  "Parameters of DFAtmo post plugin",
                                                  NULL, 20, NULL, NULL);
  if (param)
    parse_post_api_parameter_string(&atmo_param_descr, &ad->parm, param);

  char buf[512];
  build_post_api_parameter_string(buf, sizeof(buf), &atmo_param_descr, &ad->parm, &this->default_parm);
  if (!param || strcmp(param, buf))
    config->update_string(config, "post.dfatmo.parameters", buf);

  DFATMO_LOG(LOG_INFO, "plugin opened");
  return &this->post_plugin;
}


/*
 *    Plugin class
 */

#if POST_PLUGIN_IFACE_VERSION < 10
static char *atmo_get_identifier(post_class_t *class_gen)
{
  return "dfatmo";
}

static char *atmo_get_description(post_class_t *class_gen)
{
  return "Analyze video picture and generate output data for atmolight controllers";
}

static void atmo_class_dispose(post_class_t *class_gen)
{
  free(class_gen);
}
#endif

static void *atmo_init_plugin(xine_t *xine, void *data)
{
  xine_instance = xine;
  switch (xine->verbosity) {
  case XINE_VERBOSITY_LOG:
    act_log_level = LOG_INFO;
    break;
  case XINE_VERBOSITY_DEBUG:
    act_log_level = LOG_DEBUG;
    break;
  default:
    act_log_level = LOG_ERROR;
  }

  atmo_post_class_t *class = (atmo_post_class_t *) calloc(1, sizeof(atmo_post_class_t));
  if (!class)
    return NULL;

  class->xine = xine;
  class->post_class.open_plugin     = atmo_open_plugin;
#if POST_PLUGIN_IFACE_VERSION < 10
  class->post_class.get_identifier  = atmo_get_identifier;
  class->post_class.get_description = atmo_get_description;
  class->post_class.dispose         = atmo_class_dispose;
#else
  class->post_class.identifier      = "dfatmo";
  class->post_class.description     = N_("Analyze video picture and generate output data for atmolight controllers");
  class->post_class.dispose         = default_post_class_dispose;
#endif
  return &class->post_class;
}


static post_info_t info = { XINE_POST_TYPE_VIDEO_FILTER };

const plugin_info_t xine_plugin_info[] __attribute__((visibility("default"))) =
{
  /* type, API, "name", version, special_info, init_function */
  { PLUGIN_POST, POST_PLUGIN_IFACE_VERSION, "dfatmo", XINE_VERSION_CODE, &info, &atmo_init_plugin },
  { PLUGIN_NONE, 0, "", 0, NULL, NULL }
};
