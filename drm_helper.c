#include "drm_helper.h"
#include "utils.h"

#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <sys/mman.h>

int valid_connector_id = 0;

void add_conn(conn_list *list, drmModeConnector *conn) {
    conn_list *current = list;
    while(current->next != NULL) {
        current = current->next;
    }
    current->next->conn = conn;
    current->next->next = NULL;
}

static int drm_find_crtc(struct drm_manager *drm,
                         struct drm_dev *dev,
                         drmModeConnector *conn);

static int drm_create_fb(struct drm_dev *dev, struct drm_buf *buf);

void drm_manager_init(struct drm_manager *drm) {
  drm->dev_list = NULL;
  drm->res = NULL;
}

int registerConnectors(struct drm_manager *drm) {
  drmModeConnector *conn;
  struct drm_dev *dev;
  int ret;

  /* retrieve resources */
  drm->res = drmModeGetResources(drm->dri_fd);
  drmModeRes *res = drm->res;
  if (!res) {
    fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n", errno);
    return -errno;
  }
  /* iterate all connectors */
  for (int i = 0; i < res->count_connectors; ++i) {
    /* get information for each connector */
    conn = drmModeGetConnector(drm->dri_fd, res->connectors[i]);
    if (!conn) {
      fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n", i,
              res->connectors[i], errno);
      continue;
    }
    /* create new device structure */
    dev = malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));
    dev->fd = drm->dri_fd;
    dev->conn_id = conn->connector_id;
    /* setup this connector */
    ret = drm_setup_dev(drm, dev, conn);
    if (ret) {
      if (ret != -ENOENT) {
        errno = -ret;
        fprintf(stderr, "cannot setup device for connector %u:%u (%d): %m\n", i,
                res->connectors[i], errno);
      }
      free(dev);
      drmModeFreeConnector(conn);
      continue;
    }
    /* success */
    /* free connector data and link device into global list */
    drmModeFreeConnector(conn);
    dev->next = drm->dev_list;
    drm->dev_list = dev;
  }

  return 0;
}

int drm_setup_dev(struct drm_manager *drm,
                  struct drm_dev *dev, drmModeConnector *conn) {
  int ret;
  /* check if a monitor is connected */
  if (conn->connection != DRM_MODE_CONNECTED) {
    fprintf(stderr, "ignoring unused connector %u\n", conn->connector_id);
    return -ENOENT;
  }

  /* check if there is at least one valid mode */
  if (conn->count_modes == 0) {
    fprintf(stderr, "no valid mode for connector %u\n", conn->connector_id);
    return -EFAULT;
  }
 
  /* copy the mode information into our device buffer structure */
  /* TODO: this is always first mode here. selection would be nice
   */
  memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
  dev->bufs[0].width = conn->modes[0].hdisplay;
  dev->bufs[0].height = conn->modes[0].vdisplay;
  dev->bufs[1].width = conn->modes[0].hdisplay;
  dev->bufs[1].height = conn->modes[0].vdisplay;
  fprintf(stderr, "mode for connector %u is %ux%u\n", conn->connector_id,
          dev->bufs[0].width, dev->bufs[0].height);

  /* find a crtc for this connector */
  ret = drm_find_crtc(drm, dev, conn);
  if (ret) {
    fprintf(stderr, "no valid crtc for connector %u\n", conn->connector_id);
    return ret;
  }

  /* create a framebuffer for this CRTC */
  ret = drm_create_fb(dev, &dev->bufs[0]);
  if (ret) {
    fprintf(stderr, "cannot create framebuffer for connector %u\n",
            conn->connector_id);
    return ret;
  }
  /* create framebuffer #2 for this CRTC */
  ret = drm_create_fb(dev, &dev->bufs[1]);
  if (ret) {
    fprintf(stderr, "cannot create framebuffer for connector %u\n",
            conn->connector_id);
    return ret;
  }
  return 0;
}

