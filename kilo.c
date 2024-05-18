// TODO: ch04 vertical scrolling

/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

// 0x1f is a hexadecimal that we're using as a bitmask
#define CTRL_KEY(k) ((k) & 0x1f)


enum editorKey { // 1000, 1001, 1002, ...
	ARROW_LEFT = 1000, 
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/*** data ***/

typedef struct erow { // editor row
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

// global editor state
struct editorConfig {
	int cx, cy;
	int rx; // index into the render field
	int rowoff; // row offset, keeps track of what row of a file the user currently is scrolled to
	int coloff; // same as rowoff, but for columns
	int screenrows; // max number of rows that can be displayed
	int screencols; // max cols displayed
	int numrows; // number of rows 
	erow *row; // a pointer to rows? why can we index this?
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

// declare new editorConfig struct with variable name E
struct editorConfig E;

struct termios orig_termios; // store original terminal attributes

/*** terminal ***/

void die(const char *s) {
	// error handler
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	// clear and reset screen on error

	
	perror(s); // looks at the global errno and prints a descriptive message for it,
	           // also prints the string given to it
	exit(1); // exits the program with exit status 1
}

void disableRawMode(void) {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("tcsetattr");
}

void enableRawMode(void) {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
	atexit(disableRawMode);
	// atexit registers a function to be called when the
	// program ends

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST); // disables the output flag that translates \n to \r\n
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // turn off echo mode
	// ECHO is a local bitflag (set mark to ECHO with eglot enabled)
	// the tilde (~) is the bitwise NOT-operator (flip all the bits)
	// we then use the bitwise AND (with &=) to which forces every 4th bit
	// to become 0
	// ICANON is a local bitflag, when enabled it tells the terminal to wait for
	// <return> before writing input to STDIO
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
	// send raw to stdin
	// TCSAFLUSH says when to apply the changes, first drain output, then flush input
	// this means wait for all pending output to be written to terminal,
	// then discards any input that hasn't been read
}

int editorReadKey(void) {
	// read keys sent to stdin
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
					case '1': return HOME_KEY;
					case '3': return DEL_KEY;
					case '4': return END_KEY;
					case '5': return PAGE_UP;
					case '6': return PAGE_DOWN;
					case '7': return HOME_KEY;
					case '8': return END_KEY;
					}
				}				
			} else {
				switch (seq[1]) {
				case 'A': return ARROW_UP;
				case 'B': return ARROW_DOWN;
				case 'C': return ARROW_RIGHT;
				case 'D': return ARROW_LEFT;
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
			case 'H': return HOME_KEY;
			case 'F': return END_KEY;
			}
		}
		return '\x1b';
	} else {
		return c;
	}
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
  return 0;
}

int getWindowSize(int *rows, int *cols) {
	// get the window size using ictl
	// this function structure represents a common approach of returning
	// multiple values in C, we can use the return value to indicate
	// success (with 0) or failure (with -1)
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		// ioctl places the number of rows and cols of the terminal into ws
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t')
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		rx++;
	}
	return rx;
}

void editorUpdateRow(erow *row) {
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') tabs++;
	}
	
	free(row->render);
	row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

	
	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row -> chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
}

/*** file i/o ***/
void editorOpen(char *filename) {
	free(E.filename);
	E.filename = strdup(filename);
	
	FILE *fp = fopen(filename, "r");
	if (!fp) die("fopen");
	
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
													 line[linelen - 1] == '\r'))
			linelen--;
		editorAppendRow(line, linelen);
	}
	free(line);
	fclose(fp);
}

/*** append buffer ***/

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);
	// realloc abuf buffer, to a new block of memory with an increased size

	if (new == NULL) return;
	// if new is unable to allocate memory, it returns NULL
	memcpy(&new[ab->len], s, len);
	// copy s into the new block of memory starting at the end of the original string
	ab->b = new;
	// point b to the new block of memory
	ab->len += len;
	// add the length of the newly added string
}

void abFree(struct abuf *ab) {
	free(ab->b);
}

/*** output ***/

void editorScroll(void) {
	E.rx = 0;

	if (E.cy < E.numrows) {
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}

	if (E.cy < E.rowoff) {
		E.rowoff = E.cy; // keep rowoffset always below cursor y
	}
	if (E.cy >= E.rowoff + E.screenrows) {
		E.rowoff = E.cy - E.screenrows + 1; // increase rowOffset
	}
	if (E.rx < E.coloff) { // clamp coloffset
		E.coloff = E.rx;
	}
	if (E.rx >= E.coloff + E.screencols) {
		E.coloff = E.rx - E.screencols + 1;
	}
}

