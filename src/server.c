
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PORT
#define PORT 4242
#endif

#define MAX_USERS 100
#define MAX_NAME 32
#define MAX_FIELD 16
#define MAX_BIO 160
#define MAX_INBUF 8192
#define MAX_LINE 2048
#define MAX_CANDIDATES 10
#define STATE_FILE "state.txt"
#define MAX_CHAT_TEXT 512
#define MAX_CHAT_HISTORY 100
#define MAX_OUTBUF 262144

typedef struct {
    int sender;
    char text[MAX_CHAT_TEXT];
} ChatMessage;

typedef struct {
    int allocated;
    int start;
    int count;
    ChatMessage *msgs;
} Conversation;

typedef struct {
    int used;
    char username[MAX_NAME];
    int age;
    char gender[MAX_FIELD];
    char preference[MAX_FIELD];
    char bio[MAX_BIO];

    int online;
    int conn_fd;
    int open_chat;

    int seen[MAX_USERS];
    int liked[MAX_USERS];
    int matched[MAX_USERS];
    int unmatched[MAX_USERS];
    int unread[MAX_USERS];
} User;

typedef struct {
    int fd;
    int user_index;
    int active;
    int close_after_flush;
    int inbuf_len;
    char inbuf[MAX_INBUF];
    char *outbuf;
    size_t out_len;
    size_t out_cap;
} Connection;

static User users[MAX_USERS];
static Connection conns[FD_SETSIZE];
static Conversation conversations[MAX_USERS][MAX_USERS];
static volatile sig_atomic_t stop_flag = 0;

static void handle_sigint(int signo) {
    (void)signo;
    stop_flag = 1;
}

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static void safe_copy(char *dst, const char *src, size_t dst_size) {
    size_t copy_len;

    if (dst_size == 0) {
        return;
    }
    copy_len = strlen(src);
    if (copy_len >= dst_size) {
        copy_len = dst_size - 1;
    }
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static void sanitize_profile_text(char *dst, const char *src, size_t dst_size) {
    size_t i;
    size_t j = 0;

    if (dst_size == 0) {
        return;
    }
    for (i = 0; src[i] != '\0' && j + 1 < dst_size; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\n' || ch == '\r') {
            continue;
        }
        if (ch == '|' || ch == ',' || ch == ':') {
            dst[j++] = '/';
        } else if (isprint(ch)) {
            dst[j++] = (char)ch;
        }
    }
    dst[j] = '\0';
}

static void sanitize_message_text(char *dst, const char *src, size_t dst_size) {
    size_t i;
    size_t j = 0;

    if (dst_size == 0) {
        return;
    }
    for (i = 0; src[i] != '\0' && j + 1 < dst_size; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\n' || ch == '\r') {
            continue;
        }
        if (ch == '|') {
            dst[j++] = '/';
        } else if (isprint(ch)) {
            dst[j++] = (char)ch;
        }
    }
    dst[j] = '\0';
}

static int username_is_valid(const char *name) {
    size_t i;
    size_t len = strlen(name);

    if (len == 0 || len >= MAX_NAME) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-')) {
            return 0;
        }
    }
    return 1;
}

static int is_allowed_gender(const char *s) {
    return strcasecmp(s, "man") == 0 ||
           strcasecmp(s, "woman") == 0 ||
           strcasecmp(s, "nonbinary") == 0 ||
           strcasecmp(s, "other") == 0;
}

