// vim: noet:ts=4:sts=4:sw=4:et
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <assert.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
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
    

    // Matching the values into vfat_info structure
    vfat_info.bytes_per_sector = s.bytes_per_sector;
    vfat_info.sectors_per_cluster = s.sectors_per_cluster;
    vfat_info.reserved_sectors = s.reserved_sectors;
    vfat_info.sectors_per_fat = s.sectors_per_fat;
    vfat_info.cluster_size = s.bytes_per_sector * s.sectors_per_cluster;
    vfat_info.root_cluster = s.root_cluster;

    vfat_info.root_dir_sectors = ((s.root_max_entries * 32) + (s.bytes_per_sector - 1)) / s.bytes_per_sector;
    // FAT size;
    if(s.sectors_per_fat_small != 0)
        vfat_info.fat_size = s.sectors_per_fat_small;
    else
        vfat_info.fat_size = s.sectors_per_fat;
    
    // How many entries in one FAT. One entry is 4byte
    vfat_info.fat_entries = vfat_info.fat_size / sizeof(uint32_t);

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
    if(lseek(vfat_info.fd, vfat_info.fat_begin_offset, SEEK_SET) == -1)
        err(1, "lseek(%u)", vfat_info.fat_begin_offset);

    // read the first(0) FAT(1bytes) to compares 'Media info'    
    if(read(vfat_info.fd, &fat_0, sizeof(uint8_t)) != sizeof(uint8_t))
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

unsigned char ChkSum(unsigned char * pFcbName){
    short FcbNameLen;
    unsigned char sum;

    sum = 0;
    for(FcbNameLen = 11 ; FcbNameLen != 0 ; FcbNameLen--){
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *pFcbName++;
    }
    return (sum);
}

/* XXX add your code here */
// Find cluster[n]'s offset
void seek_cluster(uint32_t cluster_num)
{
    uint32_t first_sector_of_cluster;

    if(cluster_num < 2)
        err(1, "cluster number should be greater than 2!\n");
    // ((n-2) * BPB_SecPerClus) + FirstDataSector
    first_sector_of_cluster = ((cluster_num - 2) * vfat_info.sectors_per_cluster) + vfat_info.first_data_sector;

    if(lseek(vfat_info.fd, first_sector_of_cluster * vfat_info.bytes_per_sector, SEEK_SET) == -1)
        err(1, "lseek cluster_num %d\n", cluster_num);
}

int vfat_next_cluster(uint32_t cluster_num)
{
    /* TODO: Read FAT to actually get the next cluster */
    uint32_t fat_offset = vfat_info.fat_begin_offset;
    uint32_t next_cluster;
    uint32_t next_cluster_check;    // compare with FAT#2(backup)
    
    // Move fd to cluster area in FAT#1
    if(lseek(vfat_info.fd, fat_offset + cluster_num * sizeof(uint32_t), SEEK_SET) == -1)
        err(1, "lseek(%lu)", fat_offset + cluster_num * sizeof(uint32_t));
    
    if(read(vfat_info.fd, &next_cluster, sizeof(uint32_t)) != sizeof(uint32_t))
        err(1, "read(%lu)", sizeof(uint32_t));

    // check(compare) with FAT#2,
    if(lseek(vfat_info.fd, fat_offset + vfat_info.fat_size * vfat_info.bytes_per_sector + cluster_num * sizeof(uint32_t), SEEK_SET) == -1)
        err(1, "lseek(%d)", fat_offset);

    if(read(vfat_info.fd, &next_cluster_check, sizeof(uint32_t)) != sizeof(uint32_t))
        err(1, "read(%lu)", sizeof(uint32_t));

    if(next_cluster_check == next_cluster){
        if(next_cluster == 0x0FFFFFFF)
            return 0x0FFFFFFF;   // no next cluster. I am the end.
        return next_cluster;
    }
    else {
        err(1, "FAT is corrupted!!\n");
    } // FAT#1 != FAT#2
}

