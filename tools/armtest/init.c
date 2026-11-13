// The only program in the emulated machine's start-up: it is /init in the initramfs.
// It gives itself a console, runs the command in /cmd (one argument per line), prints the exit
// status and any files left in /out as base64 between markers, and powers the machine off.
// Built static (musl) so it needs nothing from the initramfs.
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void dump_base64(const char* path, const char* name) {
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	FILE* f = fopen(path, "rb");
	if (!f) return;
	printf("ARMTEST-FILE-BEGIN %s\n", name);
	unsigned char in[57];
	size_t n;
	while ((n = fread(in, 1, sizeof in, f)) > 0) {
		char out[80];
		size_t o = 0;
		for (size_t i = 0; i < n; i += 3) {
			unsigned v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
			out[o++] = t[v >> 18 & 63];
			out[o++] = t[v >> 12 & 63];
			out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
			out[o++] = i + 2 < n ? t[v & 63] : '=';
		}
		out[o] = 0;
		puts(out);
	}
	fclose(f);
	printf("ARMTEST-FILE-END %s\n", name);
}

int main(void) {
	mkdir("/dev", 0755);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mkdir("/proc", 0755);
	mount("proc", "/proc", "proc", 0, NULL);
	mkdir("/tmp", 0777);
	mkdir("/out", 0777);
	int fd = open("/dev/console", O_RDWR);
	if (fd >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("ARMTEST-BOOTED");

	char* argv[64];
	int argc = 0;
	static char text[8192];
	FILE* f = fopen("/cmd", "r");
	if (f) {
		size_t n = fread(text, 1, sizeof text - 1, f);
		text[n] = 0;
		fclose(f);
		for (char* line = strtok(text, "\n"); line && argc < 63; line = strtok(NULL, "\n")) argv[argc++] = line;
	}
	argv[argc] = NULL;
	// /env: NAME=VALUE lines for the command's environment (run.py --env).
	static char envText[2048];
	f = fopen("/env", "r");
	if (f) {
		size_t n = fread(envText, 1, sizeof envText - 1, f);
		envText[n] = 0;
		fclose(f);
		for (char* line = strtok(envText, "\n"); line; line = strtok(NULL, "\n")) putenv(line);
	}
	int status = 255;
	if (argc > 0) {
		pid_t pid = fork();
		if (pid == 0) {
			execv(argv[0], argv);
			perror("execv");
			_exit(127);
		}
		int ws = 0;
		waitpid(pid, &ws, 0);
		if (WIFEXITED(ws)) status = WEXITSTATUS(ws);
		else if (WIFSIGNALED(ws)) {
			printf("ARMTEST-SIGNAL %d\n", WTERMSIG(ws));
			status = 128 + WTERMSIG(ws);
		}
	}
	printf("ARMTEST-EXIT %d\n", status);

	DIR* d = opendir("/out");
	if (d) {
		struct dirent* e;
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.') continue;
			char path[512];
			snprintf(path, sizeof path, "/out/%s", e->d_name);
			dump_base64(path, e->d_name);
		}
		closedir(d);
	}
	// A plugin under test may leave a diagnostics file in its own folder; return that too.
	d = opendir("/plugins");
	if (d) {
		struct dirent* e;
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.') continue;
			char path[768];
			snprintf(path, sizeof path, "/plugins/%s/diagnostics.txt", e->d_name);
			dump_base64(path, "diagnostics.txt");
		}
		closedir(d);
	}
	puts("ARMTEST-DONE");
	sync();
	reboot(RB_POWER_OFF);
	for (;;) pause();
}
