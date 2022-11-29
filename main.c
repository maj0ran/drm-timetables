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

#include <draw.h>
#include <drm_helper.h>
#include <utils.h>

int find_valid_card(struct drm_manager *drm) {
#define MAX_CARDS 3
#define BUF_SIZE 32
  char buf[BUF_SIZE];
  for (size_t card_no = 3; card_no < 4; card_no--) {
    snprintf(buf, BUF_SIZE, "/dev/dri/card%u", card_no);
    int ret = drm_open(drm, buf);
    if (EXIT_SUCCESS == ret) {
      LOG("using card: %s\n", buf);
      return EXIT_SUCCESS;
    }
  }
  return EXIT_FAILURE;
}

int main(int argc, char **argv) {
  int ret, dri_fd;
  struct drm_buf *buf;
  struct drm_manager drm;

  drm_manager_init(&drm);
  if (argc == 1)
    find_valid_card(&drm);
  else
    drm_open(&drm, argv[1]);

  dri_fd = drm.dri_fd;
  /* prepare all connectors and CRTCs */
  ret = registerConnectors(&drm);
  if (ret) {
    ERROR("failed to register connnectors");
    goto out_close;
  }
  /* perform actual modesetting on each found connector+CRTC */
  for (drm_dev_list *iter = drm.devs; iter; iter = iter->next) {
    struct drm_dev *dev = iter->dev;
    dev->saved_crtc = drmModeGetCrtc(dri_fd, dev->crtc_id);
    buf = &dev->bufs[dev->front_buf];
    ret = drmModeSetCrtc(dri_fd, dev->crtc_id, buf->fb_id, 0, 0, &dev->conn_id,
                         1, &dev->mode);
    if (ret)
      fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
              dev->conn_id, errno);
  }

  // draw the timetable
  for (drm_dev_list *iter = drm.devs; iter; iter = iter->next) {
    struct drm_dev *dev = iter->dev;
    vec2 cpos;
    cpos.x = dev->mode.hdisplay / 2;
    cpos.y = dev->mode.vdisplay / 2;
    draw_tt(iter->dev, cpos, cpos.y - 10, 200);
  }

  /* cleanup everything */
  drm_cleanup(&drm);

  ret = 0;
out_close:
  close(dri_fd);
  if (ret) {
    errno = -ret;
    fprintf(stderr, "modeset failed with error %d: %m\n", errno);
  } else {
    fprintf(stderr, "exiting\n");
  }
  return ret;
}
