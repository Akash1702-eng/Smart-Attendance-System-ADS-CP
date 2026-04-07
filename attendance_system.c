#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mysql.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT             8080
#define HTTPS_PORT       8443
#define BUFFER_SIZE      65536
#define MAX_JSON         (BUFFER_SIZE * 4)
#define BODY_BUFFER_SIZE (1024 * 1024 * 4)

MYSQL *conn;

typedef struct ITNode {
    int  start, end, max_end, session_id, height;
    char date[12], name[101];
    struct ITNode *left, *right;
} ITNode;

static ITNode *it_root = NULL;

static int it_height(ITNode *n)  { return n ? n->height  : 0; }
static int it_max_end(ITNode *n) { return n ? n->max_end : 0; }

static void it_update(ITNode *n) {
    if (!n) return;
    int lh = it_height(n->left), rh = it_height(n->right);
    n->height = 1 + (lh > rh ? lh : rh);
    int mx = n->end;
    if (it_max_end(n->left)  > mx) mx = it_max_end(n->left);
    if (it_max_end(n->right) > mx) mx = it_max_end(n->right);
    n->max_end = mx;
}

static int it_balance_factor(ITNode *n) { return n ? it_height(n->left) - it_height(n->right) : 0; }

static ITNode *it_rotate_right(ITNode *y) {
    ITNode *x = y->left, *t2 = x->right;
    x->right = y; y->left = t2; it_update(y); it_update(x); return x;
}

static ITNode *it_rotate_left(ITNode *x) {
    ITNode *y = x->right, *t2 = y->left;
    y->left = x; x->right = t2; it_update(x); it_update(y); return y;
}

static ITNode *it_rebalance(ITNode *n) {
    it_update(n);
    int bf = it_balance_factor(n);
    if (bf >  1 && it_balance_factor(n->left)  >= 0) return it_rotate_right(n);
    if (bf >  1 && it_balance_factor(n->left)  <  0) { n->left  = it_rotate_left(n->left);  return it_rotate_right(n); }
    if (bf < -1 && it_balance_factor(n->right) <= 0) return it_rotate_left(n);
    if (bf < -1 && it_balance_factor(n->right) >  0) { n->right = it_rotate_right(n->right); return it_rotate_left(n); }
    return n;
}

static ITNode *it_new_node(int start, int end, int session_id, const char *date, const char *name) {
    ITNode *n = (ITNode *)malloc(sizeof(ITNode));
    if (!n) return NULL;
    n->start = start; n->end = end; n->max_end = end; n->session_id = session_id; n->height = 1;
    n->left = n->right = NULL;
    strncpy(n->date, date, 11); n->date[11] = '\0';
    strncpy(n->name, name, 100); n->name[100] = '\0';
    return n;
}

static ITNode *it_insert(ITNode *node, int start, int end, int session_id, const char *date, const char *name) {
    if (!node) return it_new_node(start, end, session_id, date, name);
    if (start < node->start) node->left  = it_insert(node->left,  start, end, session_id, date, name);
    else                     node->right = it_insert(node->right, start, end, session_id, date, name);
    return it_rebalance(node);
}

static ITNode *it_find_overlap(ITNode *node, int s, int e, const char *date) {
    if (!node || node->max_end <= s) return NULL;
    ITNode *found = it_find_overlap(node->left, s, e, date);
    if (found) return found;
    if (strcmp(node->date, date) == 0 && node->start < e && s < node->end) return node;
    return it_find_overlap(node->right, s, e, date);
}

static void it_free(ITNode *node) {
    if (!node) return; it_free(node->left); it_free(node->right); free(node);
}

static void load_interval_tree(void) {
    it_free(it_root); it_root = NULL;
    if (mysql_query(conn, "SELECT id,session_name,session_date,start_time,end_time FROM sessions WHERE session_date IS NOT NULL")) return;
    MYSQL_RES *res = mysql_store_result(conn); if (!res) return;
    MYSQL_ROW row; int count = 0;
    while ((row = mysql_fetch_row(res))) {
        int id = atoi(row[0]?row[0]:"0");
        const char *name = row[1]?row[1]:"", *date = row[2]?row[2]:"";
        int start = atoi(row[3]?row[3]:"0"), end = atoi(row[4]?row[4]:"0");
        if (strlen(date) >= 10 && end > start) { it_root = it_insert(it_root, start, end, id, date, name); count++; }
    }
    mysql_free_result(res);
    printf("[ITREE] Loaded %d session(s)\n", count);
}

static void rebuild_interval_tree(void) { load_interval_tree(); }

static int portable_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca>='A'&&ca<='Z') ca+=32; if (cb>='A'&&cb<='Z') cb+=32;
        if (ca!=cb) return ca-cb; if (!ca) return 0;
    }
    return 0;
}

static void url_decode(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (*src && i < max-1) {
        if (*src=='%' && src[1] && src[2]) { char hex[3]={src[1],src[2],'\0'}; dst[i++]=(char)strtol(hex,NULL,16); src+=3; }
        else if (*src=='+') { dst[i++]=' '; src++; }
        else { dst[i++]=*src++; }
    }
    dst[i]='\0';
}

static void db_escape(char *dst, const char *src, size_t max) {
    mysql_real_escape_string(conn, dst, src, (unsigned long)strlen(src)); (void)max;
}

static void json_escape(char *dst, size_t dstmax, const char *src) {
    size_t j=0;
    for (size_t i=0; src[i]&&j+4<dstmax; i++) {
        unsigned char c=(unsigned char)src[i];
        if      (c=='"')  { dst[j++]='\\'; dst[j++]='"'; }
        else if (c=='\\') { dst[j++]='\\'; dst[j++]='\\'; }
        else if (c=='\n') { dst[j++]='\\'; dst[j++]='n'; }
        else if (c=='\r') { dst[j++]='\\'; dst[j++]='r'; }
        else if (c=='\t') { dst[j++]='\\'; dst[j++]='t'; }
        else { dst[j++]=(char)c; }
    }
    dst[j]='\0';
}

