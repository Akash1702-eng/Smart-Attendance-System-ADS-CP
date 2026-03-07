#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mysql.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib,"ws2_32.lib")

#define PORT        8080
#define BUFFER_SIZE 65536
#define MAX_JSON    (BUFFER_SIZE * 4)

MYSQL *conn;

/* ================================================================
   INTERVAL TREE
   ================================================================

   Each node stores one session's time window [start, end] on a
   particular date, plus the session's database id and name so we
   can report the conflicting session back to the teacher.

   The tree is an augmented BST (standard interval-tree algorithm):
     - keyed on interval.start
     - each node caches the maximum end-value in its subtree (max_end)

   Overlap query:  O(log n) average,  O(n) worst-case (all overlap)
   Insert:         O(log n)
   All other paths in the server are unchanged.

   The tree is rebuilt from the database:
     - once at startup  (load_interval_tree)
     - after every successful CREATE SESSION  (it_insert)
     - after every DELETE SESSION            (rebuild_interval_tree)
     - after every DELETE ATTENDANCE         (no change – tree unchanged)
================================================================ */

typedef struct ITNode {
    int  start;          /* session start  (minutes since midnight) */
    int  end;            /* session end    (minutes since midnight) */
    int  max_end;        /* max end in this subtree                 */
    int  session_id;     /* DB primary key                          */
    char date[12];       /* "YYYY-MM-DD"                            */
    char name[101];      /* session_name for conflict messages      */
    int  height;         /* for AVL balancing                       */
    struct ITNode *left;
    struct ITNode *right;
} ITNode;

/* One tree root per date is overkill for a small system;
   we store all dates in one tree and filter by date in the query. */
static ITNode *it_root = NULL;

/* ---- helpers -------------------------------------------------- */

static int it_height(ITNode *n)
{
    return n ? n->height : 0;
}

static int it_max_end(ITNode *n)
{
    return n ? n->max_end : 0;
}

static void it_update(ITNode *n)
{
    if (!n) return;
    int lh = it_height(n->left);
    int rh = it_height(n->right);
    n->height  = 1 + (lh > rh ? lh : rh);

    int mx = n->end;
    if (it_max_end(n->left)  > mx) mx = it_max_end(n->left);
    if (it_max_end(n->right) > mx) mx = it_max_end(n->right);
    n->max_end = mx;
}

static int it_balance_factor(ITNode *n)
{
    return n ? it_height(n->left) - it_height(n->right) : 0;
}

/* AVL right rotation */
static ITNode *it_rotate_right(ITNode *y)
{
    ITNode *x  = y->left;
    ITNode *t2 = x->right;
    x->right = y;
    y->left  = t2;
    it_update(y);
    it_update(x);
    return x;
}

/* AVL left rotation */
static ITNode *it_rotate_left(ITNode *x)
{
    ITNode *y  = x->right;
    ITNode *t2 = y->left;
    y->left  = x;
    x->right = t2;
    it_update(x);
    it_update(y);
    return y;
}

/* Rebalance after insert */
static ITNode *it_rebalance(ITNode *n)
{
    it_update(n);
    int bf = it_balance_factor(n);

    /* Left-Left */
    if (bf > 1 && it_balance_factor(n->left) >= 0)
        return it_rotate_right(n);

    /* Left-Right */
    if (bf > 1 && it_balance_factor(n->left) < 0)
    {
        n->left = it_rotate_left(n->left);
        return it_rotate_right(n);
    }

    /* Right-Right */
    if (bf < -1 && it_balance_factor(n->right) <= 0)
        return it_rotate_left(n);

    /* Right-Left */
    if (bf < -1 && it_balance_factor(n->right) > 0)
    {
        n->right = it_rotate_right(n->right);
        return it_rotate_left(n);
    }

    return n;
}

/* Allocate a new node */
static ITNode *it_new_node(int start, int end, int session_id,
                           const char *date, const char *name)
{
    ITNode *n     = (ITNode *)malloc(sizeof(ITNode));
    n->start      = start;
    n->end        = end;
    n->max_end    = end;
    n->session_id = session_id;
    n->height     = 1;
    n->left       = NULL;
    n->right      = NULL;
    strncpy(n->date, date, sizeof(n->date) - 1);
    n->date[sizeof(n->date) - 1] = '\0';
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->name[sizeof(n->name) - 1] = '\0';
    return n;
}

/* Insert a session interval into the AVL interval tree */
static ITNode *it_insert(ITNode *node, int start, int end,
                         int session_id, const char *date, const char *name)
{
    if (!node)
        return it_new_node(start, end, session_id, date, name);

    /* BST insert keyed on start time */
    if (start < node->start)
        node->left  = it_insert(node->left,  start, end, session_id, date, name);
    else
        node->right = it_insert(node->right, start, end, session_id, date, name);

    return it_rebalance(node);
}

/* ---- overlap query -------------------------------------------- */

/*
 * it_find_overlap:
 *   Returns the first node whose [start, end] overlaps [new_start, new_end]
 *   on the given date.
 *
 *   Two intervals [a,b] and [c,d] overlap iff  a < d  AND  c < b.
 *
 *   The max_end augmentation lets us prune the right subtree:
 *   if the left subtree's max_end <= new_start, nothing in it can overlap.
 */