static int is_allowed_preference(const char *s) {
    return strcasecmp(s, "any") == 0 || is_allowed_gender(s);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

static int find_user_index(const char *username) {
    int i;
    for (i = 0; i < MAX_USERS; i++) {
        if (users[i].used && strcasecmp(users[i].username, username) == 0) {
            return i;
        }
    }
    return -1;
}

static int create_user(const char *username) {
    int i;
    for (i = 0; i < MAX_USERS; i++) {
        if (!users[i].used) {
            memset(&users[i], 0, sizeof(users[i]));
            users[i].used = 1;
            safe_copy(users[i].username, username, sizeof(users[i].username));
            users[i].conn_fd = -1;
            users[i].open_chat = -1;
            return i;
        }
    }
    return -1;
}

static int user_has_profile(int idx) {
    return idx >= 0 && idx < MAX_USERS && users[idx].used && users[idx].age > 0 &&
           users[idx].gender[0] != '\0' && users[idx].preference[0] != '\0';
}

static int pref_matches(const char *pref, const char *gender) {
    if (strcasecmp(pref, "any") == 0) {
        return 1;
    }
    return strcasecmp(pref, gender) == 0;
}

static int is_candidate_for(int me, int other) {
    if (me < 0 || other < 0 || me >= MAX_USERS || other >= MAX_USERS) {
        return 0;
    }
    if (me == other || !users[other].used) {
        return 0;
    }
    if (!user_has_profile(me) || !user_has_profile(other)) {
        return 0;
    }
    if (users[me].seen[other] || users[me].matched[other] || users[me].unmatched[other] ||
        users[other].unmatched[me]) {
        return 0;
    }
    if (!pref_matches(users[me].preference, users[other].gender)) {
        return 0;
    }
    if (!pref_matches(users[other].preference, users[me].gender)) {
        return 0;
    }
    return 1;
}

static void normalize_pair(int a, int b, int *lo, int *hi) {
    if (a < b) {
        *lo = a;
        *hi = b;
    } else {
        *lo = b;
        *hi = a;
    }
}

static Conversation *get_conversation(int a, int b) {
    int lo;
    int hi;

    if (a < 0 || b < 0 || a >= MAX_USERS || b >= MAX_USERS || a == b) {
        return NULL;
    }
    normalize_pair(a, b, &lo, &hi);
    return &conversations[lo][hi];
}

static int ensure_conversation_allocated(Conversation *conv) {
    if (conv == NULL) {
        return -1;
    }
    if (!conv->allocated) {
        conv->msgs = calloc((size_t)MAX_CHAT_HISTORY, sizeof(ChatMessage));
        if (conv->msgs == NULL) {
            return -1;
        }
        conv->allocated = 1;
        conv->start = 0;
        conv->count = 0;
    }
    return 0;
}

static void conversation_clear(int a, int b) {
    Conversation *conv = get_conversation(a, b);
    if (conv == NULL) {
        return;
    }
    free(conv->msgs);
    conv->msgs = NULL;
    conv->allocated = 0;
    conv->start = 0;
    conv->count = 0;
}

static int conversation_add_message(int a, int b, int sender, const char *text) {
    int pos;
    Conversation *conv = get_conversation(a, b);

    if (conv == NULL) {
        return -1;
    }
    if (ensure_conversation_allocated(conv) < 0) {
        return -1;
    }

    if (conv->count < MAX_CHAT_HISTORY) {
        pos = (conv->start + conv->count) % MAX_CHAT_HISTORY;
        conv->count++;
    } else {
        pos = conv->start;
        conv->start = (conv->start + 1) % MAX_CHAT_HISTORY;
    }

    conv->msgs[pos].sender = sender;
    safe_copy(conv->msgs[pos].text, text, sizeof(conv->msgs[pos].text));
    return 0;
}

static void free_all_conversations(void) {
    int i;
    int j;

    for (i = 0; i < MAX_USERS; i++) {
        for (j = i + 1; j < MAX_USERS; j++) {
            conversation_clear(i, j);
        }
    }
}

static int total_unread_for(int me) {
    int i;
    int total = 0;
    for (i = 0; i < MAX_USERS; i++) {
        total += users[me].unread[i];
    }
    return total;
}

static void reset_connection(int fd) {
    if (fd < 0 || fd >= FD_SETSIZE) {
        return;
    }
    free(conns[fd].outbuf);
    conns[fd].outbuf = NULL;
    conns[fd].out_len = 0;
    conns[fd].out_cap = 0;
    conns[fd].fd = -1;
    conns[fd].user_index = -1;
    conns[fd].active = 0;
    conns[fd].close_after_flush = 0;
    conns[fd].inbuf_len = 0;
    conns[fd].inbuf[0] = '\0';
}

static void disconnect_client(int fd) {
    int idx;

    if (fd < 0 || fd >= FD_SETSIZE || !conns[fd].active) {
        return;
    }

    idx = conns[fd].user_index;
    if (idx >= 0 && idx < MAX_USERS && users[idx].used) {
        users[idx].online = 0;
        users[idx].conn_fd = -1;
        users[idx].open_chat = -1;
    }

    close(fd);
    reset_connection(fd);
}

static int flush_connection(int fd) {
    while (conns[fd].active && conns[fd].out_len > 0) {
        ssize_t n = send(fd, conns[fd].outbuf, conns[fd].out_len, MSG_NOSIGNAL);
        if (n > 0) {
            if ((size_t)n < conns[fd].out_len) {
                memmove(conns[fd].outbuf, conns[fd].outbuf + n, conns[fd].out_len - (size_t)n);
            }
            conns[fd].out_len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        disconnect_client(fd);
        return -1;
    }

    if (conns[fd].active && conns[fd].close_after_flush && conns[fd].out_len == 0) {
        disconnect_client(fd);
        return -1;
    }
    return 0;
}

static int queue_bytes_fd(int fd, const char *buf, size_t len) {
    size_t need;
    size_t new_cap;
    char *tmp;

    if (fd < 0 || fd >= FD_SETSIZE || !conns[fd].active) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    need = conns[fd].out_len + len;
    if (need > MAX_OUTBUF) {
        disconnect_client(fd);
        return -1;
    }

    if (need > conns[fd].out_cap) {
        new_cap = conns[fd].out_cap == 0 ? 4096 : conns[fd].out_cap;
        while (new_cap < need) {
            new_cap *= 2;
        }
        tmp = realloc(conns[fd].outbuf, new_cap);
        if (tmp == NULL) {
            disconnect_client(fd);
            return -1;
        }
        conns[fd].outbuf = tmp;
        conns[fd].out_cap = new_cap;
    }

    memcpy(conns[fd].outbuf + conns[fd].out_len, buf, len);
    conns[fd].out_len += len;
    return 0;
}

static int queue_line_fd(int fd, const char *line) {
    if (queue_bytes_fd(fd, line, strlen(line)) < 0) {
        return -1;
    }
    return flush_connection(fd);
}

static void notify_user(int idx, const char *line) {
    int fd;
    if (idx < 0 || idx >= MAX_USERS || !users[idx].used || !users[idx].online) {
        return;
    }
    fd = users[idx].conn_fd;
    if (fd >= 0) {
        (void)queue_line_fd(fd, line);
    }
}

static void notify_total_unread(int idx) {
    char line[MAX_LINE];
    if (idx < 0 || idx >= MAX_USERS || !users[idx].used || !users[idx].online) {
        return;
    }
    snprintf(line, sizeof(line), "NOTIFY_UNREAD|%d\n", total_unread_for(idx));
    notify_user(idx, line);
}

static void send_profile_line(int fd, int idx) {
    char line[MAX_LINE];
    snprintf(line, sizeof(line), "PROFILE|%s|%d|%s|%s|%s\n",
             users[idx].username,
             users[idx].age,
             users[idx].gender,
             users[idx].preference,
             users[idx].bio);
    (void)queue_line_fd(fd, line);
}

static void save_state(void) {
    FILE *fp;
    int i;
    int j;
    int k;

    fp = fopen(STATE_FILE, "w");
    if (fp == NULL) {
        perror("fopen state.txt");
        return;
    }

    for (i = 0; i < MAX_USERS; i++) {
        if (!users[i].used) {
            continue;
        }
        fprintf(fp, "USER|%s|%d|%s|%s|%s\n",
                users[i].username,
                users[i].age,
                users[i].gender,
                users[i].preference,
                users[i].bio);

        fprintf(fp, "SEEN");
        for (j = 0; j < MAX_USERS; j++) {
            if (users[i].seen[j] && users[j].used) {
                fprintf(fp, "|%s", users[j].username);
            }
        }
        fprintf(fp, "\n");

        fprintf(fp, "LIKED");
        for (j = 0; j < MAX_USERS; j++) {
            if (users[i].liked[j] && users[j].used) {
                fprintf(fp, "|%s", users[j].username);
            }
        }
        fprintf(fp, "\n");

        fprintf(fp, "MATCHED");
        for (j = 0; j < MAX_USERS; j++) {
            if (users[i].matched[j] && users[j].used) {
                fprintf(fp, "|%s", users[j].username);
            }
        }
        fprintf(fp, "\n");

        fprintf(fp, "UNMATCHED");
        for (j = 0; j < MAX_USERS; j++) {
            if (users[i].unmatched[j] && users[j].used) {
                fprintf(fp, "|%s", users[j].username);
            }
        }
        fprintf(fp, "\n");

        fprintf(fp, "UNREAD");
        for (j = 0; j < MAX_USERS; j++) {
            if (users[i].unread[j] > 0 && users[j].used) {
                fprintf(fp, "|%s:%d", users[j].username, users[i].unread[j]);
            }
        }
        fprintf(fp, "\nENDUSER\n");
    }

    for (i = 0; i < MAX_USERS; i++) {
        for (j = i + 1; j < MAX_USERS; j++) {
            Conversation *conv = &conversations[i][j];
            if (!users[i].used || !users[j].used || !conv->allocated || conv->count == 0) {
                continue;
            }
            fprintf(fp, "CONV|%s|%s\n", users[i].username, users[j].username);
            for (k = 0; k < conv->count; k++) {
                int pos = (conv->start + k) % MAX_CHAT_HISTORY;
                int sender = conv->msgs[pos].sender;
                if (sender >= 0 && sender < MAX_USERS && users[sender].used) {
                    fprintf(fp, "MSG|%s|%s\n", users[sender].username, conv->msgs[pos].text);
                }
            }
            fprintf(fp, "ENDCONV\n");
        }
    }

    fclose(fp);
}

static void load_relation_list(int target_set[MAX_USERS], char *rest) {
    char *token = strtok(rest, "|");
    while (token != NULL) {
        int idx = find_user_index(token);
        if (idx == -1) {
            idx = create_user(token);
        }
        if (idx != -1) {
            target_set[idx] = 1;
        }
        token = strtok(NULL, "|");
    }
}

static void load_unread_list(int current, char *rest) {
    char *token = strtok(rest, "|");
    while (token != NULL) {
        char *colon = strchr(token, ':');
        if (colon != NULL) {
            int idx;
            *colon = '\0';
            idx = find_user_index(token);
            if (idx == -1) {
                idx = create_user(token);
            }
            if (idx != -1) {
                users[current].unread[idx] = atoi(colon + 1);
            }
        }
        token = strtok(NULL, "|");
    }
}

static void load_state(void) {
    FILE *fp;
    char line[MAX_LINE];
    int current = -1;
    int conv_a = -1;
    int conv_b = -1;

    fp = fopen(STATE_FILE, "r");
    if (fp == NULL) {
        if (errno != ENOENT) {
            perror("fopen state.txt");
        }
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char work[MAX_LINE];
        char *tag;
        char *rest;

        trim_newline(line);
        if (line[0] == '\0') {
            continue;
        }

        safe_copy(work, line, sizeof(work));
        tag = strtok(work, "|");
        rest = strtok(NULL, "");
        if (tag == NULL) {
            continue;
        }

        if (strcmp(tag, "USER") == 0) {
            char *username;
            char *age_s;
            char *gender;
            char *preference;
            char *bio;
            int idx;

            username = strtok(rest, "|");
            age_s = strtok(NULL, "|");
            gender = strtok(NULL, "|");
            preference = strtok(NULL, "|");
            bio = strtok(NULL, "");
            if (username == NULL || age_s == NULL || gender == NULL || preference == NULL) {
                current = -1;
                continue;
            }
            if (bio == NULL) {
                bio = "";
            }

            idx = find_user_index(username);
            if (idx == -1) {
                idx = create_user(username);
            }
            if (idx == -1) {
                current = -1;
                continue;
            }

            users[idx].used = 1;
            safe_copy(users[idx].username, username, sizeof(users[idx].username));
            users[idx].age = atoi(age_s);
            safe_copy(users[idx].gender, gender, sizeof(users[idx].gender));
            safe_copy(users[idx].preference, preference, sizeof(users[idx].preference));
            safe_copy(users[idx].bio, bio, sizeof(users[idx].bio));
            users[idx].online = 0;
            users[idx].conn_fd = -1;
            users[idx].open_chat = -1;
            current = idx;
            conv_a = -1;
            conv_b = -1;
        } else if (strcmp(tag, "SEEN") == 0 && current != -1 && rest != NULL) {
            load_relation_list(users[current].seen, rest);
        } else if (strcmp(tag, "LIKED") == 0 && current != -1 && rest != NULL) {
            load_relation_list(users[current].liked, rest);
        } else if (strcmp(tag, "MATCHED") == 0 && current != -1 && rest != NULL) {
            load_relation_list(users[current].matched, rest);
        } else if (strcmp(tag, "UNMATCHED") == 0 && current != -1 && rest != NULL) {
            load_relation_list(users[current].unmatched, rest);
        } else if (strcmp(tag, "UNREAD") == 0 && current != -1 && rest != NULL) {
            load_unread_list(current, rest);
        } else if (strcmp(tag, "ENDUSER") == 0) {
            current = -1;
        } else if (strcmp(tag, "CONV") == 0) {
            char *user_a = strtok(rest, "|");
            char *user_b = strtok(NULL, "|");
            if (user_a == NULL || user_b == NULL) {
                conv_a = -1;
                conv_b = -1;
                continue;
            }
            conv_a = find_user_index(user_a);
            if (conv_a == -1) {
                conv_a = create_user(user_a);
            }
            conv_b = find_user_index(user_b);
            if (conv_b == -1) {
                conv_b = create_user(user_b);
            }
            if (conv_a != -1 && conv_b != -1) {
                Conversation *conv = get_conversation(conv_a, conv_b);
                if (conv != NULL) {
                    conversation_clear(conv_a, conv_b);
                    (void)ensure_conversation_allocated(conv);
                }
            }
        } else if (strcmp(tag, "MSG") == 0 && conv_a != -1 && conv_b != -1) {
            char *sender_name = strtok(rest, "|");
            char *text = strtok(NULL, "");
            int sender;
            if (sender_name == NULL || text == NULL) {
                continue;
            }
            sender = find_user_index(sender_name);
            if (sender == -1) {
                sender = create_user(sender_name);
            }
            if (sender != -1) {
                (void)conversation_add_message(conv_a, conv_b, sender, text);
            }
        } else if (strcmp(tag, "ENDCONV") == 0) {
            conv_a = -1;
            conv_b = -1;
        }
    }

    fclose(fp);
}

static void notify_match(int a, int b) {
    char line[MAX_LINE];

    snprintf(line, sizeof(line), "MATCH_NEW|%s\n", users[b].username);
    notify_user(a, line);
    snprintf(line, sizeof(line), "MATCH_NEW|%s\n", users[a].username);
    notify_user(b, line);
}

static void do_like(int me, int target) {
    users[me].seen[target] = 1;
    users[me].liked[target] = 1;

    if (users[target].liked[me] && !users[me].unmatched[target] && !users[target].unmatched[me]) {
        if (!users[me].matched[target]) {
            users[me].matched[target] = 1;
            users[target].matched[me] = 1;
            notify_match(me, target);
        }
    }
}

static void do_dislike(int me, int target) {
    users[me].seen[target] = 1;
}

static int collect_matches(int me, int out[MAX_USERS]) {
    int i;
    int count = 0;
    for (i = 0; i < MAX_USERS; i++) {
        if (users[me].matched[i] && users[i].used) {
            out[count++] = i;
        }
    }
    return count;
}

static void sort_match_indices(int me, int arr[MAX_USERS], int count) {
    int i;
    int j;
    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            int left = arr[i];
            int right = arr[j];
            int swap = 0;

            if (users[me].unread[right] > users[me].unread[left]) {
                swap = 1;
            } else if (users[me].unread[right] == users[me].unread[left] &&
                       strcasecmp(users[right].username, users[left].username) < 0) {
                swap = 1;
            }

            if (swap) {
                int temp = arr[i];
                arr[i] = arr[j];
                arr[j] = temp;
            }
        }
    }
}