static void get_server_ip(char *out, size_t len) {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname))!=0) { strncpy(out,"127.0.0.1",len); return; }
    struct addrinfo hints, *res;
    memset(&hints,0,sizeof(hints)); hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    if (getaddrinfo(hostname,NULL,&hints,&res)!=0) { strncpy(out,"127.0.0.1",len); return; }
    struct sockaddr_in *addr=(struct sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET,&addr->sin_addr,out,(socklen_t)len);
    freeaddrinfo(res);
}

static int time_to_minutes(const char *t) { int h=0,m=0; sscanf(t,"%d:%d",&h,&m); return h*60+m; }
static void minutes_to_time(int mins, char *out) { sprintf(out,"%02d:%02d",mins/60,mins%60); }

static void send_response(SOCKET client, const char *type, const char *body) {
    char header[512];
    int blen=(int)strlen(body);
    sprintf(header,"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",type,blen);
    send(client,header,(int)strlen(header),0);
    send(client,body,blen,0);
}

static void send_404(SOCKET client) {
    const char *r="HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n404 Not Found";
    send(client,r,(int)strlen(r),0);
}

static void read_file(const char *filename, char *buffer) {
    FILE *f=fopen(filename,"rb");
    if (!f) { strcpy(buffer,"<h1>File not found</h1>"); return; }
    size_t n=fread(buffer,1,BUFFER_SIZE-1,f); buffer[n]='\0'; fclose(f);
}

/* ─── Extract real client IP from request headers ───────────────────────────
 * When the HTTPS proxy (https_proxy.py) forwards a request it injects a
 * "X-Forwarded-For: <real_student_ip>" header.  Without this, every student
 * request appears to come from 127.0.0.1 (the proxy) and the
 * unique_device_session constraint wrongly blocks every student after the first.
 */
static void extract_real_ip(const char *req, const char *socket_ip, char *out, size_t out_len) {
    /* Try X-Forwarded-For first */
    const char *xff = strstr(req, "X-Forwarded-For:");
    if (!xff) xff = strstr(req, "x-forwarded-for:");
    if (xff) {
        xff += 16; /* skip header name */
        while (*xff == ' ') xff++;
        const char *end = strstr(xff, "\r\n");
        if (end) {
            int len = (int)(end - xff);
            /* Take only the first IP in the list (in case of chained proxies) */
            const char *comma = (const char *)memchr(xff, ',', (size_t)len);
            if (comma) len = (int)(comma - xff);
            if (len > 0 && len < (int)out_len - 1) {
                strncpy(out, xff, (size_t)len);
                out[len] = '\0';
                /* Trim trailing space */
                while (len > 0 && out[len-1] == ' ') { out[--len] = '\0'; }
                return;
            }
        }
    }
    /* Fall back to the socket-level IP */
    strncpy(out, socket_ip, out_len - 1);
    out[out_len - 1] = '\0';
}

void init_database(void) {
    conn = mysql_init(NULL);
    if (!mysql_real_connect(conn,"localhost","root","12345","attendance_db",0,NULL,0)) {
        printf("MySQL connection failed: %s\n",mysql_error(conn)); exit(1);
    }
    mysql_query(conn,"CREATE TABLE IF NOT EXISTS students(prn VARCHAR(20) PRIMARY KEY,name VARCHAR(100) NOT NULL,email VARCHAR(100) NOT NULL)");
    mysql_query(conn,"CREATE TABLE IF NOT EXISTS sessions(id INT AUTO_INCREMENT PRIMARY KEY,teacher_name VARCHAR(100),session_name VARCHAR(100),session_date DATE,start_time INT,end_time INT,wifi_ssid VARCHAR(100),wifi_password VARCHAR(100))");
    mysql_query(conn,"CREATE TABLE IF NOT EXISTS attendance(id INT AUTO_INCREMENT PRIMARY KEY,prn VARCHAR(20),name VARCHAR(100),session_id INT,device_ip VARCHAR(45),marked_at DATETIME DEFAULT CURRENT_TIMESTAMP,UNIQUE KEY unique_prn_session(prn,session_id),UNIQUE KEY unique_device_session(device_ip,session_id))");
    mysql_query(conn,"ALTER TABLE students ADD COLUMN IF NOT EXISTS face_registered TINYINT(1) NOT NULL DEFAULT 0");
    mysql_query(conn,"ALTER TABLE students ADD COLUMN IF NOT EXISTS face_image MEDIUMTEXT NULL");
    load_interval_tree();
    printf("[DB] Database initialization complete\n");
}

static void api_register_student(SOCKET client, char *body) {
    char raw_prn[50]={0},raw_name[100]={0},raw_email[100]={0};
    char prn[100]={0},name[200]={0},email[200]={0};
    char safe_prn[200]={0},safe_name[400]={0},safe_email[400]={0};
    char *p;
    p=strstr(body,"prn=");   if(p){p+=4;char*e=strstr(p,"&");if(e){int l=(int)(e-p);if(l>0&&l<50)strncpy(raw_prn,p,l);}else strncpy(raw_prn,p,49);}
    p=strstr(body,"name=");  if(p){p+=5;char*e=strstr(p,"&");if(e){int l=(int)(e-p);if(l>0&&l<100)strncpy(raw_name,p,l);}else strncpy(raw_name,p,99);}
    p=strstr(body,"email="); if(p){p+=6;char*e=strstr(p,"&");if(e){int l=(int)(e-p);if(l>0&&l<100)strncpy(raw_email,p,l);}else strncpy(raw_email,p,99);}
    url_decode(prn,raw_prn,sizeof(prn)); url_decode(name,raw_name,sizeof(name)); url_decode(email,raw_email,sizeof(email));
    if (!strlen(prn)||!strlen(name)||!strlen(email)) { send_response(client,"application/json","{\"success\":false,\"message\":\"All fields are required\"}"); return; }
    db_escape(safe_prn,prn,sizeof(safe_prn)); db_escape(safe_name,name,sizeof(safe_name)); db_escape(safe_email,email,sizeof(safe_email));
    char query[800];
    sprintf(query,"INSERT INTO students(prn,name,email) VALUES('%s','%s','%s')",safe_prn,safe_name,safe_email);
    if (mysql_query(conn,query)) {
        if (mysql_errno(conn)==1062)
            send_response(client,"application/json","{\"success\":false,\"message\":\"A student with this PRN already exists.\"}");
        else {
            char err[300]; sprintf(err,"{\"success\":false,\"message\":\"Database error: %s\"}",mysql_error(conn));
            send_response(client,"application/json",err);
        }
        return;
    }
    send_response(client,"application/json","{\"success\":true,\"message\":\"Student registered successfully.\"}");
}

