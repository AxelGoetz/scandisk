#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
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

/**
 * For a file, goes through the FAT and marks every cluster as
 * referenced.
 */
void mark_file_cluster(uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb, uint32_t bytes_remaining, bool *referenced_clusters) {
  referenced_clusters[cluster] = true;

  int total_clusters = bpb->bpbSectors / bpb->bpbSecPerClust;
  int clust_size = bpb->bpbSecPerClust * bpb->bpbBytesPerSec;

  if (cluster == 0) {
    fprintf(stderr, "Bad file termination\n");
    return;
  } else if (is_end_of_file(cluster) || bytes_remaining < clust_size) {
    return;
  } else if (cluster > total_clusters) {
    abort(); /* this shouldn't be able to happen */
  } else {
    /* more clusters after this one */
    mark_file_cluster(get_fat_entry(cluster, image_buf, bpb), image_buf,bpb, bytes_remaining - clust_size, referenced_clusters);
  }
}

/**
 * Loops through the directory structure and marks every cluster it sees as referenced (true).
 */
void find_referenced_clusters(uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb, bool *referenced_clusters) {
  struct direntry *dirent;
  int d, length = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
  dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
  while (1) {
    for (d = 0; d < length; d += sizeof(struct direntry)) {
      char name[9];
      name[8] = ' ';
      memcpy(name, &(dirent->deName[0]), 8);

      if (name[0] == SLOT_EMPTY) {
        return;
      }

      /* skip over deleted entries */
      if (((uint8_t)name[0]) == SLOT_DELETED)
        continue;

      referenced_clusters[cluster] = true;

      removePadding(name, 8);

      if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        dirent++; continue;
      }
      else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
        uint16_t file_cluster = getushort(dirent->deStartCluster);
        find_referenced_clusters(file_cluster, image_buf, bpb, referenced_clusters);
      } else if((dirent->deAttributes & ATTR_VOLUME) == 0) { // Not a volume
        uint16_t file_cluster = getushort(dirent->deStartCluster);
        uint32_t size = getulong(dirent->deFileSize);
        mark_file_cluster(file_cluster, image_buf, bpb, size, referenced_clusters);
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

/**
 * Extracts just the filename part for the file string
 */
char* extractFileName(char *file) {
  char *p2 = file;
  for (int i = 0; i < strlen(file); i++) {
    if (p2[i] == '/' || p2[i] == '\\') {
      file = p2+i+1;
    }
  }
  return p2;
}

/**
 * Updates the string to be uppercase
 */
void get_uppercase_string(char *string) {
  for (int i = 0; i < strlen(string); i++) {
    string[i] = toupper(string[i]);
  }
}

/**
 * Writes values into the directory entry
 */
void write_dirent(struct direntry *dirent, char *filename, uint16_t start_cluster, uint32_t size) {
  char *p, *p2;
  char *uppername;
  int len;

  memset(dirent, 0, sizeof(struct direntry));

  uppername = strdup(filename);
  p2 = extractFileName(uppername);
  get_uppercase_string(uppername);

  // set the file name and extension
  memset(dirent->deName, ' ', 8);
  p = strchr(uppername, '.');
  memcpy(dirent->deExtension, "___", 3);
  if (p == NULL) {
    fprintf(stderr, "No filename extension given - defaulting to .___\n");
  } else {
    *p = '\0';
    p++;
    len = strlen(p);
    if (len > 3) len = 3;
    memcpy(dirent->deExtension, p, len);
  }
  if (strlen(uppername) > 8) {
    uppername[8]='\0';
  }
  memcpy(dirent->deName, uppername, strlen(uppername));
  free(p2);

  // set the attributes and file size
  dirent->deAttributes = ATTR_NORMAL;
  putushort(dirent->deStartCluster, start_cluster);
  putulong(dirent->deFileSize, size);
}

/**
 * Creates a new file in the root directory
 */
void create_new_file(int cluster, uint8_t *image_buf, struct bpb33* bpb, char *filename, uint32_t size) {
  struct direntry *dirent = (struct direntry*) cluster_to_addr(0, image_buf, bpb);
  while(1) {
    if (dirent->deName[0] == SLOT_EMPTY) {
      write_dirent(dirent, filename, cluster, size);
      dirent++;

      // make sure the next dirent is set to be empty, just in case it wasn't before
      memset((uint8_t*)dirent, 0, sizeof(struct direntry));
      dirent->deName[0] = SLOT_EMPTY;
      return;
    }
    if (dirent->deName[0] == SLOT_DELETED) {
      // we found a deleted entry - we can just overwrite it
      write_dirent(dirent, filename, cluster, size);
      return;
    }
    dirent++;
  }
}

/**
 * Gets the size of the given file and marks the clusters as referenced
 */
uint32_t get_file_size(int cluster, uint8_t *image_buf, struct bpb33* bpb, bool *referenced_clusters) {
  uint32_t size = 0;
  referenced_clusters[cluster] = true;
  do {
    cluster = get_fat_entry(cluster, image_buf, bpb);
    referenced_clusters[cluster] = true;
    size++;
  } while (!is_end_of_file(cluster));

  return size;
}

/**
 * Displays the unreferenced clusters, as specified in the assignment
 */
void display_unreferenced_clusters(uint8_t *image_buf, struct bpb33* bpb, bool *referenced_clusters, int total_clusters) {
  bool title_displayed = false;
  for(int i = 2; i < total_clusters; i++) {
    if(referenced_clusters[i] == false && get_fat_entry(i, image_buf, bpb) != CLUST_FREE) {
      if(!title_displayed) { printf("Unreferenced: "); title_displayed = true; }
      printf("%i ", i);
    }
  }
  if(title_displayed) printf("\n");
}

void find_unreferenced_files(uint8_t *image_buf, struct bpb33* bpb, bool *referenced_clusters, int total_clusters) {
  uint8_t files_found = 1;

  for(int i = 2; i < total_clusters; i++) {
    if(referenced_clusters[i] == false && get_fat_entry(i, image_buf, bpb) != CLUST_FREE) {
      uint16_t size = get_file_size(i, image_buf, bpb, referenced_clusters);
      printf("Lost File: %i %i\n", i, size);

      // 12 char + '\0'
      char filename[13]; filename[0] = '\0';
      sprintf(filename, "%s%i%s", "found", files_found, ".dat");

      create_new_file(i, image_buf,bpb, filename, size);
      files_found++;
    }
  }
}


void usage() {
  fprintf(stderr, "Usage: dos_scandisk <imagename>\n");
  exit(1);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    usage();
  }

  int fd;
  uint8_t *image_buf = mmap_file(argv[1], &fd);
  struct bpb33 *bpb = check_bootsector(image_buf);


  int total_clusters = bpb->bpbSectors / bpb->bpbSecPerClust;
  bool *referenced_clusters = calloc(total_clusters, sizeof(bool));

  find_referenced_clusters(0, image_buf, bpb, referenced_clusters);
  display_unreferenced_clusters(image_buf, bpb, referenced_clusters, total_clusters);
  find_unreferenced_files(image_buf, bpb, referenced_clusters, total_clusters);


  free(referenced_clusters);
  return 0;
}
