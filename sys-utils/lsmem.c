/*
 * lsmem - Show memory configuration
 *
 * Copyright IBM Corp. 2016
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <c.h>
#include <nls.h>
#include <path.h>
#include <strutils.h>
#include <closestream.h>
#include <xalloc.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <strutils.h>
#include <fcntl.h>
#include <inttypes.h>
#include <assert.h>
#include <optutils.h>
#include <libsmartcols.h>

#define _PATH_SYS_MEMORY		"/sys/devices/system/memory"
#define _PATH_SYS_MEMORY_BLOCK_SIZE	_PATH_SYS_MEMORY "/block_size_bytes"

#define MEMORY_STATE_ONLINE		0
#define MEMORY_STATE_OFFLINE		1
#define MEMORY_STATE_GOING_OFFLINE	2
#define MEMORY_STATE_UNKNOWN		3

struct memory_block {
	uint64_t	index;
	uint64_t	count;
	int		state;
	int		node;
	unsigned int	removable:1;
};

struct lsmem_desc {
	struct dirent		**dirs;
	int			ndirs;
	struct memory_block	*blocks;
	int			nblocks;
	uint64_t		block_size;
	uint64_t		mem_online;
	uint64_t		mem_offline;

	struct libscols_table	*table;
	unsigned int		have_nodes : 1,
				raw : 1,
				export : 1,
				json : 1,
				noheadings : 1,
				summary : 1,
				list_all : 1,
				bytes : 1,
				want_node : 1,
				want_state : 1,
				want_removable : 1;
};

enum {
	COL_RANGE,
	COL_SIZE,
	COL_STATE,
	COL_REMOVABLE,
	COL_BLOCK,
	COL_NODE,
};

/* column names */
struct coldesc {
	const char	*name;		/* header */
	double		whint;		/* width hint (N < 1 is in percent of termwidth) */
	int		flags;		/* SCOLS_FL_* */
	const char      *help;

	int	sort_type;		/* SORT_* */
};

/* columns descriptions */
static struct coldesc coldescs[] = {
	[COL_RANGE]	= { "RANGE", 0, 0, N_("adress range")},
	[COL_SIZE]	= { "SIZE", 5, SCOLS_FL_RIGHT, N_("size of memory")},
	[COL_STATE]	= { "STATE", 0, 0, N_("state of memory")},
	[COL_REMOVABLE]	= { "REMOVABLE", 0, SCOLS_FL_RIGHT, N_("memory is removable")},
	[COL_BLOCK]	= { "BLOCK", 0, SCOLS_FL_RIGHT, N_("memory block")},
	[COL_NODE]	= { "NODE", 0, SCOLS_FL_RIGHT, N_("node information")},
};

/* columns[] array specifies all currently wanted output column. The columns
 * are defined by coldescs[] array and you can specify (on command line) each
 * column twice. That's enough, dynamically allocated array of the columns is
 * unnecessary overkill and over-engineering in this case */
static int columns[ARRAY_SIZE(coldescs) * 2];
static size_t ncolumns;

static inline size_t err_columns_index(size_t arysz, size_t idx)
{
	if (idx >= arysz)
		errx(EXIT_FAILURE, _("too many columns specified, "
				     "the limit is %zu columns"),
				arysz - 1);
	return idx;
}

#define add_column(ary, n, id)	\
		((ary)[ err_columns_index(ARRAY_SIZE(ary), (n)) ] = (id))

static int column_name_to_id(const char *name, size_t namesz)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(coldescs); i++) {
		const char *cn = coldescs[i].name;

		if (!strncasecmp(name, cn, namesz) && !*(cn + namesz))
			return i;
	}
	warnx(_("unknown column: %s"), name);
	return -1;
}

static inline int get_column_id(int num)
{
	assert(num >= 0);
	assert((size_t) num < ncolumns);
	assert(columns[num] < (int) ARRAY_SIZE(coldescs));

	return columns[num];
}

