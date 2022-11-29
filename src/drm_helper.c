#include <asm-generic/errno-base.h>
#include <drm_helper.h>
#include <stdlib.h>
#include <utils.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <string.h>
#include <sys/mman.h>

int drm_dev_list_append(drm_dev_list **head, struct drm_dev *dev) {
  drm_dev_list *node = malloc(sizeof(drm_dev_list));
  if (node == NULL) {
    ERROR("could not allocate memory for drm_dev_list: %d", errno);
    return errno;
  }
  node->dev = dev;
  node->next = NULL;

  if (*head == NULL) {
    LOG("initialized empty list at %p\n", (void*)node);
    *head = node;
    return EXIT_SUCCESS;;
  }

  drm_dev_list *current = *head;
  while (current->next != NULL) {
    current = current->next;
  }
  LOG("appending node to list");
  current->next = node;

  return EXIT_SUCCESS;
}

int valid_connector_id = 0;

void add_conn(conn_list *list, drmModeConnector *conn) {
  conn_list *current = list;
  while (current->next != NULL) {
    current = current->next;
  }
  current->next->conn = conn;
  current->next->next = NULL;
}

static int drm_find_crtc(struct drm_manager *drm, struct drm_dev *dev,
                         drmModeConnector *conn);

static int drm_create_fb(struct drm_dev *dev, struct drm_buf *buf);

void drm_manager_init(struct drm_manager *drm) {
  drm->devs = NULL;
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
    drm_dev_list_append(&drm->devs, dev);
  }

  return 0;
}

int drm_setup_dev(struct drm_manager *drm, struct drm_dev *dev,
                  drmModeConnector *conn) {
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

static int drm_find_crtc(struct drm_manager *drm, struct drm_dev *dev,
                         drmModeConnector *conn) {
  drmModeEncoder *enc;
  int32_t crtc;

  /* first try the currently connected encoder+crtc */
  if (conn->encoder_id)
    enc = drmModeGetEncoder(dev->fd, conn->encoder_id);
  else
    enc = NULL;

  if (enc) {
    if (enc->crtc_id) {
      crtc = enc->crtc_id;
      for (struct drm_dev_list *iter = drm->devs; iter != NULL; iter = iter->next) {
        if (iter->dev->crtc_id == (uint32_t)crtc) {
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
      for (struct drm_dev_list *iter = drm->devs; iter; iter = iter->next) {
        struct drm_dev *dev = iter->dev;
        if (dev->crtc_id == (uint32_t)crtc) {
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
  buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd,
                  mreq.offset);
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

int flip_buffer(struct drm_dev *dev) {
  int ret =
      drmModeSetCrtc(dev->fd, dev->crtc_id, dev->bufs[dev->front_buf ^ 1].fb_id,
                     0, 0, &dev->conn_id, 1, &dev->mode);
  if (ret)
    fprintf(stderr, "cannot flip CRTC for connector %u (%d): %m\n",
            dev->conn_id, errno);
  else
    dev->front_buf ^= 1;
  return 0;
}

void drm_cleanup(struct drm_manager *drm) {
  struct drm_dev *iter;
  struct drm_mode_destroy_dumb dreq;

  drm_dev_list *devs = drm->devs;
  while (devs != NULL) {
    struct drm_dev *dev = devs->dev;
    /* remove from global list */
    drm->devs = devs->next;

    /* restore saved CRTC configuration */
    drmModeSetCrtc(drm->dri_fd, dev->saved_crtc->crtc_id,
                   dev->saved_crtc->buffer_id, dev->saved_crtc->x,
                   dev->saved_crtc->y, &dev->conn_id, 1,
                   &dev->saved_crtc->mode);
    drmModeFreeCrtc(dev->saved_crtc);

    /* unmap buffer */
    munmap(dev->bufs[0].map, dev->bufs[0].size);

    /* delete framebuffer */
    drmModeRmFB(drm->dri_fd, dev->bufs[0].fb_id);

    /* delete dumb buffer */
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = dev->bufs[0].handle;
    drmIoctl(drm->dri_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

    /* free allocated memory */
    free(dev);
  }
  drmModeFreeResources(drm->res);
}

int drm_open(struct drm_manager *drm, const char *path) {
  int fd, flags;
  uint64_t has_dumb;

  LOG("trying card: %s\n", path);

  fd = eopen(path, O_RDWR);
  if (fd <= 0)
    return EXIT_FAILURE;

  /* set FD_CLOEXEC flag */
  if ((flags = fcntl(fd, F_GETFD)) < 0 ||
      fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
    ERROR("fcntl FD_CLOEXEC failed");

  /* check capability */
  if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
    ERROR("drm device '%s' does not support dumb buffers\n", path);
    return -ENOTSUP;
  }
  drm->dri_fd = fd;
  return EXIT_SUCCESS;
}

void drm_destroy_fb(int fd, struct drm_buf *buf) {
  struct drm_mode_destroy_dumb dreq;

  /* unmap buffer */
  munmap(buf->map, buf->size);

  /* delete framebuffer */
  drmModeRmFB(fd, buf->fb_id);

  /* delete dumb buffer */
  memset(&dreq, 0, sizeof(dreq));
  dreq.handle = buf->handle;
  drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

void connector_find_mode(struct drm_manager *drm, struct connector *c) {
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
