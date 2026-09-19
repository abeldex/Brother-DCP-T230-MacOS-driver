/*
 * Native Brother DCP-T230 CUPS Filter (C Implementation)
 * Zero-dependency universal binary for macOS (x86_64 & arm64).
 *
 * Automatically converts CUPS Raster (3SaR) to PWG Raster via rastertopwg,
 * patches the PWG page header for Brother quirks, and wraps in Brother PJL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <ctype.h>

#define PWG_HEADER_SIZE 1796
#define UEL "\x1b%-12345X"

typedef struct {
    char paper[32];
    int borderless;
    int margin;
    char rendermode[16];
    char quality[16];
    char duplex[16];
    char mediatype[32];
    char sourcetray[16];
    char username[80];
    char jobname[80];
} PrintOptions;

static void clean_pjl_string(char *dest, const char *src, size_t maxlen) {
    size_t i = 0;
    while (*src && i < maxlen - 1) {
        unsigned char c = (unsigned char)*src;
        if (c >= 0x20 && c < 0x7F && c != '"') {
            dest[i++] = c;
        }
        src++;
    }
    dest[i] = '\0';
}

static void parse_options(const char *opt_str, const char *user, const char *title, PrintOptions *opts) {
    memset(opts, 0, sizeof(PrintOptions));
    strcpy(opts->paper, "LETTER");
    opts->borderless = 0;
    opts->margin = 300;
    strcpy(opts->rendermode, "COLOR");
    strcpy(opts->quality, "NORMAL");
    strcpy(opts->duplex, "OFF");
    strcpy(opts->mediatype, "REGULAR");
    strcpy(opts->sourcetray, "AUTO");

    clean_pjl_string(opts->username, user && *user ? user : "guest", sizeof(opts->username));
    clean_pjl_string(opts->jobname, title && *title ? title : "job", sizeof(opts->jobname));

    if (!opt_str || !*opt_str) return;

    char *buf = strdup(opt_str);
    char *token = strtok(buf, " \t\r\n");
    while (token) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            const char *key = token;
            const char *val = eq + 1;

            if (strcmp(key, "PageSize") == 0 || strcmp(key, "media") == 0) {
                if (strcasecmp(val, "A4") == 0) strcpy(opts->paper, "A4");
                else if (strcasecmp(val, "BrA4_B") == 0) { strcpy(opts->paper, "A4"); opts->borderless = 1; }
                else if (strcasecmp(val, "Letter") == 0) strcpy(opts->paper, "LETTER");
                else if (strcasecmp(val, "BrLetter_B") == 0) { strcpy(opts->paper, "LETTER"); opts->borderless = 1; }
                else if (strcasecmp(val, "Legal") == 0) strcpy(opts->paper, "LEGAL");
                else if (strcasecmp(val, "Executive") == 0) strcpy(opts->paper, "EXECUTIVE");
                else if (strcasecmp(val, "A5") == 0) strcpy(opts->paper, "A5");
                else if (strcasecmp(val, "A6") == 0) strcpy(opts->paper, "A6");
                else if (strcasecmp(val, "BrA6_B") == 0) { strcpy(opts->paper, "A6"); opts->borderless = 1; }
                else if (strcasecmp(val, "B5") == 0) strcpy(opts->paper, "JISB5");
                else if (strcasecmp(val, "JISB6") == 0) strcpy(opts->paper, "JISB6");
                else if (strcasecmp(val, "BrPostC4x6_S") == 0) strcpy(opts->paper, "P4X6");
                else if (strcasecmp(val, "BrPostC4x6_B") == 0) { strcpy(opts->paper, "P4X6"); opts->borderless = 1; }
            } else if (strcmp(key, "BRMonoColor") == 0) {
                if (strcasecmp(val, "Mono") == 0) strcpy(opts->rendermode, "GRAYSCALE");
            } else if (strcmp(key, "BRResolution") == 0) {
                if (strcasecmp(val, "Draft") == 0) strcpy(opts->quality, "DRAFT");
                else if (strcasecmp(val, "Fine") == 0) strcpy(opts->quality, "HIGH");
            } else if (strcmp(key, "Duplex") == 0) {
                if (strcasecmp(val, "None") != 0 && strcasecmp(val, "false") != 0) strcpy(opts->duplex, "ON");
            } else if (strcmp(key, "BRMediaType") == 0) {
                if (strcasecmp(val, "Glossy") == 0) strcpy(opts->mediatype, "GLOSSY");
                else if (strcasecmp(val, "Inkjet") == 0) strcpy(opts->mediatype, "INKJET");
            } else if (strcmp(key, "BRInputSlot") == 0) {
                if (strcasecmp(val, "Tray1") == 0) strcpy(opts->sourcetray, "TRAY1");
            }
        }
        token = strtok(NULL, " \t\r\n");
    }
    free(buf);

    opts->margin = opts->borderless ? 0 : 300;
}

static const char *find_rastertopwg(void) {
    static const char *paths[] = {
        "/usr/libexec/cups/filter/rastertopwg",
        "/usr/lib/cups/filter/rastertopwg",
        "/usr/local/libexec/cups/filter/rastertopwg",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], X_OK) == 0) return paths[i];
    }
    return NULL;
}

static void patch_pwg_header(unsigned char *hdr) {
    /* MediaType (offset 128) -> "auto" */
    memset(hdr + 128, 0, 64);
    memcpy(hdr + 128, "auto", 4);

    /* ImagingBoundingBox (offset 284..299) -> 0 */
    memset(hdr + 284, 0, 16);

    /* NumCopies (offset 340) -> 0 */
    memset(hdr + 340, 0, 4);

    /* PageSize (offset 352, 356) -> recompute from width/height & dpi */
    uint32_t hw_x = ntohl(*(uint32_t*)(hdr + 276));
    uint32_t hw_y = ntohl(*(uint32_t*)(hdr + 280));
    uint32_t w_px = ntohl(*(uint32_t*)(hdr + 372));
    uint32_t h_px = ntohl(*(uint32_t*)(hdr + 376));
    if (hw_x && hw_y) {
        uint32_t ps_w = (uint32_t)((w_px * 72.0 / hw_x) + 0.5);
        uint32_t ps_h = (uint32_t)((h_px * 72.0 / hw_y) + 0.5);
        *(uint32_t*)(hdr + 352) = htonl(ps_w);
        *(uint32_t*)(hdr + 356) = htonl(ps_h);
    }

    /* TraySwitch (offset 364) -> 0 */
    memset(hdr + 364, 0, 4);

    /* cupsInteger[0..7] (offset 452..483) -> 0 */
    memset(hdr + 452, 0, 32);

    /* cupsPageSizeName (offset 1732..1795) -> empty */
    memset(hdr + 1732, 0, 64);
}

