#pragma once

#include "drm_helper.h"

typedef struct {
  int r;
  int g;
  int b;
} color;


typedef struct {
  int x;
  int y;
} vec2;

void clear(struct drm_dev *dev);
void plot(struct drm_dev *dev, int x, int y, color color);
void draw_line(struct drm_dev *dev, vec2 p0, vec2 p1, color col);
void draw_ellipse(struct drm_dev *dev, vec2 c, int a, int b, color col);
void draw_tt(struct drm_dev *dev, vec2 pos, int r, size_t max_points);

