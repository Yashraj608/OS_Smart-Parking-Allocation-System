#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <ncurses.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <stdatomic.h>

#define TOTAL_SLOTS        20
#define SIM_DURATION_SEC  120
#define AGING_THRESHOLD_S   8
#define TIMEOUT_SEC        20
#define VIP_FRACTION       0.20
#define QUEUE_CAPACITY     10
#define PARK_MIN_S          8
#define PARK_MAX_S         30
#define GEN_MIN_MS        600
#define GEN_MAX_MS       1800
#define BURST_INTERVAL_S   12
#define BURST_MIN_CARS      4
#define BURST_MAX_CARS      8
#define BURST_GAP_MS       150
#define UPFRONT_REJECT_PCT  5
#define PROMOTE_CHANCE_PCT 30

typedef enum { NORMAL = 0, VIP = 1 } VehicleType;

typedef struct {
    int         id;
    VehicleType type;
    time_t      arrival_ts;
    int         promoted;
    int         aged;
} Vehicle;

typedef struct {
    Vehicle data[QUEUE_CAPACITY];
    int     head, tail, count;
} CircularQueue;

typedef struct {
    int         occupied;
    int         vehicle_id;
    VehicleType vtype;
    time_t      since;
    int         aged;
} ParkingSlot;

typedef struct {
    long   total_arrivals;
    long   total_served;
    long   total_rejected;
    long   total_promoted;
    double sum_wait_s;
    double sum_park_s;
    long   wait_samples;
} Stats;

sem_t            g_sem;
pthread_mutex_t  g_slot_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

ParkingSlot   g_slots[TOTAL_SLOTS];
CircularQueue g_queue;
Stats         g_stats;

atomic_int    g_sim_running = 1;
time_t        g_sim_start;
atomic_int    g_next_id = 1;


/* ============================================================
 *  SECTION 1 — JIYANSHU
 *  Data Structures & Core Parking Logic
 *  Covers: circular queue operations, slot management,
 *          stats tracking, utility helpers, final report
 * ============================================================ */

void msleep(int ms)
{
    struct timespec req = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&req, NULL);
}

/* ── Circular queue ── */

int queue_enqueue(const Vehicle *v)
{
    if (g_queue.count >= QUEUE_CAPACITY) return -1;
    g_queue.data[g_queue.tail] = *v;
    g_queue.tail = (g_queue.tail + 1) % QUEUE_CAPACITY;
    g_queue.count++;
    return 0;
}

int queue_remove_by_id(int id, Vehicle *out)
{
    for (int i = 0; i < g_queue.count; i++) {
        int idx = (g_queue.head + i) % QUEUE_CAPACITY;
        if (g_queue.data[idx].id == id) {
            *out = g_queue.data[idx];
            for (int j = i; j < g_queue.count - 1; j++) {
                int cur  = (g_queue.head + j    ) % QUEUE_CAPACITY;
                int next = (g_queue.head + j + 1) % QUEUE_CAPACITY;
                g_queue.data[cur] = g_queue.data[next];
            }
            g_queue.tail = (g_queue.tail - 1 + QUEUE_CAPACITY) % QUEUE_CAPACITY;
            g_queue.count--;
            return 0;
        }
    }
    return -1;
}

/* ── Slot management ── */

int claim_slot(VehicleType vtype, int vehicle_id)
{
    int chosen = -1;
    if (vtype == VIP) {
        for (int i = 0; i < TOTAL_SLOTS; i++)
            if (!g_slots[i].occupied) { chosen = i; break; }
    } else {
        for (int i = TOTAL_SLOTS - 1; i >= 0; i--)
            if (!g_slots[i].occupied) { chosen = i; break; }
    }
    if (chosen >= 0) {
        g_slots[chosen].occupied   = 1;
        g_slots[chosen].vehicle_id = vehicle_id;
        g_slots[chosen].vtype      = vtype;
        g_slots[chosen].since      = time(NULL);
        g_slots[chosen].aged       = 0;
    }
    return chosen;
}

void release_slot(int slot_index)
{
    g_slots[slot_index].occupied   = 0;
    g_slots[slot_index].vehicle_id = 0;
    g_slots[slot_index].vtype      = NORMAL;
    g_slots[slot_index].since      = 0;
    g_slots[slot_index].aged       = 0;
}