static void handle_login(int fd, char *arg) {
    int idx;
    char username[MAX_NAME];

    if (arg == NULL) {
        (void)queue_line_fd(fd, "ERROR|usage LOGIN|username\n");
        return;
    }

    safe_copy(username, arg, sizeof(username));
    trim_newline(username);
    if (!username_is_valid(username)) {
        (void)queue_line_fd(fd, "ERROR|username must use only letters, digits, _ or -\n");
        return;
    }

    idx = find_user_index(username);
    if (idx == -1) {
        idx = create_user(username);
        if (idx == -1) {
            (void)queue_line_fd(fd, "ERROR|server full\n");
            return;
        }
    } else if (users[idx].online) {
        (void)queue_line_fd(fd, "ERROR|that username is already online\n");
        return;
    }

    users[idx].online = 1;
    users[idx].conn_fd = fd;
    users[idx].open_chat = -1;
    conns[fd].user_index = idx;

    (void)queue_line_fd(fd, "LOGIN_OK\n");
    send_profile_line(fd, idx);
    notify_total_unread(idx);
}

static void handle_profile_set(int fd, int me, char *rest) {
    char *age_s;
    char *gender;
    char *pref;
    char *bio;
    char clean_bio[MAX_BIO];
    int age;

    if (rest == NULL) {
        (void)queue_line_fd(fd, "ERROR|usage PROFILE_SET|age|gender|preference|bio\n");
        return;
    }

    age_s = strtok(rest, "|");
    gender = strtok(NULL, "|");
    pref = strtok(NULL, "|");
    bio = strtok(NULL, "");

    if (age_s == NULL || gender == NULL || pref == NULL || bio == NULL) {
        (void)queue_line_fd(fd, "ERROR|usage PROFILE_SET|age|gender|preference|bio\n");
        return;
    }

    age = atoi(age_s);
    if (age <= 0 || age > 120) {
        (void)queue_line_fd(fd, "ERROR|age must be between 1 and 120\n");
        return;
    }
    if (!is_allowed_gender(gender)) {
        (void)queue_line_fd(fd, "ERROR|gender must be man, woman, nonbinary, or other\n");
        return;
    }
    if (!is_allowed_preference(pref)) {
        (void)queue_line_fd(fd, "ERROR|preference must be any, man, woman, nonbinary, or other\n");
        return;
    }

    sanitize_profile_text(clean_bio, bio, sizeof(clean_bio));
    users[me].age = age;
    safe_copy(users[me].gender, gender, sizeof(users[me].gender));
    safe_copy(users[me].preference, pref, sizeof(users[me].preference));
    safe_copy(users[me].bio, clean_bio, sizeof(users[me].bio));

    save_state();
    (void)queue_line_fd(fd, "PROFILE_OK\n");
}