static inline struct coldesc *get_column_desc(int num)
{
	return &coldescs[ get_column_id(num) ];
}

static inline int has_column(int id)
{
	size_t i;

	for (i = 0; i < ncolumns; i++)
		if (columns[i] == id)
			return 1;
	return 0;
}

static void add_scols_line(struct lsmem_desc *desc, struct memory_block *blk)
{
	size_t i;
	struct libscols_line *line;

	line = scols_table_new_line(desc->table, NULL);
	if (!line)
		err_oom();

	for (i = 0; i < ncolumns; i++) {
		char *str = NULL;

		switch (get_column_id(i)) {
		case COL_RANGE:
		{
			uint64_t start = blk->index * desc->block_size;
			uint64_t size = blk->count * desc->block_size;
			xasprintf(&str, "0x%016"PRIx64"-0x%016"PRIx64, start, start + size - 1);
			break;
		}
		case COL_SIZE:
			if (desc->bytes)
				xasprintf(&str, "%"PRId64, (uint64_t) blk->count * desc->block_size);
			else
				str = size_to_human_string(SIZE_SUFFIX_1LETTER,
						(uint64_t) blk->count * desc->block_size);
			break;
		case COL_STATE:
			str = xstrdup(
				blk->state == MEMORY_STATE_ONLINE ? _("online") :
				blk->state == MEMORY_STATE_OFFLINE ? _("offline") :
				blk->state == MEMORY_STATE_GOING_OFFLINE ? _("on->off") :
				"?");
			break;
		case COL_REMOVABLE:
			if (blk->state == MEMORY_STATE_ONLINE)
				str = xstrdup(blk->removable ? _("yes") : _("no"));
			break;
		case COL_BLOCK:
			if (blk->count == 1)
				xasprintf(&str, "%"PRId64, blk->index);
			else
				xasprintf(&str, "%"PRId64"-%"PRId64,
					 blk->index, blk->index + blk->count - 1);
			break;
		case COL_NODE:
			if (desc->have_nodes)
				xasprintf(&str, "%d", blk->node);
			break;
		}

		if (str && scols_line_refer_data(line, i, str) != 0)
			err_oom();
	}
}

static void fill_scols_table(struct lsmem_desc *desc)
{
	int i;

	for (i = 0; i < desc->nblocks; i++)
		add_scols_line(desc, &desc->blocks[i]);
}

static void print_summary(struct lsmem_desc *desc)
{
	fprintf(stdout, _("Memory block size   : %8s\n"),
		size_to_human_string(SIZE_SUFFIX_1LETTER, desc->block_size));
	fprintf(stdout, _("Total online memory : %8s\n"),
		size_to_human_string(SIZE_SUFFIX_1LETTER, desc->mem_online));
	fprintf(stdout, _("Total offline memory: %8s\n"),
		size_to_human_string(SIZE_SUFFIX_1LETTER, desc->mem_offline));
}

static int memory_block_get_node(char *name)
{
	struct dirent *de;
	char *path;
	DIR *dir;
	int node;

	path = path_strdup(_PATH_SYS_MEMORY"/%s", name);
	dir = opendir(path);
	free(path);
	if (!dir)
		err(EXIT_FAILURE, _("Failed to open %s"), path);
	node = -1;
	while ((de = readdir(dir)) != NULL) {
		if (strncmp("node", de->d_name, 4))
			continue;
		if (!isdigit_string(de->d_name + 4))
			continue;
		node = strtol(de->d_name + 4, NULL, 10);
	}
	closedir(dir);
	return node;
}

static void memory_block_read_attrs(struct lsmem_desc *desc, char *name,
				    struct memory_block *blk)
{
	char line[BUFSIZ];