static ITNode *it_find_overlap(ITNode *node, int new_start, int new_end,
                               const char *date)
{
    if (!node) return NULL;

    /* Prune: if the maximum end in this subtree <= new_start,
       no interval here can overlap. */
    if (node->max_end <= new_start) return NULL;

    /* Check left subtree first (standard interval-tree traversal) */
    ITNode *found = it_find_overlap(node->left, new_start, new_end, date);
    if (found) return found;

    /* Check this node: same date AND windows overlap */
    if (strcmp(node->date, date) == 0 &&
        node->start < new_end &&
        new_start   < node->end)
    {
        return node;
    }

    /* Check right subtree */
    return it_find_overlap(node->right, new_start, new_end, date);
}

/* ---- tree memory management ----------------------------------- */

static void it_free(ITNode *node)
{
    if (!node) return;
    it_free(node->left);
    it_free(node->right);
    free(node);
}

/* ---- load from DB at startup ---------------------------------- */

static void load_interval_tree()
{
    it_free(it_root);
    it_root = NULL;

    if (mysql_query(conn,
        "SELECT id, session_name, session_date, start_time, end_time"
        " FROM sessions"))
    {
        printf("[ITREE] Failed to load sessions: %s\n", mysql_error(conn));
        return;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) return;

    MYSQL_ROW row;
    int count = 0;

    while ((row = mysql_fetch_row(res)))
    {
        int  id    = atoi(row[0] ? row[0] : "0");
        const char *name  = row[1] ? row[1] : "";
        const char *date  = row[2] ? row[2] : "";
        int  start = atoi(row[3] ? row[3] : "0");
        int  end   = atoi(row[4] ? row[4] : "0");

        if (strlen(date) >= 10 && end > start)
        {
            it_root = it_insert(it_root, start, end, id, date, name);
            count++;
        }
    }

    mysql_free_result(res);
    printf("[ITREE] Loaded %d session(s) into interval tree\n", count);
}

/* Rebuild the entire tree (used after a session is deleted) */
static void rebuild_interval_tree()
{
    load_interval_tree();
}

/* ================================================================
   UTILITY
   ================================================================ */

