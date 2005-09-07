/*
 * MOC - music on console
 * Copyright (C) 2004,2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#ifdef HAVE_NCURSES_H
# include <ncurses.h>
#elif HAVE_CURSES_H
# include <curses.h>
#endif

#include "menu.h"
#include "themes.h"
#include "main.h"
#include "options.h"
#include "interface_elements.h"
#include "log.h"
#include "files.h"
#include "decoder.h"
#include "keys.h"

#define STARTUP_MESSAGE	"The interface code is being rewritten and is not " \
	"currently usable."

/* TODO:
 * - xterm title (state, title of the song)
 */

/* Type of the side menu. */
enum side_menu_type
{
	MENU_DIR,	/* list of files in a directory */
	MENU_PLAYLIST,	/* a playlist of files */
	MENU_TREE	/* tree of directories */
};

struct side_menu
{
	enum side_menu_type type;
	int visible;	/* is it visible (are the other fields initialized) ? */
	WINDOW *win; 	/* window for the menu */

	/* Position and size of tme menu in the window */
	int posx;
	int posy;
	int width;
	int height;
	
	union
	{
		struct menu *list;
		/* struct menu_tree *tree;*/
	} menu;
};

static struct main_win
{
	WINDOW *win;
	struct side_menu menus[3];
} main_win;

/* Bar for displaying mixer state or progress. */
struct bar
{
	int width;	/* width in chars */
	int filled;	/* how much is it filled in percent */
	char title[40];	/* optional title */
	int show_val;	/* show the title and the value? */
	int fill_color;	/* color (ncurses attributes) of the filled part */
	int empty_color;	/* color of the empty part */
};

static struct info_win
{
	WINDOW *win;

	/* TODO: message (informative and error) */
	char *info_msg; /* informative message */
	time_t msg_timeout; /* how many seconds remain before the message
				disapperars */
	
	/* true/false options values */
	int state_stereo;
	int state_net;
	int state_shuffle;
	int state_repeat;
	int state_next;

	int bitrate;		/* in Kbps */
	int rate;		/* in KHz */

	/* time in seconds */
	int curr_time;
	int total_time;

	char *title;		/* title or file name of the song. */
	char status_msg[26];	/* status message */
	int state_play;		/* STATE_(PLAY | STOP | PAUSE) */

	struct bar mixer_bar;
} info_win;

/* Are we running on xterm? */
static int has_xterm = 0;

/* Chars used to make lines (for borders etc.). */
static struct
{
	chtype vert;	/* vertical */
	chtype horiz;	/* horizontal */
	chtype ulcorn;	/* upper left corner */
	chtype urcorn;	/* upper right corner */
	chtype llcorn;	/* lower left corner */
	chtype lrcorn;	/* lower right corner */
	chtype rtee;	/* right tee */
	chtype ltee;	/* left tee */
} lines;

static void side_menu_init (struct side_menu *m, const enum side_menu_type type,
		WINDOW *parent_win, const int height, const int width,
		const int posy, const int posx)
{
	assert (m != NULL);
	assert (parent_win != NULL);
	
	m->type = type;
	m->win = parent_win;
	m->posx = posx;
	m->posy = posy;
	m->height = height;
	m->width = width;

	if (type == MENU_DIR || type == MENU_PLAYLIST)
		m->menu.list = menu_new (m->win, posx, posy, width, height);
	else
		abort ();
	
	m->visible = 1;
}

static void side_menu_destroy (struct side_menu *m)
{
	assert (m != NULL);

	if (m->visible) {
		if (m->type == MENU_DIR || m->type == MENU_PLAYLIST)
			menu_free (m->menu.list);
		else
			abort ();
		
		m->visible = 0;
	}
}

static void main_win_init (struct main_win *w)
{
	assert (w != NULL);
	
	w->win = newwin (LINES - 4, COLS, 0, 0);
	wbkgd (w->win, get_color(CLR_BACKGROUND));
	nodelay (w->win, TRUE);
	keypad (w->win, TRUE);

	side_menu_init (&w->menus[0], MENU_DIR, w->win, LINES - 4, COLS - 2,
			1, 1);
	/*side_menu_init (&w->menus[0], MENU_DIR, w->win, 5, 40,
			1, 1);*/
	w->menus[1].visible = 0;
}

static void main_win_destroy (struct main_win *w)
{
	assert (w != NULL);

	side_menu_destroy (&w->menus[0]);
	side_menu_destroy (&w->menus[1]);

	if (w->win)
		delwin (w->win);
}

/* Convert time in second to min:sec text format. buff must be 6 chars long. */
static void sec_to_min (char *buff, const int seconds)
{
	assert (seconds >= 0);

	if (seconds < 6000) {

		/* the time is less than 99:59 */
		int min, sec;
		
		min = seconds / 60;
		sec = seconds % 60;

		snprintf (buff, 6, "%02d:%02d", min, sec);
	}
	else if (seconds < 10000 * 60) 

		/* the time is less than 9999 minutes */
		snprintf (buff, 6, "%dm", seconds/60);
	else
		strcpy (buff, "!!!!!");
}