// Read cluster and parse directory entries..
static int read_cluster(uint32_t cluster_num, fuse_fill_dir_t filler, void *fillerdata){
    uint8_t check_sum = '\0'; 
    int i, j, seq_num = 0;
    size_t in_byte_size = 2*260;
    size_t out_byte_size = 260;

    char * buf = calloc(260*2, sizeof(char));   // max name size = 13byte * 20entries = 260. one char 2byte(Unicode) 
    char * char_buf = calloc(260, sizeof(char));

    struct fat32_direntry short_entry;
    struct fat32_direntry_long long_entry;

    memset(buf, 0, 2*260);

    seek_cluster(cluster_num);
    // Loop read 32byte repeatly
    for(i = 0 ; i < vfat_info.cluster_size ; i+=32){
        if(read(vfat_info.fd, &short_entry, 32) != 32)
            err(1, "read(short_dir)");
        // first two entry is . and ..   (root cluster has no . and ..) 
        if(i < 64 && cluster_num != 2){
            char * filename = (i == 0) ? "." : "..";    // . -> .. 
            setStat(short_entry,filename,filler,fillerdata,
                (((uint32_t)short_entry.cluster_hi) << 16) | ((uint32_t)short_entry.cluster_lo));
            continue;
        }
        
        if((uint8_t)short_entry.nameext[0] == 0xE5)   // Deleted file entry
            continue;
        else if(short_entry.nameext[0] == 0x05){    // Japan??
            short_entry.nameext[0] = (char)0xE5;
        }
        else if(short_entry.nameext[0] == 0x00){
            // There are no allocated directory entries after.
            free(buf);
            free(char_buf);
            return 0;
        }

        //DEBUG_PRINT("File name : %s\n", short_entry.nameext);
        // Long File Name 
        if((short_entry.attr & VFAT_ATTR_LFN) == VFAT_ATTR_LFN){    //mask
            long_entry = *((struct fat32_direntry_long * )&short_entry);
            if((long_entry.seq & VFAT_LFN_SEQ_START) == 0x40){     // sequnce mask with 0x40
                seq_num = (long_entry.seq & VFAT_LFN_SEQ_MASK) - 1;  // 0x3f is LONG_NAME_MASK
                check_sum = long_entry.csum;
                // get long file name
                for(j = 0 ; j < 13 ; j++){
                    if(j < 5 && long_entry.name1[j] != 0xFFFF){
                        buf[j*2] = long_entry.name1[j];
                        buf[j*2 + 1] = long_entry.name1[j] >> 8;    // why ??
                    }
                    else if(j < 11 && long_entry.name2[j-5] != 0xFFFF){
                        buf[j*2] = long_entry.name2[j-5];
                        buf[j*2 + 1] = long_entry.name2[j-5] >> 8;
                    }
                    else if(j < 13 && long_entry.name3[j-11] != 0xFFFF){
                        buf[j*2] = long_entry.name3[j-11];
                        buf[j*2 + 1] = long_entry.name3[j-11] >> 8;
                    }
                }
            }
            else if(check_sum == long_entry.csum && long_entry.seq == seq_num){  // ord num is not 0x4~ 1st enctry
                seq_num = -1;
                char tmp[260*2];
                memset(tmp, 0, 260 * 2);

                for(j = 0 ; j < 260 * 2 ; j++){
                    tmp[j] = buf[j];
                }

                memset(buf, 0, 260*2);

                for(j = 0 ; j < 260 ; j++){
                    if(j < 5 && long_entry.name1[j] != 0xFFFF){
                        buf[j*2] = long_entry.name1[j];
                        buf[j*2 + 1] = long_entry.name1[j] >> 8;    // why ??
                    }
                    else if(j < 11 && long_entry.name2[j-5] != 0xFFFF){
                        buf[j*2] = long_entry.name2[j-5];
                        buf[j*2 + 1] = long_entry.name2[j-5] >> 8;
                    }
                    else if(j < 13 && long_entry.name3[j-11] != 0xFFFF){
                        buf[j*2] = long_entry.name3[j-11];
                        buf[j*2 + 1] = long_entry.name3[j-11] >> 8;
                    }// At first entry, take the precede entries's file name
                    else if(j >= 13 && ((uint16_t *)tmp)[j-13] != 0xFFFF){
                        buf[j*2] = tmp[(j-13) * 2];
                        buf[j*2 + 1] = tmp[(j-13) * 2 + 1];
                    }
                }
            }
            else{   // Error field. Init all vars.
                seq_num = 0;
                check_sum = '\0';
                memset(buf, 0, 260*2);
                err(1, "Error!! Bad sequence number or checksum!!\n");
            }
        }
        else if((short_entry.attr & ATTR_VOLUME_ID) == ATTR_VOLUME_ID){
            seq_num = 0;
            check_sum = '\0';
            memset(buf, 0, 260*2);
        } // For Short entries..?
        else if(check_sum == ChkSum((unsigned char *)&(short_entry.nameext)) && seq_num == 0){
            char * buf_pointer = buf;
            char * char_buf_pointer = char_buf;
            iconv(iconv_utf16, &buf_pointer, &in_byte_size, &char_buf_pointer, &out_byte_size);
            in_byte_size = 2 * 260;
            out_byte_size = 260;
            char * filename = char_buf;
            setStat(short_entry,filename,filler,fillerdata,
                (((uint32_t)short_entry.cluster_hi) << 16) | ((uint32_t)short_entry.cluster_lo));
            check_sum = '\0';
            memset(buf, 0, 260);
        }
        else{
            char * filename = char_buf;
            GetFileName(short_entry.nameext, filename);
            setStat(short_entry,filename,filler,fillerdata,
                (((uint32_t)short_entry.cluster_hi) << 16) | ((uint32_t)short_entry.cluster_lo));
        }
    }


    free(buf);
    free(char_buf);
    return 1;   // directory is not finished.
}
void
setStat(struct fat32_direntry dir_entry, char* buffer, fuse_fill_dir_t filler, void *fillerdata, uint32_t cluster_no){
    struct stat* stat_str = malloc(sizeof(struct stat));
    memset(stat_str, 0, sizeof(struct stat));
    
    stat_str->st_dev = 0; // Ignored by FUSE
    stat_str->st_ino = cluster_no; // Ignored by FUSE unless overridden
    if((dir_entry.attr & ATTR_READ_ONLY) == ATTR_READ_ONLY){
        stat_str->st_mode = S_IRUSR | S_IRGRP | S_IROTH;
    }
    else{
        stat_str->st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
    }
    
    if((dir_entry.attr & ATTR_DIRECTORY) == ATTR_DIRECTORY) {
        stat_str->st_mode |= S_IFDIR;
        int cnt = 0;
        uint32_t next_cluster_no = cluster_no;
        off_t pos = lseek(vfat_info.fd, 0, SEEK_CUR);
        
        while(next_cluster_no < (uint32_t) 0x0FFFFFF8) {
            cnt++;
            next_cluster_no = vfat_next_cluster(0x0FFFFFFF & next_cluster_no);
        }
                
        if(lseek(vfat_info.fd, pos, SEEK_SET) == -1) {
            err(1, "Couldn't return to initial position: %lx", pos);
        }
        
        stat_str->st_size = cnt * vfat_info.sectors_per_cluster * vfat_info.bytes_per_sector;
    }
    else {
        stat_str->st_mode |= S_IFREG;
        stat_str->st_size = dir_entry.size;
    }
    stat_str->st_nlink = 1;
    stat_str->st_uid = vfat_info.mount_uid;
    stat_str->st_gid = vfat_info.mount_gid;
    stat_str->st_rdev = 0;
    stat_str->st_blksize = 0; // Ignored by FUSE
    stat_str->st_blocks = 1;
    stat_str->st_atime = conv_time(dir_entry.atime_date, 0);
    stat_str->st_mtime = conv_time(dir_entry.mtime_date, dir_entry.mtime_time);
    stat_str->st_ctime = conv_time(dir_entry.ctime_date, dir_entry.ctime_time);
    filler(fillerdata, buffer, stat_str, 0);
    free(stat_str);
}
// Handle file name from directory entry
char * GetFileName(char * nameext, char * filename){

    if(nameext[0] == 0x20)  // 0x20 is space ' '
        err(1, "filename[0] is 0x20!!\n");

    uint32_t FilenameCnt = 0;
    int i;

    bool before_extension = true;
    bool in_spaces = false;
    bool in_extention = false;
    // Check invalid chars
    for(i = 0 ; i < 11 ; i++){
        if(nameext[i] < 0x20 || nameext[i] == 0x22 || nameext[i] == 0x2A || nameext[i] == 0x2B ||
            nameext[i] == 0x2C || nameext[i] == 0x2E || nameext[i] == 0x2F || nameext[i] == 0x3A ||
            nameext[i] == 0x3B || nameext[i] == 0x3C || nameext[i] == 0x3D || nameext[i] == 0x3E ||
            nameext[i] == 0x3F || nameext[i] == 0x5B || nameext[i] == 0x5C || nameext[i] == 0x5D ||
            nameext[i] == 0x7C) {
                err(1, "invalid character in filename %x at %d\n", nameext[i] & 0xFF, i);
        }

        if(before_extension){       // Before Extention 
            if(nameext[i] == 0x20){ // remain space are filled with 0x20
                before_extension = false;
                in_spaces = true;
                filename[FilenameCnt++] = '.';  // Extention
            }
            else if(i == 8){    // If all spaces are filled with character.
                before_extension = false;   
                in_spaces = true;
                filename[FilenameCnt++] = '.';      // 9's letter is '.'
                filename[FilenameCnt++] = nameext[i];   // First extention word
                in_extention = true;
            }
            else{
                filename[FilenameCnt++] = nameext[i];
            }
        }
        else if(in_spaces){ // padded(with space) part
            if(nameext[i] == 0x20){
                in_spaces = false;
                in_extention = true;
                filename[FilenameCnt++] = nameext[i];
            }
        }
        else if(in_extention){  // after 9th letter
            if(nameext[i] == 0x20){
                break;
            }
            else{
                filename[FilenameCnt++] = nameext[i];
            }
        }
    }

    if(filename[FilenameCnt - 1] == '.'){
        filename--;
    }
    filename[FilenameCnt] = '\0';   // Fill last word with NULL
    //DEBUG_PRINT("filename : %s\n", filename);
    return filename;
}

