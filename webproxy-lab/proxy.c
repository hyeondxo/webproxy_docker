//  CS:APP Proxy Lab - Part 1 (Sequential Proxy)
//   - 요구사항: HTTP/1.0 기반 GET 프록시, 헤더 재작성(Host/User-Agent/Connection/
//   Proxy-Connection), 바이너리 안전 응답 중계, 동시성/캐시 없음

#include "csapp.h"  // RIO(견고한 I/O), 소켓 래퍼(Open_listenfd 등), 에러 처리 매크로 포함
#include <ctype.h>  // isdigit 등 문자인식 매크로
#include <errno.h>  // errno 상수
#include <signal.h> // sigaction, SIGPIPE 무시 설정

// 과제에서 지정한 고정 User-Agent 헤더 문자열
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
                                    "Gecko/20120305 Firefox/10.0.3\r\n";

// 연결 정책: 프록시-서버/클라이언트 모두 연결은 요청마다 종료(close)
static const char *conn_close_hdr = "Connection: close\r\n";
static const char *proxy_conn_close_hdr = "Proxy-Connection: close\r\n";

// 내부 사용 함수 원형 선언
static void handle_client(int connfd); // 클라이언트 1건 처리(요청 읽기 -> 서버로 전달 -> 응답 중계)
static int parse_request_line(const char *line, char *method, size_t msz, char *uri, size_t usz, char *version,
                              size_t vsz); // "METHOD URI VERSION" 파싱
static int parse_uri(const char *uri, char *host, size_t hsz, char *path, size_t psz,
                     int *port_out);                       // "http://host[:port]/path" 분해
static int connect_end_server(const char *host, int port); // 원서버에 TCP connect()
static int forward_request_headers(rio_t *client_rio, int serverfd, const char *host, int port); // 헤더 재작성/전송
static void relay_response(int serverfd, int clientfd);                                 // 서버->클라 응답 스트리밍
static void clienterror(int fd, int status, const char *shortmsg, const char *longmsg); // 간단한 에러 응답 생성
static int open_listenfd_s(const char *port);                                           // getaddrinfo 기반 리스닝 소켓
static ssize_t writen_all(int fd, const void *buf, size_t n);      // 부분쓰기까지 처리하는 write 루프
static int read_full_line(rio_t *rp, char **out, size_t *len_out); // 1줄을 끝까지 모아 반환(RIO 사용)

// 리스닝 소켓 생성
// SIGPIPE 무시(클라이언트/서버 조기 종료 시 write에서 죽지 않도록)
// accept 루프에서 순차적으로 연결 1건씩 처리
int main(int argc, char **argv) {
    int listenfd;                       // 리스닝 소켓 FD
    int connfd;                         // accept로 얻는 연결 소켓 FD
    socklen_t clientlen;                // 클라이언트 주소 길이
    struct sockaddr_storage clientaddr; // IPv4/IPv6 겸용 주소 구조체
    struct sigaction sa;                // SIGPIPE 무시 설정용

    if (argc != 2) { // 포트 인자 필수
        fprintf(stderr, "Usage: %s <listen_port>\n", argv[0]);
        exit(1);
    }
    // SIGPIPE : 소켓이 끊어진 상태에서 write 시도 시 프로세스 종료 기본 동작
    // -> 무시하도록 설정. write 오류는 -1 반환과 errno=EPIPE로 알 수 있음
    memset(&sa, 0, sizeof(sa));    // sa 구조체 초기화
    sa.sa_handler = SIG_IGN;       // SIGPIPE를 무시
    sigemptyset(&sa.sa_mask);      // 빈 시그널 집합으로 초기화
    sigaction(SIGPIPE, &sa, NULL); // SIGPIPE에 대해 sa 설정

    listenfd = open_listenfd_s(argv[1]); // 리스닝 소켓 생성
    if (listenfd < 0) {                  // 실패 시 에러 출력 후 종료
        fprintf(stderr, "Error: cannot open listen socket on port %s\n", argv[1]);
        exit(1);
    }

    for (;;) {                          // 무한 루프: 순차 처리
        clientlen = sizeof(clientaddr); // 주소 버퍼 크기 지정
        do {
            // accept는 시그널로 깨어나면 EINTR 반환 가능 -> 재시도
            connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        } while (connfd < 0 && errno == EINTR);

        if (connfd < 0) { // 기타 에러는 로그만 찍고 다음 연결 대기
            fprintf(stderr, "accept error: %s\n", strerror(errno));
            continue;
        }

        handle_client(connfd); // 단일 요청 처리
        close(connfd);         // 처리 후 반드시 소켓 닫기
    }

    close(listenfd); // 도달하지 않지만 정리
    return 0;
}