static void url_decode(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (*src && i < max - 1)
    {
        if (*src == '%' && src[1] && src[2])
        {
            char hex[3] = { src[1], src[2], '\0' };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else if (*src == '+')
        {
            dst[i++] = ' ';
            src++;
        }
        else
        {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static void db_escape(char *dst, const char *src, size_t max)
{
    mysql_real_escape_string(conn, dst, src, strlen(src));
    (void)max;
}

static void get_server_ip(char *out, size_t len)
{
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
    {
        strncpy(out, "127.0.0.1", len);
        return;
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, NULL, &hints, &res) != 0)
    {
        strncpy(out, "127.0.0.1", len);
        return;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, out, (socklen_t)len);
    freeaddrinfo(res);
}

static int time_to_minutes(const char *t)
{
    int h = 0, m = 0;
    sscanf(t, "%d:%d", &h, &m);
    return h * 60 + m;
}

static void minutes_to_time(int mins, char *out)
{
    sprintf(out, "%02d:%02d", mins / 60, mins % 60);
}

/* ================================================================
   SEND RESPONSE
   ================================================================ */

static void send_response(SOCKET client, const char *type, const char *body)
{
    char header[512];
    int  body_len = (int)strlen(body);

    sprintf(header,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        type, body_len);

    send(client, header, (int)strlen(header), 0);
    send(client, body,   body_len,             0);
}

static void send_404(SOCKET client)
{
    const char *resp =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n\r\n"
        "404 Not Found";
    send(client, resp, (int)strlen(resp), 0);
}

/* ================================================================
   READ FILE
   ================================================================ */

static void read_file(const char *filename, char *buffer)
{
    FILE *f = fopen(filename, "rb");
    if (!f)
    {
        strcpy(buffer, "<h1>File not found</h1>");
        return;
    }
    size_t n  = fread(buffer, 1, BUFFER_SIZE - 1, f);
    buffer[n] = '\0';
    fclose(f);
}

/* ================================================================
   DATABASE INIT
   ================================================================ */

void init_database()
{
    conn = mysql_init(NULL);

    if (!mysql_real_connect(conn, "localhost", "root", "12345",
                            "attendance_db", 0, NULL, 0))
    {
        printf("MySQL connection failed: %s\n", mysql_error(conn));
        exit(1);
    }

    mysql_query(conn,
        "CREATE TABLE IF NOT EXISTS students("
        "prn   VARCHAR(20)  PRIMARY KEY,"
        "name  VARCHAR(100),"
        "email VARCHAR(100))");

    mysql_query(conn,
        "CREATE TABLE IF NOT EXISTS sessions("
        "id           INT AUTO_INCREMENT PRIMARY KEY,"
        "teacher_name VARCHAR(100),"
        "session_name VARCHAR(100),"
        "session_date DATE,"
        "start_time   INT,"
        "end_time     INT,"
        "wifi_ssid     VARCHAR(100),"
        "wifi_password VARCHAR(100))");

    /* Safely add session_date to pre-existing tables (MySQL 5.x compatible) */
    {
        MYSQL_RES *col_res = NULL;
        if (mysql_query(conn,
            "SELECT COLUMN_NAME FROM information_schema.COLUMNS"
            " WHERE TABLE_SCHEMA = DATABASE()"
            "   AND TABLE_NAME   = 'sessions'"
            "   AND COLUMN_NAME  = 'session_date'") == 0)
            col_res = mysql_store_result(conn);

        int col_exists = (col_res && mysql_num_rows(col_res) > 0);
        if (col_res) mysql_free_result(col_res);

        if (!col_exists)
        {
            if (mysql_query(conn,
                "ALTER TABLE sessions ADD COLUMN"
                " session_date DATE AFTER session_name"))
                printf("[WARN] Could not add session_date column: %s\n",
                       mysql_error(conn));
            else
                printf("[DB] Added session_date column to sessions table\n");
        }
    }

    mysql_query(conn,
        "CREATE TABLE IF NOT EXISTS attendance("
        "id         INT AUTO_INCREMENT PRIMARY KEY,"
        "prn        VARCHAR(20),"
        "name       VARCHAR(100),"
        "session_id INT,"
        "device_ip  VARCHAR(45),"
        "marked_at  DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "UNIQUE KEY unique_prn_session    (prn,       session_id),"
        "UNIQUE KEY unique_device_session (device_ip, session_id))");

    /* Safely add device_ip to pre-existing tables */
    {
        MYSQL_RES *col_res = NULL;
        if (mysql_query(conn,
            "SELECT COLUMN_NAME FROM information_schema.COLUMNS"
            " WHERE TABLE_SCHEMA = DATABASE()"
            "   AND TABLE_NAME   = 'attendance'"
            "   AND COLUMN_NAME  = 'device_ip'") == 0)
            col_res = mysql_store_result(conn);

        int col_exists = (col_res && mysql_num_rows(col_res) > 0);
        if (col_res) mysql_free_result(col_res);

        if (!col_exists)
        {
            if (mysql_query(conn,
                "ALTER TABLE attendance ADD COLUMN"
                " device_ip VARCHAR(45) AFTER session_id"))
                printf("[WARN] Could not add device_ip column: %s\n",
                       mysql_error(conn));
            else
            {
                mysql_query(conn,
                    "ALTER TABLE attendance"
                    " ADD UNIQUE KEY unique_device_session"
                    " (device_ip, session_id)");
                printf("[DB] Added device_ip column to attendance table\n");
            }
        }
    }

    /* Build the in-memory interval tree from existing sessions */
    load_interval_tree();
}

/* ================================================================
   API – STUDENTS
   ================================================================ */

static void api_register_student(SOCKET client, char *body)
{
    char raw_prn[50]={0}, raw_name[100]={0}, raw_email[100]={0};
    char prn[100]={0}, name[200]={0}, email[200]={0};
    char safe_prn[100]={0}, safe_name[200]={0}, safe_email[200]={0};

    sscanf(body, "prn=%49[^&]&name=%99[^&]&email=%99s",
           raw_prn, raw_name, raw_email);

    url_decode(prn,   raw_prn,   sizeof(prn));
    url_decode(name,  raw_name,  sizeof(name));
    url_decode(email, raw_email, sizeof(email));

    db_escape(safe_prn,   prn,   sizeof(safe_prn));
    db_escape(safe_name,  name,  sizeof(safe_name));
    db_escape(safe_email, email, sizeof(safe_email));

    char query[600];
    sprintf(query,
        "INSERT INTO students(prn,name,email) VALUES('%s','%s','%s')"
        " ON DUPLICATE KEY UPDATE name='%s',email='%s'",
        safe_prn, safe_name, safe_email, safe_name, safe_email);

    if (mysql_query(conn, query))
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Database error\"}");
        return;
    }
    send_response(client, "application/json",
        "{\"success\":true,\"message\":\"Student registered successfully\"}");
}

static void api_register_students_csv(SOCKET client, char *body)
{
    char *csv_start = strstr(body, "csv=");
    if (!csv_start)
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"No CSV data\"}");
        return;
    }
    csv_start += 4;

    char *decoded = (char *)malloc(BUFFER_SIZE);
    if (!decoded)
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Server out of memory\"}");
        return;
    }
    url_decode(decoded, csv_start, BUFFER_SIZE);

    int added = 0, skipped = 0;
    char *line = strtok(decoded, "\r\n");

    while (line)
    {
        char raw_prn[50]={0}, raw_name[100]={0}, raw_email[100]={0};

        if (strncasecmp(line, "prn", 3) == 0 ||
            strncasecmp(line, "\"prn", 4) == 0)
        {
            line = strtok(NULL, "\r\n");
            continue;
        }

        if (sscanf(line, "%49[^,],%99[^,],%99[^\r\n]",
                   raw_prn, raw_name, raw_email) < 3)
        {
            line = strtok(NULL, "\r\n");
            continue;
        }

        char safe_prn[100]={0}, safe_name[200]={0}, safe_email[200]={0};
        db_escape(safe_prn,   raw_prn,   sizeof(safe_prn));
        db_escape(safe_name,  raw_name,  sizeof(safe_name));
        db_escape(safe_email, raw_email, sizeof(safe_email));

        char query[600];
        sprintf(query,
            "INSERT INTO students(prn,name,email) VALUES('%s','%s','%s')"
            " ON DUPLICATE KEY UPDATE name='%s',email='%s'",
            safe_prn, safe_name, safe_email, safe_name, safe_email);

        if (mysql_query(conn, query) == 0) added++;
        else                               skipped++;

        line = strtok(NULL, "\r\n");
    }

    free(decoded);

    char resp[256];
    sprintf(resp,
        "{\"success\":true,\"message\":\"%d student(s) added, %d skipped\"}",
        added, skipped);
    send_response(client, "application/json", resp);
}

