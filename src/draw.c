/*
 * Drawing Helper Functions.
 * Includes functions to draw pixel, line and cirlce onto the DRM buffer
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <math.h>

#include <draw.h>


/* Get a "next" color, that is, visually close to the previous color
 * to ensure a smooth gradually color-change
 */
static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod) {
  uint8_t next;

  next = cur + (*up ? 1 : -1) * (rand() % mod);
  if ((*up && next < cur) || (!*up && next > cur)) {
    *up = !*up;
    next = cur;
  }

  return next;
}

/* Set pixel at (x,y) coordinate to a given color
 */
void plot(struct drm_dev *dev, int x, int y, color color) {
  uint32_t off = dev->bufs[dev->front_buf ^ 1].stride * y + x * 4;
  *(uint32_t *)&dev->bufs[dev->front_buf ^ 1].map[off] =
      (color.r << 16) | (color.g << 8) | color.b;
}

void clear(struct drm_dev *dev) {
  uint32_t w = dev->bufs[dev->front_buf].width;
  uint32_t h = dev->bufs[dev->front_buf].height;

  memset(dev->bufs[dev->front_buf ^ 1].map, 0, w * h * 4);
}

/* Bresenham Algorithm to draw a rasterized line from one
 * point to another
 */
void draw_line(struct drm_dev *dev, vec2 p0, vec2 p1, color col) {
  vec2 d;
  d.x = abs(p1.x - p0.x);
  int sx = p0.x < p1.x ? 1 : -1;
  d.y = -abs(p1.y - p0.y);
  int sy = p0.y < p1.y ? 1 : -1;
  int err = d.x + d.y;
  int e2;
  while (1) {
    plot(dev, p0.x, p0.y, col);
    if (p0.x == p1.x && p0.y == p1.y)
      break;
    e2 = 2 * err;
    if (e2 >= d.y) {
      err += d.y;
      p0.x += sx;
    }
    if (e2 <= d.x) {
      err += d.x;
      p0.y += sy;
    }
  }
}

/* Bresenham Algorithm to draw an ellipse */
/* Note: for circle, we could implement a special version which would be
 * more efficient */
void draw_ellipse(struct drm_dev *dev, vec2 c, int a, int b, color col) {
  vec2 d;
  d.x = 0;
  d.y = b;
  int64_t a2 = a * a, b2 = b * b;
  int64_t err = b2 - (2 * b - 1) * a2, e2;
  do {
    plot(dev, c.x + d.x, c.y + d.y, col);
    plot(dev, c.x - d.x, c.y + d.y, col);
    plot(dev, c.x - d.x, c.y - d.y, col);
    plot(dev, c.x + d.x, c.y - d.y, col);

    e2 = 2 * err;
    if (e2 < (2 * d.x + 1) * b2) {
      d.x++;
      err += (2 * d.x + 1) * b2;
    }
    if (e2 > -(2 * d.y - 1) * a2) {
      d.y--;
      err -= (2 * d.y - 1) * a2;
    }
  } while (d.y >= 0);

  while (d.x++ < a) {
    plot(dev, c.x + d.x, c.y, col);
    plot(dev, c.x - d.x, c.y, col);
  }
}

void draw_tt(struct drm_dev *dev, vec2 pos, int r, size_t max_points) {

  vec2 p1;
  vec2 p2;
  double a = (M_PI * 2) / max_points;
  double step = 2.0;
  color c;
  srand(time(NULL));
  bool r_up, g_up, b_up;
  c.r = rand() % 0xff;
  c.g = rand() % 0xff;
  c.b = rand() % 0xff;
  r_up = g_up = b_up = true;
  while (step <= 200) {
    clear(dev);
    c.r = next_color(&r_up, c.r, 20);
    c.g = next_color(&g_up, c.g, 10);
    c.b = next_color(&b_up, c.b, 5);
    draw_ellipse(dev, pos, r, r, c);
    for (size_t i = 0; i < max_points; i++) {
      p1.x = pos.x + r * cos(a * i);
      p1.y = pos.y + r * sin(a * i);

      p2.x = pos.x + r * cos(a * ((int)(i * step) % max_points));
      p2.y = pos.y + r * sin(a * ((int)(i * step) % max_points));
      draw_line(dev, p1, p2, c);
    }
    flip_buffer(dev);
    step += 0.005;
  }
}
