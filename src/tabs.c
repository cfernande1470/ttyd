#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "server.h"
#include "utils.h"

#define TABS_PREFIX "ttyd-"
#define TABS_PREFIX_LEN (sizeof(TABS_PREFIX) - 1)
#define TABS_MAX_ID_LEN 9

bool tabs_valid_id(const char *s) {
  size_t len = strlen(s);
  if (len == 0 || len > TABS_MAX_ID_LEN) return false;
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

void tabs_session_name(int id, char *buf, size_t len) {
  snprintf(buf, len, TABS_PREFIX "%d", id);
}

int tabs_tmux_argv(char **argv, size_t argc_max) {
  int n = 0;
  if (n + 1 >= (int)argc_max) return -1;
  argv[n++] = "tmux";
  if (strlen(server->tmux_socket) > 0) {
    if (n + 2 >= (int)argc_max) return -1;
    // absolute socket paths need -S; relative names are resolved by tmux with -L
    if (server->tmux_socket[0] == '/') {
      argv[n++] = "-S";
    } else {
      argv[n++] = "-L";
    }
    argv[n++] = server->tmux_socket;
  }
  return n;
}

// run argv[0] with the given arguments, merging stdout+stderr into out.
// returns the exit code, or -1 on fork/exec failure.
static int run_capture(char *const argv[], char *out, size_t out_len) {
  int pipefd[2];
  if (pipe(pipefd) < 0) return -1;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);
    execvp(argv[0], argv);
    _exit(127);
  }

  close(pipefd[1]);
  size_t total = 0;
  ssize_t r;
  while (total < out_len - 1 && (r = read(pipefd[0], out + total, out_len - 1 - total)) > 0) {
    total += (size_t)r;
  }
  out[total] = '\0';
  close(pipefd[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// append s to buf as a JSON string (with quotes), returning new length or -1
static size_t json_append_string(char *buf, size_t off, size_t cap, const char *s) {
  if (off + 2 >= cap) return -1;
  buf[off++] = '"';
  for (const char *p = s; *p && off + 7 < cap; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\') {
      buf[off++] = '\\';
      buf[off++] = (char)c;
    } else if (c < 0x20) {
      off += (size_t)snprintf(buf + off, cap - off, "\\u%04x", c);
    } else {
      buf[off++] = (char)c;
    }
  }
  if (off + 1 >= cap) return -1;
  buf[off++] = '"';
  return off;
}

// collect the ids of the existing tab sessions, and return max id + 1 (1 if none)
static int tabs_next_id(char *list_output) {
  int next = 1;
  char *saveptr = NULL;
  for (char *line = strtok_r(list_output, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
    if (strncmp(line, TABS_PREFIX, TABS_PREFIX_LEN) != 0) continue;
    const char *id = line + TABS_PREFIX_LEN;
    if (!tabs_valid_id(id)) continue;
    int value = atoi(id);
    if (value >= next) next = value + 1;
  }
  return next;
}

// parse one "name<TAB>window_name" line; on success store the session id in idbuf
static bool tabs_parse_line(const char *line, char *idbuf, size_t idlen, const char **title) {
  const char *tab = strchr(line, '\t');
  size_t namelen = tab ? (size_t)(tab - line) : strlen(line);
  if (namelen < TABS_PREFIX_LEN || namelen >= idlen) return false;
  memcpy(idbuf, line, namelen);
  idbuf[namelen] = '\0';
  if (strncmp(idbuf, TABS_PREFIX, TABS_PREFIX_LEN) != 0) return false;
  if (!tabs_valid_id(idbuf + TABS_PREFIX_LEN)) return false;
  *title = tab ? tab + 1 : "";
  return true;
}

char *tabs_list_json() {
  char **argv = xmalloc(8 * sizeof(char *));
  int n = tabs_tmux_argv(argv, 8);
  if (n < 0) {
    free(argv);
    return NULL;
  }
  argv[n++] = "list-sessions";
  argv[n++] = "-F";
  argv[n++] = "#{session_name}\t#{window_name}";
  argv[n] = NULL;

  char output[8192];
  int rc = run_capture(argv, output, sizeof(output));
  free(argv);
  if (rc == -1) return NULL;  // could not exec tmux

  // count matching entries and estimate the escaped title length
  size_t count = 0, name_total = 0;
  {
    char copy[8192];
    char *saveptr = NULL;
    char idbuf[32];
    const char *title;
    strncpy(copy, output, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    for (char *line = strtok_r(copy, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
      if (!tabs_parse_line(line, idbuf, sizeof(idbuf), &title)) continue;
      count++;
      name_total += strlen(title) * 2;  // worst case escaping
    }
  }
  if (count == 0) return strdup("[]");

  size_t cap = count * (TABS_PREFIX_LEN + TABS_MAX_ID_LEN + 16 + name_total + 64) + 2;
  char *json = xmalloc(cap);
  size_t off = 0;
  json[off++] = '[';
  {
    char *saveptr = NULL;
    char idbuf[32];
    const char *title;
    for (char *line = strtok_r(output, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
      if (!tabs_parse_line(line, idbuf, sizeof(idbuf), &title)) continue;
      if (off > 1) json[off++] = ',';
      off += (size_t)snprintf(json + off, cap - off, "{\"id\":%d,\"title\":", atoi(idbuf + TABS_PREFIX_LEN));
      off = json_append_string(json, off, cap, title);
      if (off == (size_t)-1) {
        free(json);
        return NULL;
      }
      json[off++] = '}';
    }
  }
  json[off++] = ']';
  json[off] = '\0';
  return json;
}

int tabs_create(void) {
  // determine the next free session id
  char **argv = xmalloc(8 * sizeof(char *));
  int n = tabs_tmux_argv(argv, 8);
  if (n < 0) {
    free(argv);
    return -1;
  }
  argv[n++] = "list-sessions";
  argv[n++] = "-F";
  argv[n++] = "#{session_name}";
  argv[n] = NULL;
  char output[8192];
  int rc = run_capture(argv, output, sizeof(output));
  free(argv);
  if (rc == -1) return -1;
  int next = tabs_next_id(output);

  // create the session with the shared command
  const char *home = getenv("HOME");
  argv = xmalloc((14 + server->argc) * sizeof(char *));
  n = tabs_tmux_argv(argv, 14 + (size_t)server->argc);
  if (n < 0) {
    free(argv);
    return -1;
  }
  char session_name[32];
  tabs_session_name(next, session_name, sizeof(session_name));
  argv[n++] = "new-session";
  argv[n++] = "-d";
  argv[n++] = "-s";
  argv[n++] = session_name;
  if (home != NULL) {
    argv[n++] = "-c";
    argv[n++] = (char *)home;
  }
  for (int i = 0; i < server->argc; i++) {
    argv[n++] = server->argv[i];
  }
  argv[n] = NULL;

  char err[256];
  rc = run_capture(argv, err, sizeof(err));
  if (rc != 0) lwsl_err("tabs_create: tmux new-session failed (%d): %s\n", rc, err);
  free(argv);
  return rc == 0 ? next : -1;
}

int tabs_kill(int id) {
  char **argv = xmalloc(8 * sizeof(char *));
  int n = tabs_tmux_argv(argv, 8);
  if (n < 0) {
    free(argv);
    return -1;
  }
  char session_name[32];
  tabs_session_name(id, session_name, sizeof(session_name));
  argv[n++] = "kill-session";
  argv[n++] = "-t";
  argv[n++] = session_name;
  argv[n] = NULL;

  char err[256];
  int rc = run_capture(argv, err, sizeof(err));
  free(argv);
  return rc == 0 ? 0 : -1;
}