static void api_get_students(SOCKET client)
{
    if (mysql_query(conn, "SELECT prn,name,email FROM students ORDER BY name"))
    {
        send_response(client, "application/json", "[]");
        return;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    MYSQL_ROW  row;

    char *json = (char *)malloc(MAX_JSON);
    if (!json) { send_response(client, "application/json", "[]"); return; }

    strcpy(json, "[");
    int first = 1;
    size_t used = 1;

    while ((row = mysql_fetch_row(res)))
    {
        char item[512];
        int n = snprintf(item, sizeof(item),
            "%s{\"prn\":\"%s\",\"name\":\"%s\",\"email\":\"%s\"}",
            first ? "" : ",",
            row[0] ? row[0] : "",
            row[1] ? row[1] : "",
            row[2] ? row[2] : "");

        if (used + n + 2 < MAX_JSON)
        {
            strcat(json, item);
            used += n;
            first = 0;
        }
    }

    strcat(json, "]");
    mysql_free_result(res);
    send_response(client, "application/json", json);
    free(json);
}

/* ================================================================
   API – SESSIONS
   ================================================================ */

/*
 * POST /api/create_session
 * body: teacher=..&session=..&start=NNN&end=NNN&date=YYYY-MM-DD
 *
 * OVERLAP CHECK – uses the in-memory AVL interval tree instead of
 * a MySQL query.  Everything else (DB insert, response format) is
 * identical to the original implementation.
 */
static void api_create_session(SOCKET client, char *body)
{
    char raw_teacher[100]={0}, raw_session[100]={0}, raw_date[20]={0};
    char teacher[200]={0}, sess[200]={0};
    char safe_teacher[200]={0}, safe_session[200]={0};
    int  start = 0, end = 0;

    sscanf(body,
        "teacher=%99[^&]&session=%99[^&]&start=%d&end=%d&date=%19s",
        raw_teacher, raw_session, &start, &end, raw_date);

    url_decode(teacher, raw_teacher, sizeof(teacher));
    url_decode(sess,    raw_session, sizeof(sess));

    db_escape(safe_teacher, teacher, sizeof(safe_teacher));
    db_escape(safe_session, sess,    sizeof(safe_session));

    if (strlen(teacher) == 0 || strlen(sess) == 0 || end <= start)
    {
        send_response(client, "application/json",
            "{\"success\":false,"
            "\"message\":\"Invalid input: check teacher, session name, "
            "and that end time is after start time\"}");
        return;
    }

    /* Determine session date */
    char session_date[20] = {0};
    char decoded_date[20] = {0};
    url_decode(decoded_date, raw_date, sizeof(decoded_date));

    if (strlen(decoded_date) >= 10)
    {
        int y=0, m=0, d=0;
        if (sscanf(decoded_date, "%4d-%2d-%2d", &y, &m, &d) == 3 &&
            y > 2000 && m >= 1 && m <= 12 && d >= 1 && d <= 31)
        {
            snprintf(session_date, sizeof(session_date),
                     "%04d-%02d-%02d", y, m, d);
        }
    }
    if (strlen(session_date) == 0)
    {
        time_t    now = time(NULL);
        struct tm *lt = localtime(&now);
        strftime(session_date, sizeof(session_date), "%Y-%m-%d", lt);
    }

    printf("[create_session] date=%s start=%d end=%d\n",
           session_date, start, end);

    /* ---- Overlap check via interval tree (O(log n)) ----------------
     *
     * it_find_overlap walks the AVL tree and returns the first node
     * whose [start, end] overlaps [start, end] on the same date.
     * No MySQL round-trip is needed.
     * ---------------------------------------------------------------- */
    ITNode *conflict = it_find_overlap(it_root, start, end, session_date);

    if (conflict)
    {
        char existing_start[10], existing_end[10];
        minutes_to_time(conflict->start, existing_start);
        minutes_to_time(conflict->end,   existing_end);

        char conflict_msg[512];
        snprintf(conflict_msg, sizeof(conflict_msg),
            "{\"success\":false,"
            "\"message\":\"Time conflict: session '%s' already exists on %s "
            "from %s to %s. Please choose a different time.\"}",
            conflict->name, session_date, existing_start, existing_end);

        send_response(client, "application/json", conflict_msg);
        return;
    }

    /* ---- No conflict – insert into DB -------------------------------- */
    char query[700];
    sprintf(query,
        "INSERT INTO sessions"
        "(teacher_name, session_name, session_date, start_time, end_time)"
        " VALUES('%s','%s','%s',%d,%d)",
        safe_teacher, safe_session, session_date, start, end);

    if (mysql_query(conn, query))
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Database error\"}");
        return;
    }

    int new_id = (int)mysql_insert_id(conn);

    /* Insert the new session into the live interval tree so subsequent
       requests in the same server run see it immediately. */
    it_root = it_insert(it_root, start, end, new_id, session_date, sess);

    printf("[ITREE] Inserted session id=%d [%d,%d] on %s\n",
           new_id, start, end, session_date);

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"success\":true,"
        "\"message\":\"Session created for %s\","
        "\"session_id\":%d}",
        session_date, new_id);

    send_response(client, "application/json", resp);
}

