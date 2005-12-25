/* 
 * Copyright (c) 2005, 2006 Miek Gieben
 * See LICENSE for the license
 */

#include "rdump.h"

/* options */
int opt_null = 0;
int opt_onefilesystem = 0;
int opt_nobackup = 1;
int opt_verbose = 0;

int dumptype;
time_t list_mtime;

/* prototypes */
GSList * dir_crawl(char *path);
GSList * dir_prepend(GSList *l, char *path);
void gfunc_write(gpointer data, gpointer fp);
void gfunc_write_all(gpointer data, gpointer fp);
void gfunc_backup(gpointer data, gpointer usr);
void gfunc_remove(gpointer data, gpointer usr);
void gfunc_free(gpointer data, gpointer user);
gint gfunc_equal(gconstpointer a, gconstpointer b);

void
usage(FILE *f, char *p) 
{	
	fprintf(f, "Usage: %s [OPTION...] FILELIST DIR...\n", p);
	fprintf(f, "rdump generates a full or incremental file list, this\n");
	fprintf(f, "list can be used to implement a (incremental) backup scheme\n");
	fprintf(f, "\n   FILELIST\tincremental file list\n");
	fprintf(f, "   \t\tif not found or empty, a full dump is done\n");
	fprintf(f, "   DIR\t\tdirectory or directories to dump\n");
	fprintf(f, "\nOptions:\n");
	fprintf(f, "   -h\t\tgives this help\n");
	fprintf(f, "   -V\t\tprint version\n");
	fprintf(f, "   -n\t\tdo not look at" NOBACKUP "files\n");
	fprintf(f, "   -v\t\tbe more verbose\n");
	fprintf(f, "   -x\t\tstay in local file system\n");
	fprintf(f, "   -0\t\tdelimit all output with NULLs (not implemented)\n");
	fprintf(f, "\nReport bugs to <miek@miek.nl>\n");
	fprintf(f, "See LICENSE for the license\n");
}

void
version(FILE *f) 
{	
	fprintf(f, "rdump %s\n", VERSION);
}

/**
 * subtrace list *b from list *a, leaving
 * the elements that are only in *a. Essentially
 * a double diff: A diff (A diff B)
 */
GSList *
g_slist_substract(GSList *a, GSList *b)
{
	GSList 		*diff;
	guint 		i;
	gpointer 	data;

	diff = NULL;

	/* everything in a, but NOT in b */
	for(i = 0; i < g_slist_length(a); i++) {
		data = g_slist_nth_data(a, i);

		if (!g_slist_find_custom(b, data, gfunc_equal)) {
			diff = g_slist_append(diff, data);
		}
	}
	return diff;
}

/** 
 * read a filelist, which should hold our previous
 * backup list
 */
GSList *
g_slist_read_file(FILE *fp)
{
	char 	buf[BUFSIZE + 1];
	mode_t  modus;
	char    name[BUFSIZE + 1];
	GSList 	*list;
	struct entry *e;

	list = NULL;
	while ((fgets(buf, BUFSIZE, fp))) {
		/* chop annoying newline off */
		buf[strlen(buf) - 1] = '\0';

		if (sscanf(buf, "%5i %2048[^\n]", &modus, name) != 2) {
			fprintf(stderr, "** Can not parse filelist\n");
			return list;
		} else {
			e = g_malloc(sizeof(struct entry));
			e->f_name = g_strdup(name);
			e->f_mode = modus;
			e->f_uid  = 0;
			e->f_gid  = 0;
			e->f_mtime = 0;

			list = g_slist_prepend(list, (gpointer) e);
		}
	}
	return list;
}

/**
 * return the m_time of the filelist
 */
time_t
mtime(char *f)
{
	struct stat s;

	if (lstat(f, &s) != 0) {
		return 0;
	}
	return s.st_mtime;
}

int 
main(int argc, char **argv) 
{
	GSList 	*backup; 	/* on disk stuff */
	GSList 	*remove;	/* what needs to be rm'd */
	GSList 	*curlist; 	/* previous backup list */
	FILE 	*fplist;
	gint    i;
	int 	c;
	char 	*crawl;
	char    pwd[BUFSIZE + 1];

	curlist = NULL;
	backup = NULL;
	remove = NULL;
	opterr = 0;

	if (((getuid() != geteuid()) || (getgid() != getegid()))) {
		fprintf(stderr, "** For safety reasons " PROGNAME " will not run suid/sgid\n");
		exit(EXIT_FAILURE);
        }

	if (!getcwd(pwd, BUFSIZE)) {
		fprintf(stderr, "** Could not get current working directory\n");
		exit(EXIT_FAILURE);
	}

	while ((c = getopt (argc, argv, "hVnvx0")) != -1) {
		switch (c)
		{
			case 'h':
				usage(stdout, PROGNAME);
				exit(EXIT_SUCCESS);
			case 'V':
				version(stdout);
				exit(EXIT_SUCCESS);
			case 'n':
				opt_nobackup = 0;
				break;
			case 'v':
				opt_verbose = 1;
				break;
			case 'x':
				opt_onefilesystem = 1;
				break;
			case '0':
				opt_null = 1;
				break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 2) {
		fprintf(stderr, "** Can not parse command line\n");
		exit(EXIT_FAILURE); 
	}

	/* Check for full of incremental dump */
	if ((list_mtime = mtime(argv[0])) == 0) {
		dumptype = NULL_DUMP;
	} else {
		dumptype = INC_DUMP;
	}

	if (!(fplist = fopen(argv[0], "a+"))) {
		fprintf(stderr, "** Could not open file\n");
		exit(EXIT_FAILURE);
	} else {
		rewind(fplist);
	}

	curlist = g_slist_read_file(fplist);

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '/') {
			crawl = g_strdup_printf("%s%c%s", pwd, DIR_SEP, argv[i]);
		} else {
			crawl = g_strdup(argv[i]);
		}

		/* add dirs leading up the backup dir */
		backup = dir_prepend(backup, crawl);
		/* if NULL err?? */

		backup = g_slist_concat(backup, dir_crawl(crawl));
		g_free(crawl);
	}

	remove = g_slist_substract(curlist, backup); 

	/* first what to remove, then what to backup */
	g_slist_foreach(remove, gfunc_remove, NULL); 
	g_slist_foreach(backup, gfunc_backup, NULL);

	/* write new filelist */
	ftruncate(fileno(fplist), 0);  
	g_slist_foreach(backup, gfunc_write, fplist);
	fclose(fplist); 

	g_slist_foreach(curlist, gfunc_free, NULL);
	g_slist_foreach(backup, gfunc_free, NULL);
/*	g_slist_foreach(remove, gfunc_free, NULL); */
	
	/* I free too much... */
	g_slist_free(curlist);
	g_slist_free(backup);
#if 0
	g_slist_free(remove);
#endif
	
	exit(EXIT_SUCCESS);
}