//  handle_client: 프록시의 핵심 처리
//   - 요청 라인 읽기/검증(GET만)
//   - 절대 URI 파싱 -> host/port/path 추출
//   - 원서버 connect
//   - 서버로 요청라인/헤더 전송(HTTP/1.0으로 다운그레이드 + 헤더 재작성)
//   - 서버 응답을 바이너리 안전하게 클라이언트로 중계
static void handle_client(int connfd) {
    char reqline[MAXLINE];                                // 요청라인 버퍼
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE]; // 파싱된 3요소
    char host[MAXLINE], path[MAXLINE];                    // URI에서 뽑은 host/path
    int port = 80;                                        // URI 포트(기본 80)
    rio_t rio_client;                                     // 클라이언트 RIO 버퍼
    int serverfd = -1;                                    // 원서버 소켓 FD

    rio_readinitb(&rio_client, connfd); // 클라 소켓에 대해 RIO 초기화(부분읽기 안전)

    // 요청 라인 읽기
    ssize_t n = rio_readlineb(&rio_client, reqline, sizeof(reqline));
    if (n <= 0) // EOF/오류 -> 조용히 종료(브라우저가 먼저 끊었을 수 있음)
        return;

    // METHOD URI VERSION 파싱
    if (parse_request_line(reqline, method, sizeof(method), uri, sizeof(uri), version, sizeof(version)) < 0) {
        clienterror(connfd, 400, "Bad Request", "Malformed request line"); // 400
        return;
    }

    // GET 외 메서드 거부(Part 1 범위)
    if (strcasecmp(method, "GET") != 0) {
        clienterror(connfd, 501, "Not Implemented", "Proxy does not implement this method"); // 501
        return;
    }

    // 절대 URI만 허용 (http://host[:port]/path)
    if (parse_uri(uri, host, sizeof(host), path, sizeof(path), &port) < 0) {
        clienterror(connfd, 400, "Bad Request", "Only supports absolute HTTP URLs"); // 400
        return;
    }

    // 원서버 TCP 연결 시도
    serverfd = connect_end_server(host, port);
    if (serverfd < 0) {
        clienterror(connfd, 502, "Bad Gateway", "Failed to connect to end server"); // 502
        return;
    }

    // 요청 라인 전송: HTTP/1.0으로 다운그레이드(프록시 스펙)
    {
        char outline[MAXLINE];
        int len = snprintf(outline, sizeof(outline), "GET %s HTTP/1.0\r\n", path);
        // snprintf 실패/버퍼 초과/전송 실패 시 502
        if (len < 0 || (size_t)len >= sizeof(outline) || writen_all(serverfd, outline, (size_t)len) < 0) {
            close(serverfd); // 원서버 소켓 닫기
            clienterror(connfd, 502, "Bad Gateway", "Failed to write request line");
            return;
        }
    }

    // 헤더 재작성/전송 (Host/User-Agent/Connection/Proxy-Connection 정책)
    if (forward_request_headers(&rio_client, serverfd, host, port) < 0) {
        close(serverfd);
        clienterror(connfd, 400, "Bad Request", "Invalid request headers");
        return;
    }

    // 서버 응답을 클라이언트로 스트리밍(바이너리 안전)
    relay_response(serverfd, connfd);

    // 원서버 소켓 정리
    close(serverfd);
}

