// The firmware's file calls (FatFs API), served from ordinary files under one root folder.
//
// The firmware identifies an open file by FatFs's start cluster: it copies FIL.obj.sclust and
// FIL.obj.objsize into its own "FilePointer" and later rebuilds a FIL from just those two numbers
// (StorageManager::openFilePointer). So the start cluster here is an index into a table of host
// paths, and every read finds its host file through it.
//
// Samples: the firmware does not read sample data through f_read. It asks FatFs which clusters a
// file occupies and reads those sectors itself. So this layer also invents a disk geometry. A
// file's clusters are numbered from its start cluster upwards, and a sector number is a cluster
// number times the sectors per cluster, which makes every cluster and sector number say which
// file and which offset it stands for:
//
//   cluster number = (table slot + 1) << 14 | cluster index within the file     (32 KB clusters)
//   sector number  = cluster number << 6    | sector within the cluster         (512-byte sectors)
//
// A sector number is 32 bits, which leaves 12 for the slot: 4095 files of up to 512 MB each.
// A slot is never given to another file, because the firmware keeps cluster and sector numbers
// for as long as it keeps a sample (cached ones included) and nothing says when it has let go.
// So 4095 is the number of different files one run of the engine can touch. Descriptors are a
// separate matter: only MAX_OPEN host files are open at once, the least recently used is closed.
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

#define MAX_FILES 4095
#define MAX_OPEN 32
#define CLUSTER_INDEX_BITS 14
#define SECTORS_PER_CLUSTER 64 // 32768-byte clusters, the firmware's preferred size
#define SECTOR_BITS 6
#define SLOT_OF(cluster) ((int)((cluster) >> CLUSTER_INDEX_BITS) - 1)
#define START_CLUSTER(slot) ((DWORD)((slot) + 1) << CLUSTER_INDEX_BITS)

static char rootFolder[PATH_MAX] = ".";
static struct {
	char* path; // host path, owned
	int fd;     // -1 when not open
	int writable; // the descriptor was opened for writing too
	unsigned lastUse;
	unsigned long long size;
} files[MAX_FILES];
static int numFiles;  // slots handed out so far; they are used in order
static int numOpen;
static unsigned useCounter;
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

// Returns the start cluster standing for a host path, adding it to the table if needed. 0 if the
// table is full.
static DWORD idForPath(const char* host) {
	for (int i = 0; i < numFiles; i++) {
		if (strcmp(files[i].path, host) == 0) {
			return START_CLUSTER(i);
		}
	}
	if (numFiles >= MAX_FILES) {
		deluge_hosted_log("hosted files: file table full\n");
		return 0;
	}
	const int freeSlot = numFiles;
	files[freeSlot].path = strdup(host);
	if (files[freeSlot].path == NULL) {
		return 0;
	}
	numFiles++;
	files[freeSlot].fd = -1;
	struct stat st;
	files[freeSlot].size = stat(host, &st) == 0 ? (unsigned long long)st.st_size : 0;
	return START_CLUSTER(freeSlot);
}

static void closeSlot(int slot) {
	if (files[slot].fd >= 0) {
		close(files[slot].fd);
		files[slot].fd = -1;
		numOpen--;
	}
}

// Before opening one more host file: if MAX_OPEN are open already, close the one used longest ago.
static void makeRoomForOpen(void) {
	if (numOpen < MAX_OPEN) {
		return;
	}
	int oldest = -1;
	for (int i = 0; i < numFiles; i++) {
		if (files[i].fd >= 0 && (oldest < 0 || (int)(files[i].lastUse - files[oldest].lastUse) < 0)) {
			oldest = i;
		}
	}
	if (oldest >= 0) {
		closeSlot(oldest);
	}
}

// The slot's descriptor, opened if need be. flags is O_RDONLY or O_RDWR; a file open for reading
// that is now wanted for writing is reopened.
static int slotFd(int slot, int flags) {
	if (slot < 0 || slot >= numFiles) {
		return -1;
	}
	const int wantWrite = (flags & O_ACCMODE) != O_RDONLY;
	if (files[slot].fd >= 0 && wantWrite && !files[slot].writable) {
		closeSlot(slot);
	}
	if (files[slot].fd < 0) {
		makeRoomForOpen();
		files[slot].fd = open(files[slot].path, flags, 0644);
		if (files[slot].fd >= 0) {
			files[slot].writable = wantWrite;
			numOpen++;
		}
	}
	files[slot].lastUse = ++useCounter;
	return files[slot].fd;
}