static void api_register_students_csv(SOCKET client, char *body) {
    char *csv_start=strstr(body,"csv=");
    if (!csv_start) { send_response(client,"application/json","{\"success\":false,\"message\":\"No CSV data\"}"); return; }
    csv_start+=4;
    char *decoded=(char*)malloc(BUFFER_SIZE);
    if (!decoded) { send_response(client,"application/json","{\"success\":false,\"message\":\"Out of memory\"}"); return; }
    url_decode(decoded,csv_start,BUFFER_SIZE);
    int added=0,skipped=0;
    char *line=strtok(decoded,"\r\n");
    while (line) {
        char raw_prn[50]={0},raw_name[100]={0},raw_email[100]={0};
        if (portable_strncasecmp(line,"prn",3)==0||portable_strncasecmp(line,"\"prn",4)==0) { line=strtok(NULL,"\r\n"); continue; }
        if (sscanf(line,"%49[^,],%99[^,],%99[^\r\n]",raw_prn,raw_name,raw_email)>=2) {
            char safe_prn[200]={0},safe_name[400]={0},safe_email[400]={0};
            db_escape(safe_prn,raw_prn,sizeof(safe_prn)); db_escape(safe_name,raw_name,sizeof(safe_name)); db_escape(safe_email,raw_email,sizeof(safe_email));
            char q[600]; sprintf(q,"INSERT INTO students(prn,name,email) VALUES('%s','%s','%s')",safe_prn,safe_name,safe_email);
            if (mysql_query(conn,q)==0) added++; else skipped++;
        }
        line=strtok(NULL,"\r\n");
    }
    free(decoded);
    char resp[256]; sprintf(resp,"{\"success\":true,\"message\":\"%d student(s) added, %d skipped (duplicates or invalid rows).\"}",added,skipped);
    send_response(client,"application/json",resp);
}

static void api_get_students(SOCKET client) {
    if (mysql_query(conn,"SELECT prn,name,email FROM students ORDER BY name")) { send_response(client,"application/json","[]"); return; }
    MYSQL_RES *res=mysql_store_result(conn); if (!res) { send_response(client,"application/json","[]"); return; }
    char *json=(char*)malloc(MAX_JSON); if (!json) { mysql_free_result(res); send_response(client,"application/json","[]"); return; }
    strcpy(json,"["); int first=1; size_t used=1; MYSQL_ROW row;
    char ep[200]={0},en[400]={0},ee[400]={0};
    while ((row=mysql_fetch_row(res))) {
        json_escape(ep,sizeof(ep),row[0]?row[0]:""); json_escape(en,sizeof(en),row[1]?row[1]:""); json_escape(ee,sizeof(ee),row[2]?row[2]:"");
        char item[700]; int n=snprintf(item,sizeof(item),"%s{\"prn\":\"%s\",\"name\":\"%s\",\"email\":\"%s\"}",first?"":",",ep,en,ee);
        if (used+n+2<MAX_JSON){strcat(json,item);used+=n;first=0;}
    }
    strcat(json,"]"); mysql_free_result(res); send_response(client,"application/json",json); free(json);
}

static void api_create_session(SOCKET client, char *body) {
    char raw_teacher[100]={0},raw_session[100]={0},raw_date[20]={0};
    char teacher[200]={0},sess[200]={0},date[20]={0};
    char safe_teacher[400]={0},safe_session[400]={0},safe_date[40]={0};
    int start=0,end=0;
    char *p;
    p=strstr(body,"teacher=");if(p){p+=8;char*e=strstr(p,"&");if(e){int l=(int)(e-p);if(l>0&&l<100)strncpy(raw_teacher,p,l);}else strncpy(raw_teacher,p,99);}
    p=strstr(body,"session=");if(p){p+=8;char*e=strstr(p,"&");if(e){int l=(int)(e-p);if(l>0&&l<100)strncpy(raw_session,p,l);}else strncpy(raw_session,p,99);}
    p=strstr(body,"date=");   if(p){p+=5;char*e=strstr(p,"&");if(e){int l=(int)(e-p);if(l>0&&l<20)strncpy(raw_date,p,l);}else strncpy(raw_date,p,19);}
    p=strstr(body,"start=");  if(p){p+=6;char*e=strstr(p,"&");if(e){char t[20]={0};int l=(int)(e-p);if(l>0&&l<20)strncpy(t,p,l);start=atoi(t);}else start=atoi(p);}
    p=strstr(body,"end=");    if(p){p+=4;char*e=strstr(p,"&");if(e){char t[20]={0};int l=(int)(e-p);if(l>0&&l<20)strncpy(t,p,l);end=atoi(t);}else end=atoi(p);}
    url_decode(teacher,raw_teacher,sizeof(teacher)); url_decode(sess,raw_session,sizeof(sess)); url_decode(date,raw_date,sizeof(date));
    if (!strlen(teacher)||!strlen(sess)||end<=start) { send_response(client,"application/json","{\"success\":false,\"message\":\"All fields are required and end time must be after start time.\"}"); return; }
    if (strlen(date)>=10) {
        ITNode *ov=it_find_overlap(it_root,start,end,date);
        if (ov) { char msg[300],ts[10],te[10]; minutes_to_time(ov->start,ts); minutes_to_time(ov->end,te);
            snprintf(msg,sizeof(msg),"{\"success\":false,\"message\":\"Time overlap with existing session '%s' (%s–%s). Please choose a different time slot.\"}",ov->name,ts,te);
            send_response(client,"application/json",msg); return; }
    }
    db_escape(safe_teacher,teacher,sizeof(safe_teacher)); db_escape(safe_session,sess,sizeof(safe_session)); db_escape(safe_date,date,sizeof(safe_date));
    char query[800];
    if (strlen(date)>=10)
        sprintf(query,"INSERT INTO sessions(teacher_name,session_name,session_date,start_time,end_time) VALUES('%s','%s','%s',%d,%d)",safe_teacher,safe_session,safe_date,start,end);
    else
        sprintf(query,"INSERT INTO sessions(teacher_name,session_name,start_time,end_time) VALUES('%s','%s',%d,%d)",safe_teacher,safe_session,start,end);
    if (mysql_query(conn,query)) { char err[300]; sprintf(err,"{\"success\":false,\"message\":\"Database error: %s\"}",mysql_error(conn)); send_response(client,"application/json",err); return; }
    int new_id=(int)mysql_insert_id(conn);
    rebuild_interval_tree();
    char resp[128]; snprintf(resp,sizeof(resp),"{\"success\":true,\"message\":\"Session created successfully.\",\"session_id\":%d}",new_id);
    send_response(client,"application/json",resp);
}

