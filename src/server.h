#include <libwebsockets.h>
#include <stdbool.h>
#include <uv.h>

#include "pty.h"

// client message
#define INPUT '0'
#define RESIZE_TERMINAL '1'
#define PAUSE '2'
#define RESUME '3'
#define JSON_DATA '{'

// server message
#define OUTPUT '0'
#define SET_WINDOW_TITLE '1'
#define SET_PREFERENCES '2'

// url paths
struct endpoints {
  char *ws;
  char *index;
  char *token;
  char *parent;
  char *api_tabs;
};

extern volatile bool force_exit;
extern struct lws_context *context;
extern struct server *server;
extern struct endpoints endpoints;

struct pss_http {
  char path[128];
  char *buffer;
  char *ptr;
  size_t len;
};

struct pss_tty {
  bool initialized;
  int initial_cmd_index;
  bool authenticated;
  char user[30];
  char address[50];
  char path[128];
  char **args;
  int argc;

  struct lws *wsi;
  char *buffer;
  size_t len;
  int tab_id;  // tmux-tabs mode: session id of the attached tab, 0 if none

  pty_process *process;
  pty_buf_t *pty_buf;

  int lws_close_status;
};

typedef struct {
  struct pss_tty *pss;
  bool ws_closed;
} pty_ctx_t;

// tmux-backed tab session management (tmux-tabs mode only)
bool tabs_valid_id(const char *s);
void tabs_session_name(int id, char *buf, size_t len);
int tabs_tmux_argv(char **argv, size_t argc_max);
char *tabs_list_json();
int tabs_create(void);
int tabs_kill(int id);

struct server {
  int client_count;        // client count
  char *prefs_json;        // client preferences
  char *credential;        // encoded basic auth credential
  char *auth_header;       // header name used for auth proxy
  char *index;             // custom index.html
  char *command;           // full command line
  char **argv;             // command with arguments
  int argc;                // command + arguments count
  char *cwd;               // working directory
  int sig_code;            // close signal
  char sig_name[20];       // human readable signal string
  bool url_arg;            // allow client to send cli arguments in URL
  bool writable;           // whether clients to write to the TTY
  bool check_origin;       // whether allow websocket connection from different origin
  int max_clients;         // maximum clients to support
  bool once;               // whether accept only one client and exit on disconnection
  bool exit_no_conn;       // whether exit on all clients disconnection
  bool tmux_tabs;          // whether use tmux-backed persistent tab mode
  bool tmux_mouse;         // tmux-tabs mode: enable tmux mouse mode on the tab server
  char tmux_socket[108];   // tmux socket for tab mode (absolute path: -S, name: -L)
  char socket_path[255];   // UNIX domain socket path
  char terminal_type[30];  // terminal type to report

  uv_loop_t *loop;         // the libuv event loop
};