time_t conv_time(uint16_t date_entry, uint16_t time_entry){
    struct tm * time_info;  // tm struct define in <time.h>

    time_t raw_time;

    time(&raw_time);    // Get raw current time
    time_info = localtime(&raw_time);   // parse the raw time
    /* 
    0-4 bit 2senond count 0 ~ 58
    5-10 bit minute 0~59
    11-15 hours 0~23
    00000 000000 00000
    HOUR  MINUTE SECOND
    */
    time_info->tm_sec = (time_entry & 0x1f) << 1;   // masking with 0000 0000 0001 1111. shift << 1  for LSB bit
    time_info->tm_min = (time_entry & 0x7E0) >> 5;  // masking with 0000 0111 1110 0000
    time_info->tm_hour = (time_entry & 0xF800) >> 11; // masking with 1111 1000 0000 0000 
    /* 
    0-4 bit Day 1-31
    5-8 bit Month 1-12
    9-15 bit Year from 1980(when bits are all 0)
    0000000 0000 00000
    YEAR    MONT DAY
    */
    time_info->tm_mday = (date_entry & 0x1f); // 0000 0000 0001 1111
    time_info->tm_mon = (date_entry & 0x1E0) >> 5;   // 0000 0001 1110 0000
    time_info->tm_year = ((date_entry & 0xFE00) >> 9) + 80;     // 1111 1110 0000 0000

    return mktime(time_info);
}