static int read_exact(int fd, void *buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t n = read(fd, (char*)buf + total, count - total);
        if (n <= 0) return (total == 0) ? 0 : -1;
        total += n;
    }
    return (int)total;
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        fprintf(stderr, "ERROR: Brother DCP-T230 filter invoked with invalid arguments.\n");
        fprintf(stderr, "Usage: brother_dcpt230_pjl job-id user title copies options [file]\n");
        return 1;
    }

    const char *user = argv[2];
    const char *title = argv[3];
    const char *options = argv[5];
    const char *filename = (argc >= 7) ? argv[6] : NULL;

    PrintOptions opts;
    parse_options(options, user, title, &opts);

    int src_fd = STDIN_FILENO;
    if (filename && strcmp(filename, "-") != 0) {
        src_fd = open(filename, O_RDONLY);
        if (src_fd < 0) {
            fprintf(stderr, "ERROR: Cannot open input file '%s'\n", filename);
            return 1;
        }
    }

    /* Inspect lead bytes to check format */
    unsigned char lead[16];
    int lead_read = read_exact(src_fd, lead, 16);
    if (lead_read < 4) {
        fprintf(stderr, "ERROR: Empty or truncated raster stream.\n");
        if (src_fd != STDIN_FILENO) close(src_fd);
        return 1;
    }

    pid_t filter_pid = -1;
    pid_t feeder_pid = -1;

    /* Check if input is CUPS raster (e.g. 3SaR, RaSt, etc.) vs PWG Raster */
    int is_pwg = (lead_read >= 14 && memcmp(lead, "RaS2PwgRaster", 13) == 0);

    if (!is_pwg) {
        fprintf(stderr, "INFO: Converting CUPS raster to PWG raster via rastertopwg\n");
        const char *rastertopwg = find_rastertopwg();
        if (!rastertopwg) {
            fprintf(stderr, "ERROR: rastertopwg not found on system.\n");
            if (src_fd != STDIN_FILENO) close(src_fd);
            return 1;
        }

        int to_filter[2];
        int from_filter[2];
        if (pipe(to_filter) < 0 || pipe(from_filter) < 0) {
            perror("pipe");
            return 1;
        }

        filter_pid = fork();
        if (filter_pid == 0) {
            /* Child filter: run rastertopwg */
            dup2(to_filter[0], STDIN_FILENO);
            dup2(from_filter[1], STDOUT_FILENO);
            close(to_filter[0]); close(to_filter[1]);
            close(from_filter[0]); close(from_filter[1]);
            if (src_fd != STDIN_FILENO) close(src_fd);

            char *args[] = {
                (char*)rastertopwg,
                argv[1], argv[2], argv[3], argv[4], argv[5],
                NULL
            };
            execv(rastertopwg, args);
            perror("execv rastertopwg");
            _exit(127);
        }

        feeder_pid = fork();
        if (feeder_pid == 0) {
            /* Child feeder: send lead + src_fd into to_filter[1] */
            close(from_filter[0]); close(from_filter[1]); close(to_filter[0]);
            write(to_filter[1], lead, lead_read);
            char buf[65536];
            ssize_t n;
            while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
                if (write(to_filter[1], buf, n) <= 0) break;
            }
            close(to_filter[1]);
            if (src_fd != STDIN_FILENO) close(src_fd);
            _exit(0);
        }

        close(to_filter[0]); close(to_filter[1]); close(from_filter[1]);
        if (src_fd != STDIN_FILENO) close(src_fd);

        src_fd = from_filter[0];
        lead_read = read_exact(src_fd, lead, 16);
    }

    /* 1. Emit PJL Preamble */
    printf("%s@PJL \n", UEL);
    printf("@PJL SET USERNAME=\"%s\"\n", opts.username);
    printf("@PJL SET JOBNAME=\"%s\"\n", opts.jobname);
    printf("@PJL SET LOGINUSER=\"%s\"\n", opts.username);
    printf("@PJL JOB NAME=\"%s\"\n", opts.jobname);
    printf("@PJL SET PAPER=%s\n", opts.paper);
    printf("@PJL SET BORDERLESS=%s\n", opts.borderless ? "ON" : "OFF");
    printf("@PJL SET JTTOPMARGIN=%d\n", opts.margin);
    printf("@PJL SET JTBOTMARGIN=%d\n", opts.margin);
    printf("@PJL SET JTLEFTMARGIN=%d\n", opts.margin);
    printf("@PJL SET JTRIGHTMARGIN=%d\n", opts.margin);
    printf("@PJL SET RENDERMODE=%s\n", opts.rendermode);
    printf("@PJL SET PRINTQUALITY=%s\n", opts.quality);
    printf("@PJL SET DUPLEX=%s\n", opts.duplex);
    printf("@PJL SET MEDIATYPE=%s\n", opts.mediatype);
    printf("@PJL SET SOURCETRAY=%s\n", opts.sourcetray);
    printf("@PJL SET FIDELITY=TRUE\n");
    printf("@PJL ENTER LANGUAGE=PWGRASTER\n");
    fflush(stdout);

    /* 2. Write PWG Sync "RaS2" */
    fwrite("RaS2", 1, 4, stdout);

    /* 3. Read and patch page header */
    unsigned char page_hdr[PWG_HEADER_SIZE];
    int hdr_offset = (lead_read > 4) ? (lead_read - 4) : 0;
    if (hdr_offset > 0) {
        memcpy(page_hdr, lead + 4, hdr_offset);
    }
    int remaining_hdr = PWG_HEADER_SIZE - hdr_offset;
    if (read_exact(src_fd, page_hdr + hdr_offset, remaining_hdr) < remaining_hdr) {
        fprintf(stderr, "ERROR: Failed to read full PWG page header.\n");
        return 1;
    }

    patch_pwg_header(page_hdr);
    fwrite(page_hdr, 1, PWG_HEADER_SIZE, stdout);
    fflush(stdout);

    /* 4. Stream compressed pixel data */
    char buf[65536];
    ssize_t n;
    size_t total_bytes = 4 + PWG_HEADER_SIZE;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, n, stdout) != (size_t)n) break;
        total_bytes += n;
    }
    fflush(stdout);

    fprintf(stderr, "INFO: Forwarded %zu bytes of PWG raster\n", total_bytes);

    /* 5. Emit PJL Postamble */
    printf("%s@PJL EOJ NAME=\"%s\"\n%s", UEL, opts.jobname, UEL);
    fflush(stdout);

    if (src_fd != STDIN_FILENO) close(src_fd);

    if (filter_pid > 0) waitpid(filter_pid, NULL, 0);
    if (feeder_pid > 0) waitpid(feeder_pid, NULL, 0);

    fprintf(stderr, "INFO: Job complete\n");
    return 0;
}
