#pragma once

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct conn_list {
  struct conn_list *next;
  drmModeConnector *conn;
} conn_list;

struct drm_buf {
  uint32_t fb_id;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t size;
  uint32_t pitch;
  uint32_t handle;
  uint8_t *map;
};

struct drm_dev {
  int fd;
  uint32_t conn_id;
  uint32_t enc_id;
  uint32_t crtc_id;

  drmModeModeInfo mode;
  uint32_t front_buf;
  struct drm_buf bufs[2];
  drmModeCrtc *saved_crtc;
};

typedef struct drm_dev_list {
  struct drm_dev_list *next;
  struct drm_dev *dev;
} drm_dev_list;

int drm_dev_list_append(drm_dev_list **head, struct drm_dev *dev);

struct drm_manager {
  drm_dev_list *devs;
  conn_list *conns;
  drmModeConnector *activeConn;
  int dri_fd;
  drmModeRes *res;
};

struct connector {
  uint32_t id;
  char mode_str[64];
  drmModeModeInfo *mode;
  drmModeEncoder *encoder;
  int crtc;
  unsigned int fb_id[2], current_fb_id;
  struct timeval start;

  int swap_count;
};

void drm_manager_init(struct drm_manager *drm);

int registerConnectors(struct drm_manager *drm);

int drm_setup_dev(struct drm_manager *drm, struct drm_dev *dev,
                  drmModeConnector *conn);

int drm_open(struct drm_manager *drm, const char *node);
int drm_prepare(struct drm_manager *drm);
void drm_cleanup(struct drm_manager *drm);
void connector_find_mode(struct drm_manager *drm, struct connector *c);
int flip_buffer(struct drm_dev *dev);
