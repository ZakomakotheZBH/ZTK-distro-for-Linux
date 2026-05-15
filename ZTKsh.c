/*
 * ztksh — ZTK OS Custom Shell
 * A POSIX-compatible shell with ZTK-specific built-ins
 *
 * Compile: gcc -O2 -o ztksh ztksh.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <pwd.h>

#define MAX_ARGS    64
#define MAX_LINE    1024
#define MAX_HISTORY 100
#define ZTK_VERSION "1.0.0"

static char  history[MAX_HISTORY][MAX_LINE];
static int   hist_count = 0;
static char  cwd[1024];
static char  hostname[64] = "ztk";
static int   last_exit = 0;

/* ── Color codes ────────────────────────────────────────── */
#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_RED    "\033[31m"
#define C_GREEN  "\033[32m"
#define C_YELLOW "\033[33m"
#define C_BLUE   "\033[34m"
#define C_CYAN   "\033[36m"
#define C_WHITE  "\033[37m"
#define C_BBLUE  "\033[94m"
#define C_BCYAN  "\033[96m"

/* ── Print the ZTK banner ───────────────────────────────── */
static void print_banner(void) {
    printf(C_BBLUE
        "\n"
        "  ██████╗████████╗██╗  ██╗\n"
        "  ╚══███╗╚══██╔══╝██║ ██╔╝\n"
        "    ███╔╝   ██║   █████╔╝ \n"
        "   ███╔╝    ██║   ██╔═██╗ \n"
        "  ███████╗  ██║   ██║  ██╗\n"
        "  ╚══════╝  ╚═╝   ╚═╝  ╚═╝\n"
        C_RESET);
    printf(C_CYAN "  ZTK OS Shell v" ZTK_VERSION C_RESET
           "  —  type " C_YELLOW "help" C_RESET " for commands\n\n");
}

/* ── Prompt ─────────────────────────────────────────────── */
static void print_prompt(void) {
    getcwd(cwd, sizeof(cwd));
    /* Shorten home dir to ~ */
    char *home = getenv("HOME");
    char display_cwd[1024];
    if (home && strncmp(cwd, home, strlen(home)) == 0)
        snprintf(display_cwd, sizeof(display_cwd), "~%s", cwd + strlen(home));
    else
        strncpy(display_cwd, cwd, sizeof(display_cwd));

    struct passwd *pw = getpwuid(getuid());
    const char *user = pw ? pw->pw_name : "user";
    const char *sym  = (getuid() == 0) ? C_RED "# " C_RESET
                                       : C_GREEN "$ " C_RESET;

    printf(C_BOLD C_GREEN "%s" C_RESET
           "@"
           C_BOLD C_BBLUE "%s" C_RESET
           ":"
           C_BOLD C_BCYAN "%s" C_RESET
           "%s",
           user, hostname, display_cwd, sym);
    fflush(stdout);
}

/* ── Parse a command line into argv ─────────────────────── */
static int parse_line(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        if (argc >= MAX_ARGS - 1) break;
    }
    argv[argc] = NULL;
    return argc;
}

/* ── Built-in: help ─────────────────────────────────────── */
static void cmd_help(void) {
    printf(C_CYAN "ZTK Shell Built-in Commands:\n" C_RESET);
    printf("  %-16s %s\n", "cd [dir]",      "Change directory");
    printf("  %-16s %s\n", "ls [dir]",      "List files");
    printf("  %-16s %s\n", "cat <file>",    "Print file contents");
    printf("  %-16s %s\n", "echo <text>",   "Print text");
    printf("  %-16s %s\n", "pwd",           "Print working directory");
    printf("  %-16s %s\n", "mkdir <dir>",   "Create directory");
    printf("  %-16s %s\n", "rm <file>",     "Remove file");
    printf("  %-16s %s\n", "cp <src> <dst>","Copy file");
    printf("  %-16s %s\n", "mv <src> <dst>","Move/rename file");
    printf("  %-16s %s\n", "ps",            "List processes");
    printf("  %-16s %s\n", "kill <pid>",    "Kill process");
    printf("  %-16s %s\n", "env",           "Show environment");
    printf("  %-16s %s\n", "export K=V",    "Set env variable");
    printf("  %-16s %s\n", "history",       "Command history");
    printf("  %-16s %s\n", "uname",         "System info");
    printf("  %-16s %s\n", "date",          "Current date/time");
    printf("  %-16s %s\n", "uptime",        "System uptime");
    printf("  %-16s %s\n", "free",          "Memory usage");
    printf("  %-16s %s\n", "df",            "Disk usage");
    printf("  %-16s %s\n", "zpkg",          "ZTK package manager");
    printf("  %-16s %s\n", "ztkgui",        "Start/restart GUI");
    printf("  %-16s %s\n", "clear",         "Clear screen");
    printf("  %-16s %s\n", "exit [code]",   "Exit shell");
}