static void api_get_sessions(SOCKET client)
{
    if (mysql_query(conn,
        "SELECT id,teacher_name,session_name,session_date,start_time,end_time"
        " FROM sessions ORDER BY session_date DESC, start_time DESC"))
    {
        send_response(client, "application/json", "[]");
        return;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    MYSQL_ROW  row;

    char *json = (char *)malloc(MAX_JSON);
    if (!json) { send_response(client, "application/json", "[]"); return; }

    strcpy(json, "[");
    int first = 1;
    size_t used = 1;

    while ((row = mysql_fetch_row(res)))
    {
        char start_str[10], end_str[10];
        minutes_to_time(atoi(row[4] ? row[4] : "0"), start_str);
        minutes_to_time(atoi(row[5] ? row[5] : "0"), end_str);

        const char *date_val = (row[3] && strlen(row[3]) > 0) ? row[3] : "";

        char item[1024];
        int n = snprintf(item, sizeof(item),
            "%s{\"id\":%s,\"teacher\":\"%s\",\"session\":\"%s\","
            "\"date\":\"%s\",\"start\":\"%s\",\"end\":\"%s\"}",
            first ? "" : ",",
            row[0] ? row[0] : "0",
            row[1] ? row[1] : "",
            row[2] ? row[2] : "",
            date_val, start_str, end_str);

        if (used + n + 2 < MAX_JSON)
        {
            strcat(json, item);
            used += n;
            first = 0;
        }
    }

    strcat(json, "]");
    mysql_free_result(res);
    printf("[get_sessions] Sending %zu bytes: %.120s...\n", strlen(json), json);
    send_response(client, "application/json", json);
    free(json);
}

/* ================================================================
   API – ATTENDANCE
   ================================================================ */

static void api_mark_attendance(SOCKET client, char *body,
                                const char *client_ip)
{
    char raw_prn[50]={0}, raw_name[100]={0};
    char prn[100]={0}, name[200]={0};
    char safe_prn[100]={0}, safe_name[200]={0}, safe_ip[100]={0};
    int  session_id = 0;

    sscanf(body,
        "prn=%49[^&]&name=%99[^&]&session_id=%d",
        raw_prn, raw_name, &session_id);

    url_decode(prn,  raw_prn,  sizeof(prn));
    url_decode(name, raw_name, sizeof(name));

    if (session_id <= 0 || strlen(prn) == 0)
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Invalid input\"}");
        return;
    }

    char check_q[128];
    sprintf(check_q,
        "SELECT start_time,end_time FROM sessions WHERE id=%d", session_id);

    if (mysql_query(conn, check_q))
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Database error\"}");
        return;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res || mysql_num_rows(res) == 0)
    {
        if (res) mysql_free_result(res);
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Session not found\"}");
        return;
    }

    MYSQL_ROW row       = mysql_fetch_row(res);
    int       start_min = atoi(row[0] ? row[0] : "0");
    int       end_min   = atoi(row[1] ? row[1] : "0");
    mysql_free_result(res);

    time_t    now_t   = time(NULL);
    struct tm *lt     = localtime(&now_t);
    int        now_min = lt->tm_hour * 60 + lt->tm_min;

    if (now_min < start_min || now_min > end_min)
    {
        char msg[256];
        char s[10], e[10];
        minutes_to_time(start_min, s);
        minutes_to_time(end_min,   e);
        sprintf(msg,
            "{\"success\":false,\"message\":\"Session not active. "
            "Active window: %s - %s\"}",
            s, e);
        send_response(client, "application/json", msg);
        return;
    }

    /* Device check */
    db_escape(safe_ip, client_ip, sizeof(safe_ip));

    char dev_q[256];
    snprintf(dev_q, sizeof(dev_q),
        "SELECT prn FROM attendance"
        " WHERE device_ip='%s' AND session_id=%d LIMIT 1",
        safe_ip, session_id);

    if (mysql_query(conn, dev_q) == 0)
    {
        MYSQL_RES *dev_res = mysql_store_result(conn);
        if (dev_res)
        {
            if (mysql_num_rows(dev_res) > 0)
            {
                MYSQL_ROW dev_row = mysql_fetch_row(dev_res);
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "{\"success\":false,"
                    "\"message\":\"Attendance already marked from this device"
                    " (PRN: %s). Only one submission per device is allowed.\"}",
                    dev_row[0] ? dev_row[0] : "unknown");
                mysql_free_result(dev_res);
                send_response(client, "application/json", msg);
                return;
            }
            mysql_free_result(dev_res);
        }
    }

    db_escape(safe_prn,  prn,  sizeof(safe_prn));
    db_escape(safe_name, name, sizeof(safe_name));

    char query[512];
    snprintf(query, sizeof(query),
        "INSERT INTO attendance(prn, name, session_id, device_ip)"
        " VALUES('%s','%s',%d,'%s')",
        safe_prn, safe_name, session_id, safe_ip);

    if (mysql_query(conn, query))
    {
        if (mysql_errno(conn) == 1062)
        {
            const char *err = mysql_error(conn);
            if (strstr(err, "device"))
                send_response(client, "application/json",
                    "{\"success\":false,"
                    "\"message\":\"Attendance already marked from this device."
                    " Only one submission per device is allowed.\"}");
            else
                send_response(client, "application/json",
                    "{\"success\":false,"
                    "\"message\":\"Attendance already marked for this PRN.\"}");
        }
        else
        {
            send_response(client, "application/json",
                "{\"success\":false,\"message\":\"Database error\"}");
        }
        return;
    }

    printf("[attendance] PRN=%s IP=%s session=%d\n",
           prn, client_ip, session_id);

    send_response(client, "application/json",
        "{\"success\":true,\"message\":\"Attendance marked successfully\"}");
}