static int fdFor(FIL* fp, int flags) {
	return slotFd(SLOT_OF(fp->obj.sclust), flags);
}

FRESULT f_mount(FATFS* fs, const TCHAR* path, BYTE opt) {
	(void)path;
	(void)opt;
	if (fs != NULL) {
		// What the firmware reads from the mounted volume: the cluster size above all.
		fs->fs_type = FS_FAT32;
		fs->csize = SECTORS_PER_CLUSTER;
		fs->n_fatent = 0x0FFFFFF0;
		fs->free_clst = 0x00100000;
		fs->database = 0;
	}
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
	const int slot = SLOT_OF(id);
	if (writing) {
		// Reopen so creation and truncation take effect.
		closeSlot(slot);
		makeRoomForOpen();
		int flags = O_RDWR | O_CREAT | ((mode & FA_CREATE_ALWAYS) ? O_TRUNC : 0);
		files[slot].fd = open(host, flags, 0644);
		if (files[slot].fd < 0) {
			return FR_DENIED;
		}
		numOpen++;
		files[slot].writable = 1;
		files[slot].lastUse = ++useCounter;
		fstat(files[slot].fd, &st);
	}
	files[slot].size = (unsigned long long)st.st_size;
	fp->obj.fs = &dummyVolume;
	fp->obj.id = dummyVolume.id;
	fp->obj.sclust = id;
	fp->obj.objsize = (FSIZE_t)st.st_size;
	fp->flag = mode;
	fp->fptr = 0;
	return FR_OK;
}

FRESULT f_close(FIL* fp) {
	// The host file stays in the table: the firmware may come back to it through a FilePointer, and
	// reads sample clusters through it long after the close. Its descriptor is closed when it is
	// the least recently used of too many (makeRoomForOpen) and reopened on demand.
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

// --- the invented disk geometry (see the top of this file) ------------------------------------------
LBA_t clst2sect(FATFS* fs, DWORD clst) {
	(void)fs;
	return (LBA_t)clst << SECTOR_BITS;
}

// The cluster after this one in its file's chain, or 0x0FFFFFFF at the end of the file.
DWORD get_fat_from_fs(FATFS* fs, DWORD clst) {
	(void)fs;
	const int slot = SLOT_OF(clst);
	if (slot < 0 || slot >= MAX_FILES || files[slot].path == NULL) {
		return 0xFFFFFFFF; // "disk error"
	}
	const unsigned long long nextIndex = (clst & ((1u << CLUSTER_INDEX_BITS) - 1)) + 1;
	if (nextIndex * (SECTORS_PER_CLUSTER * 512ull) >= files[slot].size || nextIndex >= (1u << CLUSTER_INDEX_BITS)) {
		return 0x0FFFFFFF;
	}
	return clst + 1;
}

// Reads whole sectors by the numbering above. Past the end of the file reads as zeros.
DRESULT disk_read_without_streaming_first(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
	(void)pdrv;
	const DWORD cluster = (DWORD)(sector >> SECTOR_BITS);
	const int fd = slotFd(SLOT_OF(cluster), O_RDONLY);
	if (fd < 0) {
		return RES_ERROR;
	}
	const unsigned long long offset = (unsigned long long)(cluster & ((1u << CLUSTER_INDEX_BITS) - 1))
	                                      * (SECTORS_PER_CLUSTER * 512ull)
	                                  + (unsigned long long)(sector & (SECTORS_PER_CLUSTER - 1)) * 512ull;
	const size_t wanted = (size_t)count * 512;
	ssize_t got = pread(fd, buff, wanted, (off_t)offset);
	if (got < 0) {
		return RES_ERROR;
	}
	if ((size_t)got < wanted) {
		memset(buff + got, 0, wanted - (size_t)got);
	}
	return RES_OK;
}

// --- FatFs directory internals, used only for the firmware's "alternate sample folder" search -------
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
void disk_timerproc(UINT msPassed) {
	(void)msPassed;
}
