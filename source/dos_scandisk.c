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
 * Gets the file name
 */
void get_name(char *fullname, struct direntry *dirent)
{
  char name[9];
  char extension[4];

  name[8] = ' ';
  extension[3] = ' ';
  memcpy(name, &(dirent->deName[0]), 8);
  memcpy(extension, dirent->deExtension, 3);

  removePadding(name, 8);
  removePadding(extension, 3);

  fullname[0] = '\0';
  strcat(fullname, name);

  /* append the extension if it's not a directory */
  if ((dirent->deAttributes & ATTR_DIRECTORY) == 0) {
    strcat(fullname, ".");
    strcat(fullname, extension);
  }
}

/**
  * find_file seeks through the directories in the memory disk image,
  * until it finds the named file
  */
#define FIND_FILE 0
#define FIND_DIR 1

struct direntry* find_file(char *infilename, uint16_t cluster,
  int find_mode, uint8_t *image_buf, struct bpb33* bpb)
{
  char buf[MAXPATHLEN];
  char *seek_name, *next_name;
  int d;
  struct direntry *dirent;
  uint16_t dir_cluster;
  char fullname[13];

  /* find the first dirent in this directory */
  dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

  /* first we need to split the file name we're looking for into the
     first part of the path, and the remainder.  We hunt through the
     current directory for the first part.  If there's a remainder,
     and what we find is a directory, then we recurse, and search
     that directory for the remainder */

  strncpy(buf, infilename, MAXPATHLEN);
  seek_name = buf;

  /* trim leading slashes */
  while (*seek_name == '/' || *seek_name == '\\') {
    seek_name++;
  }

  /* search for any more slashes - if so, it's a dirname */
  next_name = seek_name;
  while (1) {
    if (*next_name == '/' || *next_name == '\\') {
      *next_name = '\0';
      next_name ++;
      break;
    }
    if (*next_name == '\0') {
      /* end of name - no slashes found */
      next_name = NULL;
      if (find_mode == FIND_DIR) {
        return dirent;
      }
      break;
    }
    next_name++;
  }

  while (1) {
    /* hunt a cluster for the relevant dirent.  If we reach the
       end of the cluster, we'll need to go to the next cluster
       for this directory */
    for (d = 0; d < bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
      d += sizeof(struct direntry)) {
      if (dirent->deName[0] == SLOT_EMPTY) {
        /* we failed to find the file */
        return NULL;
      }
      if (dirent->deName[0] == SLOT_DELETED) {
        /* skip over a deleted file */
        dirent++;
        continue;
      }
      get_name(fullname, dirent);
      if (strcmp(fullname, seek_name) == 0) {
        /* found it! */
        if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
          /* it's a directory */
          if (next_name == NULL) {
            fprintf(stderr, "Cannot copy out a directory\n");
            exit(1);
          }
          dir_cluster = getushort(dirent->deStartCluster);
          return find_file(next_name, dir_cluster,
           find_mode, image_buf, bpb);
        } else if ((dirent->deAttributes & ATTR_VOLUME) != 0) {
          /* it's a volume */
          fprintf(stderr, "Cannot copy out a volume\n");
          exit(1);
        } else {
          /* assume it's a file */
          return dirent;
        }
      }
      dirent++;
    }
    /* we've reached the end of the cluster for this directory.
       Where's the next cluster? */
    if (cluster == 0) {
      // root dir is special
      dirent++;
    } else {
      cluster = get_fat_entry(cluster, image_buf, bpb);
      dirent = (struct direntry*)cluster_to_addr(cluster,
       image_buf, bpb);
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
  } else if (is_end_of_file(cluster)) {
    return;
  } else if (cluster > total_clusters) {
    abort(); /* this shouldn't be able to happen */
  }
  /* more clusters after this one */
  mark_file_cluster(get_fat_entry(cluster, image_buf, bpb), image_buf, bpb, bytes_remaining - clust_size, referenced_clusters);
}

/**
 * Loops through the directory structure and marks every cluster it sees as referenced (true).
 */