/* ── Final report (text output after ncurses ends) ── */

#define BOX_W 58
#define TOP   "╔══════════════════════════════════════════════════════════╗"
#define MID   "╠══════════════════════════════════════════════════════════╣"
#define BOT   "╚══════════════════════════════════════════════════════════╝"

#define ROW(fmt, ...) do { \
    char _cell[BOX_W + 1]; \
    snprintf(_cell, sizeof(_cell), fmt, ##__VA_ARGS__); \
    printf("║ %-*s ║\n", BOX_W - 2, _cell); \
} while (0)

void print_final_report(void)
{
    long   arr     = g_stats.total_arrivals;
    long   srv     = g_stats.total_served;
    long   rej     = g_stats.total_rejected;
    long   prom    = g_stats.total_promoted;
    double awt     = (g_stats.wait_samples > 0) ? g_stats.sum_wait_s / g_stats.wait_samples : 0.0;
    double apk     = (srv > 0)                  ? g_stats.sum_park_s / srv                  : 0.0;
    double thput   = (double)srv / SIM_DURATION_SEC;
    double rej_pct = (arr > 0)                  ? (double)rej / arr * 100.0                 : 0.0;

    printf("\n%s\n", TOP);
    printf("║ %*s ║\n", BOX_W - 2 + 2,
           "SMART PARKING SYSTEM \xe2\x80\x94 FINAL PERFORMANCE REPORT");
    printf("%s\n", MID);
    ROW("  Simulation duration  : %3d s",           SIM_DURATION_SEC);
    ROW("  Total arrivals       : %5ld",             arr);
    ROW("  Total served         : %5ld",             srv);
    ROW("  Total rejected       : %5ld  (%5.1f%%)",  rej, rej_pct);
    ROW("  Priority promotions  : %5ld  (aging mechanism)", prom);
    printf("%s\n", MID);
    ROW("  Avg waiting time     : %6.2f s",          awt);
    ROW("  Avg parking duration : %6.2f s",          apk);
    ROW("  Throughput           : %6.3f vehicles/s", thput);
    printf("%s\n", MID);
    ROW("  Parking slots        : %2d",              TOTAL_SLOTS);
    ROW("  Queue capacity       : %2d  (reduced from 200)", QUEUE_CAPACITY);
    ROW("  Timeout threshold    : %2d s",            TIMEOUT_SEC);
    ROW("  Aging threshold      : %2d s",            AGING_THRESHOLD_S);
    ROW("  Upfront reject rate  : %2d%%",            UPFRONT_REJECT_PCT);
    ROW("  Promotion chance     : %2d%%  (when aged)", PROMOTE_CHANCE_PCT);
    printf("%s\n", BOT);
    printf("\nAll threads joined successfully. Goodbye.\n");
}

/* ============================================================
 *  END OF SECTION 1 — JIYANSHU
 * ============================================================ */


/* ============================================================
 *  SECTION 2 — ABDULLAH
 *  ncurses Window UI Library
 *  Covers: window handles, color pairs, all draw_*() functions,
 *          event log, dashboard refresh, display thread
 * ============================================================ */

WINDOW *g_win_header = NULL;
WINDOW *g_win_grid   = NULL;
WINDOW *g_win_queue  = NULL;
WINDOW *g_win_stats  = NULL;
WINDOW *g_win_log    = NULL;

#define LOG_LINES 12
#define LOG_WIDTH 70
char g_log[LOG_LINES][LOG_WIDTH + 1];
int  g_log_next = 0;
pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CP_FREE    1
#define CP_OCC     2
#define CP_VIP     3
#define CP_HEADER  4
#define CP_LABEL   5
#define CP_BORDER  6
#define CP_AGED    7

/* ── ncurses initialisation ── */