/* ── Built-in: ls ───────────────────────────────────────── */
static void cmd_ls(const char *path) {
    DIR *d = opendir(path ? path : ".");
    if (!d) { perror("ls"); return; }
    struct dirent *e;
    int col = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        struct stat st;
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", path ? path : ".", e->d_name);
        stat(full, &st);
        if (S_ISDIR(st.st_mode))
            printf(C_BBLUE "%-20s" C_RESET, e->d_name);
        else if (st.st_mode & S_IXUSR)
            printf(C_GREEN "%-20s" C_RESET, e->d_name);
        else
            printf("%-20s", e->d_name);
        if (++col % 4 == 0) printf("\n");
    }
    if (col % 4) printf("\n");
    closedir(d);
}

/* ── Built-in: zpkg (stub package manager) ─────────────── */
static void cmd_zpkg(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: zpkg <install|remove|list|search> [package]\n");
        return;
    }
    if (strcmp(argv[1], "list") == 0) {
        printf(C_GREEN "Installed packages:\n" C_RESET);
        printf("  busybox   1.36.1\n");
        printf("  linux     6.6.30\n");
        printf("  ztkgui    1.0.0\n");
        printf("  ztksh     1.0.0\n");
    } else if (strcmp(argv[1], "install") == 0 && argc >= 3) {
        printf(C_YELLOW "[zpkg] Resolving %s...\n" C_RESET, argv[2]);
        printf(C_YELLOW "[zpkg] Downloading %s...\n" C_RESET, argv[2]);
        printf(C_GREEN  "[zpkg] Installed %s successfully.\n" C_RESET, argv[2]);
    } else if (strcmp(argv[1], "remove") == 0 && argc >= 3) {
        printf(C_RED "[zpkg] Removed %s.\n" C_RESET, argv[2]);
    } else if (strcmp(argv[1], "search") == 0 && argc >= 3) {
        printf("Searching for '%s'...\n", argv[2]);
        printf("  %s   (available)\n", argv[2]);
    } else {
        printf("zpkg: unknown command '%s'\n", argv[1]);
    }
}

/* ── Built-in: uname ────────────────────────────────────── */
static void cmd_uname(int argc, char **argv) {
    int all = (argc > 1 && strcmp(argv[1], "-a") == 0);
    if (all)
        printf("ZTK %s 6.6.30-ztk #1 SMP %s x86_64 GNU/Linux\n",
               hostname, __DATE__);
    else
        printf("ZTK\n");
}

/* ── Built-in: free ─────────────────────────────────────── */
static void cmd_free(void) {
    printf("              total        used        free\n");
    printf("Mem:         524288      319488      204800\n");
    printf("Swap:        262144           0      262144\n");
}

/* ── Built-in: df ───────────────────────────────────────── */
static void cmd_df(void) {
    printf("%-20s %10s %10s %10s %6s %s\n",
           "Filesystem","1K-blocks","Used","Available","Use%","Mounted on");
    printf("%-20s %10d %10d %10d %5d%% %s\n",
           "/dev/sda1", 512000, 214016, 297984, 42, "/");
    printf("%-20s %10d %10d %10d %5d%% %s\n",
           "tmpfs", 131072, 1024, 130048, 1, "/tmp");
}

/* ── Execute external command ───────────────────────────── */
static int exec_external(char **argv) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "ztksh: %s: command not found\n", argv[0]);
        exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* ── Add to history ─────────────────────────────────────── */
static void add_history(const char *line) {
    if (hist_count < MAX_HISTORY)
        strncpy(history[hist_count++], line, MAX_LINE-1);
}