static void handle_profile_get(int fd, int me) {
    send_profile_line(fd, me);
}

static void handle_request_candidates(int fd, int me, char *rest) {
    int want = 5;
    int count = 0;
    int i;
    char line[MAX_LINE];

    if (rest != NULL && rest[0] != '\0') {
        want = atoi(rest);
        if (want <= 0) {
            want = 5;
        }
        if (want > MAX_CANDIDATES) {
            want = MAX_CANDIDATES;
        }
    }

    (void)queue_line_fd(fd, "CANDIDATE_BEGIN\n");
    for (i = 0; i < MAX_USERS && count < want; i++) {
        if (!is_candidate_for(me, i)) {
            continue;
        }
        snprintf(line, sizeof(line), "CANDIDATE|%s|%d|%s|%s|%s\n",
                 users[i].username,
                 users[i].age,
                 users[i].gender,
                 users[i].preference,
                 users[i].bio);
        (void)queue_line_fd(fd, line);
        count++;
    }
    (void)queue_line_fd(fd, "CANDIDATE_END\n");
}

static void handle_swipe_batch(int fd, int me, char *rest) {
    char copy[MAX_LINE];
    char *pair;
    int processed = 0;
    char line[MAX_LINE];

    if (rest == NULL || rest[0] == '\0') {
        (void)queue_line_fd(fd, "ERROR|empty swipe batch\n");
        return;
    }

    safe_copy(copy, rest, sizeof(copy));
    pair = strtok(copy, ",");
    while (pair != NULL) {
        char *colon = strchr(pair, ':');
        if (colon != NULL) {
            int target;
            *colon = '\0';
            target = find_user_index(pair);
            if (target != -1 && target != me && is_candidate_for(me, target)) {
                if (toupper((unsigned char)colon[1]) == 'L') {
                    do_like(me, target);
                    processed++;
                } else if (toupper((unsigned char)colon[1]) == 'D') {
                    do_dislike(me, target);
                    processed++;
                }
            }
        }
        pair = strtok(NULL, ",");
    }

    save_state();
    snprintf(line, sizeof(line), "SWIPE_OK|%d\n", processed);
    (void)queue_line_fd(fd, line);
}