void init_ncurses(void)
{
    initscr(); cbreak(); noecho(); curs_set(0);
    start_color(); use_default_colors();
    init_pair(CP_FREE,   COLOR_GREEN,   -1);
    init_pair(CP_OCC,    COLOR_RED,     -1);
    init_pair(CP_VIP,    COLOR_CYAN,    -1);
    init_pair(CP_HEADER, COLOR_BLACK,   COLOR_YELLOW);
    init_pair(CP_LABEL,  COLOR_YELLOW,  -1);
    init_pair(CP_BORDER, COLOR_BLUE,    -1);
    init_pair(CP_AGED,   COLOR_MAGENTA, -1);
    g_win_header = newwin(3,  80, 0,  0);
    g_win_grid   = newwin(14, 42, 3,  0);
    g_win_queue  = newwin(14, 38, 3, 42);
    g_win_stats  = newwin(6,  80, 17, 0);
    g_win_log    = newwin(LOG_LINES + 2, 80, 23, 0);
}

/* ── Event log ── */

void log_push(const char *fmt, ...)
{
    char buf[LOG_WIDTH + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_log_mutex);
    strncpy(g_log[g_log_next], buf, LOG_WIDTH);
    g_log[g_log_next][LOG_WIDTH] = '\0';
    g_log_next = (g_log_next + 1) % LOG_LINES;
    pthread_mutex_unlock(&g_log_mutex);
}

/* ── Individual panel renderers ── */

void draw_header(void)
{
    werase(g_win_header);
    wbkgd(g_win_header, COLOR_PAIR(CP_HEADER));
    mvwprintw(g_win_header, 1, 2,
        "  SMART PARKING ALLOCATION SYSTEM  |  Slots: %d  |  Simulation",
        TOTAL_SLOTS);
    time_t elapsed = time(NULL) - g_sim_start;
    if (elapsed > SIM_DURATION_SEC) elapsed = SIM_DURATION_SEC;
    mvwprintw(g_win_header, 1, 62, "T+%3lds / %ds", elapsed, SIM_DURATION_SEC);
    wnoutrefresh(g_win_header);
}

