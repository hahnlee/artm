#include <stddef.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(struct stat) == 128, "Android arm64 stat size drift");
_Static_assert(offsetof(struct stat, st_dev) == 0, "st_dev offset drift");
_Static_assert(offsetof(struct stat, st_ino) == 8, "st_ino offset drift");
_Static_assert(offsetof(struct stat, st_mode) == 16, "st_mode offset drift");
_Static_assert(offsetof(struct stat, st_nlink) == 20, "st_nlink offset drift");
_Static_assert(offsetof(struct stat, st_uid) == 24, "st_uid offset drift");
_Static_assert(offsetof(struct stat, st_gid) == 28, "st_gid offset drift");
_Static_assert(offsetof(struct stat, st_rdev) == 32, "st_rdev offset drift");
_Static_assert(offsetof(struct stat, st_size) == 48, "st_size offset drift");
_Static_assert(offsetof(struct stat, st_blksize) == 56, "st_blksize offset drift");
_Static_assert(offsetof(struct stat, st_blocks) == 64, "st_blocks offset drift");
_Static_assert(offsetof(struct stat, st_atim) == 72, "st_atim offset drift");
_Static_assert(offsetof(struct stat, st_mtim) == 88, "st_mtim offset drift");
_Static_assert(offsetof(struct stat, st_ctim) == 104, "st_ctim offset drift");
_Static_assert(sizeof(struct dirent) == 280, "Android arm64 dirent size drift");
_Static_assert(sizeof(struct dirent64) == 280,
               "Android arm64 dirent64 size drift");
_Static_assert(offsetof(struct dirent, d_ino) == 0, "dirent ino offset drift");
_Static_assert(offsetof(struct dirent, d_off) == 8, "dirent off offset drift");
_Static_assert(offsetof(struct dirent, d_reclen) == 16,
               "dirent reclen offset drift");
_Static_assert(offsetof(struct dirent, d_type) == 18,
               "dirent type offset drift");
_Static_assert(offsetof(struct dirent, d_name) == 19,
               "dirent name offset drift");
_Static_assert(sizeof(((struct dirent*)0)->d_name) == 256,
               "dirent name capacity drift");
_Static_assert(DT_UNKNOWN == 0 && DT_FIFO == 1 && DT_CHR == 2 && DT_DIR == 4 &&
                   DT_BLK == 6 && DT_REG == 8 && DT_LNK == 10 &&
                   DT_SOCK == 12 && DT_WHT == 14,
               "Android directory type values drift");

_Static_assert(O_RDONLY == 0 && O_WRONLY == 1 && O_RDWR == 2,
               "Android access mode drift");
_Static_assert(O_CREAT == 64 && O_TRUNC == 512 && O_APPEND == 1024,
               "Android write flag drift");
_Static_assert(O_NONBLOCK == 2048 && O_DSYNC == 4096,
               "Android status flag drift");
_Static_assert(O_DIRECTORY == 16384 && O_NOFOLLOW == 32768 && O_DIRECT == 65536,
               "Android arm64 lookup flag drift");
_Static_assert(O_LARGEFILE == 131072 && O_CLOEXEC == 524288,
               "Android arm64 descriptor flag drift");
_Static_assert(O_SYNC == 1052672 && O_PATH == 2097152 && O_TMPFILE == 4210688,
               "Android extended flag drift");

typedef int (*OpenSignature)(const char*, int, ...);
typedef int (*OpenatSignature)(int, const char*, int, ...);
typedef ssize_t (*ReadSignature)(int, void*, size_t);
typedef int (*CloseSignature)(int);
typedef int (*FstatSignature)(int, struct stat*);
typedef int (*StatSignature)(const char*, struct stat*);
typedef ssize_t (*ReadlinkSignature)(const char*, char*, size_t);
typedef char* (*GetcwdSignature)(char*, size_t);
typedef int (*ChdirSignature)(const char*);
typedef DIR* (*OpendirSignature)(const char*);
typedef struct dirent* (*ReaddirSignature)(DIR*);
typedef int (*ClosedirSignature)(DIR*);
_Static_assert(_Generic(&open, OpenSignature: 1, default: 0), "open signature drift");
_Static_assert(_Generic(&openat, OpenatSignature: 1, default: 0),
               "openat signature drift");
_Static_assert(_Generic(&read, ReadSignature: 1, default: 0), "read signature drift");
_Static_assert(_Generic(&close, CloseSignature: 1, default: 0), "close signature drift");
_Static_assert(_Generic(&fstat, FstatSignature: 1, default: 0), "fstat signature drift");
_Static_assert(_Generic(&stat, StatSignature: 1, default: 0), "stat signature drift");
_Static_assert(_Generic(&lstat, StatSignature: 1, default: 0), "lstat signature drift");
_Static_assert(_Generic(&readlink, ReadlinkSignature: 1, default: 0),
               "readlink signature drift");
_Static_assert(_Generic(&getcwd, GetcwdSignature: 1, default: 0),
               "getcwd signature drift");
_Static_assert(_Generic(&chdir, ChdirSignature: 1, default: 0),
               "chdir signature drift");
_Static_assert(_Generic(&opendir, OpendirSignature: 1, default: 0),
               "opendir signature drift");
_Static_assert(_Generic(&readdir, ReaddirSignature: 1, default: 0),
               "readdir signature drift");
_Static_assert(_Generic(&closedir, ClosedirSignature: 1, default: 0),
               "closedir signature drift");