void find_referenced_clusters(uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb, bool *referenced_clusters) {
  referenced_clusters[cluster] = true;
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
 * Returns the new int for the filename
 */
uint8_t create_new_file(int cluster, uint8_t *image_buf, struct bpb33* bpb, uint8_t file_number, uint32_t size) {
  // Find the correct filename
  char filename[13]; filename[0] = '\0';
  do {
    sprintf(filename, "%s%i%s", "FOUND", file_number, ".DAT");
    file_number++;
  } while(find_file(filename, 0, FIND_FILE, image_buf, bpb) != NULL);

  uint32_t clust_size = bpb->bpbSecPerClust * bpb->bpbBytesPerSec;

  struct direntry *dirent = (struct direntry*) cluster_to_addr(0, image_buf, bpb);
  while(1) {
    if (dirent->deName[0] == SLOT_EMPTY) {
      write_dirent(dirent, filename, cluster, size * clust_size);
      dirent++;

      // make sure the next dirent is set to be empty, just in case it wasn't before
      memset((uint8_t*)dirent, 0, sizeof(struct direntry));
      dirent->deName[0] = SLOT_EMPTY;
      return file_number;
    }
    if (dirent->deName[0] == SLOT_DELETED) {
      // we found a deleted entry - we can just overwrite it
      write_dirent(dirent, filename, cluster, size * clust_size);
      return file_number;
    }
    dirent++;
  }
}

/**
 * Gets the size of the given file (in clusters)
 */
uint32_t get_file_size(int cluster, uint8_t *image_buf, struct bpb33* bpb) {
  uint32_t size = 0;
  while (!is_end_of_file(cluster)) {
    cluster = get_fat_entry(cluster, image_buf, bpb);
    size++;
  }
  return size;
}

/**
 * Marks all the clusters as referenced
 */
void mark_clusters_referenced(int cluster, uint8_t *image_buf, struct bpb33* bpb, bool *referenced_clusters) {
  while (!is_end_of_file(cluster)) {
    referenced_clusters[cluster] = true;
    cluster = get_fat_entry(cluster, image_buf, bpb);
  }
  referenced_clusters[cluster] = true;
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
      uint16_t size = get_file_size(i, image_buf, bpb);
      mark_clusters_referenced(i, image_buf, bpb, referenced_clusters);
      printf("Lost File: %i %i\n", i, size);

      files_found = create_new_file(i, image_buf,bpb, files_found, size);
    }
  }
}

/**
 * Frees all the clusters inbetween and including the true end and the false end cluster
 */
void free_clusters(uint16_t true_end, uint16_t false_end, uint8_t *image_buf, struct bpb33* bpb) {
  uint16_t current = true_end;

  while(true_end != false_end && !is_end_of_file(current)) {
      uint16_t next = get_fat_entry(current, image_buf, bpb);
      set_fat_entry(current, FAT12_MASK&CLUST_FREE, image_buf, bpb);
      current = next;
  }

  set_fat_entry(true_end, FAT12_MASK&CLUST_EOFS, image_buf, bpb);
}

/**
 * Checks if the length of a file matches the one in the FAT
 */
void check_file_length(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb, char *name, char *extension) {
  uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;

  uint32_t size = getulong(dirent->deFileSize);
  int32_t size_in_clusters = (size + cluster_size - 1) / cluster_size;

  uint16_t cluster = getushort(dirent->deStartCluster);
  uint16_t fat_size_in_clusters = get_file_size(cluster, image_buf, bpb);
  uint32_t fat_size = fat_size_in_clusters * cluster_size;

  if(fat_size_in_clusters > size_in_clusters) {
    printf("%s.%s %i %i\n", name, extension, size, fat_size);
    free_clusters(cluster + size_in_clusters - 1, cluster + fat_size_in_clusters, image_buf, bpb);
  }
  // No need to check smaller because that would not make sense
}

/**
 * Goes through the directory tree and checks if the length of all files match.
 */
void find_length_mismatches(uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb) {
  struct direntry *dirent;
  int d, length = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
  dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
  while (1) {
    for (d = 0; d < length; d += sizeof(struct direntry)) {
      char name[9], extension[4];
      name[8] = ' ';
      extension[3] = ' ';
      memcpy(name, &(dirent->deName[0]), 8);
      memcpy(extension, dirent->deExtension, 3);

      if (name[0] == SLOT_EMPTY) {
        return;
      }

      /* skip over deleted entries */
      if (((uint8_t)name[0]) == SLOT_DELETED)
        continue;

      removePadding(name, 8);
      removePadding(extension, 3);

      if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        dirent++; continue;
      } else if ((dirent->deAttributes & ATTR_VOLUME) != 0) {
        continue;
      } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
        uint16_t file_cluster = getushort(dirent->deStartCluster);
        find_length_mismatches(file_cluster, image_buf, bpb);
      } else {
        check_file_length(dirent, image_buf, bpb, name, extension);
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
  find_length_mismatches(0, image_buf, bpb);

  free(bpb);
  close(fd);
  free(referenced_clusters);
  return 0;
}
