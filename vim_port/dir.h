/*
 * dir.h - Minimal directory header stub for vim on seolsem
 */

#ifndef _DIR_H_SEOLSEM
#define _DIR_H_SEOLSEM

/* Maximum path length */
#define MAXPATH     64
#define MAXDIR      64
#define MAXDRIVE    3
#define MAXFILE     9
#define MAXEXT      5

/* Directory functions (stubbed - flat filesystem) */
static int chdir(const char *path) {
    (void)path;
    return 0;  /* Always succeed, single directory */
}

static char *getcwd(char *buf, int size) {
    if (buf && size > 1) {
        buf[0] = '/';
        buf[1] = '\0';
    }
    return buf;
}

static int mkdir(const char *path) {
    (void)path;
    return -1;  /* Not supported */
}

static int rmdir(const char *path) {
    (void)path;
    return -1;  /* Not supported */
}

/* Path splitting (minimal) */
static void _splitpath(const char *path, char *drive, char *dir, char *fname, char *ext) {
    const char *p;
    const char *lastslash = path;
    const char *lastdot = 0;
    
    if (drive) drive[0] = '\0';
    if (dir) dir[0] = '\0';
    if (fname) fname[0] = '\0';
    if (ext) ext[0] = '\0';
    
    /* Find last slash and last dot */
    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\') lastslash = p + 1;
        if (*p == '.') lastdot = p;
    }
    
    /* Copy filename */
    if (fname) {
        const char *end = lastdot ? lastdot : p;
        int i = 0;
        while (lastslash < end && i < MAXFILE - 1) {
            fname[i++] = *lastslash++;
        }
        fname[i] = '\0';
    }
    
    /* Copy extension */
    if (ext && lastdot) {
        int i = 0;
        while (*lastdot && i < MAXEXT - 1) {
            ext[i++] = *lastdot++;
        }
        ext[i] = '\0';
    }
}

static void _makepath(char *path, const char *drive, const char *dir, 
                      const char *fname, const char *ext) {
    path[0] = '\0';
    (void)drive;  /* Ignore drive on seolsem */
    
    if (dir && dir[0]) {
        /* strcat equivalent */
        char *p = path;
        while (*p) p++;
        while (*dir) *p++ = *dir++;
        *p = '\0';
    }
    
    if (fname && fname[0]) {
        char *p = path;
        while (*p) p++;
        while (*fname) *p++ = *fname++;
        *p = '\0';
    }
    
    if (ext && ext[0]) {
        char *p = path;
        while (*p) p++;
        if (ext[0] != '.') *p++ = '.';
        while (*ext) *p++ = *ext++;
        *p = '\0';
    }
}

/* findfirst/findnext stubs */
#define findfirst(path, buf, attr)  (-1)
#define findnext(buf)               (-1)

struct ffblk {
    char ff_reserved[21];
    char ff_attrib;
    unsigned short ff_ftime;
    unsigned short ff_fdate;
    unsigned long ff_fsize;
    char ff_name[13];
};

#endif /* _DIR_H_SEOLSEM */