static void api_get_sessions(SOCKET client) {
    if (mysql_query(conn,"SELECT id,teacher_name,session_name,session_date,start_time,end_time FROM sessions ORDER BY id DESC")) { send_response(client,"application/json","[]"); return; }
    MYSQL_RES *res=mysql_store_result(conn); if (!res) { send_response(client,"application/json","[]"); return; }
    char *json=(char*)malloc(MAX_JSON); if (!json) { mysql_free_result(res); send_response(client,"application/json","[]"); return; }
    strcpy(json,"["); int first=1; size_t used=1; MYSQL_ROW row;
    char et[200]={0},es[200]={0};
    while ((row=mysql_fetch_row(res))) {
        char start_str[10],end_str[10];
        int st=row[4]?atoi(row[4]):0,en=row[5]?atoi(row[5]):0;
        minutes_to_time(st,start_str); minutes_to_time(en,end_str);
        json_escape(et,sizeof(et),row[1]?row[1]:""); json_escape(es,sizeof(es),row[2]?row[2]:"");
        const char *date_val=(row[3]&&strcmp(row[3],"0000-00-00")!=0)?row[3]:"";
        char item[600]; int n=snprintf(item,sizeof(item),"%s{\"id\":%s,\"teacher\":\"%s\",\"session\":\"%s\",\"date\":\"%s\",\"start\":\"%s\",\"end\":\"%s\"}",first?"":",",row[0]?row[0]:"0",et,es,date_val,start_str,end_str);
        if (used+n+2<MAX_JSON){strcat(json,item);used+=n;first=0;}
    }
    strcat(json,"]"); mysql_free_result(res); send_response(client,"application/json",json); free(json);
}

/* ─── Mark attendance ────────────────────────────────────────────────────────
 * Uses the real client IP (resolved from X-Forwarded-For by handle_request).
 * Two unique constraints are enforced:
 *   unique_prn_session   – same student cannot mark twice for one session
 *   unique_device_session – same device cannot mark twice for one session
 * MySQL error 1062 is checked and the message tells the student exactly which
 * rule was violated.
 */
static void api_mark_attendance(SOCKET client, char *body, const char *client_ip) {
    char raw_prn[50]={0},raw_name[100]={0};
    char prn[100]={0},name[200]={0};
    char safe_prn[200]={0},safe_name[400]={0},safe_ip[100]={0};
    int session_id=0;
    sscanf(body,"prn=%49[^&]&name=%99[^&]&session_id=%d",raw_prn,raw_name,&session_id);
    url_decode(prn,raw_prn,sizeof(prn)); url_decode(name,raw_name,sizeof(name));
    if (session_id<=0||!strlen(prn)) { send_response(client,"application/json","{\"success\":false,\"message\":\"Invalid input. Please go back and try again.\"}"); return; }
    db_escape(safe_prn,prn,sizeof(safe_prn)); db_escape(safe_name,name,sizeof(safe_name)); db_escape(safe_ip,client_ip,sizeof(safe_ip));
    char query[600];
    snprintf(query,sizeof(query),"INSERT INTO attendance(prn,name,session_id,device_ip) VALUES('%s','%s',%d,'%s')",safe_prn,safe_name,session_id,safe_ip);
    if (mysql_query(conn,query)) {
        if (mysql_errno(conn)==1062) {
            const char *dberr = mysql_error(conn);
            if (strstr(dberr,"unique_prn_session"))
                send_response(client,"application/json","{\"success\":false,\"message\":\"Your attendance for this session is already recorded.\"}");
            else if (strstr(dberr,"unique_device_session"))
                send_response(client,"application/json","{\"success\":false,\"message\":\"Attendance has already been marked from this device for this session. Each device may only be used once per session.\"}");
            else
                send_response(client,"application/json","{\"success\":false,\"message\":\"Attendance already marked for this session.\"}");
        } else {
            send_response(client,"application/json","{\"success\":false,\"message\":\"Database error. Please try again.\"}");
        }
        return;
    }
    send_response(client,"application/json","{\"success\":true,\"message\":\"Attendance marked successfully! You may now close this page.\"}");
}

