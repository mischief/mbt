// SPDX-License-Identifier: ISC
/*
 * mbt-tui — ncurses BitTorrent client
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <locale.h>
#include <ncurses.h>

#include "mbt/event.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/storage.h"

static struct mbt_ev *ev;
static struct mbt_session *session;
static WINDOW *win_list, *win_detail, *win_cmd;
static int selected;
static int detail_view; /* 0=overview, 1=peers, 2=pieces */

/* Rate tracking: snapshot have_count each refresh, compute delta */
static uint32_t last_have;
static float dl_rate_kbps;
static int quit;
static char cmdbuf[512];
static int cmdlen;
static int mode; /* 0=normal, 1=command input */

static void
draw_header(void)
{
	int w = COLS;
	attron(A_BOLD);
	mvhline(0, 0, ' ', w);
	mvprintw(0, 1, "mbt-tui");
	mvprintw(0, w - 30, "DL %.0f KB/s  [%d torrents]",
	         dl_rate_kbps, mbt_session_torrent_count(session));
	attroff(A_BOLD);
}

static void
draw_torrent_list(void)
{
	werase(win_list);
	box(win_list, 0, 0);
	mvwprintw(win_list, 0, 2, " Torrents ");

	int row = 1;
	int idx = 0;
	int maxrow = getmaxy(win_list) - 2;
	struct mbt_torrent *t = mbt_session_torrent_first(session);
	while (t && row <= maxrow) {
		const char *name = mbt_torrent_name(t);
		if (!name) name = "(no metadata)";
		uint32_t have = mbt_torrent_have_count(t);
		uint32_t total = mbt_torrent_piece_count(t);
		float pct = total ? (100.0f * have / total) : 0;

		if (idx == selected)
			wattron(win_list, A_REVERSE);
		mvwprintw(win_list, row, 1, " %d %-30.30s %5.1f%%", idx + 1, name, pct);
		if (idx == selected)
			wattroff(win_list, A_REVERSE);
		row++;
		idx++;
		t = mbt_session_torrent_next(session, t);
	}
	wrefresh(win_list);
}

static void
draw_detail(void)
{
	werase(win_detail);
	box(win_detail, 0, 0);

	/* Find selected torrent */
	struct mbt_torrent *t = mbt_session_torrent_first(session);
	for (int i = 0; i < selected && t; i++)
		t = mbt_session_torrent_next(session, t);

	if (!t) {
		mvwprintw(win_detail, 1, 1, "No torrent selected");
		wrefresh(win_detail);
		return;
	}

	const char *name = mbt_torrent_name(t);
	uint32_t have = mbt_torrent_have_count(t);
	uint32_t total = mbt_torrent_piece_count(t);
	float pct = total ? (100.0f * have / total) : 0;
	int peers = mbt_torrent_peer_count(t);
	uint32_t plen = mbt_torrent_piece_length(t);
	uint64_t total_bytes = (uint64_t)total * plen;
	uint64_t have_bytes = (uint64_t)have * plen;

	const char *views[] = {"Overview", "Peers", "Pieces"};
	mvwprintw(win_detail, 0, 2, " %s [%s] (h/l to switch) ",
	          name ? name : "(no metadata)", views[detail_view]);

	if (detail_view == 0) {
		/* Overview */
		mvwprintw(win_detail, 1, 1, "Progress: %u/%u pieces (%.1f%%)  %.1f/%.1f MB",
		          have, total, pct,
		          have_bytes / (1024.0*1024.0), total_bytes / (1024.0*1024.0));
		mvwprintw(win_detail, 2, 1, "Peers: %d  Rate: %.0f KB/s  Piece size: %u KB",
		          peers, dl_rate_kbps, plen / 1024);

		/* Progress bar */
		int barw = getmaxx(win_detail) - 4;
		int filled = total ? (barw * have / total) : 0;
		mvwaddch(win_detail, 3, 1, '[');
		for (int i = 0; i < barw; i++)
			waddch(win_detail, i < filled ? '#' : '.');
		waddch(win_detail, ']');

	} else if (detail_view == 1) {
		/* Peers — show what we can from torrent_peer_count */
		mvwprintw(win_detail, 1, 1, "Connected peers: %d", peers);
		mvwprintw(win_detail, 2, 1, "(detailed peer list requires API extension)");

	} else if (detail_view == 2) {
		/* Piece map */
		mvwprintw(win_detail, 1, 1, "Piece map (%u pieces):", total);
		int maxw = getmaxx(win_detail) - 2;
		int maxh = getmaxy(win_detail) - 3;
		int row = 2, col = 1;
		for (uint32_t i = 0; i < total && row <= maxh; i++) {
			waddch(win_detail, mbt_torrent_has_piece(t, i) ? '#' : '.');
			col++;
			if (col >= maxw) { col = 1; row++; wmove(win_detail, row, col); }
		}
	}

	wrefresh(win_detail);
}

static void
draw_cmd(void)
{
	werase(win_cmd);
	if (mode == 1) {
		mvwprintw(win_cmd, 0, 0, "> %s", cmdbuf);
	} else {
		mvwprintw(win_cmd, 0, 0, " [a]dd  [q]uit  [j/k] navigate");
	}
	wrefresh(win_cmd);
}

static void
draw_all(void)
{
	draw_header();
	draw_torrent_list();
	draw_detail();
	draw_cmd();
	refresh();
}

