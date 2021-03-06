
/****************************************************************************
 ** lirc-lsremotes **********************************************************
 ****************************************************************************
 *
 * lirc-lsremotes - list remotes from the remotes database.
 *
 */

#define _GNU_SOURCE

#include <config.h>

#include <dirent.h>
#include <getopt.h>
#include <errno.h>
#include <fnmatch.h>


#include <glob.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lirc_private.h"
#include "lirc_client.h"

static int opt_silent = 0;
static int opt_dump = 0;

static const char* current_dir = NULL;


static const char* const USAGE =
	"List remotes from a copy of the remotes database.\n"
	"Synopsis:\n"
        "    lirc-lsremotes <path> [remote]\n"
        "    lirc-lsremotes [-h | -v]\n\n"
        "<path> is the path to the remotes directory or a single file.\n"
        "[remote] is remotes to list, using wildcards. Defaults  to '*'.\n\n"
        "Options:\n"
        "    -s  --silent       Just parse and print diagnostics.\n"
	"    -d  --dump         Dump complete configuration (noisy).\n"
        "    -v, --version      Print version.\n"
	"    -h, --help         Print this message.\n";


static struct option options[] = {
	{"dump", no_argument, NULL, 'd'},
	{"silent", no_argument, NULL, 's'},
	{"help", no_argument, NULL, 'h'},
	{"version", no_argument, NULL, 'v'},
	{0, 0, 0, 0}
};

/** Given lircd.conf path, write lircmd path or "no_lircmd" in buff. */
void get_lircmd(const char* path, char* buff, ssize_t size)
{
	char pattern[256];
	char* base;
	char* dir;
	char* ext;

	strncpy(buff, path, size - 1);
        ext = strstr(buff, ".lircd.conf");
        if (ext != NULL) {
		*ext = '\0';
	}
	base = basename(buff);
	dir = dirname(buff);
	strncpy(pattern, dir, sizeof(pattern) - 1);
	strncat(pattern, "/", sizeof(pattern) - strlen(pattern) -1);
	strncat(pattern, base, sizeof(pattern) - strlen(pattern) -1);
	strncat(pattern, ".lircmd.conf", sizeof(pattern) - strlen(pattern) -1);
	if (access(pattern, R_OK) == 0) {
		strncpy(buff, basename(pattern), size - 1);
	} else {
		strcpy(buff, "no_lircmd");
	}
}


/** Given lircd.conf path, write photo path or "no_photo" in buff. */
void get_photo(const char* path, char* buff, ssize_t size)
{
	glob_t globbuf;
	int r;
	char* ext;

	strncpy(buff, path, size);
        ext = strstr(buff, ".lircd.conf");
        if (ext != NULL) {
		*ext = '\0';
	}
	strncat(buff,
		"{.jpg,.png,.gif,.JPG,.PNG,.GIF}",
		size - strlen(buff) - 1);
	r = glob(buff, GLOB_BRACE, NULL, &globbuf);
	if (r == GLOB_NOMATCH) {
		strcpy(buff, "no_photo");
		return;
	}
	if (globbuf.gl_pathc > 1) {
		logprintf(LOG_WARNING, "Multiple photos for %s", buff);
	}
	strncpy(buff, basename(globbuf.gl_pathv[0]), size);
}


/** Print a line for each remote definition in lircd.conf file on path. */
void print_remotes(const char* path)
{
	char my_path[256];
	char photo[256];
	char lircmd[256];
	struct ir_remote* r;
	FILE* f;
	const char* dir;
	const char* base;
	const char* timing;

	strncpy(my_path, path, sizeof(my_path));
	base = basename(my_path);
	dir = dirname(my_path);
	if (strrchr(dir, '/') != NULL ) {
		dir = strrchr(dir, '/') + 1;
	}
	f  = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "Cannot open %s (!)\n", path);
		return;
	}
	r = read_config(f, path);
	if (opt_silent) {
		return;
	}
	while (r != NULL && r != (void*) -1) {
		timing = r->pzero != 0 || r->pzero != 0 || is_raw(r) ?
				"timing" : "no_timing";
		strncpy(photo, path, sizeof(photo));
		get_photo(path, photo, sizeof(photo));
		get_lircmd(path, lircmd, sizeof(lircmd));
		printf("%s;%s;%s;%s;%s;%s;%s;%s\n",
			dir,
			base,
			lircmd,
			photo,
			r->name,
			timing,
			is_raw(r) ? "raw" : "no_raw",
			r->driver != NULL ? r->driver : "no_driver");
		fflush(stdout);
		if (opt_dump) {
			fprint_remote(stdout, r, "Dumped by lirc-lsremotes");
		}
		r = r->next;
	};
	fclose(f);
}


