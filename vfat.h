// vim: noet:ts=4:sts=4:sw=4:et
#ifndef VFAT_H
#define VFAT_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fuse.h>

typedef enum {false, true} bool;

// Boot sector
struct fat_boot_header {
    /* General */
    /* 0*/  uint8_t  jmp_boot[3];
    /* 3*/  char     oemname[8];
    /**  BIOS Parameter Block (BPB)  **/
    /*11*/  uint16_t bytes_per_sector;
    /*13*/  uint8_t  sectors_per_cluster; 
    /*14*/  uint16_t reserved_sectors;
    /*16*/  uint8_t  fat_count;
    /*17*/  uint16_t root_max_entries;
    /*19*/  uint16_t total_sectors_small;
    /*21*/  uint8_t  media_info;
    /*22*/  uint16_t sectors_per_fat_small;
    /*24*/  uint16_t sectors_per_track;
    /*26*/  uint16_t head_count;
    /*28*/  uint32_t fs_offset;
    /*32*/  uint32_t total_sectors;
    /* FAT32-only */
    /*36*/  uint32_t sectors_per_fat;
    /*40*/  uint16_t fat_flags;
    /*42*/  uint16_t version;
    /*44*/  uint32_t root_cluster;
    /*48*/  uint16_t fsinfo_sector;
    /*50*/  uint16_t backup_sector;
    /*52*/  uint8_t  reserved2[12];
    /**  End of BPB  **/
    /*64*/  uint8_t  drive_number;
    /*65*/  uint8_t  reserved3;
    /*66*/  uint8_t  ext_sig;
    /*67*/  uint32_t serial;
    /*71*/  char     label[11];
    /*82*/  char     fat_name[8];
    /* Rest */
    /*90*/  char     executable_code[420];
    /*510*/ uint16_t signature;
} __attribute__ ((__packed__));


struct fat32_direntry {
    /* 0*/  union {
                struct {
                    char name[8];
                    char ext[3];
                };
                char nameext[11];
            };
    /*11*/  uint8_t  attr;
    /*12*/  uint8_t  res;
    /*13*/  uint8_t  ctime_ms;
    /*14*/  uint16_t ctime_time;
    /*16*/  uint16_t ctime_date;
    /*18*/  uint16_t atime_date;
    /*20*/  uint16_t cluster_hi;
    /*22*/  uint16_t mtime_time;
    /*24*/  uint16_t mtime_date;
    /*26*/  uint16_t cluster_lo;
    /*28*/  uint32_t size;
} __attribute__ ((__packed__));

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LONG_NAME  (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)

#define VFAT_ATTR_DIR   0x10
#define VFAT_ATTR_LFN   0x0f        // same with ATTR_LONG_NAME
#define VFAT_ATTR_INVAL (0x80|0x40|0x08)

struct fat32_direntry_long {
    /* 0*/  uint8_t  seq;
    /* 1*/  uint16_t name1[5];
    /*11*/  uint8_t  attr;
    /*12*/  uint8_t  type;
    /*13*/  uint8_t  csum;
    /*14*/  uint16_t name2[6];
    /*26*/  uint16_t reserved2;
    /*28*/  uint16_t name3[2];
} __attribute__ ((__packed__));

#define VFAT_LFN_SEQ_START      0x40
#define VFAT_LFN_SEQ_DELETED    0x80
#define VFAT_LFN_SEQ_MASK       0x3f

// A kitchen sink for all important data about filesystem
struct vfat_data {
    const char* dev;
    int         fd;
    uid_t mount_uid;
    gid_t mount_gid;
    time_t mount_time;

    /* TODO: add your code here */
    size_t      root_dir_sectors;
    size_t      first_data_sector;
    size_t      total_sectors;
    size_t      data_sectors;
    size_t      count_of_cluster;
    size_t      root_cluster;

    size_t      fat_entries;            // FAT ? 
    off_t       cluster_begin_offset;   // o
    size_t      direntry_per_cluster;   // o
    // BPB ; 0x0B ~ 0x40 (53 bytes)
    size_t      bytes_per_sector;       // Sector Size  512
    size_t      sectors_per_cluster;    // Cluster Size 8*512
    size_t      reserved_sectors;       // 512 * 32 byte
    // num of FATs 0x11 ~ 0x15 (FAT12/16 only)
    size_t      sectors_per_fat;        // 999
    size_t      cluster_size;           // 8 * 512
    off_t       fat_begin_offset;       // boot record + reseved area;
    size_t      fat_size;               // 32 -> 
    struct stat root_inode;
    uint32_t*   fat; // use util::mmap_file() to map this directly into the memory 
};

struct vfat_data vfat_info;

void seek_cluster(uint32_t cluster_num);

/// FOR debugfs
int vfat_next_cluster(uint32_t cluster_num);
int vfat_resolve(const char *path, struct stat *st);
int vfat_fuse_getattr(const char *path, struct stat *st);
///
static int read_cluster(uint32_t cluster_num, fuse_fill_dir_t filler, void *fillerdata);
char * GetFileName(char * nameext, char * filename);
time_t conv_time(uint16_t date_entry, uint16_t time_entry);
void setStat(struct fat32_direntry dir_entry, char* buffer, fuse_fill_dir_t filler, void *fillerdata, uint32_t cluster_no);

#endif