/* Add an item from the playlist to the menu. */
static void add_to_menu (struct menu *menu, const struct plist *plist,
		const int num)
{
	int added;
	const struct plist_item *item = &plist->items[num];
	char *title = xstrdup (item->title);

	if (item->title == item->title_tags)
		title = iconv_str (title, 0);
	else
		title = iconv_str (title, 1);
	
	added = menu_add (menu, title, num, plist_file_type(plist, num),
			item->file);
	free (title);

	if (item->tags && item->tags->time != -1) {
		char time_str[6];
		
		sec_to_min (time_str, item->tags->time);
		menu_item_set_time (menu, added, time_str);
	}

	menu_item_set_attr_normal (menu, added, get_color(CLR_MENU_ITEM_FILE));
	menu_item_set_attr_sel (menu, added,
			get_color(CLR_MENU_ITEM_FILE_SELECTED));
	menu_item_set_attr_marked (menu, added,
			get_color(CLR_MENU_ITEM_FILE_MARKED));
	menu_item_set_attr_sel_marked (menu, added,
			get_color(CLR_MENU_ITEM_FILE_MARKED_SELECTED));
	
	menu_item_set_format (menu, added, file_type_name(item->file));
}

/* Fill the directory or playlist side menu with this content. */
static void side_menu_make_list_content (struct side_menu *m,
		const struct plist *files, const struct file_list *dirs,
		const struct file_list *playlists)
{
	int added;
	int i;

	assert (m != NULL);
	assert (m->type == MENU_DIR || m->type == MENU_PLAYLIST);
	assert (m->menu.list != NULL);

	menu_free (m->menu.list);
	m->menu.list = menu_new (m->win, m->posx, m->posy, m->width, m->height);

	added = menu_add (m->menu.list, "../", -1, F_DIR, "..");
	menu_item_set_attr_normal (m->menu.list, added,
			get_color(CLR_MENU_ITEM_DIR));
	menu_item_set_attr_sel (m->menu.list, added,
			get_color(CLR_MENU_ITEM_DIR_SELECTED));
	
	if (dirs)
		for (i = 0; i < dirs->num; i++) {
			char title[PATH_MAX];

			strcpy (title, strrchr(dirs->items[i], '/') + 1);
			strcat (title, "/");
			
			added = menu_add (m->menu.list, title, -1, F_DIR,
					dirs->items[i]);
			menu_item_set_attr_normal (m->menu.list, added,
					get_color(CLR_MENU_ITEM_DIR));
			menu_item_set_attr_sel (m->menu.list, added,
					get_color(CLR_MENU_ITEM_DIR_SELECTED));
		}

	if (playlists)
		for (i = 0; i < playlists->num; i++){
			added = menu_add (m->menu.list,
					strrchr(playlists->items[i], '/') + 1,
					-1, F_PLAYLIST,	playlists->items[i]);
			menu_item_set_attr_normal (m->menu.list, added,
					get_color(CLR_MENU_ITEM_PLAYLIST));
			menu_item_set_attr_sel (m->menu.list, added,
					get_color(
					CLR_MENU_ITEM_PLAYLIST_SELECTED));
		}
	
	/* playlist items */
	for (i = 0; i < files->num; i++) {
		if (!plist_deleted(files, i))
			add_to_menu (m->menu.list, files, i);
	}
	
	menu_set_show_format (m->menu.list, options_get_int("ShowFormat"));
	menu_set_show_time (m->menu.list,
			strcasecmp(options_get_str("ShowTime"), "no"));
	menu_set_info_attr (m->menu.list, get_color(CLR_MENU_ITEM_INFO));
}

static void side_menu_draw (const struct side_menu *m)
{
	assert (m != NULL);
	assert (m->visible);
	
	if (m->type == MENU_DIR || m->type == MENU_PLAYLIST)
		menu_draw (m->menu.list);
	else
		abort ();
}

static void side_menu_cmd (struct side_menu *m, const enum key_cmd cmd)
{
	assert (m != NULL);
	assert (m->visible);

	if (m->type == MENU_DIR || m->type == MENU_PLAYLIST) {
		switch (cmd) {
			case KEY_CMD_MENU_DOWN:
				menu_driver (m->menu.list, REQ_DOWN);
				break;
			case KEY_CMD_MENU_UP:
				menu_driver (m->menu.list, REQ_UP);
				break;
			case KEY_CMD_MENU_NPAGE:
				menu_driver (m->menu.list, REQ_PGDOWN);
				break;
			case KEY_CMD_MENU_PPAGE:
				menu_driver (m->menu.list, REQ_PGUP);
				break;
			case KEY_CMD_MENU_FIRST:
				menu_driver (m->menu.list, REQ_TOP);
				break;
			case KEY_CMD_MENU_LAST:
				menu_driver (m->menu.list, REQ_BOTTOM);
				break;
			default:
				abort ();
		}
	}
	else
		abort ();

	side_menu_draw (m);
}