static void api_session_attendance(SOCKET client, const char *query_str) {
    int session_id=0; sscanf(query_str,"session_id=%d",&session_id);
    if (session_id<=0) { send_response(client,"application/json","[]"); return; }
    char query[256]; sprintf(query,"SELECT prn,name,marked_at,device_ip FROM attendance WHERE session_id=%d ORDER BY marked_at",session_id);
    if (mysql_query(conn,query)) { send_response(client,"application/json","[]"); return; }
    MYSQL_RES *res=mysql_store_result(conn); if (!res) { send_response(client,"application/json","[]"); return; }
    char *json=(char*)malloc(MAX_JSON); if (!json) { mysql_free_result(res); send_response(client,"application/json","[]"); return; }
    strcpy(json,"["); int first=1; size_t used=1; MYSQL_ROW row;
    char ep[200]={0},en[400]={0},em[40]={0},ei[100]={0};
    while ((row=mysql_fetch_row(res))) {
        json_escape(ep,sizeof(ep),row[0]?row[0]:""); json_escape(en,sizeof(en),row[1]?row[1]:""); json_escape(em,sizeof(em),row[2]?row[2]:""); json_escape(ei,sizeof(ei),row[3]?row[3]:"");
        char item[600]; int n=snprintf(item,sizeof(item),"%s{\"prn\":\"%s\",\"name\":\"%s\",\"marked_at\":\"%s\",\"device_ip\":\"%s\"}",first?"":",",ep,en,em,ei);
        if (used+n+2<MAX_JSON){strcat(json,item);used+=n;first=0;}
    }
    strcat(json,"]"); mysql_free_result(res); send_response(client,"application/json",json); free(json);
}

static void api_search_attendance(SOCKET client, const char *query_str) {
    char raw_prn[50]={0},prn[100]={0},safe_prn[200]={0};
    sscanf(query_str,"prn=%49s",raw_prn); url_decode(prn,raw_prn,sizeof(prn)); db_escape(safe_prn,prn,sizeof(safe_prn));
    char query[512]; sprintf(query,"SELECT s.session_name,s.teacher_name,a.marked_at,s.session_date FROM attendance a JOIN sessions s ON a.session_id=s.id WHERE a.prn='%s' ORDER BY a.marked_at DESC",safe_prn);
    if (mysql_query(conn,query)) { send_response(client,"application/json","[]"); return; }
    MYSQL_RES *res=mysql_store_result(conn); if (!res) { send_response(client,"application/json","[]"); return; }
    char *json=(char*)malloc(MAX_JSON); if (!json) { mysql_free_result(res); send_response(client,"application/json","[]"); return; }
    strcpy(json,"["); int first=1; size_t used=1; MYSQL_ROW row;
    char es[400]={0},et[400]={0},em[40]={0},ed[20]={0};
    while ((row=mysql_fetch_row(res))) {
        json_escape(es,sizeof(es),row[0]?row[0]:""); json_escape(et,sizeof(et),row[1]?row[1]:""); json_escape(em,sizeof(em),row[2]?row[2]:""); json_escape(ed,sizeof(ed),row[3]?row[3]:"");
        char item[700]; int n=snprintf(item,sizeof(item),"%s{\"session_name\":\"%s\",\"teacher_name\":\"%s\",\"marked_at\":\"%s\",\"session_date\":\"%s\"}",first?"":",",es,et,em,ed);
        if (used+n+2<MAX_JSON){strcat(json,item);used+=n;first=0;}
    }
    strcat(json,"]"); mysql_free_result(res); send_response(client,"application/json",json); free(json);
}

static void api_dashboard_stats(SOCKET client) {
    int ts=0,tse=0,ta=0;
    MYSQL_RES *res; MYSQL_ROW row;
    if (!mysql_query(conn,"SELECT COUNT(*) FROM students"))   { res=mysql_store_result(conn); if(res&&(row=mysql_fetch_row(res))){ts=atoi(row[0]?row[0]:"0");} if(res)mysql_free_result(res); }
    if (!mysql_query(conn,"SELECT COUNT(*) FROM sessions"))   { res=mysql_store_result(conn); if(res&&(row=mysql_fetch_row(res))){tse=atoi(row[0]?row[0]:"0");} if(res)mysql_free_result(res); }
    if (!mysql_query(conn,"SELECT COUNT(*) FROM attendance")) { res=mysql_store_result(conn); if(res&&(row=mysql_fetch_row(res))){ta=atoi(row[0]?row[0]:"0");} if(res)mysql_free_result(res); }
    char resp[128]; sprintf(resp,"{\"total_students\":%d,\"total_sessions\":%d,\"total_attendance\":%d}",ts,tse,ta);
    send_response(client,"application/json",resp);
}

static void api_delete_student(SOCKET client, char *body) {
    char raw_prn[50]={0},prn[100]={0},safe_prn[200]={0};
    char *p=strstr(body,"prn=");
    if (p) { p+=4; char *e=strstr(p,"&"); if(e){int l=(int)(e-p);if(l>0&&l<50)strncpy(raw_prn,p,l);}else strncpy(raw_prn,p,49); }
    url_decode(prn,raw_prn,sizeof(prn));
    if (!strlen(prn)) { send_response(client,"application/json","{\"success\":false,\"message\":\"PRN is required.\"}"); return; }
    db_escape(safe_prn,prn,sizeof(safe_prn));
    char q1[256],q2[256];
    sprintf(q1,"DELETE FROM attendance WHERE prn='%s'",safe_prn);
    sprintf(q2,"DELETE FROM students WHERE prn='%s'",safe_prn);
    mysql_query(conn,q1);
    if (mysql_query(conn,q2)) { send_response(client,"application/json","{\"success\":false,\"message\":\"Database error. Could not delete student.\"}"); return; }
    send_response(client,"application/json","{\"success\":true,\"message\":\"Student and all attendance records deleted.\"}");
}

static void api_delete_attendance(SOCKET client, char *body) {
    int sid=0; sscanf(body,"session_id=%d",&sid);
    if (sid<=0) { send_response(client,"application/json","{\"success\":false,\"message\":\"Invalid session ID.\"}"); return; }
    char q[128]; sprintf(q,"DELETE FROM attendance WHERE session_id=%d",sid);
    if (mysql_query(conn,q)) { send_response(client,"application/json","{\"success\":false,\"message\":\"Database error.\"}"); return; }
    send_response(client,"application/json","{\"success\":true,\"message\":\"All attendance records for this session have been cleared.\"}");
}

static void api_delete_session(SOCKET client, char *body) {
    int sid=0; sscanf(body,"session_id=%d",&sid);
    if (sid<=0) { send_response(client,"application/json","{\"success\":false,\"message\":\"Invalid session ID.\"}"); return; }
    char q1[128],q2[128];
    sprintf(q1,"DELETE FROM attendance WHERE session_id=%d",sid);
    sprintf(q2,"DELETE FROM sessions WHERE id=%d",sid);
    mysql_query(conn,q1); mysql_query(conn,q2);
    rebuild_interval_tree();
    send_response(client,"application/json","{\"success\":true,\"message\":\"Session and all its attendance records deleted.\"}");
}