static void handle_match_list(int fd, int me) {
    int arr[MAX_USERS];
    int count;
    int i;
    char line[MAX_LINE];

    count = collect_matches(me, arr);
    sort_match_indices(me, arr, count);

    (void)queue_line_fd(fd, "MATCH_LIST_BEGIN\n");
    for (i = 0; i < count; i++) {
        int idx = arr[i];
        snprintf(line, sizeof(line), "MATCH|%s|%s|%d\n",
                 users[idx].username,
                 users[idx].online ? "online" : "offline",
                 users[me].unread[idx]);
        (void)queue_line_fd(fd, line);
    }
    (void)queue_line_fd(fd, "MATCH_LIST_END\n");
}

static void handle_chat_open(int fd, int me, char *rest) {
    int target;
    int k;
    char line[MAX_LINE];
    Conversation *conv;

    if (rest == NULL || rest[0] == '\0') {
        (void)queue_line_fd(fd, "ERROR|usage CHAT_OPEN|username\n");
        return;
    }

    target = find_user_index(rest);
    if (target == -1 || !users[me].matched[target]) {
        (void)queue_line_fd(fd, "ERROR|you are not matched with that user\n");
        return;
    }

    users[me].open_chat = target;
    users[me].unread[target] = 0;
    save_state();

    snprintf(line, sizeof(line), "CHAT_OPEN_OK|%s|%s\n",
             users[target].username,
             users[target].online ? "online" : "offline");
    (void)queue_line_fd(fd, line);
    (void)queue_line_fd(fd, "CHAT_HISTORY_BEGIN\n");

    conv = get_conversation(me, target);
    if (conv != NULL && conv->allocated) {
        for (k = 0; k < conv->count; k++) {
            int pos = (conv->start + k) % MAX_CHAT_HISTORY;
            int sender = conv->msgs[pos].sender;
            if (sender >= 0 && sender < MAX_USERS && users[sender].used) {
                snprintf(line, sizeof(line), "CHAT_MSG|%s|%s\n",
                         users[sender].username,
                         conv->msgs[pos].text);
                (void)queue_line_fd(fd, line);
            }
        }
    }

    (void)queue_line_fd(fd, "CHAT_HISTORY_END\n");
    notify_total_unread(me);
}

