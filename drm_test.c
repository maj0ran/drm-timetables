#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <math.h>

static const char *dri_path = "/dev/dri/card1";

struct drm_dev;
static int drm_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
                         struct drm_dev *dev);
static int drm_create_fb(int fd, struct drm_dev *dev);
static int drm_setup(int fd, drmModeRes *res, drmModeConnector *conn,
                     struct drm_dev *dev);
static int drm_open(const char *node);
static int drm_prepare(int fd);
static void drm_draw(void);
static void drm_cleanup(int fd);

void fatal(char *str) {
  fprintf(stderr, "%s\n", str);
  exit(EXIT_FAILURE);
}

void error(char *str) {
  perror(str);
  exit(EXIT_FAILURE);
}

int eopen(const char *path, int flag) {
  int fd;

  if ((fd = open(path, flag)) < 0) {
    fprintf(stderr, "cannot open \"%s\"\n", path);
    error("[open]:");
  }
  return fd;
}

void *emmap(int addr, size_t len, int prot, int flag, int fd, off_t offset) {
  uint32_t *fp;

  (void)addr;

  if ((fp = (uint32_t *)mmap(0, len, prot, flag, fd, offset)) == MAP_FAILED)
    error("mmap");
  return fp;
}

struct drm_buffer {
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

  //  uint32_t *buf;
  uint32_t conn_id;
  uint32_t enc_id;
  uint32_t crtc_id;

  drmModeModeInfo mode;
  unsigned int front_buf;
  struct drm_buffer bufs[2];

  drmModeCrtc *saved_crtc;
};

static struct drm_dev *dev_list = NULL;

int drm_open(const char *path) {
  int fd, flags;
  uint64_t has_dumb;

  fd = eopen(path, O_RDWR);

  /* set FD_CLOEXEC flag */
  if ((flags = fcntl(fd, F_GETFD)) < 0 ||
      fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
    fatal("fcntl FD_CLOEXEC failed");

  /* check capability */
  if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
    fprintf(stderr, "drm device '%s' does not support dumb buffers\n", path);
    return 0;
  }
  return fd;
}

static int drm_prepare(int fd) {
  drmModeRes *res;
  drmModeConnector *conn;
  struct drm_dev *dev;
  int ret;
  /* retrieve resources */
  res = drmModeGetResources(fd);
  if (!res) {
    fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n", errno);
    return -errno;
  }

  /* iterate all connectors */
  for (int i = 0; i < res->count_connectors; ++i) {
    /* get information for each connector */
    conn = drmModeGetConnector(fd, res->connectors[i]);
    if (!conn) {
      fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n", i,
              res->connectors[i], errno);
      continue;
    }

    /* create new device structure */
    dev = malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));
    dev->conn_id = conn->connector_id;

    /* setup this connector */
    ret = drm_setup(fd, res, conn, dev);
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
    /* free connector data and link device into global list */
    drmModeFreeConnector(conn);
    dev->next = dev_list;
    dev_list = dev;
  }

  /* free resources again */
  drmModeFreeResources(res);
  return 0;
}

int drm_setup(int fd, drmModeRes *res, drmModeConnector *conn,
              struct drm_dev *dev) {
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
  memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
  dev->bufs[0].width = conn->modes[0].hdisplay;
  dev->bufs[0].height = conn->modes[0].vdisplay;
  fprintf(stderr, "mode for connector %u is %ux%u\n", conn->connector_id,
          dev->bufs[0].width, dev->bufs[0].height);

  /* find a crtc for this connector */
  ret = drm_find_crtc(fd, res, conn, dev);
  if (ret) {
    fprintf(stderr, "no valid crtc for connector %u\n", conn->connector_id);
    return ret;
  }

  /* create a framebuffer for this CRTC */
  ret = drm_create_fb(fd, dev);
  if (ret) {
    fprintf(stderr, "cannot create framebuffer for connector %u\n",
            conn->connector_id);
    return ret;
  }

  return 0;
}