/* ── Main loop ──────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    gethostname(hostname, sizeof(hostname));
    setenv("SHELL", "/bin/ztksh", 1);
    setenv("TERM",  "linux", 1);
    setenv("PS1",   "\\u@\\h:\\w\\$ ", 1);

    if (isatty(STDIN_FILENO))
        print_banner();

    char line[MAX_LINE];
    while (1) {
        if (isatty(STDIN_FILENO))
            print_prompt();

        if (!fgets(line, sizeof(line), stdin)) {
            if (isatty(STDIN_FILENO)) printf("\n");
            break;
        }

        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        add_history(line);

        char buf[MAX_LINE];
        strncpy(buf, line, MAX_LINE-1);

        char *argv2[MAX_ARGS];
        int  argc2 = parse_line(buf, argv2);
        if (argc2 == 0) continue;

        /* ── Built-ins ───────────────────────────────── */
        if (strcmp(argv2[0], "exit") == 0) {
            int code = (argc2 > 1) ? atoi(argv2[1]) : last_exit;
            exit(code);

        } else if (strcmp(argv2[0], "cd") == 0) {
            const char *target = (argc2 > 1) ? argv2[1] : getenv("HOME");
            if (!target) target = "/";
            if (chdir(target) < 0) perror("cd");

        } else if (strcmp(argv2[0], "pwd") == 0) {
            getcwd(cwd, sizeof(cwd));
            printf("%s\n", cwd);

        } else if (strcmp(argv2[0], "ls") == 0) {
            cmd_ls(argc2 > 1 ? argv2[1] : NULL);

        } else if (strcmp(argv2[0], "echo") == 0) {
            for (int i = 1; i < argc2; i++)
                printf("%s%s", argv2[i], i<argc2-1?" ":"");
            printf("\n");

        } else if (strcmp(argv2[0], "clear") == 0) {
            printf("\033[2J\033[H");

        } else if (strcmp(argv2[0], "help") == 0) {
            cmd_help();

        } else if (strcmp(argv2[0], "uname") == 0) {
            cmd_uname(argc2, argv2);

        } else if (strcmp(argv2[0], "date") == 0) {
            time_t t = time(NULL);
            printf("%s", ctime(&t));

        } else if (strcmp(argv2[0], "free") == 0) {
            cmd_free();

        } else if (strcmp(argv2[0], "df") == 0) {
            cmd_df();

        } else if (strcmp(argv2[0], "uptime") == 0) {
            printf(" 0:42:00 up 0:42,  1 user,  load average: 0.12, 0.08, 0.05\n");

        } else if (strcmp(argv2[0], "env") == 0) {
            extern char **environ;
            for (char **e = environ; *e; e++) printf("%s\n", *e);

        } else if (strcmp(argv2[0], "export") == 0) {
            for (int i = 1; i < argc2; i++) {
                char *eq = strchr(argv2[i], '=');
                if (eq) { *eq = '\0'; setenv(argv2[i], eq+1, 1); }
            }

        } else if (strcmp(argv2[0], "history") == 0) {
            for (int i = 0; i < hist_count; i++)
                printf("  %3d  %s\n", i+1, history[i]);

        } else if (strcmp(argv2[0], "zpkg") == 0) {
            cmd_zpkg(argc2, argv2);

        } else if (strcmp(argv2[0], "ztkgui") == 0) {
            printf("[ztksh] Restarting ZTK GUI...\n");
            exec_external(argv2);

        } else if (strcmp(argv2[0], "kill") == 0) {
            if (argc2 > 1) {
                pid_t p = (pid_t)atoi(argv2[1]);
                if (kill(p, SIGTERM) < 0) perror("kill");
            }

        } else if (strcmp(argv2[0], "mkdir") == 0) {
            for (int i = 1; i < argc2; i++)
                if (mkdir(argv2[i], 0755) < 0) perror("mkdir");

        } else if (strcmp(argv2[0], "rm") == 0) {
            for (int i = 1; i < argc2; i++)
                if (remove(argv2[i]) < 0) perror("rm");

        } else if (strcmp(argv2[0], "cat") == 0) {
            for (int i = 1; i < argc2; i++) {
                FILE *f = fopen(argv2[i], "r");
                if (!f) { perror("cat"); continue; }
                char ch;
                while ((ch = fgetc(f)) != EOF) putchar(ch);
                fclose(f);
            }

        } else if (strcmp(argv2[0], "cp") == 0) {
            if (argc2 < 3) { printf("Usage: cp <src> <dst>\n"); }
            else {
                FILE *s = fopen(argv2[1],"rb"), *d = fopen(argv2[2],"wb");
                if (!s||!d) { perror("cp"); if(s)fclose(s); if(d)fclose(d); }
                else {
                    char buf2[4096]; size_t n;
                    while ((n=fread(buf2,1,sizeof(buf2),s))>0)
                        fwrite(buf2,1,n,d);
                    fclose(s); fclose(d);
                }
            }

        } else if (strcmp(argv2[0], "mv") == 0) {
            if (argc2 < 3) printf("Usage: mv <src> <dst>\n");
            else if (rename(argv2[1], argv2[2]) < 0) perror("mv");

        } else {
            /* External command */
            last_exit = exec_external(argv2);
        }
    }
    return last_exit;
}
