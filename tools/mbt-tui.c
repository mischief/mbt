// SPDX-License-Identifier: ISC
/*
 * mbt-tui — ncurses BitTorrent client
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <locale.h>
#include <sys/ioctl.h>
#include <sys/termios.h>
#include <ncurses.h>
#include <curl/curl.h>
#include <stdint.h>
#include <unistd.h>

#include "mbt/event.h"
#include "mbt/infohash.h"
#include "mbt/session.h"
#include "mbt/torrent.h"
#include "mbt/peer.h"
#include "mbt/tracker.h"
#include "mbt/storage.h"
#include "mbt/log.h"

struct mbt_ev_io;
struct mbt_ev_timer;

/* --- HTTP tracker via curl (async-shaped, blocks internally) --- */
struct curl_buf { char *data; size_t len; };

static size_t
curl_write_cb(void *ptr, size_t size, size_t nmemb, void *v)
{
	struct curl_buf *b = v;
	size_t total = size * nmemb;
	char *tmp = realloc(b->data, b->len + total);
	if (!tmp) return 0;
	memcpy(tmp + b->len, ptr, total);
	b->data = tmp;
	b->len += total;
	return total;
}

static int
curl_http_fn(struct mbt_http_request *req, mbt_http_done_fn done_cb, void *done_arg, void *arg)
{
	(void)arg;
	CURL *c = curl_easy_init();
	if (!c) return -1;
	struct curl_buf buf = {0};
	curl_easy_setopt(c, CURLOPT_URL, req->url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	CURLcode rc = curl_easy_perform(c);
	curl_easy_cleanup(c);
	struct mbt_http_response resp = {0};
	if (rc == CURLE_OK) {
		resp.data = buf.data;
		resp.len = buf.len;
	} else {
		free(buf.data);
	}
	done_cb(&resp, done_arg);
	return 0;
}

static struct mbt_ev *ev;
static struct mbt_session *session;
static WINDOW *win_list, *win_detail, *win_cmd;
static int selected;
static int detail_view; /* 0=overview, 1=peers, 2=pieces */
static int detail_scroll;
static int debug_mode;
static volatile sig_atomic_t resize_pending;
static void do_resize(void);

/* Log ring buffer */
#define LOG_LINES 256
#define LOG_COLS 256
static char log_ring[LOG_LINES][LOG_COLS];
static int log_head; /* next write position */
static int log_count;

static void
tui_log_cb(enum mbt_log_level level, enum mbt_log_component comp,
           const char *fmt, va_list ap, void *arg)
{
	(void)arg;
	char msg[LOG_COLS];
	int off = snprintf(msg, sizeof(msg), "%s[%s]: ",
	                   mbt_log_component_str(comp), mbt_log_level_str(level));
	vsnprintf(msg + off, sizeof(msg) - off, fmt, ap);
	memcpy(log_ring[log_head], msg, LOG_COLS);
	log_ring[log_head][LOG_COLS - 1] = 0;
	log_head = (log_head + 1) % LOG_LINES;
	if (log_count < LOG_LINES) log_count++;
}

/* Rate tracking: sliding window over bytes received */
static uint64_t last_bytes;
static float dl_rate_kbps;
#define RATE_SLOTS 5
static uint64_t rate_slots[RATE_SLOTS];
static int rate_idx;
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
	int width = getmaxx(win_list) - 2;
	struct mbt_torrent *t = mbt_session_torrent_first(session);
	while (t && row <= maxrow) {
		const char *name = mbt_torrent_name(t);
		if (!name) name = "(no metadata)";
		uint32_t have = mbt_torrent_have_count(t);
		uint32_t total = mbt_torrent_piece_count(t);
		float pct = total ? (100.0f * have / total) : 0;
		int peers = mbt_torrent_peer_count(t);

		/* Truncate name with ... */
		int name_width = width - 30; /* space for stats */
		if (name_width < 10) name_width = 10;
		char namebuf[256];
		int nlen = strlen(name);
		if (nlen > name_width) {
			snprintf(namebuf, sizeof(namebuf), "%.*s...", name_width - 3, name);
		} else {
			snprintf(namebuf, sizeof(namebuf), "%s", name);
		}

		const char *state = "";
		enum mbt_torrent_state st = mbt_torrent_state(t);
		if (!total) state = "meta";
		else if (st == MBT_TORRENT_VERIFYING) state = "vrfy";
		else if (pct >= 100.0f) state = "seed";
		else if (peers == 0) state = "wait";
		else state = "dl";

		if (idx == selected)
			wattron(win_list, A_REVERSE);
		mvwprintw(win_list, row, 1, " %-*s %5.1f%% %3dp %-4s",
		          name_width, namebuf, pct, peers, state);
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

	const char *views[] = {"Overview", "Peers", "Pieces", "Log"};
	mvwprintw(win_detail, 0, 2, " %s [%s] (h/l to switch) ",
	          name ? name : "(no metadata)", views[detail_view]);

	if (detail_view == 0) {
		/* Overview */
		const char *state_str[] = {"init", "verifying", "downloading", "STALLED", "seeding"};
		enum mbt_torrent_state st = mbt_torrent_state(t);
		mvwprintw(win_detail, 1, 1, "Progress: %u/%u pieces (%.1f%%)  %.1f/%.1f MB  [%s]%s",
		          have, total, pct,
		          have_bytes / (1024.0*1024.0), total_bytes / (1024.0*1024.0),
		          state_str[st],
		          mbt_torrent_is_private(t) ? "  [private]" : "");
		mvwprintw(win_detail, 2, 1, "Peers: %d  Rate: %.0f KB/s  Piece size: %u KB  Listen: %u",
		          peers, dl_rate_kbps, plen / 1024, mbt_session_listen_port(session));
		/* Progress bar */
		int barw = getmaxx(win_detail) - 4;
		int filled = total ? (barw * have / total) : 0;
		mvwaddch(win_detail, 3, 1, '[');
		for (int i = 0; i < barw; i++)
			waddch(win_detail, i < filled ? '#' : '.');
		waddch(win_detail, ']');
		/* Infohash */
		struct mbt_infohash *ih = mbt_torrent_infohash(t);
		char ihbuf[128] = "(none)";
		if (ih) mbt_infohash_string(ih, ihbuf, sizeof(ihbuf));
		mvwprintw(win_detail, 4, 1, "Hash: %s", ihbuf);
		/* File count */
		int nfiles = mbt_torrent_file_count(t);
		mvwprintw(win_detail, 5, 1, "Files: %d", nfiles > 0 ? nfiles : 1);
		/* Trackers */
		int ntrackers = mbt_session_torrent_tracker_count(session, t);
		if (ntrackers == 0) {
			mvwprintw(win_detail, 6, 1, "Trackers: (none)");
		} else {
			mvwprintw(win_detail, 6, 1, "Trackers:");
			int maxrow = getmaxy(win_detail) - 1;
			for (int i = 0; i < ntrackers && 7 + i < maxrow; i++) {
				const char *url = mbt_session_torrent_tracker(session, t, i);
				mvwprintw(win_detail, 7 + i, 3, "%s", url ? url : "");
			}
		}

	} else if (detail_view == 1) {
		/* Peers */
		int np = mbt_torrent_peer_count(t);
		mvwprintw(win_detail, 1, 1, "%-47s %5s %4s %7s %7s %4s %3s %-5s %-19s %s",
		          "ENDPOINT", "DONE", "REQ", "DOWN", "UP", "FAIL", "PEN", "FLAGS", "CLIENT", "SRC");
		int maxrow = getmaxy(win_detail) - 2;
		int max_scroll = np - (maxrow - 1);
		if (detail_scroll > max_scroll) detail_scroll = max_scroll > 0 ? max_scroll : 0;
		for (int i = detail_scroll; i < np && (i - detail_scroll) + 2 <= maxrow; i++) {
			struct mbt_peer_stats ps;
			if (mbt_torrent_peer_stats(t, i, &ps) < 0) break;
			char flags[8] = "";
			int fi = 0;
			if (ps.choked) flags[fi++] = 'C';
			if (ps.interested) flags[fi++] = 'I';
			if (ps.snubbed) flags[fi++] = 'S';
			flags[fi] = 0;
			char endpoint[64];
			if (strchr(ps.addr, ':'))
				snprintf(endpoint, sizeof(endpoint), "[%s]:%hu", ps.addr, ps.conn_port);
			else
				snprintf(endpoint, sizeof(endpoint), "%s:%hu", ps.addr, ps.conn_port);
			char down[16], up[16];
			if (ps.downloaded >= 1024*1024)
				snprintf(down, sizeof(down), "%.1fM", ps.downloaded / (1024.0*1024));
			else if (ps.downloaded >= 1024)
				snprintf(down, sizeof(down), "%.1fK", ps.downloaded / 1024.0);
			else
				snprintf(down, sizeof(down), "%luB", (unsigned long)ps.downloaded);
			if (ps.uploaded >= 1024*1024)
				snprintf(up, sizeof(up), "%.1fM", ps.uploaded / (1024.0*1024));
			else if (ps.uploaded >= 1024)
				snprintf(up, sizeof(up), "%.1fK", ps.uploaded / 1024.0);
			else
				snprintf(up, sizeof(up), "%luB", (unsigned long)ps.uploaded);
			mvwprintw(win_detail, (i - detail_scroll) + 2, 1, "%-47s %4d%% %4d %7s %7s %4d %3d %-5s %-19.19s %s",
			          endpoint, ps.done_pct, ps.outstanding,
			          down, up, ps.hash_failures, ps.score, flags,
			          ps.client_version ? ps.client_version : "",
			          mbt_peer_source_str(ps.source));
		}

	} else if (detail_view == 2) {
		/* Piece map */
		mvwprintw(win_detail, 1, 1, "Piece map (%u pieces):", total);
		int maxw = getmaxx(win_detail) - 3;
		int maxh = getmaxy(win_detail) - 2;
		int row = 2, col = 0;
		wmove(win_detail, row, 1);
		for (uint32_t i = 0; i < total && row < maxh; i++) {
			waddch(win_detail, mbt_torrent_has_piece(t, i) ? '#' : '.');
			col++;
			if (col >= maxw) { col = 0; row++; wmove(win_detail, row, 1); }
		}
	} else if (detail_view == 3) {
		/* Log */
		mvwprintw(win_detail, 0, 2, " Log [%s] (D to toggle debug) ",
		          debug_mode ? "DEBUG" : "INFO");
		int maxrow = getmaxy(win_detail) - 2;
		int max_scroll = log_count - maxrow;
		if (detail_scroll > max_scroll) detail_scroll = max_scroll > 0 ? max_scroll : 0;
		int start = log_head - maxrow - detail_scroll;
		if (start < 0) start += LOG_LINES;
		for (int row = 1; row <= maxrow; row++) {
			int idx = (start + row - 1) % LOG_LINES;
			if (row > log_count) break;
			mvwprintw(win_detail, row, 1, "%.*s",
			          getmaxx(win_detail) - 2, log_ring[idx]);
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
		mvwprintw(win_cmd, 0, 0, " [a]dd  [q]uit  [j/k] select  [J/K] scroll  [h/l] view  [D] debug");
	}
	wrefresh(win_cmd);
}

static void
draw_all(void)
{
	if (!session) {
		refresh();
		return;
	}
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
		int rv = mbt_session_add_torrent_file(session, cmdbuf, NULL);
		if (rv < 0)
			rv = mbt_session_add_magnet(session, cmdbuf, NULL);
		if (rv < 0 && cmdlen == 40) {
			/* Try as raw SHA1 infohash hex */
			char mag[256];
			snprintf(mag, sizeof(mag), "magnet:?xt=urn:btih:%.40s", cmdbuf);
			mbt_session_add_magnet(session, mag, NULL);
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
	if (resize_pending) { resize_pending = 0; do_resize(); }
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
		case 'D':
			debug_mode = !debug_mode;
			mbt_log_set_debug(debug_mode ? MBT_LOG_ALL : 0);
			break;
		case 'a': case ':': mode = 1; break;
		case 'j': case KEY_DOWN: {
			int n = mbt_session_torrent_count(session);
			if (selected < n - 1) { selected++; detail_scroll = 0; }
			break;
		}
		case 'k': case KEY_UP:
			if (selected > 0) { selected--; detail_scroll = 0; }
			break;
		case 'h': case KEY_LEFT:
			if (detail_view > 0) { detail_view--; detail_scroll = 0; }
			break;
		case 'l': case KEY_RIGHT:
			if (detail_view < 3) { detail_view++; detail_scroll = 0; }
			break;
		case 'J': case KEY_NPAGE: detail_scroll++; break;
		case 'K': case KEY_PPAGE: if (detail_scroll > 0) detail_scroll--; break;
		}
	}
	draw_all();
}

static void
on_refresh(struct mbt_ev_timer *timer, void *arg)
{
	(void)arg;
	if (resize_pending) { resize_pending = 0; do_resize(); }

	/* Compute download rate from bytes received across all active torrents */
	uint64_t total_bytes = 0;
	struct mbt_torrent *t = mbt_session_torrent_first(session);
	while (t) {
		if (mbt_torrent_state(t) == MBT_TORRENT_DOWNLOADING)
			total_bytes += mbt_torrent_bytes_downloaded(t);
		t = mbt_session_torrent_next(session, t);
	}
	uint64_t delta = total_bytes >= last_bytes ? total_bytes - last_bytes : 0;
	last_bytes = total_bytes;

	rate_idx = (rate_idx + 1) % RATE_SLOTS;
	rate_slots[rate_idx] = delta;

	uint64_t sum = 0;
	for (int i = 0; i < RATE_SLOTS; i++) sum += rate_slots[i];
	dl_rate_kbps = (float)sum / RATE_SLOTS / 1024.0f;

	draw_all();
	mbt_ev_timer_mod(timer, 1000000); /* 1 sec */
}

static void
on_resize(int sig)
{
	(void)sig;
	resize_pending = 1;
}

static void
do_resize(void)
{
	endwin();
	refresh();

	/* Use ioctl directly — LINES/COLS may not update until after resizeterm
	 * on some ncurses implementations, so compute layout from ws ourselves. */
	struct winsize ws;
	int rows, cols;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
		rows = ws.ws_row;
		cols = ws.ws_col;
	} else {
		rows = LINES;
		cols = COLS;
	}
	resizeterm(rows, cols);

	int list_h = rows / 2;
	int det_h = rows - list_h - 2;
	wresize(win_list, list_h, cols);
	mvwin(win_list, 1, 0);
	wresize(win_detail, det_h, cols);
	mvwin(win_detail, list_h + 1, 0);
	wresize(win_cmd, 1, cols);
	mvwin(win_cmd, rows - 1, 0);

	/* Force full redraw — sub-windows need touch after endwin/resizeterm */
	clearok(curscr, TRUE);
	touchwin(win_list);
	touchwin(win_detail);
	touchwin(win_cmd);
	draw_all();
}