static int drm_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
                         struct drm_dev *dev) {
  drmModeEncoder *enc;
  int32_t crtc;
  struct drm_dev *iter;

  /* first try the currently conected encoder+crtc */
  if (conn->encoder_id)
    enc = drmModeGetEncoder(fd, conn->encoder_id);
  else
    enc = NULL;

  if (enc) {
    if (enc->crtc_id) {
      crtc = enc->crtc_id;
      for (iter = dev_list; iter; iter = iter->next) {
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
    enc = drmModeGetEncoder(fd, conn->encoders[i]);
    if (!enc) {
      fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n", i,
              conn->encoders[i], errno);
      continue;
    }

    /* iterate all global CRTCs */
    for (int j = 0; j < res->count_crtcs; ++j) {
      /* check whether this CRTC works with the encoder */
      if (!(enc->possible_crtcs & (1 << j)))
        continue;

      /* check that no other device already uses this CRTC */
      crtc = res->crtcs[j];
      for (iter = dev_list; iter; iter = iter->next) {
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
int drm_create_fb(int fd, struct drm_dev *dev) {
  struct drm_mode_create_dumb creq;
  struct drm_mode_destroy_dumb dreq;
  struct drm_mode_map_dumb mreq;
  int ret;

  /* create dumb buffer */
  memset(&creq, 0, sizeof(creq));
  creq.width = dev->bufs[0].width;
  creq.height = dev->bufs[0].height;
  creq.bpp = 32;
  ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
  if (ret < 0) {
    fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
    return -errno;
  }
  dev->bufs[0].stride = creq.pitch;
  dev->bufs[0].size = creq.size;
  dev->bufs[0].handle = creq.handle;

  /* create framebuffer object for the dumb-buffer */
  ret = drmModeAddFB(fd, dev->bufs[0].width, dev->bufs[0].height, 24, 32,
                     dev->bufs[0].stride, dev->bufs[0].handle,
                     &dev->bufs[0].fb_id);
  if (ret) {
    fprintf(stderr, "cannobufs[0].t create framebuffer (%d): %m\n", errno);
    ret = -errno;
    goto err_destroy;
  }

  /* prepare buffer for memory mapping */
  memset(&mreq, 0, sizeof(mreq));
  mreq.handle = dev->bufs[0].handle;
  ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
  if (ret) {
    fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
    ret = -errno;
    goto err_fb;
  }

  /* perform actual memory mapping */
  dev->bufs[0].map = mmap(0, dev->bufs[0].size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, mreq.offset);
  if (dev->bufs[0].map == MAP_FAILED) {
    fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
    ret = -errno;
    goto err_fb;
  }

  /* clear the framebuffer to 0 */
  memset(dev->bufs[0].map, 0, dev->bufs[0].size);

  return 0;

err_fb:
  drmModeRmFB(fd, dev->bufs[0].fb_id);
err_destroy:
  memset(&dreq, 0, sizeof(dreq));
  dreq.handle = dev->bufs[0].handle;
  drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
  return ret;
}

static void drm_cleanup(int fd) {
  struct drm_dev *iter;
  struct drm_mode_destroy_dumb dreq;

  while (dev_list) {
    /* remove from global list */
    iter = dev_list;
    dev_list = iter->next;

    /* restore saved CRTC configuration */
    drmModeSetCrtc(fd, iter->saved_crtc->crtc_id, iter->saved_crtc->buffer_id,
                   iter->saved_crtc->x, iter->saved_crtc->y, &iter->conn_id, 1,
                   &iter->saved_crtc->mode);
    drmModeFreeCrtc(iter->saved_crtc);

    /* unmap buffer */
    munmap(iter->bufs[0].map, iter->bufs[0].size);

    /* delete framebuffer */
    drmModeRmFB(fd, iter->bufs[0].fb_id);

    /* delete dumb buffer */
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = iter->bufs[0].handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

    /* free allocated memory */
    free(iter);
  }
}

static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod) {
  uint8_t next;

  next = cur + (*up ? 1 : -1) * (rand() % mod);
  if ((*up && next < cur) || (!*up && next > cur)) {
    *up = !*up;
    next = cur;
  }

  return next;
}

#define LINE_DEBUG(MSG) printf("%s:%d : %s\n", __func__, __LINE__, MSG);

/* Drawing Helper Functions */

#define BLACK 0x000000
#define RED 0xFF0000

typedef struct {
  int x;
  int y;
} vec2;

void plot(struct drm_dev *dev, int x, int y, uint32_t color) {
  uint32_t off = dev->bufs[0].stride * y + x * 4;
  *(uint32_t *)&dev->bufs[0].map[off] = color;
}

void clear(struct drm_dev *dev) {
  uint32_t w = dev->bufs[dev->front_buf].width;
  uint32_t h = dev->bufs[dev->front_buf].height;

  memset(dev->bufs[0].map, 0, w * h * 4);
}

void draw_line(struct drm_dev *dev, vec2 p0, vec2 p1) {
  vec2 d;
  d.x = abs(p1.x - p0.x);
  int sx = p0.x < p1.x ? 1 : -1;
  d.y = -abs(p1.y - p0.y);
  int sy = p0.y < p1.y ? 1 : -1;
  int err = d.x + d.y;
  int e2;
  while (1) {
    plot(dev, p0.x, p0.y, RED);
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

void draw_ellipse(struct drm_dev *dev, vec2 c, int a, int b) {
  vec2 d;
  d.x = 0;
  d.y = b;
  int64_t a2 = a * a, b2 = b * b;
  int64_t err = b2 - (2 * b - 1) * a2, e2;
  do {
    plot(dev, c.x + d.x, c.y + d.y, 0xFFFFFF);
    plot(dev, c.x - d.x, c.y + d.y, 0x0077FF);
    plot(dev, c.x - d.x, c.y - d.y, RED);
    plot(dev, c.x + d.x, c.y - d.y, 0x00FF00);

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
    plot(dev, c.x + d.x, c.y, RED);
    plot(dev, c.x - d.x, c.y, RED);
  }
}

void draw_tt(struct drm_dev *dev, vec2 pos, int r, size_t max_points) {

  vec2 p1;
  vec2 p2;
  double a = (M_PI * 2) / max_points;
  double step = 2.0;
  while (step <= 100.0) {
    draw_ellipse(dev, pos, r, r);
    for (size_t i = 0; i < max_points; i++) {
      p1.x = pos.x + r * cos(a * i);
      p1.y = pos.y + r * sin(a * i);

      p2.x = pos.x + r * cos(a * ((int)(i * step) % max_points));
      p2.y = pos.y + r * sin(a * ((int)(i * step) % max_points));
      draw_line(dev, p1, p2);
    }
    clear(dev);
    step += 0.1;
  }
}

static void drm_draw(void) {
  uint8_t r, g, b;
  bool r_up, g_up, b_up;
  unsigned int i, y, x, off;
  struct drm_dev *iter;

  srand(time(NULL));
  r = rand() % 0xff;
  g = rand() % 0xff;
  b = rand() % 0xff;
  r_up = g_up = b_up = true;

  for (i = 0; i < 50; ++i) {
    r = next_color(&r_up, r, 20);
    g = next_color(&g_up, g, 10);
    b = next_color(&b_up, b, 5);

    for (iter = dev_list; iter; iter = iter->next) {
      for (y = 0; y < iter->bufs[0].height; ++y) {
        for (x = 0; x < iter->bufs[0].width; ++x) {
          off = iter->bufs[0].stride * y + x * 4;
          // *(uint32_t *)&iter->bufs[0].map[off] = 0xFF0000;
          plot(iter, x, y, (r << 16) | (g << 8) | b);
        }
      }
    }

    usleep(1e6 * 2);
  }
}

int main(int argc, char **argv) {
  int ret, fd;
  const char *card;
  struct drm_dev *iter;

  /* check which DRM device to open */
  if (argc > 1)
    card = argv[1];
  else
    card = "/dev/dri/card1";

  fprintf(stderr, "using card '%s'\n", card);

  /* open the DRM device */
  fd = drm_open(card);
  if (!fd)
    return 0;

  /* prepare all connectors and CRTCs */
  ret = drm_prepare(fd);
  if (ret)
    goto out_close;

  /* perform actual modesetting on each found connector+CRTC */
  for (iter = dev_list; iter; iter = iter->next) {
    iter->saved_crtc = drmModeGetCrtc(fd, iter->crtc_id);
    ret = drmModeSetCrtc(fd, iter->crtc_id, iter->bufs[0].fb_id, 0, 0,
                         &iter->conn_id, 1, &iter->mode);
    if (ret)
      fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
              iter->conn_id, errno);
  }

  /* draw some colors for 5seconds */
  for (iter = dev_list; iter; iter = iter->next) {
    vec2 cpos;
    cpos.x = iter->bufs[0].width / 2;
    cpos.y = iter->bufs[0].height / 2;
    vec2 p1;
    p1.x = 0, p1.y = 0;
    vec2 p2;
    p2.x = 3840 - 1;
    p2.y = 2160 - 1;
    // drm_draw();
    //draw_ellipse(iter, cpos, 1000, 500);
    //        draw_line(iter, p1, p2);
     draw_tt(iter, cpos, 1000, 200);
  }
  usleep(1e6 * 3);

  /* cleanup everything */
  drm_cleanup(fd);

  ret = 0;

out_close:
  close(fd);
out_return:
  if (ret) {
    errno = -ret;
    fprintf(stderr, "modeset failed with error %d: %m\n", errno);
  } else {
    fprintf(stderr, "exiting\n");
  }
  return ret;
}