static void handle_chat_close(int fd, int me) {
    users[me].open_chat = -1;
    (void)queue_line_fd(fd, "CHAT_CLOSE_OK\n");
}

static void handle_chat_send(int fd, int me, char *rest) {
    char *target_name;
    char *message;
    int target;
    char clean_message[MAX_CHAT_TEXT];
    char line[MAX_LINE];
    char self_line[MAX_LINE];

    if (rest == NULL) {
        (void)queue_line_fd(fd, "ERROR|usage CHAT_SEND|username|message\n");
        return;
    }

    target_name = strtok(rest, "|");
    message = strtok(NULL, "");
    if (target_name == NULL || message == NULL || message[0] == '\0') {
        (void)queue_line_fd(fd, "ERROR|usage CHAT_SEND|username|message\n");
        return;
    }

    target = find_user_index(target_name);
    if (target == -1 || !users[me].matched[target]) {
        (void)queue_line_fd(fd, "ERROR|you are not matched with that user\n");
        return;
    }

    sanitize_message_text(clean_message, message, sizeof(clean_message));
    if (clean_message[0] == '\0') {
        (void)queue_line_fd(fd, "ERROR|message cannot be empty\n");
        return;
    }

    if (conversation_add_message(me, target, me, clean_message) < 0) {
        (void)queue_line_fd(fd, "ERROR|could not store message\n");
        return;
    }

    if (users[target].online && users[target].open_chat == me) {
        snprintf(line, sizeof(line), "CHAT_APPEND|%s|%s\n", users[me].username, clean_message);
        notify_user(target, line);
    } else {
        users[target].unread[me] += 1;
        notify_total_unread(target);
    }

    save_state();
    snprintf(self_line, sizeof(self_line), "CHAT_SELF|%s\n", clean_message);
    (void)queue_line_fd(fd, self_line);
}

