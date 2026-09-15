#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef PORT
#define PORT 4242
#endif

#define DEFAULT_HOST "127.0.0.1"
#define MAX_LINE 2048
#define MAX_MATCHES 100
#define MAX_CANDIDATES 10

typedef struct {
    char username[64];
    int age;
    char gender[32];
    char preference[32];
    char bio[160];
} Candidate;

typedef struct {
    char username[64];
    int online;
    int unread;
} MatchItem;

static int sock_fd = -1;
static char my_username[64] = "";
static int total_unread = 0;

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

static int send_all(const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sock_fd, buf + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int send_line(const char *line) {
    return send_all(line, strlen(line));
}

static int recv_line(char *out, size_t out_size) {
    static char buf[8192];
    static int buf_len = 0;
    char *newline;

    while (1) {
        newline = memchr(buf, '\n', (size_t)buf_len);
        if (newline != NULL) {
            int line_len = (int)(newline - buf) + 1;
            int remaining;
            if ((size_t)line_len >= out_size) {
                fprintf(stderr, "incoming line too long\n");
                return -1;
            }
            memcpy(out, buf, (size_t)line_len);
            out[line_len] = '\0';
            remaining = buf_len - line_len;
            memmove(buf, buf + line_len, (size_t)remaining);
            buf_len = remaining;
            return 1;
        }

        if (buf_len >= (int)sizeof(buf)) {
            fprintf(stderr, "incoming buffer overflow\n");
            return -1;
        }

        ssize_t n = recv(sock_fd, buf + buf_len, sizeof(buf) - (size_t)buf_len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        buf_len += (int)n;
    }
}

static void print_divider(void) {
    printf("--------------------------------------------------------------\n");
}

static void print_logo(const char *username) {
    printf("\n==============================================================\n");
    printf(" __        __   _   _               _ _                       \n");
    printf(" \\ \\      / /__| \\ | | ___  ___  __| | |    _____   _____    \n");
    printf("  \\ \\ /\\ / / _ \\  \\| |/ _ \\/ _ \\/ _` | |   / _ \\ \\ / / _ \\   \n");
    printf("   \\ V  V /  __/ |\\  |  __/  __/ (_| | |__| (_) \\ V /  __/   \n");
    printf("    \\_/\\_/ \\___|_| \\_|\\___|\\___|\\__,_|_____\\___/ \\_/ \\___|   \n\n");
    printf("            ♥ WELCOME TO THE DATING APP, %s ♥            \n", username);
    printf("==============================================================\n\n");
}

static int is_async_line(const char *line) {
    return strncmp(line, "MATCH_NEW|", 10) == 0 ||
           strncmp(line, "NOTIFY_UNREAD|", 14) == 0 ||
           strncmp(line, "UNMATCHED_BY|", 13) == 0 ||
           strncmp(line, "CHAT_APPEND|", 12) == 0;
}

static void handle_async_line(const char *line, const char *current_chat, int *chat_should_close, const char *prompt) {
    char copy[MAX_LINE];
    char *cmd;
    char *a;
    char *b;

    safe_copy(copy, line, sizeof(copy));
    trim_newline(copy);
    cmd = strtok(copy, "|");
    a = strtok(NULL, "|");
    b = strtok(NULL, "");

    if (cmd == NULL) {
        return;
    }

    if (strcmp(cmd, "MATCH_NEW") == 0 && a != NULL) {
        printf("\n[♥] New match with %s\n", a);
    } else if (strcmp(cmd, "NOTIFY_UNREAD") == 0 && a != NULL) {
        total_unread = atoi(a);
        if (current_chat == NULL && total_unread > 0) {
            printf("\n[%d New Messages]\n", total_unread);
        }
    } else if (strcmp(cmd, "UNMATCHED_BY") == 0 && a != NULL) {
        printf("\n[!] %s unmatched you.\n", a);
        if (current_chat != NULL && strcmp(current_chat, a) == 0 && chat_should_close != NULL) {
            *chat_should_close = 1;
        }
    } else if (strcmp(cmd, "CHAT_APPEND") == 0 && a != NULL && b != NULL) {
        if (current_chat != NULL && strcmp(current_chat, a) == 0) {
            printf("%s: %s\n", a, b);
        }
    } else {
        printf("\n%s", line);
        if (line[strlen(line) - 1] != '\n') {
            printf("\n");
        }
    }

    if (prompt != NULL) {
        printf("%s", prompt);
        fflush(stdout);
    }
}

static int wait_non_async_line(char *out, size_t out_size) {
    while (1) {
        if (recv_line(out, out_size) <= 0) {
            fprintf(stderr, "Server disconnected.\n");
            return -1;
        }
        if (is_async_line(out)) {
            handle_async_line(out, NULL, NULL, NULL);
            continue;
        }
        return 1;
    }
}

static int prompt_line(const char *prompt, char *out, size_t out_size) {
    fd_set rfds;

    while (1) {
        printf("%s", prompt);
        fflush(stdout);

        FD_ZERO(&rfds);
        FD_SET(sock_fd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);

        if (select((sock_fd > STDIN_FILENO ? sock_fd : STDIN_FILENO) + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            return 0;
        }

        if (FD_ISSET(sock_fd, &rfds)) {
            char line[MAX_LINE];
            if (recv_line(line, sizeof(line)) <= 0) {
                fprintf(stderr, "Server disconnected.\n");
                return 0;
            }
            if (is_async_line(line)) {
                handle_async_line(line, NULL, NULL, prompt);
            } else {
                printf("\n%s", line);
                if (line[strlen(line) - 1] != '\n') {
                    printf("\n");
                }
                printf("%s", prompt);
                fflush(stdout);
            }
            continue;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (fgets(out, (int)out_size, stdin) == NULL) {
                return 0;
            }
            trim_newline(out);
            return 1;
        }
    }
}

static void print_profile_payload(const char *profile_line) {
    char copy[MAX_LINE];
    char *cmd;
    char *username;
    char *age;
    char *gender;
    char *pref;
    char *bio;

    safe_copy(copy, profile_line, sizeof(copy));
    trim_newline(copy);
    cmd = strtok(copy, "|");
    username = strtok(NULL, "|");
    age = strtok(NULL, "|");
    gender = strtok(NULL, "|");
    pref = strtok(NULL, "|");
    bio = strtok(NULL, "");

    if (cmd == NULL || strcmp(cmd, "PROFILE") != 0) {
        printf("%s", profile_line);
        return;
    }

    print_divider();
    printf("My profile\n");
    print_divider();
    printf("Username   : %s\n", username ? username : "");
    printf("Age        : %s\n", age ? age : "0");
    printf("Gender     : %s\n", gender ? gender : "");
    printf("Preference : %s\n", pref ? pref : "");
    printf("Bio        : %s\n", bio ? bio : "");
    print_divider();
}

static int parse_match_line(const char *line, MatchItem *item) {
    char copy[MAX_LINE];
    char *cmd;
    char *name;
    char *status;
    char *unread;

    safe_copy(copy, line, sizeof(copy));
    trim_newline(copy);
    cmd = strtok(copy, "|");
    name = strtok(NULL, "|");
    status = strtok(NULL, "|");
    unread = strtok(NULL, "|");
    if (cmd == NULL || name == NULL || status == NULL || unread == NULL) {
        return 0;
    }
    if (strcmp(cmd, "MATCH") != 0) {
        return 0;
    }
    safe_copy(item->username, name, sizeof(item->username));
    item->online = strcmp(status, "online") == 0;
    item->unread = atoi(unread);
    return 1;
}

static int fetch_match_list(MatchItem items[MAX_MATCHES]) {
    char line[MAX_LINE];
    int count = 0;

    if (send_line("MATCH_LIST\n") < 0) {
        fprintf(stderr, "Failed to request match list.\n");
        return -1;
    }

    while (1) {
        if (recv_line(line, sizeof(line)) <= 0) {
            fprintf(stderr, "Server disconnected.\n");
            return -1;
        }
        if (is_async_line(line)) {
            handle_async_line(line, NULL, NULL, NULL);
            continue;
        }
        if (strncmp(line, "MATCH_LIST_BEGIN", 16) == 0) {
            continue;
        }
        if (strncmp(line, "MATCH_LIST_END", 14) == 0) {
            break;
        }
        if (count < MAX_MATCHES && parse_match_line(line, &items[count])) {
            count++;
        }
    }

    return count;
}

static void edit_profile(void) {
    char age[32];
    char gender[32];
    char preference[32];
    char bio[160];
    char line[MAX_LINE];

    print_divider();
    printf("Edit profile\n");
    print_divider();
    printf("Allowed genders: man, woman, nonbinary, other\n");
    printf("Allowed preferences: any, man, woman, nonbinary, other\n");

    if (!prompt_line("Age: ", age, sizeof(age))) {
        return;
    }
    if (!prompt_line("Gender: ", gender, sizeof(gender))) {
        return;
    }
    if (!prompt_line("Preference: ", preference, sizeof(preference))) {
        return;
    }
    if (!prompt_line("Short bio: ", bio, sizeof(bio))) {
        return;
    }

    snprintf(line, sizeof(line), "PROFILE_SET|%s|%s|%s|%s\n", age, gender, preference, bio);
    if (send_line(line) < 0) {
        fprintf(stderr, "Failed to send PROFILE_SET.\n");
        return;
    }
    if (wait_non_async_line(line, sizeof(line)) < 0) {
        return;
    }
    printf("%s", line);
}

static int fetch_candidates(Candidate items[MAX_CANDIDATES]) {
    char line[MAX_LINE];
    int count = 0;

    if (send_line("REQUEST_CANDIDATES|10\n") < 0) {
        fprintf(stderr, "Failed to request candidates.\n");
        return -1;
    }

    while (1) {
        if (recv_line(line, sizeof(line)) <= 0) {
            fprintf(stderr, "Server disconnected.\n");
            return -1;
        }
        if (is_async_line(line)) {
            handle_async_line(line, NULL, NULL, NULL);
            continue;
        }
        if (strncmp(line, "CANDIDATE_BEGIN", 15) == 0) {
            continue;
        }
        if (strncmp(line, "CANDIDATE_END", 13) == 0) {
            break;
        }
        if (strncmp(line, "CANDIDATE|", 10) == 0 && count < MAX_CANDIDATES) {
            char copy[MAX_LINE];
            char *cmd;
            char *username;
            char *age;
            char *gender;
            char *pref;
            char *bio;

            safe_copy(copy, line, sizeof(copy));
            trim_newline(copy);
            cmd = strtok(copy, "|");
            username = strtok(NULL, "|");
            age = strtok(NULL, "|");
            gender = strtok(NULL, "|");
            pref = strtok(NULL, "|");
            bio = strtok(NULL, "");

            if (cmd != NULL && username != NULL && age != NULL && gender != NULL && pref != NULL && bio != NULL) {
                safe_copy(items[count].username, username, sizeof(items[count].username));
                items[count].age = atoi(age);
                safe_copy(items[count].gender, gender, sizeof(items[count].gender));
                safe_copy(items[count].preference, pref, sizeof(items[count].preference));
                safe_copy(items[count].bio, bio, sizeof(items[count].bio));
                count++;
            }
        }
    }

    return count;
}

static void swipe_mode(void) {
    Candidate items[MAX_CANDIDATES];
    char line[MAX_LINE];
    char batch[MAX_LINE];
    int count;
    int i;
    int first = 1;

    count = fetch_candidates(items);
    if (count < 0) {
        return;
    }
    if (count == 0) {
        printf("No candidates right now.\n");
        return;
    }

    strcpy(batch, "SWIPE_BATCH|");

    for (i = 0; i < count; i++) {
        char choice[16];

        printf("\n");
        print_divider();
        printf("Candidate %d of %d\n", i + 1, count);
        print_divider();
        printf("Username   : %s\n", items[i].username);
        printf("Age        : %d\n", items[i].age);
        printf("Gender     : %s\n", items[i].gender);
        printf("Preference : %s\n", items[i].preference);
        printf("Bio        : %s\n", items[i].bio);
        print_divider();

        while (1) {
            if (!prompt_line("Swipe [l]ike / [d]islike / [q]uit batch: ", choice, sizeof(choice))) {
                return;
            }
            if (choice[0] == 'q' || choice[0] == 'Q') {
                i = count;
                break;
            }
            if (choice[0] == 'l' || choice[0] == 'L' || choice[0] == 'd' || choice[0] == 'D') {
                break;
            }
            printf("Please type l, d, or q.\n");
        }

        if (i >= count) {
            break;
        }

        if (!first) {
            strncat(batch, ",", sizeof(batch) - strlen(batch) - 1);
        }
        strncat(batch, items[i].username, sizeof(batch) - strlen(batch) - 1);
        strncat(batch, ":", sizeof(batch) - strlen(batch) - 1);
        if (choice[0] == 'l' || choice[0] == 'L') {
            strncat(batch, "L", sizeof(batch) - strlen(batch) - 1);
        } else {
            strncat(batch, "D", sizeof(batch) - strlen(batch) - 1);
        }
        first = 0;
    }

    strncat(batch, "\n", sizeof(batch) - strlen(batch) - 1);
    if (send_line(batch) < 0) {
        fprintf(stderr, "Failed to send swipe batch.\n");
        return;
    }
    if (wait_non_async_line(line, sizeof(line)) < 0) {
        return;
    }
    printf("%s", line);
}

static void view_matches(void) {
    MatchItem items[MAX_MATCHES];
    int count;
    int i;

    count = fetch_match_list(items);
    if (count < 0) {
        return;
    }

    printf("\n");
    print_divider();
    printf("Matches\n");
    print_divider();
    if (count == 0) {
        printf("No matches yet.\n");
    } else {
        for (i = 0; i < count; i++) {
            printf("- %s (%s, unread %d)\n",
                   items[i].username,
                   items[i].online ? "online" : "offline",
                   items[i].unread);
        }
    }
    print_divider();
}

static void show_profile(void) {
    char line[MAX_LINE];

    if (send_line("PROFILE_GET\n") < 0) {
        fprintf(stderr, "Failed to send PROFILE_GET.\n");
        return;
    }

    while (1) {
        if (recv_line(line, sizeof(line)) <= 0) {
            fprintf(stderr, "Server disconnected.\n");
            return;
        }
        if (is_async_line(line)) {
            handle_async_line(line, NULL, NULL, NULL);
            continue;
        }
        if (strncmp(line, "PROFILE|", 8) == 0) {
            print_profile_payload(line);
            return;
        }
        printf("%s", line);
        return;
    }
}

static int open_chat_and_print_history(const char *partner) {
    char line[MAX_LINE];
    char cmd[MAX_LINE];
    int got_begin = 0;

    snprintf(cmd, sizeof(cmd), "CHAT_OPEN|%s\n", partner);
    if (send_line(cmd) < 0) {
        fprintf(stderr, "Failed to open chat.\n");
        return 0;
    }
    if (wait_non_async_line(line, sizeof(line)) < 0) {
        return 0;
    }
    if (strncmp(line, "CHAT_OPEN_OK|", 13) != 0) {
        printf("%s", line);
        return 0;
    }

    printf("\n");
    print_divider();
    printf("Chat with %s\n", partner);
    print_divider();
    printf("Type your message and press Enter. Type /back to return.\n");
    print_divider();

    while (1) {
        char copy[MAX_LINE];
        char *tok;
        char *sender;
        char *msg;

        if (recv_line(line, sizeof(line)) <= 0) {
            fprintf(stderr, "Server disconnected.\n");
            return 0;
        }
        if (is_async_line(line)) {
            handle_async_line(line, partner, NULL, NULL);
            continue;
        }
        if (strncmp(line, "CHAT_HISTORY_BEGIN", 18) == 0) {
            got_begin = 1;
            continue;
        }
        if (strncmp(line, "CHAT_HISTORY_END", 16) == 0) {
            break;
        }
        if (!got_begin || strncmp(line, "CHAT_MSG|", 9) != 0) {
            continue;
        }

        safe_copy(copy, line, sizeof(copy));
        trim_newline(copy);
        tok = strtok(copy, "|");
        sender = strtok(NULL, "|");
        msg = strtok(NULL, "");
        if (tok != NULL && sender != NULL && msg != NULL) {
            if (strcmp(sender, my_username) == 0) {
                printf("You: %s\n", msg);
            } else {
                printf("%s: %s\n", sender, msg);
            }
        }
    }

    print_divider();
    return 1;
}

static void clear_prompt_line(void) {
    printf("\r\33[2K");
    fflush(stdout);
}

static void erase_last_input_line(void) {
    printf("\33[A\r\33[2K");
    fflush(stdout);
}

static void chat_room(const char *partner) {
    fd_set rfds;
    char line[MAX_LINE];
    char input[900];
    int chat_should_close = 0;
    int need_prompt = 1;
    const char *prompt = "you> ";

    if (!open_chat_and_print_history(partner)) {
        return;
    }

    while (!chat_should_close) {
        if (need_prompt) {
            printf("%s", prompt);
            fflush(stdout);
            need_prompt = 0;
        }

        FD_ZERO(&rfds);
        FD_SET(sock_fd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);

        if (select((sock_fd > STDIN_FILENO ? sock_fd : STDIN_FILENO) + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (FD_ISSET(sock_fd, &rfds)) {
            if (recv_line(line, sizeof(line)) <= 0) {
                fprintf(stderr, "Server disconnected.\n");
                exit(1);
            }

            clear_prompt_line();

            if (strncmp(line, "CHAT_SELF|", 10) == 0) {
                char *msg = line + 10;
                msg[strcspn(msg, "\n")] = '\0';
                printf("You: %s\n", msg);
                printf("%s", prompt);
                fflush(stdout);
                need_prompt = 0;
                continue;
            }

            if (is_async_line(line)) {
                handle_async_line(line, partner, &chat_should_close, chat_should_close ? NULL : prompt);
                need_prompt = 0;
                continue;
            }

            if (strncmp(line, "ERROR|", 6) == 0) {
                printf("%s", line);
                if (line[strlen(line) - 1] != '\n') {
                    printf("\n");
                }
                if (!chat_should_close) {
                    printf("%s", prompt);
                    fflush(stdout);
                }
                need_prompt = 0;
                continue;
            }

            if (strncmp(line, "BYE", 3) == 0) {
                printf("Server closed the connection.\n");
                exit(0);
            }

            printf("%s", line);
            if (line[strlen(line) - 1] != '\n') {
                printf("\n");
            }
            if (!chat_should_close) {
                printf("%s", prompt);
                fflush(stdout);
            }
            need_prompt = 0;
            continue;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char cmd[MAX_LINE];

            if (fgets(input, sizeof(input), stdin) == NULL) {
                break;
            }
            trim_newline(input);

            if (strcmp(input, "/back") == 0) {
                erase_last_input_line();
                if (send_line("CHAT_CLOSE\n") >= 0 && wait_non_async_line(line, sizeof(line)) >= 0) {
                    if (strncmp(line, "CHAT_CLOSE_OK", 13) != 0) {
                        printf("%s", line);
                        if (line[strlen(line) - 1] != '\n') {
                            printf("\n");
                        }
                    }
                }
                break;
            }

            if (input[0] == '\0') {
                erase_last_input_line();
                need_prompt = 1;
                continue;
            }

            snprintf(cmd, sizeof(cmd), "CHAT_SEND|%s|%s\n", partner, input);
            if (send_line(cmd) < 0) {
                fprintf(stderr, "Failed to send message.\n");
                break;
            }

            erase_last_input_line();
            need_prompt = 1;
        }
    }
}

static void chat_menu(void) {
    MatchItem items[MAX_MATCHES];
    char line[64];
    int count;
    int i;
    int choice;

    while (1) {
        count = fetch_match_list(items);
        if (count < 0) {
            return;
        }
        if (count == 0) {
            printf("\nNo matches to chat with yet.\n");
            return;
        }

        printf("\n");
        print_divider();
        printf("Chat inbox");
        if (total_unread > 0) {
            printf(" [%d New Messages]", total_unread);
        }
        printf("\n");
        print_divider();
        printf("0. Back\n");
        for (i = 0; i < count; i++) {
            printf("%d. %s (%s, unread %d)\n",
                   i + 1,
                   items[i].username,
                   items[i].online ? "online" : "offline",
                   items[i].unread);
        }
        print_divider();

        if (!prompt_line("Open chat number: ", line, sizeof(line))) {
            return;
        }
        choice = atoi(line);
        if (choice == 0) {
            return;
        }
        if (choice < 1 || choice > count) {
            printf("Please choose a valid chat.\n");
            continue;
        }

        chat_room(items[choice - 1].username);
    }
}

static void unmatch_menu(void) {
    MatchItem items[MAX_MATCHES];
    char line[64];
    int count;
    int i;
    int choice;

    while (1) {
        count = fetch_match_list(items);
        if (count < 0) {
            return;
        }
        if (count == 0) {
            printf("\nNo matches to remove.\n");
            return;
        }

        printf("\n");
        print_divider();
        printf("Unmatch / Block\n");
        print_divider();
        printf("0. Back\n");
        for (i = 0; i < count; i++) {
            printf("%d. %s (%s, unread %d)\n",
                   i + 1,
                   items[i].username,
                   items[i].online ? "online" : "offline",
                   items[i].unread);
        }
        print_divider();

        if (!prompt_line("Choose a user to unmatch: ", line, sizeof(line))) {
            return;
        }
        choice = atoi(line);
        if (choice == 0) {
            return;
        }
        if (choice < 1 || choice > count) {
            printf("Please choose a valid user.\n");
            continue;
        }

        if (!prompt_line("Type YES to confirm: ", line, sizeof(line))) {
            return;
        }
        if (strcmp(line, "YES") != 0) {
            printf("Cancelled.\n");
            continue;
        }

        {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "UNMATCH|%s\n", items[choice - 1].username);
            if (send_line(cmd) < 0) {
                fprintf(stderr, "Failed to send UNMATCH.\n");
                return;
            }
        }
        if (wait_non_async_line(line, sizeof(line)) < 0) {
            return;
        }
        printf("%s", line);
        return;
    }
}

static void show_menu(void) {
    print_divider();
    printf("1. Edit profile\n");
    printf("2. Swipe\n");
    printf("3. View matches\n");
    if (total_unread > 0) {
        printf("4. Chat [%d New Messages]\n", total_unread);
    } else {
        printf("4. Chat\n");
    }
    printf("5. Unmatch / Block\n");
    printf("6. Show my profile\n");
    printf("7. Quit\n");
    print_divider();
}

int main(int argc, char **argv) {
    const char *host = DEFAULT_HOST;
    struct sockaddr_in server_addr;
    struct hostent *hp;
    char line[MAX_LINE];

    if (argc >= 2) {
        host = argv[1];
    }

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    hp = gethostbyname(host);
    if (hp == NULL) {
        fprintf(stderr, "Unknown host: %s\n", host);
        close(sock_fd);
        return 1;
    }
    memcpy(&server_addr.sin_addr, hp->h_addr_list[0], (size_t)hp->h_length);

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        return 1;
    }

    if (recv_line(line, sizeof(line)) <= 0) {
        fprintf(stderr, "Could not read welcome message.\n");
        close(sock_fd);
        return 1;
    }
    printf("%s", line);

    if (!prompt_line("Username: ", my_username, sizeof(my_username))) {
        close(sock_fd);
        return 1;
    }

    snprintf(line, sizeof(line), "LOGIN|%s\n", my_username);
    if (send_line(line) < 0) {
        fprintf(stderr, "Failed to send LOGIN.\n");
        close(sock_fd);
        return 1;
    }

    if (wait_non_async_line(line, sizeof(line)) < 0) {
        close(sock_fd);
        return 1;
    }
    if (strncmp(line, "LOGIN_OK", 8) != 0) {
        printf("%s", line);
        close(sock_fd);
        return 1;
    }

    while (1) {
        if (wait_non_async_line(line, sizeof(line)) < 0) {
            close(sock_fd);
            return 1;
        }
        if (strncmp(line, "PROFILE|", 8) == 0) {
            break;
        }
        printf("%s", line);
    }

    print_logo(my_username);
    print_profile_payload(line);

    while (1) {
        show_menu();
        if (!prompt_line("Choose 1-7: ", line, sizeof(line))) {
            break;
        }

        switch (atoi(line)) {
            case 1:
                edit_profile();
                break;
            case 2:
                swipe_mode();
                break;
            case 3:
                view_matches();
                break;
            case 4:
                chat_menu();
                break;
            case 5:
                unmatch_menu();
                break;
            case 6:
                show_profile();
                break;
            case 7:
                if (send_line("QUIT\n") >= 0 && wait_non_async_line(line, sizeof(line)) >= 0) {
                    printf("%s", line);
                }
                close(sock_fd);
                return 0;
            default:
                printf("Please choose a number from 1 to 7.\n");
        }
    }

    close(sock_fd);
    return 0;
}
