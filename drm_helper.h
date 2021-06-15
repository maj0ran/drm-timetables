#pragma once

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct conn_list {
  drmModeConnector *conn;
  struct conn_list *next;
} conn_list;

struct drm_manager {
  struct drm_dev *dev_list;
  conn_list *conns;
  drmModeConnector *activeConn;
  int dri_fd;
  drmModeRes *res;
};

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
  struct drm_dev *next;
  int fd;
  //  uint32_t *buf;
  uint32_t conn_id;
  uint32_t enc_id;
  uint32_t crtc_id;

  drmModeModeInfo mode;
  uint32_t front_buf;
  struct drm_buf bufs[2];
  drmModeCrtc *saved_crtc;
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


static void connector_find_mode(struct drm_manager *drm, struct connector *c) {
  drmModeConnector *connector;
  int i, j;
  drmModeRes *resources = drm->res;
  /* First, find the connector & mode */
  c->mode = NULL;
  for (i = 0; i < resources->count_connectors; i++) {
    connector = drmModeGetConnector(drm->dri_fd, resources->connectors[i]);

    if (!connector) {
      fprintf(stderr, "could not get connector %i: %s\n",
              resources->connectors[i], strerror(errno));
      drmModeFreeConnector(connector);
      continue;
    }

    if (!connector->count_modes) {
      drmModeFreeConnector(connector);
      continue;
    }

    if (connector->connector_id != c->id) {
      drmModeFreeConnector(connector);
      continue;
    }

    for (j = 0; j < connector->count_modes; j++) {
      c->mode = &connector->modes[j];
      if (!strcmp(c->mode->name, c->mode_str))
        break;
    }

    /* Found it, break out */
    if (c->mode)
      break;

    drmModeFreeConnector(connector);
  }

  if (!c->mode) {
    fprintf(stderr, "failed to find mode \"%s\"\n", c->mode_str);
    return;
  }

  /* Now get the encoder */
  for (i = 0; i < resources->count_encoders; i++) {
    c->encoder = drmModeGetEncoder(drm->dri_fd, resources->encoders[i]);

    if (!c->encoder) {
      fprintf(stderr, "could not get encoder %i: %s\n", resources->encoders[i],
              strerror(errno));
      drmModeFreeEncoder(c->encoder);
      continue;
    }

    if (c->encoder->encoder_id == connector->encoder_id)
      break;

    drmModeFreeEncoder(c->encoder);
  }

  if (c->crtc == -1)
    c->crtc = c->encoder->crtc_id;
}
