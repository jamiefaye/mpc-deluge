// Directory reading for files.c, kept apart because <dirent.h> and FatFs both define "DIR".
// SPDX-License-Identifier: GPL-3.0-or-later
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_DIRS 16
static struct {
	DIR* dir;
	char* path;
} dirs[MAX_DIRS];

int deluge_hosted_dir_open(const char* hostPath) {
	for (int i = 0; i < MAX_DIRS; i++) {
		if (dirs[i].dir == NULL) {
			dirs[i].dir = opendir(hostPath);
			if (dirs[i].dir == NULL) {
				return -1;
			}
			dirs[i].path = strdup(hostPath);
			return i;
		}
	}
	return -1;
}

const char* deluge_hosted_dir_path(int handle) {
	return (handle >= 0 && handle < MAX_DIRS && dirs[handle].path) ? dirs[handle].path : "";
}

// Returns 0 at the end of the directory.
int deluge_hosted_dir_next(int handle, char* name, size_t nameSize, int* isDir, unsigned long long* size) {
	if (handle < 0 || handle >= MAX_DIRS || dirs[handle].dir == NULL) {
		return 0;
	}
	struct dirent* e;
	while ((e = readdir(dirs[handle].dir)) != NULL) {
		if (e->d_name[0] == '.') {
			continue; // FatFs does not report dot entries; hidden host files are skipped too
		}
		char full[4096];
		snprintf(full, sizeof full, "%s/%s", dirs[handle].path, e->d_name);
		struct stat st;
		if (stat(full, &st) != 0) {
			continue;
		}
		snprintf(name, nameSize, "%s", e->d_name);
		*isDir = S_ISDIR(st.st_mode);
		*size = (unsigned long long)st.st_size;
		return 1;
	}
	return 0;
}

void deluge_hosted_dir_close(int handle) {
	if (handle >= 0 && handle < MAX_DIRS && dirs[handle].dir != NULL) {
		closedir(dirs[handle].dir);
		free(dirs[handle].path);
		dirs[handle].dir = NULL;
		dirs[handle].path = NULL;
	}
}
