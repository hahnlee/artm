#include <stddef.h>
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
_Static_assert(_Generic(&open, OpenSignature: 1, default: 0), "open signature drift");
_Static_assert(_Generic(&openat, OpenatSignature: 1, default: 0),
               "openat signature drift");
_Static_assert(_Generic(&read, ReadSignature: 1, default: 0), "read signature drift");
_Static_assert(_Generic(&close, CloseSignature: 1, default: 0), "close signature drift");
_Static_assert(_Generic(&fstat, FstatSignature: 1, default: 0), "fstat signature drift");