static void api_session_attendance(SOCKET client, const char *query_str)
{
    int session_id = 0;
    sscanf(query_str, "session_id=%d", &session_id);

    if (session_id <= 0)
    {
        send_response(client, "application/json", "[]");
        return;
    }

    char query[256];
    sprintf(query,
        "SELECT a.prn, a.name, a.marked_at, a.device_ip"
        " FROM attendance a"
        " WHERE a.session_id=%d ORDER BY a.marked_at",
        session_id);

    if (mysql_query(conn, query))
    {
        send_response(client, "application/json", "[]");
        return;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    MYSQL_ROW  row;

    char *json = (char *)malloc(MAX_JSON);
    if (!json) { send_response(client, "application/json", "[]"); return; }

    strcpy(json, "[");
    int first = 1;
    size_t used = 1;

    while ((row = mysql_fetch_row(res)))
    {
        char item[256];
        int n = snprintf(item, sizeof(item),
            "%s{\"prn\":\"%s\",\"name\":\"%s\","
            "\"marked_at\":\"%s\",\"device_ip\":\"%s\"}",
            first ? "" : ",",
            row[0] ? row[0] : "",
            row[1] ? row[1] : "",
            row[2] ? row[2] : "",
            row[3] ? row[3] : "");

        if (used + n + 2 < MAX_JSON)
        {
            strcat(json, item);
            used += n;
            first = 0;
        }
    }

    strcat(json, "]");
    mysql_free_result(res);
    send_response(client, "application/json", json);
    free(json);
}

static void api_search_attendance(SOCKET client, const char *query_str)
{
    char raw_prn[50]={0}, prn[100]={0}, safe_prn[100]={0};

    sscanf(query_str, "prn=%49s", raw_prn);
    url_decode(prn, raw_prn, sizeof(prn));
    db_escape(safe_prn, prn, sizeof(safe_prn));

    char query[512];
    sprintf(query,
        "SELECT s.session_name, s.teacher_name, a.marked_at"
        " FROM attendance a"
        " JOIN sessions s ON a.session_id = s.id"
        " WHERE a.prn='%s' ORDER BY a.marked_at DESC",
        safe_prn);

    if (mysql_query(conn, query))
    {
        send_response(client, "application/json", "[]");
        return;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    MYSQL_ROW  row;

    char *json = (char *)malloc(MAX_JSON);
    if (!json) { send_response(client, "application/json", "[]"); return; }

    strcpy(json, "[");
    int first = 1;
    size_t used = 1;

    while ((row = mysql_fetch_row(res)))
    {
        char item[256];
        int n = snprintf(item, sizeof(item),
            "%s{\"session_name\":\"%s\","
            "\"teacher_name\":\"%s\","
            "\"marked_at\":\"%s\"}",
            first ? "" : ",",
            row[0] ? row[0] : "",
            row[1] ? row[1] : "",
            row[2] ? row[2] : "");

        if (used + n + 2 < MAX_JSON)
        {
            strcat(json, item);
            used += n;
            first = 0;
        }
    }

    strcat(json, "]");
    mysql_free_result(res);
    send_response(client, "application/json", json);
    free(json);
}

/* ================================================================
   API – DASHBOARD
   ================================================================ */

static void api_dashboard_stats(SOCKET client)
{
    int total_students=0, total_sessions=0, total_attendance=0;
    MYSQL_RES *res;
    MYSQL_ROW  row;

    if (!mysql_query(conn, "SELECT COUNT(*) FROM students"))
    {
        res = mysql_store_result(conn);
        if ((row = mysql_fetch_row(res)))
            total_students = atoi(row[0] ? row[0] : "0");
        mysql_free_result(res);
    }
    if (!mysql_query(conn, "SELECT COUNT(*) FROM sessions"))
    {
        res = mysql_store_result(conn);
        if ((row = mysql_fetch_row(res)))
            total_sessions = atoi(row[0] ? row[0] : "0");
        mysql_free_result(res);
    }
    if (!mysql_query(conn, "SELECT COUNT(*) FROM attendance"))
    {
        res = mysql_store_result(conn);
        if ((row = mysql_fetch_row(res)))
            total_attendance = atoi(row[0] ? row[0] : "0");
        mysql_free_result(res);
    }

    char resp[256];
    sprintf(resp,
        "{\"total_students\":%d,"
        "\"total_sessions\":%d,"
        "\"total_attendance\":%d}",
        total_students, total_sessions, total_attendance);

    send_response(client, "application/json", resp);
}

static void api_get_server_ip(SOCKET client)
{
    char ip[64];
    get_server_ip(ip, sizeof(ip));
    char resp[128];
    sprintf(resp, "{\"ip\":\"%s\"}", ip);
    send_response(client, "application/json", resp);
}

/* ================================================================
   API – DELETE OPERATIONS
   ================================================================ */

static void api_delete_student(SOCKET client, char *body)
{
    char raw_prn[50]={0}, prn[100]={0}, safe_prn[100]={0};

    sscanf(body, "prn=%49s", raw_prn);
    url_decode(prn, raw_prn, sizeof(prn));

    if (strlen(prn) == 0)
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"PRN is required\"}");
        return;
    }

    db_escape(safe_prn, prn, sizeof(safe_prn));

    char q1[256], q2[256];
    sprintf(q1, "DELETE FROM attendance WHERE prn='%s'", safe_prn);
    sprintf(q2, "DELETE FROM students   WHERE prn='%s'", safe_prn);

    mysql_query(conn, q1);

    if (mysql_query(conn, q2))
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Database error\"}");
        return;
    }
    if (mysql_affected_rows(conn) == 0)
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Student not found\"}");
        return;
    }

    send_response(client, "application/json",
        "{\"success\":true,\"message\":\"Student and attendance records deleted\"}");
}