// parse_request_line: METHOD URI VERSION를 공백 구분으로 파싱
// - 3개 토큰이 모두 있어야 함
// - 출력 버퍼 크기 점검 후 복사
static int parse_request_line(const char *line, char *method, size_t msz, char *uri, size_t usz, char *version,
                              size_t vsz) {
    char m[MAXLINE], u[MAXLINE], v[MAXLINE];
    if (sscanf(line, "%s %s %s", m, u, v) != 3) // 토큰 3개 필수
        return -1;
    if (strlen(m) >= msz || strlen(u) >= usz || strlen(v) >= vsz)
        return -1; // 버퍼 초과 방지
    // 안전하게 복사
    strcpy(method, m);
    strcpy(uri, u);
    strcpy(version, v);
    return 0;
}

// parse_uri: "http://host[:port]/path" 형태만 지원(HTTPS/상대경로 불가)
//  - host, port(기본 80), path(없으면 "/") 추출
//  - 유효성 체크: 호스트 비어있지 않아야 함, 포트는 1~65535, 경로 길이 등
static int parse_uri(const char *uri, char *host, size_t hsz, char *path, size_t psz, int *port_out) {
    const char *p;                 // 사용 안하지만 예비 포인터(가독성)
    const char *host_begin;        // 호스트 시작
    const char *host_end;          // 호스트 끝(':', '/', '\0' 중 하나)
    const char *port_begin = NULL; // 포트 시작(있다면 ':')
    const char *path_begin;        // 경로 시작('/')
    int port = 80;                 // 기본 포트

    if (strncasecmp(uri, "http://", 7) != 0) // 스킴은 http만
        return -1;

    host_begin = uri + 7; // "http://" 뒤부터 호스트
    host_end = host_begin;
    while (*host_end && *host_end != ':' && *host_end != '/')
        host_end++; // 호스트 끝 위치 찾기

    size_t host_len = (size_t)(host_end - host_begin);
    if (host_len == 0 || host_len >= hsz) // 빈 호스트/버퍼 초과
        return -1;

    memcpy(host, host_begin, host_len); // 호스트 복사
    host[host_len] = '\0';

    if (*host_end == ':') {        // 포트 명시된 경우
        port_begin = host_end + 1; // 포트 숫자 시작
        port = 0;                  // 포트 초기화
        // 숫자가 아닌 문자가 나오거나 끝날때까지 반복
        while (*port_begin && isdigit((unsigned char)*port_begin)) {
            port = port * 10 + (*port_begin - '0'); // 10진수 파싱
            port_begin++;
        }
        if (port <= 0 || port > 65535) // 포트 유효성 검사
            return -1;
        path_begin = (*port_begin == '/') ? port_begin : NULL; // 포트 뒤 '/' 유무 확인
    } else {
        path_begin = (*host_end == '/') ? host_end : NULL; // ':' 없으면 host_end가 '/'일 수도
    }

    if (!path_begin) { // 경로 없으면 "/"
        if (psz < 2)   // "/" 넣을 공간도 없으면 실패
            return -1;
        strcpy(path, "/");
    } else {
        if (strlen(path_begin) >= psz) // 경로 전체 길이 체크
            return -1;
        strcpy(path, path_begin);
    }

    *port_out = port; // 출력 포트 설정
    return 0;
}