static struct side_menu *find_side_menu (struct main_win *w,
		const enum side_menu_type type)
{
	int i;

	assert (w != NULL);

	for (i = 0; i < (int)(sizeof(w->menus)/sizeof(w->menus[0])); i++) {
		struct side_menu *m = &w->menus[i];
	
		if (m->visible && m->type == type)
			return m;
	}

	abort (); /* menu not found - BUG */
}

static void main_win_set_dir_content (struct main_win *w,
		const struct plist *files,
		const struct file_list *dirs, const struct file_list *playlists)
{
	struct side_menu *m = find_side_menu (w, MENU_DIR);

	side_menu_make_list_content (m, files, dirs, playlists);
	side_menu_draw (m);
}

static void main_win_menu_cmd (struct main_win *w, const enum key_cmd cmd)
{
	struct side_menu *m;
	
	assert (w != NULL);
	
	m = find_side_menu (w, MENU_DIR);
	side_menu_cmd (m, cmd);
}

static void main_win_draw (struct main_win *w)
{
	int i;

	for (i = 0; i < (int)(sizeof(w->menus)/sizeof(w->menus[0])); i++)
		if (w->menus[i].visible)
			side_menu_draw (&w->menus[i]);
}

/* Set the has_xterm variable. */
static void detect_term ()
{
	char *term;

	if ((((term = getenv("TERM")) && !strcmp(term, "xterm"))
				|| !strcmp(term, "rxvt")
				|| !strcmp(term, "eterm")
				|| !strcmp(term, "Eterm")))
		has_xterm = 1;
}

/* Based on ASCIILines option initialize line characters with curses lines or
 * ASCII characters. */
static void init_lines ()
{
	if (options_get_int("ASCIILines")) {
		lines.vert = '|';
		lines.horiz = '-';
		lines.ulcorn = '+';
		lines.urcorn = '+';
		lines.llcorn = '+';
		lines.lrcorn = '+';
		lines.rtee = '|';
		lines.ltee = '|';
	}
	else {
		lines.vert = ACS_VLINE;
		lines.horiz = ACS_HLINE;
		lines.ulcorn = ACS_ULCORNER;
		lines.urcorn = ACS_URCORNER;
		lines.llcorn = ACS_LLCORNER;
		lines.lrcorn = ACS_LRCORNER;
		lines.rtee = ACS_RTEE;
		lines.ltee = ACS_LTEE;
	}
}

/* End the program if the terminal is too small. */
static void check_term_size ()
{
	if (COLS < 72 || LINES < 7)
		fatal ("The terminal is too small after resizeing.");
}

static void bar_set_title (struct bar *b, const char *title)
{
	assert (b != NULL);
	assert (b->show_val);
	assert (title != NULL);
	assert (strlen(title) < sizeof(b->title) - 5);
	
	if (b->filled < 100)
		sprintf (b->title, "%*s  %02d%%", b->width - 5, title,
				b->filled);
	else
		sprintf (b->title, "%*s 100%%", b->width - 5, title);
}

static void bar_init (struct bar *b, const int width, const char *title,
		const int show_val, const int fill_color,
		const int empty_color)
{
	assert (b != NULL);
	assert (width > 5);
	assert (title != NULL || !show_val);
	
	b->width = width;
	b->filled = 0;
	b->show_val = show_val;
	b->fill_color = fill_color;
	b->empty_color = empty_color;
	
	if (show_val)
		bar_set_title (b, title);
	else {
		int i;

		for (i = 0; i < (int)sizeof(b->title) - 1; i++)
			b->title[i] = ' ';
		b->title[sizeof(b->title)-1] = 0;
	}	
}

static void bar_draw (struct bar *b, WINDOW *win, const int pos_x,
		const int pos_y)
{
	int fill_chars; /* how many chars are "filled" */
	
	assert (b != NULL);
	assert (win != NULL);
	assert (pos_x >= 0 && pos_x < COLS - b->width);
	assert (pos_y >= 0 && pos_y < LINES);

	fill_chars = b->filled * b->width / 100;
	
	wattrset (win, b->fill_color);
	mvwaddnstr (win, pos_y, pos_x, b->title, fill_chars);

	wattrset (win, b->empty_color);
	waddstr (win, b->title + fill_chars);
}