static void api_remove_attendance_record(SOCKET client, char *body) {
    int sid=0; char raw_prn[50]={0},prn[100]={0},safe_prn[200]={0};
    sscanf(body,"session_id=%d&prn=%49s",&sid,raw_prn);
    url_decode(prn,raw_prn,sizeof(prn));
    if (sid<=0||!strlen(prn)) { send_response(client,"application/json","{\"success\":false,\"message\":\"Invalid input.\"}"); return; }
    db_escape(safe_prn,prn,sizeof(safe_prn));
    char q[256]; sprintf(q,"DELETE FROM attendance WHERE session_id=%d AND prn='%s'",sid,safe_prn);
    if (mysql_query(conn,q)) { send_response(client,"application/json","{\"success\":false,\"message\":\"Database error.\"}"); return; }
    send_response(client,"application/json","{\"success\":true,\"message\":\"Attendance record removed. Student marked absent.\"}");
}

static void api_teacher_mark_attendance(SOCKET client, char *body) {
    char raw_prn[50]={0},prn[100]={0};
    char safe_prn[200]={0},safe_name[400]={0};
    int session_id=0;
    sscanf(body,"session_id=%d&prn=%49s",&session_id,raw_prn);
    url_decode(prn,raw_prn,sizeof(prn));
    if (session_id<=0||!strlen(prn)) { send_response(client,"application/json","{\"success\":false,\"message\":\"Invalid input.\"}"); return; }
    db_escape(safe_prn,prn,sizeof(safe_prn));
    char name_query[256];
    sprintf(name_query,"SELECT name FROM students WHERE prn='%s'",safe_prn);
    if (mysql_query(conn,name_query)) { send_response(client,"application/json","{\"success\":false,\"message\":\"Database error.\"}"); return; }
    MYSQL_RES *name_res=mysql_store_result(conn);
    if (!name_res) { send_response(client,"application/json","{\"success\":false,\"message\":\"Database error.\"}"); return; }
    MYSQL_ROW name_row=mysql_fetch_row(name_res);
    if (!name_row) { mysql_free_result(name_res); send_response(client,"application/json","{\"success\":false,\"message\":\"Student not found. Please check the PRN.\"}"); return; }
    char name[200]={0};
    strncpy(name,name_row[0]?name_row[0]:"",sizeof(name)-1);
    mysql_free_result(name_res);
    db_escape(safe_name,name,sizeof(safe_name));
    char device_ip_val[80]={0};
    snprintf(device_ip_val, sizeof(device_ip_val), "teacher-%s", prn);
    char safe_device[200]={0};
    db_escape(safe_device, device_ip_val, sizeof(safe_device));
    char query[600];
    snprintf(query,sizeof(query),
        "INSERT INTO attendance(prn,name,session_id,device_ip) VALUES('%s','%s',%d,'%s')",
        safe_prn,safe_name,session_id,safe_device);
    if (mysql_query(conn,query)) {
        if (mysql_errno(conn)==1062)
            send_response(client,"application/json","{\"success\":false,\"message\":\"This student's attendance is already recorded for this session.\"}");
        else
            send_response(client,"application/json","{\"success\":false,\"message\":\"Database error.\"}");
        return;
    }
    send_response(client,"application/json","{\"success\":true,\"message\":\"Student marked present.\"}");
}

static void api_update_face_status(SOCKET client, char *body) {
    char raw_prn[50]={0},prn[100]={0},safe_prn[200]={0};
    int status=0;
    char *p=strstr(body,"prn=");
    if(p){p+=4;char *e=strstr(p,"&");if(e){int l=(int)(e-p);if(l>0&&l<50)strncpy(raw_prn,p,l);}else strncpy(raw_prn,p,49);}
    p=strstr(body,"status=");
    if(p){p+=7;status=atoi(p);}
    url_decode(prn,raw_prn,sizeof(prn));
    if (!strlen(prn)) { send_response(client,"application/json","{\"success\":false,\"message\":\"PRN required.\"}"); return; }
    db_escape(safe_prn,prn,sizeof(safe_prn));
    char *img_start = strstr(body, "face_image=");
    if (img_start) {
        img_start += 11;
        size_t img_raw_len = strlen(img_start);
        char *img_decoded  = (char*)malloc(img_raw_len + 1);
        if (img_decoded) {
            url_decode(img_decoded, img_start, img_raw_len + 1);
            char tmp_filename[64];
            snprintf(tmp_filename, sizeof(tmp_filename), "face_tmp_%s.b64", prn);
            FILE *f = fopen(tmp_filename, "w");
            if (f) {
                fputs(img_decoded, f);
                fclose(f);
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "python save_face_image.py \"%s\" \"%s\"", prn, tmp_filename);
                system(cmd);
                remove(tmp_filename);
            }
            free(img_decoded);
        }
    }
    char query[256];
    sprintf(query,"UPDATE students SET face_registered=%d WHERE prn='%s'",status,safe_prn);
    if (mysql_query(conn,query)) {
        char err[300];
        sprintf(err,"{\"success\":false,\"message\":\"Database error: %s\"}",mysql_error(conn));
        send_response(client,"application/json",err);
        return;
    }
    send_response(client,"application/json","{\"success\":true,\"message\":\"Face registered successfully!\"}");
}