// forward_request_headers: 클라이언트 헤더를 서버로 전달(필수 헤더 재작성)
//  - 필터링: User-Agent / Connection / Proxy-Connection -> 무시 후 나중에 고정값 추가
//  - Host: 있으면 그대로 전달, 없으면 생성해서 추가
//  - 나머지 헤더는 그대로 서버로 전달
//  - 마지막에 강제 헤더(User-Agent/Connection/Proxy-Connection) + 빈 줄 전송
static int forward_request_headers(rio_t *client_rio, int serverfd, const char *host, int port) {
    int saw_host = 0; // Host 헤더를 봤는지

    for (;;) {
        char *line = NULL;  // 가변 길이 라인 버퍼(동적)
        size_t linelen = 0; // 읽은 라인 길이

        // 한 줄을 끝까지 읽기
        if (read_full_line(client_rio, &line, &linelen) < 0)
            return -1; // 읽기 실패(EOF/오류)

        // 헤더 종료(빈 줄 \r\n 또는 \n) → 루프 탈출
        if ((linelen == 2 && line[0] == '\r' && line[1] == '\n') || (linelen == 1 && line[0] == '\n')) {
            free(line);
            break;
        }

        // Host:는 원본 유지(있으면 전달, saw_host=1)
        if (!strncasecmp(line, "Host:", 5)) {
            saw_host = 1;
            if (writen_all(serverfd, line, linelen) < 0) {
                free(line);
                return -1;
            }
            free(line);
            continue;
        }

        // User-Agent / Connection / Proxy-Connection 은 제거
        if (!strncasecmp(line, "User-Agent:", 11) || !strncasecmp(line, "Connection:", 11) ||
            !strncasecmp(line, "Proxy-Connection:", 17)) {
            free(line);
            continue; // 나중에 고정 헤더로 대체 전송
        }

        // 그 외 헤더는 수정 없이 그대로 서버로 전달
        if (writen_all(serverfd, line, linelen) < 0) {
            free(line);
            return -1;
        }
        free(line);
    }

    // Host가 없었다면 생성해서 추가(포트가 80이 아니면 host:port)
    if (!saw_host) {
        char hosthdr[MAXLINE];
        int len = (port == 80) ? snprintf(hosthdr, sizeof(hosthdr), "Host: %s\r\n", host)
                               : snprintf(hosthdr, sizeof(hosthdr), "Host: %s:%d\r\n", host, port);
        if (len < 0 || (size_t)len >= sizeof(hosthdr))
            return -1;
        if (writen_all(serverfd, hosthdr, (size_t)len) < 0)
            return -1;
    }

    // 고정/강제 헤더 3종 전송 (과제 명세)
    // 1. User-Agent
    // 2. Connection: close
    // 3. Proxy-Connection: close
    if (writen_all(serverfd, user_agent_hdr, strlen(user_agent_hdr)) < 0)
        return -1;
    if (writen_all(serverfd, conn_close_hdr, strlen(conn_close_hdr)) < 0)
        return -1;
    if (writen_all(serverfd, proxy_conn_close_hdr, strlen(proxy_conn_close_hdr)) < 0)
        return -1;

    // 헤더 종료 빈 줄
    if (writen_all(serverfd, "\r\n", 2) < 0)
        return -1;

    return 0;
}

// relay_response: 원서버의 응답을 클라이언트로 그대로 복사(바이너리 안전)
//  - RIO의 rio_readnb는 "정확히 n바이트 이하"를 읽어주며, 내부 버퍼로 부분읽기 안전
//  - writen_all은 부분쓰기 발생 시 끝까지 재시도
static void relay_response(int serverfd, int clientfd) {
    rio_t rio_server; // 서버 소켓용 RIO
    char buf[MAXBUF]; // 큰 전송 버퍼(MAXBUF는 csapp.h 정의)
    ssize_t n;

    rio_readinitb(&rio_server, serverfd); // 서버 FD로 RIO 초기화

    // EOF까지 반복(HTTP/1.0 close 정책이므로 서버가 닫을 때까지)
    while ((n = rio_readnb(&rio_server, buf, sizeof(buf))) > 0) {
        if (writen_all(clientfd, buf, (size_t)n) < 0) {
            break; // 클라 조기 종료같은 상황이면 탈출
        }
    }
}

