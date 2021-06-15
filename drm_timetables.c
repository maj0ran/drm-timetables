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

#include "utils.h"
#include "draw.h"
#include "drm_helper.h"

static int drm_open(struct drm_manager *drm, const char *node);
static int drm_prepare(struct drm_manager *drm);
static void drm_cleanup(struct drm_manager *drm);

int drm_open(struct drm_manager *drm, const char *path) {
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
    return -ENOTSUP;
  }
  drm->dri_fd = fd;
  return fd;
}

static void drm_destroy_fb(int fd, struct drm_buf *buf) {
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

int main(int argc, char **argv) {
  int ret, dri_fd;
  const char *card;
  struct drm_dev *iter;
  struct drm_buf *buf;
  struct drm_manager drm;

  /* check which DRM device to open */
  if (argc > 1)
    card = argv[1];
  else
    card = "/dev/dri/card0";

  fprintf(stderr, "using card '%s'\n", card);

  drm_manager_init(&drm);

  /* open the DRM device */
  dri_fd = drm_open(&drm, card);
  if (!dri_fd)
    return 0;

  /* prepare all connectors and CRTCs */
  ret = registerConnectors(&drm);
  if (ret)
    goto out_close;

  /* perform actual modesetting on each found connector+CRTC */
  for (iter = drm.dev_list; iter; iter = iter->next) {
    iter->saved_crtc = drmModeGetCrtc(dri_fd, iter->crtc_id);
    buf = &iter->bufs[iter->front_buf];
    ret = drmModeSetCrtc(dri_fd, iter->crtc_id, buf->fb_id, 0, 0, &iter->conn_id, 1,
                         &iter->mode);
    if (ret)
      fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
              iter->conn_id, errno);
  }

  // draw the timetable
  for (iter = drm.dev_list; iter; iter = iter->next) {
  //    if(iter->conn_id != 32)
  //        continue;
    vec2 cpos;
    cpos.x = iter->mode.hdisplay / 2;
    cpos.y = iter->mode.vdisplay / 2;
    draw_tt(iter, cpos, cpos.y - 200, 200);
  }

  /* cleanup everything */
  drm_cleanup(&drm);

  ret = 0;

out_close:
  close(dri_fd);
out_return:
  if (ret) {
    errno = -ret;
    fprintf(stderr, "modeset failed with error %d: %m\n", errno);
  } else {
    fprintf(stderr, "exiting\n");
  }
  return ret;
}
