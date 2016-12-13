# FAT12 Scandisk

This is a very simple scandisk for a FAT12 filesystem that performs several checks to see if the filesystem is consistent:
- Checks for unreferenced clusters. If there are any, it prints all of them out.
- Checks for any files that are unreferenced in the directory tree and prints them out in the following format `Lost File: <start_cluster> <size_in_clusters>`.
- Adds all of the lost files to the root directory.
- Checks if the file length in the FAT is consistent with the directory entries. Otherwise prints out the files in the format `<filename> <length_dir> <length_fat>`.
- Frees any clusters that are beyond the end of a file.

## Usage

First run `make` and then run `./dos_scandisk <imagename>`.
There are 3 images provided in the `images` directory.
For `floppy.img`, the program does not output anything because the filesystem is already consistent.

## File Structure
```
.
├── dos_scandisk.c            # The file that checks the file structure and produces the output to stdout
├── badfloppy2-output.txt     # Output of badfloppy2.img in a txt format
├── badfloppy2-output.png     # Screenshot of output of badfloppy2.img
├── description.pdf           # One page description of how dos_scandisk.c workss
└── README.md
```