// clienterror: 간단한 HTML 에러 응답 생성 및 전송
//  - 상태줄/헤더/바디를 만들어 클라이언트에 보냄
static void clienterror(int fd, int status, const char *shortmsg, const char *longmsg) {
    char body[MAXBUF]; // HTML 본문 버퍼
    char hdr[MAXLINE]; // 상태줄+헤더 버퍼

    // HTML 본문 구성
    int bodylen = snprintf(body, sizeof(body),
                           "<html><title>Proxy Error</title>"
                           "<body bgcolor=\"ffffff\">"
                           "%d %s<br>%s"
                           "</body></html>",
                           status, shortmsg, longmsg ? longmsg : "");
    if (bodylen < 0) // snprintf 실패
        return;

    // 상태줄/헤더 구성(HTTP/1.0, close)
    int len = snprintf(hdr, sizeof(hdr),
                       "HTTP/1.0 %d %s\r\n"
                       "Content-type: text/html\r\n"
                       "Connection: close\r\n"
                       "Content-length: %d\r\n\r\n",
                       status, shortmsg, bodylen);
    if (len < 0)
        return;

    // 전송
    writen_all(fd, hdr, (size_t)len);
    writen_all(fd, body, (size_t)bodylen);
}

// connect_end_server: DNS 해석 + TCP connect
//  - getaddrinfo로 (IPv4/IPv6) 후보 목록을 받고 차례대로 connect 시도
//  - 성공하면 그 소켓 FD 반환, 실패하면 -1
static int connect_end_server(const char *host, int port) {
    int clientfd = -1; // 성공하면 이 FD 반환. 소켓 fd
    // hints : getaddrinfo 호출 시 원하는 조건을 지정하는 입력 구조체
    // listp : getaddrinfo가 돌려주는 결과 리스트의 시작 포인터 (여러 연결 후보 주소들)
    struct addrinfo hints, *listp = NULL, *p;
    char portstr[16]; // 포트 번호를 문자열로 담는 버퍼
    int rc;           // 함수 호출의 반환 코드를 임시로 보관하는 변수

    memset(&hints, 0, sizeof(hints));                // hints 구조체 초기화
    hints.ai_socktype = SOCK_STREAM;                 // TCP
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV; // 로컬 주소 구성 고려, 서비스는 숫자(포트)

    snprintf(portstr, sizeof(portstr), "%d", port); // 포트 번호 문자열로 변환

    rc = getaddrinfo(host, portstr, &hints, &listp); // DNS 해석
    if (rc != 0)                                     // DNS/AI 에러
        return -1;

    for (p = listp; p != NULL; p = p->ai_next) {                         // 후보 주소 순회
        clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol); // 소켓 생성
        if (clientfd < 0)
            continue; // 생성 실패 → 다음 후보

        if (connect(clientfd, p->ai_addr, p->ai_addrlen) == 0) {
            break; // 연결 성공
        }
        close(clientfd); // 실패 시 닫고 다음 후보
        clientfd = -1;
    }

    freeaddrinfo(listp); // 할당 해제
    return clientfd;     // 성공 FD 또는 -1
}

// open_listenfd_s: getaddrinfo 기반 리스닝 소켓 생성
// - AI_PASSIVE로 서버 소켓 바인드 주소 획득
// - SO_REUSEADDR로 빠른 재바인드 허용
static int open_listenfd_s(const char *port) {
    int listenfd = -1;
    struct addrinfo hints, *listp = NULL, *p;
    int optval = 1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM; // TCP

    // AI_PASSIVE: 서버(리스닝) 소켓용 주소를 요청.
    // 호스트명을 NULL로 주면 모든 인터페이스(INADDR_ANY/::)에 바인드할 수 있는 바인드용 주소를 돌려줌
    // AI_ADDRCONFIG: 로컬 호스트가 실제로 구성한 주소 패밀리만 돌려달라는 의미.
    // 예를 들어 IPv6가 없으면 IPv6 주소는 제외. 불필요한 주소 후보를 줄여 실패/대기 시간을 줄임
    // AI_NUMERICSERV: 서비스(두 번째 인자)가 “숫자 문자열”임을 명시.
    // 예: "15213"처럼 포트 번호를 이름 해석 없이 그대로 사용해 불필요한 네임 서비스 조회를 피함
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICSERV;

    rc = getaddrinfo(NULL, port, &hints, &listp); // NULL 호스트명으로 바인드용 주소 획득
    if (rc != 0)
        return -1;

    for (p = listp; p != NULL; p = p->ai_next) { // 주소 후보 순회
        // 소켓 생성
        listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listenfd < 0)
            continue;

        // TIME_WAIT 등에서 빠르게 재사용
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0) { // 바인드 성공
            if (listen(listenfd, LISTENQ) == 0) {             // 리슨 성공
                break;
            }
        }

        close(listenfd); // 실패 시 소켓 닫고 다음 후보
        listenfd = -1;
    }

    freeaddrinfo(listp);
    return listenfd; // 성공 FD 또는 -1
}