static void handle_unmatch(int fd, int me, char *rest) {
    int target;
    char line[MAX_LINE];

    if (rest == NULL || rest[0] == '\0') {
        (void)queue_line_fd(fd, "ERROR|usage UNMATCH|username\n");
        return;
    }

    target = find_user_index(rest);
    if (target == -1 || !users[me].matched[target]) {
        (void)queue_line_fd(fd, "ERROR|that user is not in your match list\n");
        return;
    }

    users[me].matched[target] = 0;
    users[target].matched[me] = 0;
    users[me].unmatched[target] = 1;
    users[target].unmatched[me] = 1;
    users[me].seen[target] = 1;
    users[target].seen[me] = 1;
    users[me].unread[target] = 0;
    users[target].unread[me] = 0;

    if (users[me].open_chat == target) {
        users[me].open_chat = -1;
    }
    if (users[target].open_chat == me) {
        users[target].open_chat = -1;
    }

    conversation_clear(me, target);
    save_state();

    (void)queue_line_fd(fd, "UNMATCH_OK\n");
    notify_total_unread(me);
    if (users[target].online) {
        snprintf(line, sizeof(line), "UNMATCHED_BY|%s\n", users[me].username);
        notify_user(target, line);
        notify_total_unread(target);
    }
}

static void handle_quit(int fd) {
    (void)queue_line_fd(fd, "BYE\n");
    if (fd >= 0 && fd < FD_SETSIZE && conns[fd].active) {
        conns[fd].close_after_flush = 1;
        (void)flush_connection(fd);
    }
}

static void handle_command(int fd, char *line) {
    char work[MAX_LINE];
    char *cmd;
    char *rest;
    int me;

    safe_copy(work, line, sizeof(work));
    trim_newline(work);

    cmd = strtok(work, "|");
    rest = strtok(NULL, "");
    if (cmd == NULL) {
        return;
    }

    me = conns[fd].user_index;
    if (strcmp(cmd, "LOGIN") == 0) {
        handle_login(fd, rest);
        return;
    }

    if (me < 0 || me >= MAX_USERS || !users[me].used) {
        (void)queue_line_fd(fd, "ERROR|please LOGIN first\n");
        return;
    }

    if (strcmp(cmd, "PROFILE_SET") == 0) {
        handle_profile_set(fd, me, rest);
    } else if (strcmp(cmd, "PROFILE_GET") == 0) {
        handle_profile_get(fd, me);
    } else if (strcmp(cmd, "REQUEST_CANDIDATES") == 0) {
        handle_request_candidates(fd, me, rest);
    } else if (strcmp(cmd, "SWIPE_BATCH") == 0) {
        handle_swipe_batch(fd, me, rest);
    } else if (strcmp(cmd, "MATCH_LIST") == 0) {
        handle_match_list(fd, me);
    } else if (strcmp(cmd, "CHAT_OPEN") == 0) {
        handle_chat_open(fd, me, rest);
    } else if (strcmp(cmd, "CHAT_CLOSE") == 0) {
        handle_chat_close(fd, me);
    } else if (strcmp(cmd, "CHAT_SEND") == 0) {
        handle_chat_send(fd, me, rest);
    } else if (strcmp(cmd, "UNMATCH") == 0) {
        handle_unmatch(fd, me, rest);
    } else if (strcmp(cmd, "QUIT") == 0) {
        handle_quit(fd);
    } else {
        (void)queue_line_fd(fd, "ERROR|unknown command\n");
    }
}

