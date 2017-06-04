// vim: noet:ts=4:sts=4:sw=4:et
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <assert.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <iconv.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "vfat.h"
#include "util.h"
#include "debugfs.h"

#define DEBUG_PRINT(...) printf(__VA_ARGS__)

iconv_t iconv_utf16;
char* DEBUGFS_PATH = "/.debug";


static void
vfat_init(const char *dev)
{
    struct fat_boot_header s;
    //int i;
    uint8_t fat_0;

    iconv_utf16 = iconv_open("utf-8", "utf-16"); // from utf-16 to utf-8
    // These are useful so that we can setup correct permissions in the mounted directories
    vfat_info.mount_uid = getuid();
    vfat_info.mount_gid = getgid();

    // Use mount time as mtime and ctime for the filesystem root entry (e.g. "/")
    vfat_info.mount_time = time(NULL);

    vfat_info.fd = open(dev, O_RDONLY);
    if (vfat_info.fd < 0)
        err(1, "open(%s)", dev);
    if (pread(vfat_info.fd, &s, sizeof(s), 0) != sizeof(s))
        err(1, "read super block");
    /*
    DEBUG_PRINT("jmp_boot : "); 
    for(i = 0 ; i < 3; i++)    
        DEBUG_PRINT("%c", s.jmp_boot[i]);
    DEBUG_PRINT("\noem name : "); 
    for(i = 0 ; i < 8; i++)    
        DEBUG_PRINT("%c", s.oemname[i]);
    DEBUG_PRINT("\nBytesPerSec : 0x%x\n", s.bytes_per_sector);
    DEBUG_PRINT("SecPerClus : 0x%x\n", s.sectors_per_cluster);
    DEBUG_PRINT("RsvdSecCnt : 0x%x\n", s.reserved_sectors);
    DEBUG_PRINT("NumFATs : 0x%x\n", s.fat_count);
    DEBUG_PRINT("RootEntCnt(fat12/16) : 0x%x\n", s.root_max_entries);
    DEBUG_PRINT("TotSec16(fat12/16) : 0x%x\n", s.total_sectors_small);
    DEBUG_PRINT("media : 0x%x\n", s.media_info);
    DEBUG_PRINT("FATSz16 : 0x%x\n", s.sectors_per_fat_small);
    DEBUG_PRINT("SecPerTrk : 0x%x\n", s.sectors_per_track);
    DEBUG_PRINT("NumHeads : 0x%x\n", s.head_count);
    DEBUG_PRINT("Hidden Sectors : 0x%x\n", s.fs_offset);
    DEBUG_PRINT("TotSec32 : 0x%x\n", s.total_sectors);
    
    DEBUG_PRINT("\n======= FAT32 only infomation ======\n");
    DEBUG_PRINT("FATSz32 : 0x%x\n", s.sectors_per_fat);
    DEBUG_PRINT("ExtFlags : 0x%x\n", s.fat_flags);
    DEBUG_PRINT("FS version : 0x%x\n", s.version);
    DEBUG_PRINT("RootCluster Number : 0x%x\n", s.root_cluster);
    DEBUG_PRINT("FS info Sector num : 0x%x\n", s.fsinfo_sector);
    DEBUG_PRINT("Backup Boot sector : 0x%x\n", s.backup_sector);
    DEBUG_PRINT("Reserved(To be expanded) : 0x%s\n", s.reserved2);
   
    DEBUG_PRINT("Drive num : 0x%x\n", s.drive_number);
    DEBUG_PRINT("Reserved : 0x%x\n", s.reserved3);
    DEBUG_PRINT("Extended boot sig : 0x%x\n", s.ext_sig);
    DEBUG_PRINT("Volume Serial num : 0x%x\n", s.serial);
    DEBUG_PRINT("Volume Label : ");
    for(i = 0; i < 11; i++)
        DEBUG_PRINT("%c", s.label[i]);
    DEBUG_PRINT("\nFS type : ");
    for(i = 0; i < 8; i++)
        DEBUG_PRINT("%c", s.fat_name[i]);
  
    //DEBUG_PRINT(" : 0x%x\n", s.executable_code[420]);
    DEBUG_PRINT("\nSignature : 0x%x\n", s.signature);
    */
    
    /*** Check this volume is FAT32 ***/ 
    //'root_max_entries' field has zero in FAT32 volume
    if(s.root_max_entries != 0)
        err(1, "This is not FAT32!\n");

    /* Check signature */
    if(s.signature != 0xAA55)
        err(1, "Magic number 0xAA55 not present\n");

    // bytes per sector check(512, 1024, 2048, 4096)
    if(s.bytes_per_sector != 512 && s.bytes_per_sector != 1024 &&
        s.bytes_per_sector != 2048 && s.bytes_per_sector != 4096)
        err(1, "bytes_per_sector is wrong!!\n");

    // sector per cluster mostly have 1, 2, 4 ,8 ,16, 32 ,64
    if(s.sectors_per_cluster != 1 && s.sectors_per_cluster % 2 != 0)
        err(1, "sectors_per_cluster is wrong!!\n");

    // bytes per cluster size check ( x < (32 * 1024) )
    if(s.sectors_per_cluster * s.bytes_per_sector > 32 * 1024)
        err(1, "bytes_per_cluster is too large!!\n");

    // reserved_sectors check(should not be zero)
    if(s.reserved_sectors == 0)
        err(1, "reserved_sectors is zero!!\n");

    // fat count check(2)
    if(s.fat_count < 2)
        err(1, "fat count is less than 2!!\n");

    // total_sectors small is zero in FAT32 volume
    if(s.total_sectors_small != 0)
        err(1, "total_sectors_small must be zero!!\n");

    // Media info check(0xF0, 0xF9 ~ 0xFE)
    if(s.media_info != 0xF0 && s.media_info < 0xF8)
        err(1, "Wrong Media info!!\n");

    // sectors_per_fat check
    if(s.sectors_per_fat_small != 0)
        err(1, "sectors_per_fat_small must be zero!!\n");
    if(s.sectors_per_fat == 0)
        err(1, "sectors_per_fat must be non-zero!!\n");
    

    // Matching the values into vfat_info structure.
    vfat_info.fat_entries = s.fat_count;    // fat_entries mean what?
    vfat_info.bytes_per_sector = s.bytes_per_sector;
    vfat_info.sectors_per_cluster = s.sectors_per_cluster;
    vfat_info.reserved_sectors = s.reserved_sectors;
    vfat_info.sectors_per_fat = s.sectors_per_fat;
    vfat_info.cluster_size = s.bytes_per_sector * s.sectors_per_cluster;

    vfat_info.root_dir_sectors = ((s.root_max_entries * 32) + (s.bytes_per_sector - 1)) / s.bytes_per_sector;
    // FAT size;
    if(s.sectors_per_fat_small != 0)
        vfat_info.fat_size = s.sectors_per_fat_small;
    else
        vfat_info.fat_size = s.sectors_per_fat;
    
    // Total sector
    if(s.total_sectors_small != 0)
        vfat_info.total_sectors = s.total_sectors_small;
    else
        vfat_info.total_sectors = s.total_sectors;

    // data sector count
    vfat_info.data_sectors = vfat_info.total_sectors - (s.reserved_sectors + (s.fat_count * vfat_info.fat_size) + vfat_info.root_dir_sectors);

    // cluster count
    vfat_info.count_of_cluster = vfat_info.data_sectors / s.sectors_per_cluster;
    
    // verify FAT type(by cluster counts)
    DEBUG_PRINT("========  FAT type check(count of clusters)  ========\n");
    if(vfat_info.count_of_cluster < 4085)
        err(1, "error : This volume is FAT12\n");
    else if(vfat_info.count_of_cluster < 65525)
        err(1, "error : This volume is FAT16\n");
    else
        DEBUG_PRINT("This volume is FAT32!!\n");
    DEBUG_PRINT("count of cluster = %ld\n", vfat_info.count_of_cluster);
    
    // FAT begin offset
    vfat_info.fat_begin_offset = s.reserved_sectors * s.bytes_per_sector;
    //DEBUG_PRINT("FAT begin offset = 0x%x\n", vfat_info.fat_begin_offset);
    
    // fd move to FAT area
    if( lseek(vfat_info.fd, vfat_info.fat_begin_offset, SEEK_SET) == -1 )
        err(1, "lseek(%u)", vfat_info.fat_begin_offset);

    // read the first(0) FAT(1bytes) to compares 'Media info'    
    if( read(vfat_info.fd, &fat_0, sizeof(uint8_t)) != sizeof(uint8_t) )
        err(1, "read(%lu)", sizeof(uint8_t));

    if(fat_0 != s.media_info)
        err(1, "Media info is different in FAT[0]!!\n");
    
    // fd init... to start of file
    if(lseek(vfat_info.fd, 0, SEEK_SET) == -1)
        err(1, "lseek(0)");

    // First Data Sector
    vfat_info.first_data_sector = s.reserved_sectors + (s.fat_count * vfat_info.fat_size) + vfat_info.root_dir_sectors;
    //DEBUG_PRINT("First Data Sector = 0x%x\n", vfat_info.first_data_sector);
    
    // cluster begin offset
    vfat_info.cluster_begin_offset = vfat_info.first_data_sector * vfat_info.bytes_per_sector;
    DEBUG_PRINT("Cluster begin Offset = 0x%x\n", vfat_info.cluster_begin_offset);

    // direntry_per_cluster
    vfat_info.direntry_per_cluster = (vfat_info.cluster_size * vfat_info.bytes_per_sector) / 32; 
    DEBUG_PRINT("Directory Entry per Cluster : 0x%x\n", vfat_info.direntry_per_cluster);
    
    /* XXX add your code here */
    vfat_info.root_inode.st_ino = le32toh(s.root_cluster);
    vfat_info.root_inode.st_mode = 0555 | S_IFDIR;
    vfat_info.root_inode.st_nlink = 1;
    vfat_info.root_inode.st_uid = vfat_info.mount_uid;
    vfat_info.root_inode.st_gid = vfat_info.mount_gid;
    vfat_info.root_inode.st_size = 0;
    vfat_info.root_inode.st_atime = vfat_info.root_inode.st_mtime = vfat_info.root_inode.st_ctime = vfat_info.mount_time;

}