static int drm_find_crtc(struct drm_manager *drm,
                         struct drm_dev *dev, drmModeConnector *conn) {
  drmModeEncoder *enc;
  int32_t crtc;
  struct drm_dev *iter;

  /* first try the currently connected encoder+crtc */
  if (conn->encoder_id)
    enc = drmModeGetEncoder(dev->fd, conn->encoder_id);
  else
    enc = NULL;

  if (enc) {
    if (enc->crtc_id) {
      crtc = enc->crtc_id;
      for (iter = drm->dev_list; iter; iter = iter->next) {
        if (iter->crtc_id == crtc) {
          crtc = -1;
          break;
        }
      }

      if (crtc >= 0) {
        drmModeFreeEncoder(enc);
        dev->crtc_id = crtc;
        return 0;
      }
    }

    drmModeFreeEncoder(enc);
  }

  /* If the connector is not currently bound to an encoder or if the
   * encoder+crtc is already used by another connector (actually unlikely
   * but lets be safe), iterate all other available encoders to find a
   * matching CRTC. */
  for (int i = 0; i < conn->count_encoders; ++i) {
    enc = drmModeGetEncoder(dev->fd, conn->encoders[i]);
    if (!enc) {
      fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n", i,
              conn->encoders[i], errno);
      continue;
    }

    drmModeRes *res = drm->res;
    /* iterate all global CRTCs */
    for (int j = 0; j < res->count_crtcs; ++j) {
      /* check whether this CRTC works with the encoder */
      if (!(enc->possible_crtcs & (1 << j)))
        continue;

      /* check that no other device already uses this CRTC */
      crtc = res->crtcs[j];
      for (iter = drm->dev_list; iter; iter = iter->next) {
        if (iter->crtc_id == crtc) {
          crtc = -1;
          break;
        }
      }

      /* we have found a CRTC, so save it and return */
      if (crtc >= 0) {
        drmModeFreeEncoder(enc);
        dev->crtc_id = crtc;
        return 0;
      }
    }
    drmModeFreeEncoder(enc);
  }

  fprintf(stderr, "cannot find suitable CRTC for connector %u\n",
          conn->connector_id);
  return -ENOENT;
}

/* TODO: Only create the generic 'dumb buffer' type for now */
int drm_create_fb(struct drm_dev *dev, struct drm_buf *buf) {
  struct drm_mode_create_dumb creq;
  struct drm_mode_destroy_dumb dreq;
  struct drm_mode_map_dumb mreq;
  int ret;

  /* create dumb buffer */
  memset(&creq, 0, sizeof(creq));
  creq.width = buf->width;
  creq.height = buf->height;
  creq.bpp = 32;
  ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
  if (ret < 0) {
    fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
    return -errno;
  }
  buf->stride = creq.pitch;
  buf->size = creq.size;
  buf->handle = creq.handle;

  /* create framebuffer object for the dumb-buffer */
  ret = drmModeAddFB(dev->fd, buf->width, buf->height, 24, 32, buf->stride,
                     buf->handle, &buf->fb_id);
  if (ret) {
    fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
    ret = -errno;
    goto err_destroy;
  }

  /* prepare buffer for memory mapping */
  memset(&mreq, 0, sizeof(mreq));
  mreq.handle = buf->handle;
  ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
  if (ret) {
    fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
    ret = -errno;
    goto err_fb;
  }

  /* perform actual memory mapping */
  buf->map =
      mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, mreq.offset);
  if (buf->map == MAP_FAILED) {
    fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
    ret = -errno;
    goto err_fb;
  }

  /* clear the framebuffer to 0 */
  memset(buf->map, 0, buf->size);

  return 0;

err_fb:
  drmModeRmFB(dev->fd, buf->fb_id);
err_destroy:
  memset(&dreq, 0, sizeof(dreq));
  dreq.handle = buf->handle;
  drmIoctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
  return ret;
}

void drm_cleanup(struct drm_manager *drm) {
  struct drm_dev *iter;
  struct drm_mode_destroy_dumb dreq;

  while (drm->dev_list) {
    /* remove from global list */
    iter = drm->dev_list;
    drm->dev_list = iter->next;

    /* restore saved CRTC configuration */
    drmModeSetCrtc(drm->dri_fd, iter->saved_crtc->crtc_id, iter->saved_crtc->buffer_id,
                   iter->saved_crtc->x, iter->saved_crtc->y, &iter->conn_id, 1,
                   &iter->saved_crtc->mode);
    drmModeFreeCrtc(iter->saved_crtc);

    /* unmap buffer */
    munmap(iter->bufs[0].map, iter->bufs[0].size);

    /* delete framebuffer */
    drmModeRmFB(drm->dri_fd, iter->bufs[0].fb_id);

    /* delete dumb buffer */
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = iter->bufs[0].handle;
    drmIoctl(drm->dri_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

    /* free allocated memory */
    free(iter);
  }
  drmModeFreeResources(drm->res);
}