int isdir(const struct dirent* ent)
{
 	struct stat statbuf;
	char buff[256];

	snprintf(buff, sizeof(buff), "%s/%s", current_dir, ent->d_name);
	if (stat(buff, &statbuf) == -1) {
		return 0;
	}
	return S_ISDIR(statbuf.st_mode);
}


/** Return true if argument defines a lircd.conf file. */
int isfile(const struct dirent* ent)
{
   	char* dot = strrchr(ent->d_name, '.');
    	if (dot != NULL) {
    		if (strcasecmp(dot + 1, "jpg") == 0) return 0;
    		if (strcasecmp(dot + 1, "gif") == 0) return 0;
    		if (strcasecmp(dot + 1, "png") == 0) return 0;
    		if (strcasecmp(dot + 1, "html") == 0) return 0;
    		if (strcasecmp(dot + 1, "txt") == 0) return 0;
	}
        if (fnmatch("*lircrc*", ent->d_name, 0) == 0) return 0;

	return !isdir(ent);
}


/** List all remotes found in files in dir. */
void listdir(const char* dirname)
{
	char dirpath[256];
	char filepath[256];
	struct dirent **namelist;
	int size;
	int i;

	if (strcmp(dirname, "..") == 0) {
		return;
	}
	if (strlen(dirname) > 0 && dirname[0] == '.') {
		return;
	}
	snprintf(dirpath, sizeof(dirpath), "%s/%s", current_dir, dirname);
	size = scandir(dirpath, &namelist, isfile, alphasort);
	for (i = 0; i < size; i += 1) {
		snprintf(filepath, sizeof(filepath), "%s/%s",
		       dirpath,  namelist[i]->d_name);
		free(namelist[i]);
		print_remotes(filepath);
	}
}


int lsremotes(const char* dirpath, const char* remote)
{
	struct dirent **namelist;
	int size;
	int i;
 	struct stat statbuf;

	if (stat(dirpath, &statbuf) == -1) {
		return 2;
	}
	if (!S_ISDIR(statbuf.st_mode)) {
		print_remotes(dirpath);
		return 0;
	}
	current_dir = dirpath;
	size = scandir(dirpath, &namelist, isdir, alphasort);
	for (i = 0; i < size; i += 1) {
		listdir(namelist[i]->d_name);
		free(namelist[i]);
	}
	return 0;
}


int main(int argc, char** argv)
{
	const char* configs;
	const char* dirpath;
	char path[128];
	int c;

	while ((c = getopt_long(argc, argv, "shdv", options, NULL)) != EOF) {
		switch (c) {
		case 'd':
			opt_dump = 1;
			break;
		case 's':
			opt_silent = 1;
			break;
		case 'h':
			printf(USAGE);
			return (EXIT_SUCCESS);
		case 'v':
			printf("%s\n", "lirc-lsremotes " VERSION);
			return (EXIT_SUCCESS);
		case '?':
			fprintf(stderr, "unrecognized option: -%c\n", optopt);
			fprintf(stderr,
                                "Try `lirc-lsremotes -h' for more information.\n");
			return (EXIT_FAILURE);
		}
	}
	if (argc == optind + 2) {
		dirpath = argv[optind];
		configs = argv[optind + 1];
	} else if (argc == optind + 1) {
		dirpath = argv[optind];
		configs = "*";
	} else {
		fprintf(stderr, USAGE);
		return EXIT_FAILURE;
	}
	lirc_log_get_clientlog("lirc-lsremotes", path, sizeof(path));
	lirc_log_set_file(path);
	lirc_log_open("lirc-lsremotes", 1, LIRC_NOTICE);
	lsremotes(dirpath, configs);
	return 0;
}