int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t filler, void *fillerdata)
{
    struct stat st; // we can reuse same stat entry over and over again

    uint32_t next_cluster_num = first_cluster;
    bool eof = false;
    int end_of_read;

    memset(&st, 0, sizeof(st));
    st.st_uid = vfat_info.mount_uid;
    st.st_gid = vfat_info.mount_gid;
    st.st_nlink = 1;

    while(!eof){
        end_of_read = read_cluster(next_cluster_num, filler, fillerdata);

        if(end_of_read == 0)
            eof = true;
        else{
            next_cluster_num = 0x0FFFFFFF & vfat_next_cluster(next_cluster_num);
            if(next_cluster_num >= (uint32_t)0xFFFFFF8){
                eof = true;
            }
        }
    }

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
    struct vfat_search_data sd;
    int i;
    const char *final_name;
    char *token = NULL, *path_copy;

    path_copy = malloc(strlen(path) + 1);
    strncpy(path_copy, path, strlen(path) + 1);

    memset(&sd, 0, sizeof(struct vfat_search_data));
    sd.st = st;

    for(i = strlen(path); path[i] != '/'; i--);
    final_name = path + i + 1;

    token = strtok(path_copy, "/");
    sd.name = token;

    vfat_readdir(vfat_info.root_cluster, vfat_search_entry, &sd);

    if(sd.found == 1) {
        while(strcmp(sd.name, final_name) != 0) {
            token = strtok(NULL, "/");
            if(token == NULL) {
                free(path_copy);
                return -ENOENT;
            }
            sd.name = token;
            sd.found = 0;
            vfat_readdir(((uint32_t) (sd.st)->st_ino), vfat_search_entry, &sd);
            if(sd.found != 1) {
                free(path_copy);
                return -ENOENT;
            }
        }
        free(path_copy);
        return 0;
    } else {
        free(path_copy);
        return -ENOENT;
    }
}