void draw_grid(void)
{
    werase(g_win_grid);
    wattron(g_win_grid, COLOR_PAIR(CP_BORDER));
    box(g_win_grid, 0, 0);
    wattroff(g_win_grid, COLOR_PAIR(CP_BORDER));
    wattron(g_win_grid, COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvwprintw(g_win_grid, 0, 2, " PARKING GRID (4x5) ");
    wattroff(g_win_grid, COLOR_PAIR(CP_LABEL) | A_BOLD);

    wattron(g_win_grid, COLOR_PAIR(CP_FREE));  mvwprintw(g_win_grid, 12,  2, "[FREE]"); wattroff(g_win_grid, COLOR_PAIR(CP_FREE));
    wattron(g_win_grid, COLOR_PAIR(CP_OCC));   mvwprintw(g_win_grid, 12, 10, "[OCC] "); wattroff(g_win_grid, COLOR_PAIR(CP_OCC));
    wattron(g_win_grid, COLOR_PAIR(CP_VIP));   mvwprintw(g_win_grid, 12, 17, "[VIP]");  wattroff(g_win_grid, COLOR_PAIR(CP_VIP));
    wattron(g_win_grid, COLOR_PAIR(CP_AGED));  mvwprintw(g_win_grid, 12, 24, "[AGED]"); wattroff(g_win_grid, COLOR_PAIR(CP_AGED));

    pthread_mutex_lock(&g_slot_mutex);
    for (int i = 0; i < TOTAL_SLOTS; i++) {
        int row = (i / 5) * 2 + 2;
        int col = (i % 5) * 8 + 1;
        char label[11];

        if (!g_slots[i].occupied) {
            wattron(g_win_grid, COLOR_PAIR(CP_FREE) | A_BOLD);
            snprintf(label, sizeof(label), " P%02d:OK", i + 1);
        } else if (g_slots[i].aged) {
            wattron(g_win_grid, COLOR_PAIR(CP_AGED) | A_BOLD);
            snprintf(label, sizeof(label), " P%02d:AGD", i + 1);
        } else if (g_slots[i].vtype == VIP) {
            wattron(g_win_grid, COLOR_PAIR(CP_VIP) | A_BOLD);
            snprintf(label, sizeof(label), " P%02d:V%d", i + 1, g_slots[i].vehicle_id % 100);
        } else {
            wattron(g_win_grid, COLOR_PAIR(CP_OCC) | A_BOLD);
            snprintf(label, sizeof(label), " P%02d:%3d", i + 1, g_slots[i].vehicle_id % 1000);
        }
        mvwprintw(g_win_grid, row, col, "%s", label);

        if (g_slots[i].occupied)
            mvwprintw(g_win_grid, row + 1, col + 1, "%3ds    ", (int)(time(NULL) - g_slots[i].since));
        else
            mvwprintw(g_win_grid, row + 1, col + 1, "        ");

        wattroff(g_win_grid, A_BOLD | COLOR_PAIR(CP_FREE) | COLOR_PAIR(CP_OCC)
                                    | COLOR_PAIR(CP_VIP)  | COLOR_PAIR(CP_AGED));
    }
    pthread_mutex_unlock(&g_slot_mutex);
    wnoutrefresh(g_win_grid);
}

void draw_queue(void)
{
    werase(g_win_queue);
    wattron(g_win_queue, COLOR_PAIR(CP_BORDER));
    box(g_win_queue, 0, 0);
    wattroff(g_win_queue, COLOR_PAIR(CP_BORDER));
    wattron(g_win_queue, COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvwprintw(g_win_queue, 0, 2, " WAIT QUEUE ");
    wattroff(g_win_queue, COLOR_PAIR(CP_LABEL) | A_BOLD);

    pthread_mutex_lock(&g_queue_mutex);
    int count = g_queue.count;
    mvwprintw(g_win_queue, 1, 2, "Vehicles waiting: %d / %d", count, QUEUE_CAPACITY);
    mvwprintw(g_win_queue, 2, 2, "%-4s %-4s %-6s %-6s", "ID", "Type", "Wait", "Status");
    mvwhline(g_win_queue, 3, 1, ACS_HLINE, 36);

    int display = (count < 9) ? count : 9;
    for (int i = 0; i < display; i++) {
        int idx = (g_queue.head + i) % QUEUE_CAPACITY;
        Vehicle *v = &g_queue.data[idx];
        int wait = (int)(time(NULL) - v->arrival_ts);
        const char *type   = (v->type == VIP) ? "VIP" : "NRM";
        const char *status = v->aged          ? "AGED"
                           : v->promoted      ? "PROM"
                           : (v->type == VIP) ? "PRIO"
                                              : "WAIT";
        int pair = v->aged || v->promoted ? CP_AGED
                 : (v->type == VIP)       ? CP_VIP
                                          : CP_OCC;
        wattron(g_win_queue, COLOR_PAIR(pair));
        mvwprintw(g_win_queue, 4 + i, 2, "%-4d %-4s %3ds   %-6s", v->id, type, wait, status);
        wattroff(g_win_queue, COLOR_PAIR(pair));
    }
    if (count > 9)
        mvwprintw(g_win_queue, 13, 2, "  ... and %d more", count - 9);
    pthread_mutex_unlock(&g_queue_mutex);
    wnoutrefresh(g_win_queue);
}

void draw_stats(void)
{
    werase(g_win_stats);
    wattron(g_win_stats, COLOR_PAIR(CP_BORDER));
    box(g_win_stats, 0, 0);
    wattroff(g_win_stats, COLOR_PAIR(CP_BORDER));
    wattron(g_win_stats, COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvwprintw(g_win_stats, 0, 2, " LIVE STATISTICS ");
    wattroff(g_win_stats, COLOR_PAIR(CP_LABEL) | A_BOLD);

    pthread_mutex_lock(&g_stats_mutex);
    long   arrivals = g_stats.total_arrivals;
    long   served   = g_stats.total_served;
    long   rejected = g_stats.total_rejected;
    long   promoted = g_stats.total_promoted;
    double avg_wait = (g_stats.wait_samples > 0) ? g_stats.sum_wait_s / g_stats.wait_samples : 0.0;
    double avg_park = (served > 0)               ? g_stats.sum_park_s / served               : 0.0;
    pthread_mutex_unlock(&g_stats_mutex);

    int occ = 0;
    pthread_mutex_lock(&g_slot_mutex);
    for (int i = 0; i < TOTAL_SLOTS; i++) occ += g_slots[i].occupied;
    pthread_mutex_unlock(&g_slot_mutex);

    double util        = (double)occ / TOTAL_SLOTS * 100.0;
    double reject_rate = (arrivals > 0) ? (double)rejected / arrivals * 100.0 : 0.0;

    mvwprintw(g_win_stats, 1, 2,
        "Arrivals: %-5ld  Served: %-5ld  Rejected: %-5ld  Promoted: %-5ld",
        arrivals, served, rejected, promoted);
    mvwprintw(g_win_stats, 2, 2,
        "Occupied: %2d/%-2d  Utilization: %5.1f%%  Reject Rate: %5.1f%%",
        occ, TOTAL_SLOTS, util, reject_rate);
    mvwprintw(g_win_stats, 3, 2,
        "Avg Wait: %5.1f s  Avg Park: %5.1f s  Queue cap: %d",
        avg_wait, avg_park, QUEUE_CAPACITY);

    int bar_len = (int)(util / 100.0 * 40);
    wattron(g_win_stats, COLOR_PAIR(CP_OCC));
    mvwprintw(g_win_stats, 4, 2, "Util [");
    for (int i = 0; i < 40; i++) waddch(g_win_stats, (i < bar_len) ? '|' : '.');
    wprintw(g_win_stats, "] %5.1f%%", util);
    wattroff(g_win_stats, COLOR_PAIR(CP_OCC));

    wnoutrefresh(g_win_stats);
}

void draw_log(void)
{
    werase(g_win_log);
    wattron(g_win_log, COLOR_PAIR(CP_BORDER));
    box(g_win_log, 0, 0);
    wattroff(g_win_log, COLOR_PAIR(CP_BORDER));
    wattron(g_win_log, COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvwprintw(g_win_log, 0, 2, " EVENT LOG ");
    wattroff(g_win_log, COLOR_PAIR(CP_LABEL) | A_BOLD);

    pthread_mutex_lock(&g_log_mutex);
    for (int i = 0; i < LOG_LINES; i++) {
        int idx = (g_log_next + i) % LOG_LINES;
        mvwprintw(g_win_log, i + 1, 2, "%-68s", g_log[idx]);
    }
    pthread_mutex_unlock(&g_log_mutex);
    wnoutrefresh(g_win_log);
}

/* ── Dashboard refresh & display thread ── */

void refresh_dashboard(void)
{
    draw_header(); draw_grid(); draw_queue();
    draw_stats();  draw_log();  doupdate();
}

void *display_thread(void *arg)
{
    (void)arg;
    while (g_sim_running) { refresh_dashboard(); msleep(500); }
    for (int i = 0; i < (PARK_MAX_S + 6) * 2; i++) {
        refresh_dashboard();
        msleep(500);
    }
    return NULL;
}

/* ============================================================
 *  END OF SECTION 2 — ABDULLAH
 * ============================================================ */


/* ============================================================
 *  SECTION 3 — YASH
 *  Threading & Simulation Engine
 *  Covers: aging thread, per-vehicle thread, vehicle spawner,
 *          generator thread (bursts + normal flow), main()
 * ============================================================ */

void *aging_thread(void *arg)
{
    (void)arg;
    unsigned int seed = (unsigned int)(time(NULL) ^ (uintptr_t)pthread_self());

    while (g_sim_running) {
        sleep(1);
        time_t now = time(NULL);

        /* Age vehicles waiting too long in the queue */
        pthread_mutex_lock(&g_queue_mutex);
        for (int i = 0; i < g_queue.count; i++) {
            int idx = (g_queue.head + i) % QUEUE_CAPACITY;
            Vehicle *v = &g_queue.data[idx];
            long waited = (long)(now - v->arrival_ts);

            if (v->type == NORMAL && !v->promoted && !v->aged && waited >= AGING_THRESHOLD_S) {
                if (rand_r(&seed) % 100 < PROMOTE_CHANCE_PCT) {
                    v->type     = VIP;
                    v->promoted = 1;
                    log_push("[PROM] Vehicle %d promoted VIP after %lds wait", v->id, waited);
                    pthread_mutex_lock(&g_stats_mutex);
                    g_stats.total_promoted++;
                    pthread_mutex_unlock(&g_stats_mutex);
                } else {
                    v->aged = 1;
                    log_push("[AGED] Vehicle %d aged in queue (%lds wait)", v->id, waited);
                }
            }
        }
        pthread_mutex_unlock(&g_queue_mutex);

        /* Mark long-staying parked vehicles as aged */
        int long_stay_thresh = (int)(PARK_MAX_S * 1.5);
        pthread_mutex_lock(&g_slot_mutex);
        for (int i = 0; i < TOTAL_SLOTS; i++) {
            if (g_slots[i].occupied && !g_slots[i].aged) {
                int dur = (int)(now - g_slots[i].since);
                if (dur >= long_stay_thresh) {
                    g_slots[i].aged = 1;
                    log_push("[AGED] Slot P%02d vehicle %d aged (parked %ds)",
                             i + 1, g_slots[i].vehicle_id, dur);
                }
            }
        }
        pthread_mutex_unlock(&g_slot_mutex);
    }
    return NULL;
}

void *vehicle_thread(void *arg)
{
    Vehicle v = *(Vehicle *)arg;
    free(arg);

    struct timespec ts_r;
    clock_gettime(CLOCK_MONOTONIC, &ts_r);
    unsigned int seed = (unsigned int)(ts_r.tv_nsec ^ (uintptr_t)pthread_self() ^ (unsigned)v.id);

    /* Upfront random rejection */
    if ((rand_r(&seed) % 100) < UPFRONT_REJECT_PCT) {
        log_push("[RJCT] Vehicle %d rejected upfront (%d%% chance)", v.id, UPFRONT_REJECT_PCT);
        pthread_mutex_lock(&g_stats_mutex);
        g_stats.total_rejected++;
        pthread_mutex_unlock(&g_stats_mutex);
        return NULL;
    }

    /* Attempt to join the wait queue */
    pthread_mutex_lock(&g_queue_mutex);
    int enq = queue_enqueue(&v);
    pthread_mutex_unlock(&g_queue_mutex);

    if (enq < 0) {
        log_push("[RJCT] Vehicle %d rejected — queue full (%d cap)", v.id, QUEUE_CAPACITY);
        pthread_mutex_lock(&g_stats_mutex);
        g_stats.total_rejected++;
        pthread_mutex_unlock(&g_stats_mutex);
        return NULL;
    }

    log_push("[ARR ] Vehicle %d (%s) arrived and queued", v.id, (v.type == VIP) ? "VIP" : "NRM");

    /* Wait for a slot with timeout */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += TIMEOUT_SEC;

    if (sem_timedwait(&g_sem, &deadline) != 0) {
        pthread_mutex_lock(&g_queue_mutex);
        Vehicle removed;
        queue_remove_by_id(v.id, &removed);
        pthread_mutex_unlock(&g_queue_mutex);
        log_push("[TOUT] Vehicle %d timed out after %ds", v.id, TIMEOUT_SEC);
        pthread_mutex_lock(&g_stats_mutex);
        g_stats.total_rejected++;
        pthread_mutex_unlock(&g_stats_mutex);
        return NULL;
    }

    /* Refresh vehicle state (may have been promoted/aged while waiting) */
    pthread_mutex_lock(&g_queue_mutex);
    for (int i = 0; i < g_queue.count; i++) {
        int idx = (g_queue.head + i) % QUEUE_CAPACITY;
        if (g_queue.data[idx].id == v.id) {
            v.type     = g_queue.data[idx].type;
            v.promoted = g_queue.data[idx].promoted;
            v.aged     = g_queue.data[idx].aged;
            break;
        }
    }
    Vehicle removed;
    queue_remove_by_id(v.id, &removed);
    pthread_mutex_unlock(&g_queue_mutex);

    /* Claim a physical slot */
    pthread_mutex_lock(&g_slot_mutex);
    int slot = claim_slot(v.type, v.id);
    pthread_mutex_unlock(&g_slot_mutex);

    if (slot < 0) {
        sem_post(&g_sem);
        log_push("[ERR ] Vehicle %d: semaphore ok but no slot found!", v.id);
        return NULL;
    }

    double wait_s = difftime(time(NULL), v.arrival_ts);
    log_push("[PARK] Vehicle %d (%s%s) -> Slot P%02d (waited %.0fs)",
             v.id,
             (v.type == VIP) ? "VIP" : "NRM",
             v.promoted ? "/PROM" : (v.aged ? "/AGED" : ""),
             slot + 1, wait_s);

    pthread_mutex_lock(&g_stats_mutex);
    g_stats.total_served++;
    g_stats.sum_wait_s += wait_s;
    g_stats.wait_samples++;
    pthread_mutex_unlock(&g_stats_mutex);

    /* Park for a random duration then leave */
    struct timespec ts_seed;
    clock_gettime(CLOCK_MONOTONIC, &ts_seed);
    unsigned int seed2 = (unsigned int)(ts_seed.tv_nsec ^ (uintptr_t)pthread_self() ^ (unsigned)v.id);
    int park_s = PARK_MIN_S + rand_r(&seed2) % (PARK_MAX_S - PARK_MIN_S + 1);
    sleep(park_s);

    pthread_mutex_lock(&g_slot_mutex);
    release_slot(slot);
    pthread_mutex_unlock(&g_slot_mutex);
    sem_post(&g_sem);

    log_push("[LEAV] Vehicle %d left Slot P%02d after %ds", v.id, slot + 1, park_s);

    pthread_mutex_lock(&g_stats_mutex);
    g_stats.sum_park_s += park_s;
    pthread_mutex_unlock(&g_stats_mutex);

    return NULL;
}

void spawn_vehicle(unsigned int *seed)
{
    Vehicle *v = malloc(sizeof(Vehicle));
    if (!v) return;
    v->id         = atomic_fetch_add(&g_next_id, 1);
    v->type       = (rand_r(seed) % 100 < (int)(VIP_FRACTION * 100)) ? VIP : NORMAL;
    v->arrival_ts = time(NULL);
    v->promoted   = 0;
    v->aged       = 0;

    pthread_mutex_lock(&g_stats_mutex);
    g_stats.total_arrivals++;
    pthread_mutex_unlock(&g_stats_mutex);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, vehicle_thread, v);
    pthread_attr_destroy(&attr);
}

void *generator_thread(void *arg)
{
    (void)arg;
    unsigned int seed = (unsigned int)(time(NULL) ^ (uintptr_t)pthread_self());
    time_t last_burst = g_sim_start;

    while (g_sim_running) {
        int gap_ms = GEN_MIN_MS + rand_r(&seed) % (GEN_MAX_MS - GEN_MIN_MS + 1);
        msleep(gap_ms);
        if (!g_sim_running) break;

        spawn_vehicle(&seed);

        /* Periodic burst arrivals */
        time_t now = time(NULL);
        if ((now - last_burst) >= BURST_INTERVAL_S) {
            last_burst = now;
            int burst = BURST_MIN_CARS + rand_r(&seed) % (BURST_MAX_CARS - BURST_MIN_CARS + 1);
            log_push("[BRST] Burst arriving: %d vehicles", burst);
            for (int b = 0; b < burst && g_sim_running; b++) {
                msleep(BURST_GAP_MS);
                spawn_vehicle(&seed);
            }
        }
    }
    return NULL;
}

int main(void)
{
    sem_init(&g_sem, 0, TOTAL_SLOTS);
    memset(g_slots,  0, sizeof(g_slots));
    memset(&g_queue, 0, sizeof(g_queue));
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_log,    0, sizeof(g_log));

    g_sim_start = time(NULL);
    init_ncurses();

    pthread_t t_gen, t_disp, t_aging;
    pthread_create(&t_disp,  NULL, display_thread,   NULL);
    pthread_create(&t_aging, NULL, aging_thread,     NULL);
    pthread_create(&t_gen,   NULL, generator_thread, NULL);

    sleep(SIM_DURATION_SEC);
    atomic_store(&g_sim_running, 0);

    pthread_join(t_gen,   NULL);
    pthread_join(t_aging, NULL);
    sleep(PARK_MAX_S + 5);

    refresh_dashboard();
    msleep(800);
    pthread_join(t_disp, NULL);

    endwin();

    sem_destroy(&g_sem);
    pthread_mutex_destroy(&g_slot_mutex);
    pthread_mutex_destroy(&g_queue_mutex);
    pthread_mutex_destroy(&g_stats_mutex);
    pthread_mutex_destroy(&g_log_mutex);

    print_final_report();
    return 0;
}

/* ============================================================
 *  END OF SECTION 3 — YASH
 * ============================================================ */