static void info_win_init (struct info_win *w)
{
	assert (w != NULL);

	w->win = newwin (4, COLS, LINES - 4, 0);
	wbkgd (w->win, get_color(CLR_BACKGROUND));

	w->state_stereo = 0;
	w->state_net = 0;
	w->state_shuffle = 0;
	w->state_repeat = 0;
	w->state_next = 0;

	w->bitrate = -1;
	w->rate = -1;

	w->curr_time = -1;
	w->total_time = -1;

	w->title = NULL;
	w->status_msg[0] = 0;

	w->info_msg = xstrdup (STARTUP_MESSAGE);
	w->msg_timeout = time(NULL) + 3;

	bar_init (&w->mixer_bar, 20, "", 1, get_color(CLR_MIXER_BAR_FILL),
			get_color(CLR_MIXER_BAR_EMPTY));
}

static void info_win_destroy (struct info_win *w)
{
	assert (w != NULL);

	if (w->win)
		delwin (w->win);
	if (w->info_msg)
		free (w->info_msg);
}

static void info_win_set_mixer_name (struct info_win *w, const char *name)
{
	assert (w != NULL);
	assert (name != NULL);
	
	bar_set_title (&w->mixer_bar, name);
	bar_draw (&w->mixer_bar, w->win, COLS - 37, 0);
}

static void info_win_set_status (struct info_win *w, const char *msg)
{
	assert (w != NULL);
	assert (msg != NULL);
	assert (strlen(msg) < sizeof(w->status_msg));

	strcpy (w->status_msg, msg);
	mvwaddstr (w->win, 0, 6, msg);
}

static void info_win_draw (struct info_win *w)
{
	wattrset (w->win, get_color(CLR_MESSAGE));
	mvwaddstr (w->win, 1, 1, w->info_msg);
}

void windows_init ()
{
	initscr ();
	cbreak ();
	noecho ();
	curs_set (0);
	use_default_colors ();

	check_term_size ();
	
	detect_term ();
	start_color ();
	theme_init (has_xterm);
	init_lines ();

	main_win_init (&main_win);
	info_win_init (&info_win);

	main_win_draw (&main_win);
	info_win_draw (&info_win);
	
	wrefresh (main_win.win);
	wrefresh (info_win.win);
}

void windows_end ()
{
	main_win_destroy (&main_win);
	info_win_destroy (&info_win);
	
	/* endwin() sometimes fails on x terminals when we get SIGCHLD
	 * at this moment. Double invokation seems to solve this. */
	if (endwin() == ERR && endwin() == ERR)
		logit ("endwin() failed!");

	/* Make sure that the next line after we exit will be "clear". */
	putchar ('\n');
}

/* Set state of the options displayed in the information window. */
void iface_set_option_state (const char *name, const int value)
{
	assert (name != NULL);

	if (!strcasecmp(name, "stereo"))
		info_win.state_stereo = value;
	else if (!strcasecmp(name, "net"))
		info_win.state_net = value;
	else if (!strcasecmp(name, "Shuffle"))
		info_win.state_shuffle = value;
	else if (!strcasecmp(name, "Repeat"))
		info_win.state_repeat = value;
	else if (!strcasecmp(name, "AutoNext"))
		info_win.state_next = value;
	else
		abort ();
}

/* Set the mixer name. */
void iface_set_mixer_name (const char *name)
{
	assert (name != NULL);
	
	info_win_set_mixer_name (&info_win, name);
}

/* Set the status message in the info window. */
void iface_set_status (const char *msg)
{
	assert (msg != NULL);

	info_win_set_status (&info_win, msg);
}

/* Change the content of the directory menu to these files, directories, and
 * playlists. */
void iface_set_dir_content (const struct plist *files,
		const struct file_list *dirs, const struct file_list *playlists)
{
	main_win_set_dir_content (&main_win, files, dirs, playlists);
	wrefresh (main_win.win);
	
	/* TODO: also display the number of items */
}

/* Chenge the current item in the directory menu to this item. */
void iface_set_curr_dir_item (const char *fname)
{
}

/* Set the title for the directory menu. */
void iface_set_dir_title (const char *title)
{
}

/* Get the char code from the user with meta flag set if necessary. */
int iface_get_char ()
{
	int meta;
	int ch = wgetch (main_win.win);
	
	/* Recognize meta sequences */
	if (ch == KEY_ESCAPE
			&& (meta = wgetch(main_win.win))
			!= ERR)
		ch = meta | META_KEY_FLAG;

	return ch;
}

/* Return a non zero value if the help screen is displayed. */
int iface_in_help ()
{
	return 0;
}

/* Return a non zero value if the key is not a real key - KEY_RESIZE. */
int iface_key_is_resize (const int ch)
{
	return ch == KEY_RESIZE;
}

/* Handle a key command for the menu. */
void iface_menu_key (const enum key_cmd cmd)
{
	main_win_menu_cmd (&main_win, cmd);
	wrefresh (main_win.win);
}