void editorDrawRows(struct abuf *ab) {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		int filerow = y + E.rowoff;
		if (y >= E.numrows) {
			if (E.numrows == 0 && y == E.screenrows / 3) {
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome),
																	"Kilo editor -- version %s", KILO_VERSION);
				if (welcomelen > E.screencols) welcomelen = E.screencols;
				int padding = (E.screencols - welcomelen) / 2;
				if (padding) {
					abAppend(ab, "~", 1);
					padding--;
				}
				while (padding--) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			} else {
				abAppend(ab, "~", 1);
			}
		} else {
			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0) len = 0;
			if (len > E.screencols) len = E.screencols;
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
		}
		
		abAppend(ab, "\x1b[K", 3);
			abAppend(ab, "\r\n", 2);
		}
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
										 E.filename ? E.filename : "[No Name]", E.numrows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
											E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;
	if (msglen && time(NULL) - E.statusmsg_time < 5) {
		abAppend(ab, E.statusmsg, msglen);
	}
}

void editorRefreshScreen(void) {
	editorScroll();
	
	struct abuf ab = ABUF_INIT;
	
	abAppend(&ab, "\x1b[?25l", 6); // hide cursor
	abAppend(&ab, "\x1b[H", 3); // set cursor position to 1;1
	
	// write(STDOUT_FILENO, "\x1b[2J", 4); // replaced with apAppend
	// the 4 means we're writing four bytes to stdout
	// the string is the escape sequence we are writing
	// \x1b is the escape character (27 in decimal)
	// the other 3 bytes are [2J

	// escape sequences always start with "\x1b[" ( the escape character followed by '[' )
	// escape sequences instruct the terminal to do certain formatting tasks
	// 'J' means Erase in display, the 2 is an argument that means erase the entire display
	// "2J" means erase everything
	// "1J" means erase everything above the cursor
	// "0J" means erase everything below the cursor
	// this program primarily usese VT100 escape sequences
	// we could use the ncurses library and query terminfo
	// to target a larger number of terminals, but that is out of scope from this
	// project

	// write(STDOUT_FILENO, "\x1b[H", 3);
	// sets the cursor to row 1, col 1 of the terminal ("\x1b[1;1H")
	// this is default behavior, to pass in different coordinates, the escape seq
	// would look something like this "\x1b[5;10H"
	// row and col coords start at 1, not 0

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
	abAppend(&ab, buf, strlen(buf));
	
	abAppend(&ab, "\x1b[?25h", 6); // show cursor

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void editorStatusMessage(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

/*** input ***/

void editorMoveCursor(int key) {
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	
	switch(key) {
	case ARROW_LEFT:
		if (E.cx != 0) {
			E.cx--;
		} else if (E.cy > 0) {
			E.cy--;
			E.cx = E.row[E.cy].size;
		}
		break;
	case ARROW_RIGHT:
		if (row && E.cx < row->size) {
			E.cx++;
		} else if (row && E.cx == row->size) {
			E.cy++;
			E.cx = 0;
		}
		break;
	case ARROW_UP:
		if (E.cy != 0) {
			E.cy--;
		}
		break;
	case ARROW_DOWN:
		if (E.cy < E.numrows) {
			E.cy++;
		}
		break;
	}
	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen) {
		E.cx = rowlen;
	}
	
}

void editorProcessKeypress(void) {
	int c = editorReadKey();

	switch (c) {
	case CTRL_KEY('q'):
		write(STDOUT_FILENO, "\x1b[2J", 4); // clear screen & reset cursor
		write(STDOUT_FILENO, "\x1b[H", 3);
		exit(0);
		break;

	case HOME_KEY:
		E.cx = 0;
		break;
	case END_KEY:
		if (E.cy < E.numrows) {
			E.cx = E.row[E.cy].size;
		}
		break;

	case PAGE_UP:
  case PAGE_DOWN:
    {
      if (c == PAGE_UP) {
        E.cy = E.rowoff;
      } else if (c == PAGE_DOWN) {
        E.cy = E.rowoff + E.screenrows - 1;
        if (E.cy > E.numrows) E.cy = E.numrows;
      }
      int times = E.screenrows;
      while (times--)
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;

	case ARROW_UP:
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
		editorMoveCursor(c);
		break;
	}
}

/*** init ***/

void initEditor(void) {
	E.cx = 0;	
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
	E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
	enableRawMode(); // enable raw mode
	initEditor();

	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	editorStatusMessage("HELP: Ctrl-q = quit");
	
	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