/* XXX add your code here */

int vfat_next_cluster(uint32_t cluster_num)
{
    /* TODO: Read FAT to actually get the next cluster */
    uint32_t fat_offset = vfat_info.fat_begin_offset;
    
    
    
    return 0xffffff; // no next cluster
}

int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t callback, void *callbackdata)
{
    struct stat st; // we can reuse same stat entry over and over again

    memset(&st, 0, sizeof(st));
    st.st_uid = vfat_info.mount_uid;
    st.st_gid = vfat_info.mount_gid;
    st.st_nlink = 1;

    /* XXX add your code here */
    return 0;
}


// Used by vfat_search_entry()
struct vfat_search_data {
    const char*  name;
    int          found;
    struct stat* st;
};


// You can use this in vfat_resolve as a callback function for vfat_readdir
// This way you can get the struct stat of the subdirectory/file.
int vfat_search_entry(void *data, const char *name, const struct stat *st, off_t offs)
{
    struct vfat_search_data *sd = data;

    if (strcmp(sd->name, name) != 0) return 0;

    sd->found = 1;
    *sd->st = *st;

    return 1;
}

/**
 * Fills in stat info for a file/directory given the path
 * @path full path to a file, directories separated by slash
 * @st file stat structure
 * @returns 0 iff operation completed succesfully -errno on error
*/
int vfat_resolve(const char *path, struct stat *st)
{
    /* TODO: Add your code here.
        You should tokenize the path (by slash separator) and then
        for each token search the directory for the file/dir with that name.
        You may find it useful to use following functions:
        - strtok to tokenize by slash. See manpage
        - vfat_readdir in conjuction with vfat_search_entry
    */
    int res = -ENOENT; // Not Found
    if (strcmp("/", path) == 0) {
        *st = vfat_info.root_inode;
        res = 0;
    }
    return res;
}