static void api_delete_attendance(SOCKET client, char *body)
{
    int session_id = 0;
    sscanf(body, "session_id=%d", &session_id);

    if (session_id <= 0)
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Invalid session ID\"}");
        return;
    }

    char query[128];
    sprintf(query, "DELETE FROM attendance WHERE session_id=%d", session_id);

    if (mysql_query(conn, query))
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Database error\"}");
        return;
    }

    int rows = (int)mysql_affected_rows(conn);
    char resp[128];
    sprintf(resp,
        "{\"success\":true,\"message\":\"%d attendance record(s) deleted\"}",
        rows);
    send_response(client, "application/json", resp);
    /* No tree change needed – attendance rows don't affect session intervals */
}

static void api_delete_session(SOCKET client, char *body)
{
    int session_id = 0;
    sscanf(body, "session_id=%d", &session_id);

    if (session_id <= 0)
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Invalid session ID\"}");
        return;
    }

    char q1[128], q2[128];
    sprintf(q1, "DELETE FROM attendance WHERE session_id=%d", session_id);
    sprintf(q2, "DELETE FROM sessions    WHERE id=%d",        session_id);

    mysql_query(conn, q1);

    if (mysql_query(conn, q2))
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Database error\"}");
        return;
    }
    if (mysql_affected_rows(conn) == 0)
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Session not found\"}");
        return;
    }

    /* AVL interval tree deletion is complex; rebuilding is safe and
       correct since sessions are rarely deleted.                        */
    rebuild_interval_tree();
    printf("[ITREE] Rebuilt after session %d deleted\n", session_id);

    send_response(client, "application/json",
        "{\"success\":true,\"message\":\"Session and attendance records deleted\"}");
}

/* ================================================================
   API – TRIGGER EMAIL
   ================================================================ */

static void api_send_session_emails(SOCKET client, const char *query_str)
{
    int session_id = 0;
    sscanf(query_str, "session_id=%d", &session_id);

    if (session_id <= 0)
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Invalid session ID\"}");
        return;
    }

    char cmd[256];
    sprintf(cmd, "python send_emails.py %d 2>&1", session_id);
    printf("[EMAIL] Running: %s\n", cmd);

    FILE *pipe = _popen(cmd, "r");
    if (!pipe)
    {
        send_response(client, "application/json",
            "{\"success\":false,\"message\":\"Failed to launch email script\"}");
        return;
    }

    char output[4096] = {0};
    char line[256];
    while (fgets(line, sizeof(line), pipe))
    {
        printf("[EMAIL] %s", line);
        if (strlen(output) + strlen(line) < sizeof(output) - 1)
            strcat(output, line);
    }

    int exit_code = _pclose(pipe);
    printf("[EMAIL] Exit code: %d\n", exit_code);

    /* Sanitize output for JSON embedding */
    char safe_out[512] = {0};
    int  si = 0;
    for (int i = 0; output[i] && si < (int)sizeof(safe_out) - 3; i++)
    {
        unsigned char c = (unsigned char)output[i];
        if      (c == '"')               { safe_out[si++] = '\\'; safe_out[si++] = '"';  }
        else if (c == '\\')              { safe_out[si++] = '\\'; safe_out[si++] = '\\'; }
        else if (c == '\r' || c == '\n') { safe_out[si++] = ' '; }
        else if (c < 0x20)               { safe_out[si++] = ' '; }
        else                             { safe_out[si++] = (char)c; }
    }
    safe_out[si] = '\0';

    if (exit_code == 0)
    {
        char summary[128] = "Emails sent successfully";
        char *sent_line = strstr(output, "Emails Sent:");
        if (sent_line)
        {
            int j = 0;
            for (int i = 0; sent_line[i] && sent_line[i] != '\n' && j < 100; i++)
            {
                unsigned char c = (unsigned char)sent_line[i];
                if      (c == '"')  { summary[j++] = '\''; }
                else if (c == '\\') { summary[j++] = '/';  }
                else if (c < 0x20)  { summary[j++] = ' ';  }
                else                { summary[j++] = (char)c; }
            }
            summary[j] = '\0';
        }
        char resp[256];
        sprintf(resp, "{\"success\":true,\"message\":\"%s\"}", summary);
        send_response(client, "application/json", resp);
    }
    else
    {
        char resp[700];
        sprintf(resp,
            "{\"success\":false,\"message\":\"Email script failed: %s\"}",
            safe_out);
        send_response(client, "application/json", resp);
    }
}