// writen_all: write의 부분쓰기/시그널 중단을 모두 처리하는 보장된 쓰기
//  - write는 커널 버퍼 여유 등에 따라 일부만 쓰고 돌아올 수 있음 → 남은 만큼 반복
//  - EINTR(시그널로 중단) 시 재시도
//  - 0바이트 쓰기(상대가 이미 닫은 경우)는 EPIPE 준수로 에러 처리
static ssize_t writen_all(int fd, const void *buf, size_t n) {
    size_t left = n;                   // 남은 바이트 수
    const char *p = (const char *)buf; // 진행 포인터
    while (left > 0) {
        ssize_t w = write(fd, p, left); // 최대 left 바이트 시도
        if (w < 0) {
            if (errno == EINTR) // 시그널로 중단 → 다시 시도
                continue;
            return -1; // 기타 에러
        }
        if (w == 0) { // 상대가 끊겨 0을 반환하는 비정상 상황
            errno = EPIPE;
            return -1;
        }
        left -= (size_t)w; // 남은 양 갱신
        p += w;            // 포인터 전진
    }
    return (ssize_t)n; // 요청한 전량을 성공적으로 전송
}

// read_full_line: RIO로 한 줄을 끝까지 읽어 동적 버퍼에 누적해 반환
//  - 헤더 라인은 MAXLINE을 넘지 않지만, 방어적으로 충분히 확장(두 배씩)
//  - 호출자가 free해야 함
static int read_full_line(rio_t *rp, char **out, size_t *len_out) {
    char chunk[MAXLINE]; // RIO 단위 읽기 버퍼
    char *acc = NULL;    // 누적 버퍼(동적)
    size_t cap = 0;      // 누적 버퍼 용량
    size_t len = 0;      // 현재까지 누적 길이

    for (;;) {
        ssize_t n = rio_readlineb(rp, chunk, sizeof(chunk)); // \n까지 읽기(최대 MAXLINE-1)
        if (n <= 0) {                                        // EOF/오류 → 누적 해제 후 실패
            free(acc);
            return -1;
        }

        // 지금까지 누적된 길이 len에, 새로 더할 바이트 수 n과 문자열 널 종료용 1바이트를 합친 값이
        // 현재 용량 cap을 넘어가는지 확인
        if (len + (size_t)n + 1 > cap) {
            // 아직 버퍼가 없거나(cap이 0) 최초 할당이면 4096 바이트로 시작
            // 이미 버퍼가 있다면 현재 용량의 2배로 늘려 재할당
            size_t ncap = (cap == 0) ? 4096 : cap * 2;
            // 2배로 한 번 늘리는 것만으로 부족할 수 있으므로
            // 필요한 크기(기존 len + 새 데이터 n + 널 종료 1)를 만족할 때까지 2배씩 반복 확장
            while (ncap < len + (size_t)n + 1)
                ncap *= 2;
            char *tmp = realloc(acc, ncap); // 재할당
            if (!tmp) {                     // 재할당 실패 (메모리 부족 등으로)
                free(acc);
                return -1;
            }
            acc = tmp;  // 성공 시 새 버퍼 주소를 반영
            cap = ncap; // 현재 버퍼 용량을 갱신
        }

        memcpy(acc + len, chunk, (size_t)n); // 조각을 누적 끝에 붙임
        len += (size_t)n;
        acc[len] = '\0'; // C 문자열 종료 보장

        if (n > 0 && chunk[n - 1] == '\n') // 이번 읽기가 줄의 끝(\n)까지면 종료
            break;
    }

    *out = acc; // 호출자에게 버퍼 소유권 전달
    *len_out = len;
    return 0; // 성공
}