/* dos.h — high-level emulated DOS: loader, PSP/MCB memory, INT 21h.
 *
 * The host directory given on the command line is drive C:. Paths are
 * resolved case-insensitively component by component; files created by
 * the guest keep the name it used. Handles map to host file
 * descriptors, with 0-4 being CON/CON/CON/AUX/PRN like DOS.
 *
 * The DOS kernel has no guest-side code at all: INT 21h reaches the HLE
 * stub (pc/pc_bios.c) and dos_int21 runs here. Guest-visible kernel
 * data that programs poke at (InDOS flag, DTA, PSP chain, MCB chain,
 * environment) does live in guest memory so the usual tricks work.
 */
#ifndef DOS_H
#define DOS_H

#include "../core/x86.h"
#include "../pc/pc.h"
#include <time.h>

#define DOS_VERSION_MAJOR 5
#define DOS_VERSION_MINOR 0

#define DOS_SEG          0x0060      /* kernel data: InDOS flag, default DTA, scratch */
#define DOS_FIRST_MCB    0x0100      /* start of the arena; conventional memory ends at A000 */
#define DOS_TOP_SEG      0xA000
#define DOS_MAX_HANDLES  64
#define DOS_MAX_PATH     260

/* Error codes */
enum {
    DE_OK = 0, DE_INVALID_FN = 1, DE_FILE_NOT_FOUND = 2, DE_PATH_NOT_FOUND = 3, DE_TOO_MANY_OPEN = 4,
    DE_ACCESS_DENIED = 5, DE_INVALID_HANDLE = 6, DE_MCB_DESTROYED = 7, DE_NO_MEMORY = 8,
    DE_INVALID_BLOCK = 9, DE_INVALID_ENV = 10, DE_INVALID_FORMAT = 11, DE_INVALID_ACCESS = 12,
    DE_INVALID_DATA = 13, DE_INVALID_DRIVE = 15, DE_RMDIR_CURRENT = 16, DE_NOT_SAME_DEVICE = 17,
    DE_NO_MORE_FILES = 18, DE_WRITE_PROTECT = 19, DE_NOT_READY = 21, DE_FILE_EXISTS = 80,
};

/* Attributes */
enum { DA_RDONLY = 1, DA_HIDDEN = 2, DA_SYSTEM = 4, DA_VOLUME = 8, DA_DIR = 0x10, DA_ARCHIVE = 0x20 };

typedef struct {
    int      fd;             /* host fd, -1 = free */
    uint8_t  dev;            /* 0 file, 1 CON, 2 AUX/NUL, 3 PRN */
    uint8_t  binary;         /* IOCTL raw mode on a device */
    uint8_t  mode;           /* open mode byte */
    uint8_t  drive;
    uint16_t owner_psp;
    char     path[DOS_MAX_PATH];
} dos_handle;

/* A drive letter mapped to a host directory. Removable drives (A:, B:)
 * can carry a list of directories standing in for successive diskettes;
 * the swap key (see pc_kbd.c) advances to the next one. */
typedef struct {
    char     root[DOS_MAX_PATH];      /* host directory that is X:\ ; "" = no such drive */
    char     cwd[DOS_MAX_PATH];       /* current directory, "\\DIR" form without drive */
    char   **disks;                   /* removable: the diskette directories */
    int      ndisks, cur_disk;
} dos_drive;

typedef struct {
    x86_cpu *cpu;
    dos_drive drives[26];
    int      cur_drive;               /* 0 = A:, 2 = C: */
    #define dos_root (dos.drives[dos.cur_drive].root)
    #define dos_cwd  (dos.drives[dos.cur_drive].cwd)
    uint16_t psp;                     /* current PSP segment */
    uint16_t root_psp;                /* the program we loaded; its exit ends the run */
    uint16_t dta_seg, dta_off;
    uint8_t  alloc_strategy;
    uint8_t  verify;
    uint8_t  break_flag;
    int      last_error;
    int      return_code;             /* of the last terminated child (4D) */
    int      terminated;
    dos_handle handles[DOS_MAX_HANDLES];
} dos_state;

extern dos_state dos;

/* dos_load.c */
void dos_init(x86_cpu *cpu, const char *root);
int  dos_mount(int drive, const char *dirs);            /* "dir" or "dir1:dir2:..." for a diskette sequence */
int  dos_swap_disk(void);                              /* next diskette in the current removable drive(s); 1 if swapped */
int  dos_load_program(x86_cpu *cpu, const char *host_path, const char *dos_name, const char *args);
uint16_t dos_mem_alloc(uint16_t paras, uint16_t owner, uint16_t *largest);
int  dos_mem_free(uint16_t seg);
int  dos_mem_resize(uint16_t seg, uint16_t paras, uint16_t *largest);
void dos_mem_free_owner(uint16_t owner);
void dos_terminate(x86_cpu *c, int code, int keep_resident_paras);
void dos_make_child_psp(x86_cpu *c, uint16_t seg, uint16_t top);

/* dos_host.c */
int  dos_resolve(const char *dos_path, char *host_out, size_t n, int *exists, int *is_dir);
int  dos_path_drive(const char *dos_path);              /* drive index of a path (current drive if none) */
void dos_shortname(const char *host_name, char *out13);      /* 8.3 upper-case, "" if not representable */
int  dos_match(const char *pattern83, const char *name83);
uint16_t dos_ftime(time_t t, uint16_t *date);
int  dos_search_first(const char *dos_spec, int attr, uint16_t *id);
int  dos_search_next(uint16_t id, char *name13, int *attr, uint32_t *size, time_t *mtime);
void dos_search_close(uint16_t id);
int  dos_errno(void);

/* dos_int21.c */
void dos_int21(x86_cpu *c, int vector);
void dos_int20(x86_cpu *c, int vector);
void dos_int29(x86_cpu *c, int vector);
void dos_int2f(x86_cpu *c, int vector);

/* Guest string helpers */
void dos_read_str(x86_cpu *c, uint16_t seg, uint16_t off, char *out, size_t n);   /* ASCIIZ */
void dos_write_str(x86_cpu *c, uint16_t seg, uint16_t off, const char *s);

#endif /* DOS_H */