static void api_verify_face(SOCKET client, char *body) {
    char raw_prn[50]={0}, prn[100]={0}, safe_prn[200]={0};
    char *p = strstr(body, "prn=");
    if (p) {
        p += 4;
        char *e = strstr(p, "&");
        if (e) { int l=(int)(e-p); if(l>0&&l<50) strncpy(raw_prn,p,l); }
        else strncpy(raw_prn, p, 49);
    }
    url_decode(prn, raw_prn, sizeof(prn));
    if (!strlen(prn)) {
        send_response(client,"application/json","{\"success\":false,\"verified\":false,\"message\":\"PRN required.\"}"); return;
    }
    db_escape(safe_prn, prn, sizeof(safe_prn));
    char chk[256];
    sprintf(chk,"SELECT face_registered FROM students WHERE prn='%s'",safe_prn);
    if (mysql_query(conn,chk)) { send_response(client,"application/json","{\"success\":false,\"verified\":false,\"message\":\"Database error.\"}"); return; }
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) { send_response(client,"application/json","{\"success\":false,\"verified\":false,\"message\":\"Database error.\"}"); return; }
    MYSQL_ROW row = mysql_fetch_row(res);
    int face_reg = row ? atoi(row[0]?row[0]:"0") : 0;
    mysql_free_result(res);
    if (!face_reg) {
        send_response(client,"application/json",
            "{\"success\":true,\"verified\":false,\"face_registered\":false,"
            "\"message\":\"Face not registered. Please register your face first using the link sent to your email.\"}");
        return;
    }
    char *img_start = strstr(body, "face_image=");
    if (!img_start) {
        send_response(client,"application/json","{\"success\":false,\"verified\":false,\"message\":\"No face image provided.\"}"); return;
    }
    img_start += 11;
    size_t img_raw_len = strlen(img_start);
    char *img_decoded  = (char*)malloc(img_raw_len + 1);
    if (!img_decoded) {
        send_response(client,"application/json","{\"success\":false,\"verified\":false,\"message\":\"Out of memory.\"}"); return;
    }
    url_decode(img_decoded, img_start, img_raw_len + 1);
    char tmp_b64[64];
    snprintf(tmp_b64, sizeof(tmp_b64), "verify_tmp_%s.b64", prn);
    FILE *f = fopen(tmp_b64, "w");
    if (!f) { free(img_decoded); send_response(client,"application/json","{\"success\":false,\"verified\":false,\"message\":\"Temp file error.\"}"); return; }
    fputs(img_decoded, f);
    fclose(f);
    free(img_decoded);
    char result_file[64];
    snprintf(result_file, sizeof(result_file), "verify_result_%s.json", prn);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "python verify_face.py \"%s\" \"%s\" \"%s\"", prn, tmp_b64, result_file);
    system(cmd);
    remove(tmp_b64);
    FILE *rf = fopen(result_file, "r");
    if (!rf) {
        send_response(client,"application/json",
            "{\"success\":true,\"verified\":true,\"face_registered\":true,"
            "\"message\":\"Verification service unavailable — proceeding.\"}");
        return;
    }
    char result_buf[512]={0};
    fread(result_buf, 1, sizeof(result_buf)-1, rf);
    fclose(rf);
    remove(result_file);
    if (strlen(result_buf) > 0) {
        send_response(client, "application/json", result_buf);
    } else {
        send_response(client,"application/json",
            "{\"success\":true,\"verified\":true,\"face_registered\":true,\"message\":\"Face verified.\"}");
    }
}

static void api_send_session_emails(SOCKET client, const char *query_str) {
    int session_id=0; sscanf(query_str,"session_id=%d",&session_id);
    if (session_id<=0) { send_response(client,"application/json","{\"success\":false,\"message\":\"Invalid session ID.\"}"); return; }
    char cmd[256]; snprintf(cmd,sizeof(cmd),"python send_emails.py %d",session_id);
    int ret=system(cmd);
    if (ret==0) send_response(client,"application/json","{\"success\":true,\"message\":\"Session emails sent successfully to all students.\"}");
    else        send_response(client,"application/json","{\"success\":false,\"message\":\"Email sending failed. Check server logs for details.\"}");
}

static void api_send_face_reg_email(SOCKET client, const char *query_str) {
    char raw_prn[100]={0},raw_name[200]={0},raw_email[200]={0};
    char prn[100]={0},name[200]={0},email[200]={0};
    char *p;
    p=strstr(query_str,"prn=");    if(p){p+=4;char*e=strstr(p,"&");if(e){int l=(int)(e-p);if(l>0&&l<100)strncpy(raw_prn,p,l);}else strncpy(raw_prn,p,99);}
    p=strstr(query_str,"&name=");  if(p){p+=6;char*e=strstr(p,"&");if(e){int l=(int)(e-p);if(l>0&&l<200)strncpy(raw_name,p,l);}else strncpy(raw_name,p,199);}
    p=strstr(query_str,"&email="); if(p){p+=7;strncpy(raw_email,p,199);}
    url_decode(prn,raw_prn,sizeof(prn)); url_decode(name,raw_name,sizeof(name)); url_decode(email,raw_email,sizeof(email));
    if (!strlen(prn)||!strlen(email)) { send_response(client,"application/json","{\"success\":false,\"message\":\"Invalid PRN or email address.\"}"); return; }
    char server_ip[64]; get_server_ip(server_ip,sizeof(server_ip));
    char cmd[1024];
    snprintf(cmd,sizeof(cmd),"python send_face_reg_email.py \"%s\" \"%s\" \"%s\" \"%s\"",prn,name,email,server_ip);
    int ret=system(cmd);
    char resp[512];
    if (ret==0) snprintf(resp,sizeof(resp),"{\"success\":true,\"message\":\"Face registration email sent to %s (%s).\"}",name,email);
    else        snprintf(resp,sizeof(resp),"{\"success\":false,\"message\":\"Email sending failed for %s (%s). Check SMTP settings.\"}",name,email);
    send_response(client,"application/json",resp);
}

/* ─── Main request dispatcher ────────────────────────────────────────────────
 * Resolves the real client IP before dispatching to any handler that needs it.
 * This is critical when the HTTPS proxy is in use — without it, every student
 * looks like 127.0.0.1 and the device-uniqueness constraint fires after the
 * very first attendance submission.
 */
