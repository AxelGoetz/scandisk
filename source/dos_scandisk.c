#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

/**
 * Removes padding from both name and extension
 */
void removePadding(char *string, u_int8_t length) {
  int i;
  for(i = length; i > 0; i--) {
    if(string[i] == ' ') {
      string[i] = '\0';
    } else {
      break;
    }
  }
}

void follow_dir(uint16_t cluster, int indent,
  uint8_t *image_buf, struct bpb33* bpb) {
  struct direntry *dirent;
  int d, i;
  dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
  while (1) {
    for (d = 0; d < bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
        d += sizeof(struct direntry)) {
      char name[9];
      char extension[4];
      uint32_t size;
      uint16_t file_cluster;
      name[8] = ' ';
      extension[3] = ' ';
      memcpy(name, &(dirent->deName[0]), 8);
      memcpy(extension, dirent->deExtension, 3);

      if (name[0] == SLOT_EMPTY)
        return;

      if (((uint8_t)name[0]) == SLOT_DELETED)
        continue;

      removePadding(name, 8);
      removePadding(extension, 3);

      /* don't print "." or ".." directories */
      if (strcmp(name, ".") == 0 || strcmp(name, "..")) {
        dirent++;
        continue;
      } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
        file_cluster = getushort(dirent->deStartCluster);
        follow_dir(file_cluster, indent+2, image_buf, bpb);
      } else {
        size = getulong(dirent->deFileSize);
        // TODO: markFileCluster()
      }
      dirent++;
    }
    if (cluster == 0) {
      dirent++;
    } else {
      cluster = get_fat_entry(cluster, image_buf, bpb);
      dirent = (struct direntry*)cluster_to_addr(cluster,
        image_buf, bpb);
    }
  }
}

void usage() {
  fprintf(stderr, "Usage: dos_scandisk <imagename>\n");
  exit(1);
}

int main(int argc, char** argv) {
  uint8_t *image_buf;
  int fd;
  struct bpb33* bpb;

  if (argc != 2) {
    usage();
  }

  image_buf = mmap_file(argv[1], &fd);
  bpb = check_bootsector(image_buf);

  return 0;
}