/* ================================================================
   ROUTER
   ================================================================ */

void handle_request(SOCKET client, char *req, const char *client_ip)
{
    char method[10] = {0};
    char path[512]  = {0};

    sscanf(req, "%9s %511s", method, path);
    printf("[%s] %s\n", method, path);

    char *query_str = strchr(path, '?');
    if (query_str) { *query_str = '\0'; query_str++; }
    else             query_str = "";

    char *body = strstr(req, "\r\n\r\n");
    if (body) body += 4; else body = "";

    /* HTML pages */
    if (strcmp(path,"/")==0 || strcmp(path,"/teacher")==0 ||
        strcmp(path,"/teacher.html")==0)
    {
        char *html = (char *)malloc(BUFFER_SIZE);
        read_file("teacher.html", html);
        send_response(client, "text/html", html);
        free(html); return;
    }
    if (strcmp(path,"/student")==0 || strcmp(path,"/student.html")==0)
    {
        char *html = (char *)malloc(BUFFER_SIZE);
        read_file("student.html", html);
        send_response(client, "text/html", html);
        free(html); return;
    }
    if (strcmp(path,"/favicon.ico")==0)
    { send_response(client,"text/plain",""); return; }

    /* Students */
    if (strcmp(path,"/api/register_student_manual")==0)
    { api_register_student(client,body);        return; }
    if (strcmp(path,"/api/register_students")==0)
    { api_register_students_csv(client,body);   return; }
    if (strcmp(path,"/api/get_all_students")==0)
    { api_get_students(client);                 return; }

    /* Sessions */
    if (strcmp(path,"/api/create_session")==0)
    { api_create_session(client,body);          return; }
    if (strcmp(path,"/api/get_sessions")==0)
    { api_get_sessions(client);                 return; }

    /* Attendance */
    if (strcmp(path,"/api/mark_attendance")==0)
    { api_mark_attendance(client,body,client_ip); return; }
    if (strcmp(path,"/api/session_attendance")==0)
    { api_session_attendance(client,query_str); return; }
    if (strcmp(path,"/api/search_attendance")==0)
    { api_search_attendance(client,query_str);  return; }

    /* Delete */
    if (strcmp(path,"/api/delete_student")==0)
    { api_delete_student(client,body);          return; }
    if (strcmp(path,"/api/delete_attendance")==0)
    { api_delete_attendance(client,body);       return; }
    if (strcmp(path,"/api/delete_session")==0)
    { api_delete_session(client,body);          return; }

    /* Email */
    if (strcmp(path,"/api/send_session_emails")==0)
    { api_send_session_emails(client,query_str); return; }

    /* Dashboard */
    if (strcmp(path,"/api/dashboard_stats")==0)
    { api_dashboard_stats(client);              return; }
    if (strcmp(path,"/api/get_server_ip")==0)
    { api_get_server_ip(client);                return; }

    send_404(client);
}

/* ================================================================
   RECEIVE FULL REQUEST
   ================================================================ */

static int recv_full(SOCKET client, char *buffer, int max)
{
    int   total = 0, bytes, content_length = -1;
    char *body_start = NULL;

    while (total < max - 1)
    {
        bytes = recv(client, buffer + total, max - 1 - total, 0);
        if (bytes <= 0) break;
        total += bytes;
        buffer[total] = '\0';

        if (!body_start)
        {
            body_start = strstr(buffer, "\r\n\r\n");
            if (body_start)
            {
                body_start += 4;
                char *cl = strstr(buffer, "Content-Length:");
                if (!cl) cl = strstr(buffer, "content-length:");
                content_length = cl ? atoi(cl + 15) : 0;
            }
        }
        if (body_start && content_length >= 0)
        {
            int body_received = total - (int)(body_start - buffer);
            if (body_received >= content_length) break;
        }
    }

    buffer[total] = '\0';
    return total;
}

/* ================================================================
   MAIN
   ================================================================ */

int main()
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    init_database();   /* also calls load_interval_tree() */

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(PORT);

    bind(server_socket,  (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(server_socket, 10);

    char ip[64];
    get_server_ip(ip, sizeof(ip));
    printf("Server running:\n");
    printf("  Local:   http://127.0.0.1:%d\n", PORT);
    printf("  Network: http://%s:%d\n\n", ip, PORT);

    while (1)
    {
        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);

        SOCKET client = accept(server_socket,
                               (struct sockaddr *)&client_addr,
                               &client_addr_len);
        if (client == INVALID_SOCKET) continue;

        char client_ip[46] = "unknown";
        inet_ntop(AF_INET, &client_addr.sin_addr,
                  client_ip, sizeof(client_ip));

        char *buffer = (char *)malloc(BUFFER_SIZE);
        if (!buffer) { closesocket(client); continue; }

        int bytes = recv_full(client, buffer, BUFFER_SIZE);

        if (bytes > 0)
        {
            printf("\n---- REQUEST from %s (%d bytes) ----\n%.200s\n",
                   client_ip, bytes, buffer);
            handle_request(client, buffer, client_ip);
        }

        free(buffer);
        closesocket(client);
    }

    closesocket(server_socket);
    it_free(it_root);   /* clean up interval tree */
    mysql_close(conn);
    WSACleanup();
    return 0;
}