	blk->count = 1;
	blk->index = strtoumax(name + 6, NULL, 10); /* get <num> of "memory<num>" */
	blk->removable = path_read_u64(_PATH_SYS_MEMORY"/%s/%s", name, "removable");
	blk->state = MEMORY_STATE_UNKNOWN;
	path_read_str(line, sizeof(line), _PATH_SYS_MEMORY"/%s/%s", name, "state");
	if (strcmp(line, "offline") == 0)
		blk->state = MEMORY_STATE_OFFLINE;
	else if (strcmp(line, "online") == 0)
		blk->state = MEMORY_STATE_ONLINE;
	else if (strcmp(line, "going-offline") == 0)
		blk->state = MEMORY_STATE_GOING_OFFLINE;
	if (desc->have_nodes)
		blk->node = memory_block_get_node(name);
}

static int is_mergeable(struct lsmem_desc *desc, struct memory_block *blk)
{
	struct memory_block *curr;

	if (!desc->nblocks)
		return 0;
	curr = &desc->blocks[desc->nblocks - 1];
	if (desc->list_all)
		return 0;
	if (curr->index + curr->count != blk->index)
		return 0;
	if (desc->want_state && curr->state != blk->state)
		return 0;
	if (desc->want_removable && curr->removable != blk->removable)
		return 0;
	if (desc->want_node && desc->have_nodes) {
		if (curr->node != blk->node)
			return 0;
	}
	return 1;
}

static void read_info(struct lsmem_desc *desc)
{
	struct memory_block blk;
	char line[BUFSIZ];
	int i;

	path_read_str(line, sizeof(line), _PATH_SYS_MEMORY_BLOCK_SIZE);
	desc->block_size = strtoumax(line, NULL, 16);

	for (i = 0; i < desc->ndirs; i++) {
		memory_block_read_attrs(desc, desc->dirs[i]->d_name, &blk);
		if (is_mergeable(desc, &blk)) {
			desc->blocks[desc->nblocks - 1].count++;
			continue;
		}
		desc->nblocks++;
		desc->blocks = xrealloc(desc->blocks, desc->nblocks * sizeof(blk));
		*&desc->blocks[desc->nblocks - 1] = blk;
	}
	for (i = 0; i < desc->nblocks; i++) {
		if (desc->blocks[i].state == MEMORY_STATE_ONLINE)
			desc->mem_online += desc->block_size * desc->blocks[i].count;
		else
			desc->mem_offline += desc->block_size * desc->blocks[i].count;
	}
}

static int memory_block_filter(const struct dirent *de)
{
	if (strncmp("memory", de->d_name, 6))
		return 0;
	return isdigit_string(de->d_name + 6);
}

static void read_basic_info(struct lsmem_desc *desc)
{
	char *dir;

	if (!path_exist(_PATH_SYS_MEMORY_BLOCK_SIZE))
		errx(EXIT_FAILURE, _("This system does not support memory blocks"));

	dir = path_strdup(_PATH_SYS_MEMORY);
	desc->ndirs = scandir(dir, &desc->dirs, memory_block_filter, versionsort);
	free(dir);
	if (desc->ndirs <= 0)
		err(EXIT_FAILURE, _("Failed to read %s"), _PATH_SYS_MEMORY);

	if (memory_block_get_node(desc->dirs[0]->d_name) != -1)
		desc->have_nodes = 1;
}

