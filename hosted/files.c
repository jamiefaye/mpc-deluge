// The firmware's file calls (FatFs API), served from ordinary files under one root folder.
//
// The firmware identifies an open file by FatFs's start cluster: it copies FIL.obj.sclust and
// FIL.obj.objsize into its own "FilePointer" and later rebuilds a FIL from just those two numbers
// (StorageManager::openFilePointer). So the start cluster here is an index into a table of host
// paths, and every read finds its host file through it.
//
// The parts of FatFs the firmware reaches into directly for sample streaming (cluster chains,
// raw sector reads) have no equivalent here and report failure; samples are not supported yet.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hosted.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fatfs/diskio.h"
#include "fatfs/ff.h"

// Directory reading lives in dirs.c: FatFs and <dirent.h> both define a type called DIR, so the two
// cannot meet in one file. A FatFs DIR here carries the host handle in obj.sclust.
int deluge_hosted_dir_open(const char* hostPath);
int deluge_hosted_dir_next(int handle, char* name, size_t nameSize, int* isDir, unsigned long long* size);
void deluge_hosted_dir_close(int handle);
const char* deluge_hosted_dir_path(int handle);

#define MAX_FILES 256
#define FIRST_ID 2 // FatFs treats cluster 0 as "none"

static char rootFolder[PATH_MAX] = ".";
static struct {
	char* path; // host path, owned
	int fd;     // -1 until first read or write
} files[MAX_FILES];
static FATFS dummyVolume;

void deluge_hosted_set_root(const char* folder) {
	snprintf(rootFolder, sizeof rootFolder, "%s", folder);
}

static void hostPath(const char* path, char* out, size_t outSize) {
	while (*path == '/' || *path == '\\') {
		path++;
	}
	snprintf(out, outSize, "%s/%s", rootFolder, path);
}

// Returns the table id for a host path, adding it if needed. 0 if the table is full.
static DWORD idForPath(const char* host) {
	int freeSlot = -1;
	for (int i = 0; i < MAX_FILES; i++) {
		if (files[i].path == NULL) {
			if (freeSlot < 0) {
				freeSlot = i;
			}
		}
		else if (strcmp(files[i].path, host) == 0) {
			return (DWORD)(i + FIRST_ID);
		}
	}
	if (freeSlot < 0) {
		return 0;
	}
	files[freeSlot].path = strdup(host);
	files[freeSlot].fd = -1;
	return (DWORD)(freeSlot + FIRST_ID);
}

static int fdFor(FIL* fp, int flags) {
	DWORD id = fp->obj.sclust;
	if (id < FIRST_ID || id >= FIRST_ID + MAX_FILES || files[id - FIRST_ID].path == NULL) {
		return -1;
	}
	if (files[id - FIRST_ID].fd < 0) {
		files[id - FIRST_ID].fd = open(files[id - FIRST_ID].path, flags, 0644);
	}
	return files[id - FIRST_ID].fd;
}

FRESULT f_mount(FATFS* fs, const TCHAR* path, BYTE opt) {
	(void)fs;
	(void)path;
	(void)opt;
	return FR_OK;
}

FRESULT f_open(FIL* fp, const TCHAR* path, BYTE mode) {
	char host[PATH_MAX];
	hostPath(path, host, sizeof host);
	struct stat st;
	int exists = stat(host, &st) == 0;
	if (exists && S_ISDIR(st.st_mode)) {
		return FR_NO_FILE;
	}
	int writing = (mode & FA_WRITE) != 0;
	if (!exists) {
		if (!(mode & (FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS))) {
			return FR_NO_FILE;
		}
	}
	else if (mode & FA_CREATE_NEW) {
		return FR_EXIST;
	}
	memset(fp, 0, sizeof *fp);
	DWORD id = idForPath(host);
	if (id == 0) {
		return FR_TOO_MANY_OPEN_FILES;
	}
	if (writing) {
		// Reopen so creation and truncation take effect.
		if (files[id - FIRST_ID].fd >= 0) {
			close(files[id - FIRST_ID].fd);
		}
		int flags = O_RDWR | O_CREAT | ((mode & FA_CREATE_ALWAYS) ? O_TRUNC : 0);
		files[id - FIRST_ID].fd = open(host, flags, 0644);
		if (files[id - FIRST_ID].fd < 0) {
			return FR_DENIED;
		}
		fstat(files[id - FIRST_ID].fd, &st);
	}
	fp->obj.fs = &dummyVolume;
	fp->obj.id = dummyVolume.id;
	fp->obj.sclust = id;
	fp->obj.objsize = (FSIZE_t)st.st_size;
	fp->flag = mode;
	fp->fptr = 0;
	return FR_OK;
}

FRESULT f_close(FIL* fp) {
	// The host file stays in the table: the firmware may come back to it through a FilePointer.
	DWORD id = fp->obj.sclust;
	if (id >= FIRST_ID && id < FIRST_ID + MAX_FILES && files[id - FIRST_ID].fd >= 0) {
		close(files[id - FIRST_ID].fd);
		files[id - FIRST_ID].fd = -1;
	}
	fp->obj.fs = NULL;
	return FR_OK;
}

FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br) {
	*br = 0;
	int fd = fdFor(fp, O_RDONLY);
	if (fd < 0) {
		return FR_INVALID_OBJECT;
	}
	ssize_t n = pread(fd, buff, btr, (off_t)fp->fptr);
	if (n < 0) {
		return FR_DISK_ERR;
	}
	fp->fptr += (FSIZE_t)n;
	*br = (UINT)n;
	return FR_OK;
}

FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw) {
	*bw = 0;
	int fd = fdFor(fp, O_RDWR);
	if (fd < 0) {
		return FR_INVALID_OBJECT;
	}
	ssize_t n = pwrite(fd, buff, btw, (off_t)fp->fptr);
	if (n < 0) {
		return FR_DISK_ERR;
	}
	fp->fptr += (FSIZE_t)n;
	if (fp->fptr > fp->obj.objsize) {
		fp->obj.objsize = fp->fptr;
	}
	*bw = (UINT)n;
	return FR_OK;
}

FRESULT f_lseek(FIL* fp, FSIZE_t ofs) {
	fp->fptr = ofs;
	return FR_OK;
}

FRESULT f_truncate(FIL* fp) {
	int fd = fdFor(fp, O_RDWR);
	if (fd < 0 || ftruncate(fd, (off_t)fp->fptr) != 0) {
		return FR_DISK_ERR;
	}
	fp->obj.objsize = fp->fptr;
	return FR_OK;
}

static void fillInfo(FILINFO* fno, const char* name, const struct stat* st) {
	memset(fno, 0, sizeof *fno);
	fno->fsize = (FSIZE_t)st->st_size;
	fno->fattrib = S_ISDIR(st->st_mode) ? AM_DIR : 0;
	snprintf(fno->fname, sizeof fno->fname, "%s", name);
}

FRESULT f_stat(const TCHAR* path, FILINFO* fno) {
	char host[PATH_MAX];
	hostPath(path, host, sizeof host);
	struct stat st;
	if (stat(host, &st) != 0) {
		return FR_NO_FILE;
	}
	if (fno != NULL) {
		const char* slash = strrchr(host, '/');
		fillInfo(fno, slash ? slash + 1 : host, &st);
	}
	return FR_OK;
}

FRESULT f_mkdir(const TCHAR* path) {
	char host[PATH_MAX];
	hostPath(path, host, sizeof host);
	if (mkdir(host, 0755) == 0) {
		return FR_OK;
	}
	struct stat st;
	return stat(host, &st) == 0 ? FR_EXIST : FR_NO_PATH;
}

FRESULT f_unlink(const TCHAR* path) {
	char host[PATH_MAX];
	hostPath(path, host, sizeof host);
	return remove(host) == 0 ? FR_OK : FR_NO_FILE;
}

FRESULT f_rename(const TCHAR* path_old, const TCHAR* path_new) {
	char a[PATH_MAX], b[PATH_MAX];
	hostPath(path_old, a, sizeof a);
	hostPath(path_new, b, sizeof b);
	return rename(a, b) == 0 ? FR_OK : FR_NO_FILE;
}

FRESULT f_opendir(DIR* dp, const TCHAR* path) {
	char host[PATH_MAX];
	hostPath(path, host, sizeof host);
	int handle = deluge_hosted_dir_open(host);
	if (handle < 0) {
		return FR_NO_PATH;
	}
	memset(dp, 0, sizeof *dp);
	dp->obj.fs = &dummyVolume;
	dp->obj.id = dummyVolume.id;
	dp->obj.sclust = (DWORD)handle;
	return FR_OK;
}

FRESULT f_closedir(DIR* dp) {
	deluge_hosted_dir_close((int)dp->obj.sclust);
	dp->obj.fs = NULL;
	return FR_OK;
}

// Like f_readdir, and also reports the entry as a FilePointer (the firmware's addition to FatFs).
// The end of the directory is an entry with an empty name, as in FatFs.
FRESULT f_readdir_get_filepointer(DIR* dp, FILINFO* fno, FilePointer* filePointer) {
	char name[FF_LFN_BUF + 1];
	int isDir = 0;
	unsigned long long size = 0;
	memset(fno, 0, sizeof *fno);
	if (!deluge_hosted_dir_next((int)dp->obj.sclust, name, sizeof name, &isDir, &size)) {
		return FR_OK;
	}
	fno->fsize = (FSIZE_t)size;
	fno->fattrib = isDir ? AM_DIR : 0;
	snprintf(fno->fname, sizeof fno->fname, "%s", name);
	if (filePointer != NULL) {
		char host[PATH_MAX];
		snprintf(host, sizeof host, "%s/%s", deluge_hosted_dir_path((int)dp->obj.sclust), name);
		filePointer->sclust = isDir ? 0 : idForPath(host);
		filePointer->objsize = (FSIZE_t)size;
	}
	return FR_OK;
}

FRESULT f_readdir(DIR* dp, FILINFO* fno) {
	return f_readdir_get_filepointer(dp, fno, NULL);
}

// --- FatFs internals the firmware calls for sample streaming: not available --------------------------
LBA_t clst2sect(FATFS* fs, DWORD clst) {
	(void)fs;
	(void)clst;
	return 0; // "invalid cluster"
}
DWORD get_fat_from_fs(FATFS* fs, DWORD clst) {
	(void)fs;
	(void)clst;
	return 0xFFFFFFFF; // "disk error"
}
DWORD ld_clust(FATFS* fs, const BYTE* dir) {
	(void)fs;
	(void)dir;
	return 0;
}
DWORD ld_dword(const BYTE* ptr) {
	(void)ptr;
	return 0;
}
FRESULT dir_find(DIR* dp) {
	(void)dp;
	return FR_NO_FILE;
}
FRESULT create_name(DIR* dp, const TCHAR** path) {
	(void)dp;
	(void)path;
	return FR_INVALID_NAME;
}
DSTATUS disk_status(BYTE pdrv) {
	(void)pdrv;
	return 0; // present and initialised
}
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
	(void)pdrv;
	(void)buff;
	(void)sector;
	(void)count;
	return RES_ERROR;
}
DRESULT disk_read_without_streaming_first(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
	(void)pdrv;
	(void)buff;
	(void)sector;
	(void)count;
	return RES_ERROR;
}
void disk_timerproc(UINT msPassed) {
	(void)msPassed;
}