void handle_request(SOCKET client, char *req, const char *socket_ip) {
    char method[10]={0},path[512]={0};
    sscanf(req,"%9s %511s",method,path);
    printf("[%s] %s\n",method,path);
    char *query_str=strchr(path,'?');
    if (query_str){*query_str='\0';query_str++;}else query_str="";
    char *body=strstr(req,"\r\n\r\n");
    if (body) body+=4; else body="";

    /* Resolve the real client IP (from X-Forwarded-For if behind proxy) */
    char real_ip[46]={0};
    extract_real_ip(req, socket_ip, real_ip, sizeof(real_ip));

    if (strcmp(path,"/")==0||strcmp(path,"/teacher")==0||strcmp(path,"/teacher.html")==0) {
        char *html=(char*)malloc(BUFFER_SIZE); read_file("teacher.html",html); send_response(client,"text/html",html); free(html); return; }
    if (strcmp(path,"/student")==0||strcmp(path,"/student.html")==0) {
        char *html=(char*)malloc(BUFFER_SIZE); read_file("student.html",html); send_response(client,"text/html",html); free(html); return; }
    if (strcmp(path,"/face-register")==0||strcmp(path,"/face-register.html")==0) {
        char *html=(char*)malloc(BUFFER_SIZE); read_file("face-register.html",html); send_response(client,"text/html",html); free(html); return; }
    if (strcmp(path,"/favicon.ico")==0) { send_response(client,"text/plain",""); return; }

    if (strcmp(path,"/api/register_student_manual")==0)   { api_register_student(client,body);                   return; }
    if (strcmp(path,"/api/register_students")==0)          { api_register_students_csv(client,body);              return; }
    if (strcmp(path,"/api/get_all_students")==0)           { api_get_students(client);                            return; }
    if (strcmp(path,"/api/delete_student")==0)             { api_delete_student(client,body);                     return; }
    if (strcmp(path,"/api/create_session")==0)             { api_create_session(client,body);                     return; }
    if (strcmp(path,"/api/get_sessions")==0)               { api_get_sessions(client);                            return; }
    if (strcmp(path,"/api/delete_session")==0)             { api_delete_session(client,body);                     return; }
    if (strcmp(path,"/api/mark_attendance")==0)            { api_mark_attendance(client,body,real_ip);            return; }
    if (strcmp(path,"/api/session_attendance")==0)         { api_session_attendance(client,query_str);            return; }
    if (strcmp(path,"/api/search_attendance")==0)          { api_search_attendance(client,query_str);             return; }
    if (strcmp(path,"/api/delete_attendance")==0)          { api_delete_attendance(client,body);                  return; }
    if (strcmp(path,"/api/remove_attendance_record")==0)   { api_remove_attendance_record(client,body);           return; }
    if (strcmp(path,"/api/teacher_mark_attendance")==0)    { api_teacher_mark_attendance(client,body);            return; }
    if (strcmp(path,"/api/dashboard_stats")==0)            { api_dashboard_stats(client);                         return; }
    if (strcmp(path,"/api/update_face_status")==0)         { api_update_face_status(client,body);                 return; }
    if (strcmp(path,"/api/verify_face")==0)                { api_verify_face(client,body);                        return; }
    if (strcmp(path,"/api/send_session_emails")==0)        { api_send_session_emails(client,query_str);           return; }
    if (strcmp(path,"/api/send_face_reg_email")==0)        { api_send_face_reg_email(client,query_str);           return; }

    send_404(client);
}

static int recv_full(SOCKET client, char *buffer, int max) {
    int total=0,bytes,content_length=-1;
    char *body_start=NULL;
    while (total<max-1) {
        bytes=recv(client,buffer+total,max-1-total,0);
        if (bytes<=0) break;
        total+=bytes; buffer[total]='\0';
        if (!body_start) {
            body_start=strstr(buffer,"\r\n\r\n");
            if (body_start) {
                body_start+=4;
                char *cl=strstr(buffer,"Content-Length:");
                if (!cl) cl=strstr(buffer,"content-length:");
                if (cl) { cl+=15; while(*cl==' ')cl++; content_length=atoi(cl); }
                else content_length=0;
            }
        }
        if (body_start&&content_length>=0) {
            int body_received=total-(int)(body_start-buffer);
            if (body_received>=content_length) break;
        }
    }
    buffer[total]='\0'; return total;
}

int main(void) {
    WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa);
    init_database();

    SOCKET server_socket=socket(AF_INET,SOCK_STREAM,0);
    int opt=1; setsockopt(server_socket,SOL_SOCKET,SO_REUSEADDR,(const char*)&opt,sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr,0,sizeof(server_addr));
    server_addr.sin_family=AF_INET; server_addr.sin_addr.s_addr=INADDR_ANY; server_addr.sin_port=htons(PORT);
    bind(server_socket,(struct sockaddr*)&server_addr,sizeof(server_addr));
    listen(server_socket,10);

    char ip[64]; get_server_ip(ip,sizeof(ip));
    printf("\n========================================\n");
    printf("Smart Attendance System Server Running\n");
    printf("========================================\n");
    printf("Teacher Portal (HTTP):   http://127.0.0.1:%d\n", PORT);
    printf("Teacher Portal (local):  http://%s:%d\n", ip, PORT);
    printf("----------------------------------------\n");
    printf("HTTPS Proxy (for students):\n");
    printf("  Run:  python https_proxy.py\n");
    printf("  Then share: https://%s:%d\n", ip, HTTPS_PORT);
    printf("========================================\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        int client_addr_len=(int)sizeof(client_addr);
        SOCKET client=accept(server_socket,(struct sockaddr*)&client_addr,&client_addr_len);
        if (client==INVALID_SOCKET) continue;
        char socket_ip[46]="unknown";
        inet_ntop(AF_INET,&client_addr.sin_addr,socket_ip,sizeof(socket_ip));

        char *buffer=(char*)malloc(BODY_BUFFER_SIZE);
        if (!buffer){closesocket(client);continue;}
        int bytes=recv_full(client,buffer,BODY_BUFFER_SIZE);
        if (bytes>0) handle_request(client,buffer,socket_ip);
        free(buffer);
        closesocket(client);
    }

    closesocket(server_socket);
    it_free(it_root); mysql_close(conn); WSACleanup();
    return 0;
}