static void __attribute__((__noreturn__)) lsmem_usage(FILE *out)
{
	unsigned int i;

	fputs(USAGE_HEADER, out);
	fprintf(out, _(" %s [options]\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("List the ranges of available memory with their online status.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -J, --json           use JSON output format\n"), out);
	fputs(_(" -P, --pairs          use key=\"value\" output format\n"), out);
	fputs(_(" -a, --all            list each individiual memory block\n"), out);
	fputs(_(" -b, --bytes          print SIZE in bytes rather than in human readable format\n"), out);
	fputs(_(" -n, --noheadings     don't print headings\n"), out);
	fputs(_(" -o, --output <list>  output columns\n"), out);
	fputs(_(" -r, --raw            use raw output format\n"), out);
	fputs(_(" -s, --sysroot <dir>  use the specified directory as system root\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);

	fputs(_("\nAvailable columns:\n"), out);

	for (i = 0; i < ARRAY_SIZE(coldescs); i++)
		fprintf(out, " %10s  %s\n", coldescs[i].name, coldescs[i].help);

	fprintf(out, USAGE_MAN_TAIL("lsmem(1)"));

	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	struct lsmem_desc _desc = { }, *desc = &_desc;
	const char *outarg = NULL;
	int c;
	size_t i;

	static const struct option longopts[] = {
		{"all",		no_argument,		NULL, 'a'},
		{"bytes",	no_argument,		NULL, 'b'},
		{"help",	no_argument,		NULL, 'h'},
		{"json",	no_argument,		NULL, 'J'},
		{"noheadings",	no_argument,		NULL, 'n'},
		{"output",	required_argument,	NULL, 'o'},
		{"pairs",	no_argument,		NULL, 'P'},
		{"raw",		no_argument,		NULL, 'r'},
		{"sysroot",	required_argument,	NULL, 's'},
		{"version",	no_argument,		NULL, 'V'},
		{NULL,		0,			NULL, 0}
	};
	static const ul_excl_t excl[] = {	/* rows and cols in ASCII order */
		{ 'J', 'P', 'r' },
		{ 0 }
	};
	int excl_st[ARRAY_SIZE(excl)] = UL_EXCL_STATUS_INIT;

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((c = getopt_long(argc, argv, "abhJno:Prs:V", longopts, NULL)) != -1) {

		err_exclusive_options(c, longopts, excl, excl_st);

		switch (c) {
		case 'a':
			desc->list_all = 1;
			break;
		case 'b':
			desc->bytes = 1;
			break;
		case 'h':
			lsmem_usage(stdout);
			break;
		case 'J':
			desc->json = 1;
			break;
		case 'n':
			desc->noheadings = 1;
			break;
		case 'o':
			outarg = optarg;
			break;
		case 'P':
			desc->export = 1;
			break;
		case 'r':
			desc->raw = 1;
			break;
		case 's':
			path_set_prefix(optarg);
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return 0;
		default:
			lsmem_usage(stderr);
		}
	}

	if (argc != optind)
		lsmem_usage(stderr);

	/*
	 * Default columns
	 */
	if (!ncolumns) {
		add_column(columns, ncolumns++, COL_RANGE);
		add_column(columns, ncolumns++, COL_SIZE);
		add_column(columns, ncolumns++, COL_STATE);
		add_column(columns, ncolumns++, COL_REMOVABLE);
		add_column(columns, ncolumns++, COL_BLOCK);
	}

	if (outarg && string_add_to_idarray(outarg, columns, ARRAY_SIZE(columns),
					 &ncolumns, column_name_to_id) < 0)
		return EXIT_FAILURE;

	/*
	 * Initialize output
	 */
	scols_init_debug(0);

	if (!(desc->table = scols_new_table()))
		errx(EXIT_FAILURE, _("failed to initialize output table"));
	scols_table_enable_raw(desc->table, desc->raw);
	scols_table_enable_export(desc->table, desc->export);
	scols_table_enable_json(desc->table, desc->json);
	scols_table_enable_noheadings(desc->table, desc->noheadings);

	if (desc->json)
		scols_table_set_name(desc->table, "memory");

	for (i = 0; i < ncolumns; i++) {
		struct coldesc *ci = get_column_desc(i);
		if (!scols_table_new_column(desc->table, ci->name, ci->whint, ci->flags))
			err(EXIT_FAILURE, _("Failed to initialize output column"));
	}

	if (has_column(COL_STATE))
		desc->want_state = 1;
	if (has_column(COL_NODE))
		desc->want_node = 1;
	if (has_column(COL_REMOVABLE))
		desc->want_removable = 1;

	/*
	 * Read data and print output
	 */
	read_basic_info(desc);
	read_info(desc);

	fill_scols_table(desc);
	scols_print_table(desc->table);

	fputc('\n', stdout);
	print_summary(desc);

	scols_unref_table(desc->table);
	return 0;
}