// Get file attributes
int vfat_fuse_getattr(const char *path, struct stat *st)
{
    /*
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_getattr(path + strlen(DEBUGFS_PATH), st);
    } else {
        // Normal file
        return vfat_resolve(path, st);
    }
    */
    // No such file
    if (strcmp(path, "/") == 0) {
        st->st_dev = 0; // Ignored by FUSE
        st->st_ino = 0; // Ignored by FUSE unless overridden
        st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR;
        st->st_nlink = 1;
        st->st_uid = vfat_info.mount_uid;
        st->st_gid = vfat_info.mount_gid;
        st->st_rdev = 0;
        int cnt = 0;
        uint32_t next_cluster_no = vfat_info.root_cluster;
        off_t pos = lseek(vfat_info.fd, 0, SEEK_CUR);
        while(next_cluster_no < (uint32_t) 0x0FFFFFF8) {
            cnt++;
            next_cluster_no = vfat_next_cluster(0x0FFFFFFF & next_cluster_no);
        }
        if(lseek(vfat_info.fd, pos, SEEK_SET) == -1) {
            err(1, "Couldn't return to initial position: %lx", pos);
        }
        st->st_size = cnt * vfat_info.cluster_size;
        st->st_blksize = 0; // Ignored by FUSE
        st->st_blocks = 1;
        return 0;
    }
    if(vfat_resolve(path + 1, st) != 0) {
        return -ENOENT;
    } else {
        return 0;
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
        const char *path, void *buf,
        fuse_fill_dir_t filler, off_t unused_offs, struct fuse_file_info *unused_fi)
{
    struct stat st;     
    if(strcmp(path, "/") != 0) {
        vfat_resolve(path+1, &st);
        vfat_readdir((uint32_t)st.st_ino, filler, buf);
    } else {
        vfat_readdir(vfat_info.root_cluster, filler, buf);
    }
    /* TODO: Add your code here. You should reuse vfat_readdir and vfat_resolve functions
    */
    return 0;
}

int vfat_fuse_read(
        const char *path, char *buf, size_t size, off_t offs,
        struct fuse_file_info *unused)
{
    /*
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_read(path + strlen(DEBUGFS_PATH), buf, size, offs, unused);
    }
    TODO: Add your code here. Look at debugfs_fuse_read for example interaction.
    */
    struct stat st;
    vfat_resolve(path+1, &st);
    if(!(st.st_mode & S_IFREG)) {
        DEBUG_PRINT("Trying to read a directory or not regular file\n");
        return -1;
    }

    size_t cnt = 0;
    uint32_t cluster_no = (uint32_t) st.st_ino;

    if(offs >= st.st_size) {
        memset(buf, 0, size);
        return 0;
    }

    while(offs >= vfat_info.cluster_size) {
        cluster_no = vfat_next_cluster(cluster_no);
        offs -= vfat_info.cluster_size;
    }

    seek_cluster(cluster_no);
    if(lseek(vfat_info.fd, offs, SEEK_CUR) == -1) {
        err(1, "seek last part of offset failed\n");
    }

    if(vfat_info.cluster_size - offs > size) {
        if(read(vfat_info.fd, buf+cnt, size) != size) {
            err(1, "read cluster-offs > size failed\n");
        }
        return 0; // TODO CHECK THIS
    } else {
        if(read(vfat_info.fd, buf+cnt, vfat_info.cluster_size - offs) != vfat_info.cluster_size-offs) {
            err(1, "read cluster-offs <= size failed\n");
        }
        cnt += vfat_info.cluster_size - offs;
    }

    while(size - cnt > vfat_info.cluster_size) {
        cluster_no = vfat_next_cluster(cluster_no);
        seek_cluster(cluster_no);
        DEBUG_PRINT("Read cluster_no %x\n", cluster_no);
        if((cluster_no & 0x0fffffff) >= 0x0FFFFFF8) {
            memset(buf+cnt, 0, size-cnt);
            return cnt;
        }
        if(read(vfat_info.fd, buf+cnt, vfat_info.cluster_size) != vfat_info.cluster_size) {
            err(1, "read cluster_size failed\n");
        }
        cnt += vfat_info.cluster_size;
    }

    cluster_no = vfat_next_cluster(cluster_no);
    seek_cluster(cluster_no);
    if((cluster_no & 0x0fffffff) >= 0x0FFFFFF8) {
        memset(buf+cnt, 0, size-cnt);
        return cnt;
    }

    if(cnt < size) {
        if(read(vfat_info.fd, buf+cnt, size - cnt) != size - cnt) {
            err(1, "read cluster_size failed\n");
        }
        cnt += size-cnt;
    }


    return cnt; // number of bytes read from the file
          // must be size unless EOF reached, negative for an error
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
    printf("size of 0x55 is %ld\n", sizeof("0x55"));        //
*/
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

    if (!vfat_info.dev)
        errx(1, "missing file system parameter");

    vfat_init(vfat_info.dev);
    //read_cluster(2);
    return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