// Get file attributes
int vfat_fuse_getattr(const char *path, struct stat *st)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_getattr(path + strlen(DEBUGFS_PATH), st);
    } else {
        // Normal file
        return vfat_resolve(path, st);
    }
}

// Extended attributes useful for debugging
int vfat_fuse_getxattr(const char *path, const char* name, char* buf, size_t size)
{
    struct stat st;
    int ret = vfat_resolve(path, &st);
    if (ret != 0) return ret;
    if (strcmp(name, "debug.cluster") != 0) return -ENODATA;

    if (buf == NULL) {
        ret = snprintf(NULL, 0, "%u", (unsigned int) st.st_ino);
        if (ret < 0) err(1, "WTF?");
        return ret + 1;
    } else {
        ret = snprintf(buf, size, "%u", (unsigned int) st.st_ino);
        if (ret >= size) return -ERANGE;
        return ret;
    }
}

int vfat_fuse_readdir(
        const char *path, void *callback_data,
        fuse_fill_dir_t callback, off_t unused_offs, struct fuse_file_info *unused_fi)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_readdir(path + strlen(DEBUGFS_PATH), callback_data, callback, unused_offs, unused_fi);
    }
    /* TODO: Add your code here. You should reuse vfat_readdir and vfat_resolve functions
    */
    return 0;
}

int vfat_fuse_read(
        const char *path, char *buf, size_t size, off_t offs,
        struct fuse_file_info *unused)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_read(path + strlen(DEBUGFS_PATH), buf, size, offs, unused);
    }
    /* TODO: Add your code here. Look at debugfs_fuse_read for example interaction.
    */
    return 0;
}

////////////// No need to modify anything below this point
int
vfat_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
{
    if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
        vfat_info.dev = strdup(arg);
        return (0);
    }
    return (1);
}

struct fuse_operations vfat_available_ops = {
    .getattr = vfat_fuse_getattr,
    .getxattr = vfat_fuse_getxattr,
    .readdir = vfat_fuse_readdir,
    .read = vfat_fuse_read,
};

int main(int argc, char **argv)
{
    /*
    printf("size of size_t is %ld\n", sizeof(size_t));  // 8    
    printf("size of int is %ld\n", sizeof(int));        // 4
    printf("size of off_t is %ld\n", sizeof(off_t));       // 8
    printf("size of uint8_t is %ld\n", sizeof(uint8_t));    // 1
    printf("size of uint16_t is %ld\n", sizeof(uint16_t));  //2
    printf("size of 0x55 is %ld\n", sizeof("0x55"));        //5
*/
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

    if (!vfat_info.dev)
        errx(1, "missing file system parameter");

    vfat_init(vfat_info.dev);
    return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