static void process_buffered_lines(int fd) {
    char *newline;

    while (conns[fd].active &&
           (newline = memchr(conns[fd].inbuf, '\n', (size_t)conns[fd].inbuf_len)) != NULL) {
        int line_len = (int)(newline - conns[fd].inbuf) + 1;
        int remaining;
        char line[MAX_LINE];

        if (line_len >= MAX_LINE) {
            (void)queue_line_fd(fd, "ERROR|input line too long\n");
            conns[fd].close_after_flush = 1;
            return;
        }

        memcpy(line, conns[fd].inbuf, (size_t)line_len);
        line[line_len] = '\0';

        remaining = conns[fd].inbuf_len - line_len;
        memmove(conns[fd].inbuf, conns[fd].inbuf + line_len, (size_t)remaining);
        conns[fd].inbuf_len = remaining;
        conns[fd].inbuf[remaining] = '\0';

        handle_command(fd, line);
        if (!conns[fd].active || conns[fd].close_after_flush) {
            return;
        }
    }
}

static void read_from_client(int fd) {
    while (conns[fd].active) {
        char temp[1024];
        ssize_t n = recv(fd, temp, sizeof(temp), 0);

        if (n > 0) {
            if (conns[fd].inbuf_len + (int)n >= MAX_INBUF) {
                (void)queue_line_fd(fd, "ERROR|input too long\n");
                conns[fd].close_after_flush = 1;
                return;
            }
            memcpy(conns[fd].inbuf + conns[fd].inbuf_len, temp, (size_t)n);
            conns[fd].inbuf_len += (int)n;
            conns[fd].inbuf[conns[fd].inbuf_len] = '\0';
            continue;
        }
        if (n == 0) {
            disconnect_client(fd);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        disconnect_client(fd);
        return;
    }

    if (conns[fd].active) {
        process_buffered_lines(fd);
        if (conns[fd].active) {
            (void)flush_connection(fd);
        }
    }
}

static void accept_new_clients(int listen_fd) {
    while (1) {
        int client_fd;
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("accept");
            break;
        }

        if (client_fd >= FD_SETSIZE) {
            close(client_fd);
            continue;
        }
        if (set_nonblocking(client_fd) < 0) {
            close(client_fd);
            continue;
        }

        reset_connection(client_fd);
        conns[client_fd].fd = client_fd;
        conns[client_fd].user_index = -1;
        conns[client_fd].active = 1;
        conns[client_fd].inbuf_len = 0;
        conns[client_fd].inbuf[0] = '\0';
        (void)queue_line_fd(client_fd, "WELCOME|NeedLove server ready\n");
    }
}

int main(void) {
    int listen_fd;
    struct sockaddr_in server_addr;
    int yes = 1;
    int i;
    fd_set readfds;
    fd_set writefds;

    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    for (i = 0; i < FD_SETSIZE; i++) {
        reset_connection(i);
    }
    for (i = 0; i < MAX_USERS; i++) {
        memset(&users[i], 0, sizeof(users[i]));
        users[i].conn_fd = -1;
        users[i].open_chat = -1;
    }
    memset(conversations, 0, sizeof(conversations));

    load_state();

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        free_all_conversations();
        return 1;
    }

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        free_all_conversations();
        return 1;
    }
    if (set_nonblocking(listen_fd) < 0) {
        perror("fcntl");
        close(listen_fd);
        free_all_conversations();
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        free_all_conversations();
        return 1;
    }

    if (listen(listen_fd, 10) < 0) {
        perror("listen");
        close(listen_fd);
        free_all_conversations();
        return 1;
    }

    printf("NeedLove server loaded state from %s and is listening on port %d\n", STATE_FILE, PORT);

    while (!stop_flag) {
        int max_fd = listen_fd;

        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(listen_fd, &readfds);

        for (i = 0; i < FD_SETSIZE; i++) {
            if (conns[i].active) {
                FD_SET(i, &readfds);
                if (conns[i].out_len > 0) {
                    FD_SET(i, &writefds);
                }
                if (i > max_fd) {
                    max_fd = i;
                }
            }
        }

        if (select(max_fd + 1, &readfds, &writefds, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (FD_ISSET(listen_fd, &readfds)) {
            accept_new_clients(listen_fd);
        }

        for (i = 0; i <= max_fd; i++) {
            if (i == listen_fd || !conns[i].active) {
                continue;
            }
            if (FD_ISSET(i, &readfds)) {
                read_from_client(i);
            }
        }

        for (i = 0; i <= max_fd; i++) {
            if (i == listen_fd || !conns[i].active) {
                continue;
            }
            if (FD_ISSET(i, &writefds)) {
                (void)flush_connection(i);
            }
        }
    }

    save_state();
    for (i = 0; i < FD_SETSIZE; i++) {
        if (conns[i].active) {
            close(i);
            reset_connection(i);
        }
    }
    close(listen_fd);
    free_all_conversations();
    return 0;
}