static void
handle_command(void)
{
	if (cmdlen == 0) return;

	if (strncmp(cmdbuf, "quit", 4) == 0 || strcmp(cmdbuf, "q") == 0) {
		quit = 1;
		mbt_ev_stop(ev);
	} else {
		/* Try as .torrent file, magnet, or infohash */
		int rv = mbt_session_add_torrent_file(session, cmdbuf);
		if (rv < 0)
			rv = mbt_session_add_magnet(session, cmdbuf);
		if (rv < 0 && cmdlen == 40) {
			/* Try as raw SHA1 infohash hex */
			char mag[256];
			snprintf(mag, sizeof(mag), "magnet:?xt=urn:btih:%.40s", cmdbuf);
			mbt_session_add_magnet(session, mag);
		}
	}
	cmdbuf[0] = 0;
	cmdlen = 0;
	mode = 0;
}

static void
on_input(struct mbt_ev_io *io, int fd, uint32_t events, void *arg)
{
	(void)io; (void)fd; (void)events; (void)arg;
	int ch = getch();
	if (ch == ERR) return;

	if (mode == 1) {
		if (ch == '\n' || ch == KEY_ENTER) {
			handle_command();
		} else if (ch == 27) { /* ESC */
			mode = 0; cmdbuf[0] = 0; cmdlen = 0;
		} else if (ch == KEY_BACKSPACE || ch == 127) {
			if (cmdlen > 0) cmdbuf[--cmdlen] = 0;
		} else if (cmdlen < (int)sizeof(cmdbuf) - 1) {
			cmdbuf[cmdlen++] = ch;
			cmdbuf[cmdlen] = 0;
		}
	} else {
		switch (ch) {
		case 'q': quit = 1; mbt_ev_stop(ev); return;
		case 'a': case ':': mode = 1; break;
		case 'j': case KEY_DOWN: selected++; break;
		case 'k': case KEY_UP: if (selected > 0) selected--; break;
		case 'h': case KEY_LEFT: if (detail_view > 0) detail_view--; break;
		case 'l': case KEY_RIGHT: if (detail_view < 2) detail_view++; break;
		}
	}
	draw_all();
}

static void
on_refresh(struct mbt_ev_timer *timer, void *arg)
{
	(void)arg;

	/* Compute download rate from piece count delta */
	uint32_t total_have = 0;
	uint32_t piece_len = 0;
	struct mbt_torrent *t = mbt_session_torrent_first(session);
	while (t) {
		total_have += mbt_torrent_have_count(t);
		if (!piece_len) piece_len = mbt_torrent_piece_length(t);
		t = mbt_session_torrent_next(session, t);
	}
	uint32_t delta = total_have - last_have;
	dl_rate_kbps = (float)delta * piece_len / 1024.0f;
	last_have = total_have;

	draw_all();
	mbt_ev_timer_mod(timer, 1000000); /* 1 sec */
}

static void
on_resize(int sig)
{
	(void)sig;
	endwin();
	refresh();
	/* Recreate windows */
	int list_h = LINES / 2;
	int det_h = LINES - list_h - 2;
	wresize(win_list, list_h, COLS);
	wresize(win_detail, det_h, COLS);
	mvwin(win_detail, list_h, 0);
	wresize(win_cmd, 1, COLS);
	mvwin(win_cmd, LINES - 1, 0);
	draw_all();
}

int
main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");
	signal(SIGPIPE, SIG_IGN);
	signal(SIGWINCH, on_resize);

	mbt_ev_new_default(&ev);

	struct mbt_session_config cfg = {
		.ev = ev,
		.storage_ops = mbt_storage_file_ops(),
		.storage_ctx = (void *)".",
		.listen_port = 6881,
	};
	mbt_session_new(&session, &cfg);
	mbt_session_enable_dht(session);

	/* Add any args as torrents (.torrent, magnet, or raw infohash) */
	for (int i = 1; i < argc; i++) {
		int rv = mbt_session_add_torrent_file(session, argv[i]);
		if (rv < 0)
			rv = mbt_session_add_magnet(session, argv[i]);
		if (rv < 0 && strlen(argv[i]) == 40) {
			char mag[256];
			snprintf(mag, sizeof(mag), "magnet:?xt=urn:btih:%.40s", argv[i]);
			mbt_session_add_magnet(session, mag);
		}
	}

	/* Init ncurses */
	initscr();
	cbreak();
	noecho();
	nodelay(stdscr, TRUE);
	keypad(stdscr, TRUE);
	curs_set(0);

	int list_h = LINES / 2;
	int det_h = LINES - list_h - 2;
	win_list = newwin(list_h, COLS, 1, 0);
	win_detail = newwin(det_h, COLS, list_h + 1, 0);
	win_cmd = newwin(1, COLS, LINES - 1, 0);

	/* Watch stdin for input */
	struct mbt_ev_io *input_io;
	mbt_ev_io_new(ev, &input_io, 0, MBT_EV_READ, on_input, NULL);

	/* Refresh timer */
	struct mbt_ev_timer *refresh_timer;
	mbt_ev_timer_new(ev, &refresh_timer, 1000000, on_refresh, NULL);

	draw_all();
	mbt_ev_run(ev);

	/* Cleanup */
	delwin(win_list);
	delwin(win_detail);
	delwin(win_cmd);
	endwin();

	mbt_ev_io_free(input_io);
	mbt_ev_timer_free(refresh_timer);
	mbt_session_free(session);
	mbt_ev_destroy(ev);

	return 0;
}