int
main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");
	signal(SIGPIPE, SIG_IGN);

	/* Parse options */
	int ch;
	while ((ch = getopt(argc, argv, "dh")) != -1) {
		switch (ch) {
		case 'd': debug_mode = 1; break;
		case 'h':
		default:
			fprintf(stderr, "usage: %s [-d] [torrent|magnet|infohash ...]\n", argv[0]);
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	/* Route logs to our ring buffer */
	mbt_log_set(tui_log_cb, NULL);
	if (debug_mode)
		mbt_log_set_debug(MBT_LOG_ALL);

	mbt_ev_new_default(&ev);

	/* Init ncurses early so UI renders while session starts */
	initscr();
	cbreak();
	noecho();
	nodelay(stdscr, TRUE);
	keypad(stdscr, TRUE);
	curs_set(0);
	signal(SIGWINCH, on_resize);

	int list_h = LINES / 2;
	int det_h = LINES - list_h - 2;
	win_list = newwin(list_h, COLS, 1, 0);
	win_detail = newwin(det_h, COLS, list_h + 1, 0);
	win_cmd = newwin(1, COLS, LINES - 1, 0);
	draw_all();

	struct mbt_session_config cfg = {
		.ev = ev,
		.default_storage = mbt_storage_file_new("."),
		.listen_port = 0,
		.mbt_lsd_announce = 1,
		.http = curl_http_fn,
	};
	mbt_session_new(&session, &cfg);
	mbt_session_enable_dht(session);
	mbt_session_enable_lsd(session);

	/* Add any args as torrents (.torrent, magnet, or raw infohash) */
	for (int i = 0; i < argc; i++) {
		int rv = mbt_session_add_torrent_file(session, argv[i], NULL);
		if (rv < 0)
			rv = mbt_session_add_magnet(session, argv[i], NULL);
		if (rv < 0 && strlen(argv[i]) == 40) {
			char mag[256];
			snprintf(mag, sizeof(mag), "magnet:?xt=urn:btih:%.40s", argv[i]);
			mbt_session_add_magnet(session, mag, NULL);
		}
	}

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
