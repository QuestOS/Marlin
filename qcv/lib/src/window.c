/*                    The Quest Operating System
 *  Copyright (C) 2005-2012  Richard West, Boston University
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "window.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <video.h>

int qcv_create_window(qcv_window_t* window)
{
  int res;
  if((res = enable_video(1, &window->video_memory, DISTRIBUTED_COLOR_PALLETE)) < 0) {
    return res;
  }
  window->height = 200;
  window->width = 320;
  return 0;
}

int qcv_window_display_frame(qcv_window_t* window, qcv_frame_t* frame)
{
  int x, y;
  unsigned int r, g, b;
  int width = window->width < qcv_frame_width(frame) ? window->width : qcv_frame_width(frame);
  int height = window->height < qcv_frame_height(frame) ? window->height : qcv_frame_height(frame);
  unsigned char* double_buffer = malloc(window->height * window->width);

  if(!double_buffer) return -1;

  memset(double_buffer, 0, window->height * window->width);
  
  switch(frame->type) {
  case QCV_FRAME_TYPE_3BYTE_RGB:
    for(y = 0; y < height; ++y) {
      for(x = 0; x < width; ++x) {
        
        r = (unsigned int)round(qcv_frame_element(frame, unsigned char,
                                                  (x + y * frame->pixel_matrix.width), 0) / 51.0);
        g = (unsigned int)round(qcv_frame_element(frame, unsigned char,
                                                  (x + y * frame->pixel_matrix.width), 1) / 51.0);
        b = (unsigned int)round(qcv_frame_element(frame, unsigned char,
                                                  (x + y * frame->pixel_matrix.width), 2) / 51.0);
        double_buffer[x + y * window->width] = r + 6*g + 36*b;
      }
    }
    break;

  case QCV_FRAME_TYPE_1BYTE_GREY:
    for(y = 0; y < height; ++y) {
      for(x = 0; x < width; ++x) {
        r = (unsigned int)round(qcv_frame_element(frame, unsigned int,
                                                  (x + y * frame->pixel_matrix.width), 0) / 51.0);
        double_buffer[x + y * window->width] = r + 6*r + 36*r;
      }
    }
    break;

  default:
    free(double_buffer);
    return -1;
  }
  memcpy(window->video_memory, double_buffer, window->height * window->width);
  free(double_buffer);
  return 0;
}

int qcv_destroy_window(qcv_window_t* window)
{
  int res;
  if((res = enable_video(1, &window->video_memory, 0)) < 0) {
    return res;
  }
  window->video_memory = NULL;
  window->height = 0;
  window->width = 0;
  
  return 0;
}


/*
 * Local Variables:
 * indent-tabs-mode: nil
 * mode: C
 * c-file-style: "gnu"
 * c-basic-offset: 2
 * End:
 */

/* vi: set et sw=2 sts=2: */
