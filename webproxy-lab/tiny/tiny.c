/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);                // 한 연결(confd)를 처리하는 핵심 함수(요청 파싱->정적/동적 처리)
void read_requesthdrs(rio_t *rp); // 요청 헤더들을 RIO로 줄 단위 읽기
int parse_uri(char *uri, char *filename,
              char *cgiargs); // URI 해석 : 정적?동적? + 파일명/CGI 인자 분리(반환은 정적=1, 동적=0)
void serve_static(int fd, char *filename, int filesize);            // 정적 파일 전송 : 헤더 작성 + 파일 바디 송신
void get_filetype(char *filename, char *filetype);                  // 확장자로 MIME 타입 추정
void serve_dynamic(int fd, char *filename, char *cgiargs);          // 동적 컨텐츠 처리 : fork/execve + dup2로 CGI 실행
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, // 에러 응답 생성(상태줄/헤더/간단 HTML 바디)
                 char *longmsg);

int main(int argc, char **argv) {          // 서버 진입점 : ./tiny <port>
    int listenfd, connfd;                  // 리스닝 소켓, 연결 전용 소켓
    char hostname[MAXLINE], port[MAXLINE]; // 접속 클라이언트의 호스트/포트 문자열 출력 버퍼
    socklen_t clientlen;                   // accept에 넘길 주소 길이(입력=버퍼 길이, 출력=실제 길이)
    struct sockaddr_storage clientaddr;    // IPv4, IPv6 모두 수용 가능한 넉넉한 주소 버퍼

    /* Check command line args */
    if (argc != 2) {                                    // 명령어 인자 점검 : 포트 1개만 요구
        fprintf(stderr, "usage: %s <port>\n", argv[0]); // 사용법 안내
        exit(1);                                        // 잘못된 사용이라 비정상 종료 코드로 종료
    }

    listenfd = Open_listenfd(argv[1]);  // 리스닝 소켓 생성 : getaddrinfo->socket->SO_REUSEADDR->bind->listen
    while (1) {                         // 반복형 서버 : 한 번에 한 연결만 처리(동시성 없음)
        clientlen = sizeof(clientaddr); // 커널에 주소 버퍼 크기 알려주기
        connfd = Accept(listenfd, (SA *)&clientaddr,
                        &clientlen); // line:netp:tiny:accept // 완료 큐에서 연결 하나 수락 -> 새 FD(connfd) 획득
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                    0); // 이진 주소 -> 사람이 읽는 문자열(IP, 포트)로 변환
        printf("Accepted connection from (%s, %s)\n", hostname, port); // 접속 로그 출력
        doit(connfd);  // line:netp:tiny:doit 핵심 처리 : 요청줄/헤더 읽기 -> URI 해석 -> 정적/동적 응답
        Close(connfd); // line:netp:tiny:close 소켓 닫기(HTTP/1.0 : Connection : close 의미
    }
}

// 한 HTTP 트랜잭션(한 연결의 한 요청)을 처리
void doit(int fd) {
    int is_static;    // 정적 컨텐츠인지(1) 동적 컨텐츠인지(0) 표시
    struct stat sbuf; // stat 결과(파일 타입/권한/크기)를 담을 구조체
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE],
        version[MAXLINE]; // 요청줄 파싱용 버퍼들 buf는 한 줄 전체 임시 저장, 나머지 3개는 sscanf로 각각 뽑아 저장
    char filename[MAXLINE], cgiargs[MAXLINE]; // 정적: 파일 경로 / 동적: CGI 인자 저장할 배열(? 뒷부분)
    rio_t rio;                                // RIO 버퍼(라인 단위 안전 읽기용)

    /* Read request line and headers */ // 요청줄과 헤더들을 읽는다
    Rio_readinitb(&rio, fd);            // connfd에 대해 RIO 내부 버퍼 초기화
    Rio_readlineb(&rio, buf, MAXLINE);  // 첫 줄(요청줄) 한 줄 읽기: "GET /path HTTP/1.0\r\n" 개행(CRLF)까지 읽음
    printf("Request headers:\n");       // 디버그: 헤더 출력 시작 안내
    printf("%s", buf);                  // 요청줄 자체를 콘솔에 출력
    sscanf(buf, "%s %s %s", method, uri, version); // 요청줄에서 메서드/URI/버전 분리
    // ex) method="GET", uri="/cgi-bin/adder?x=3&y=5", version="HTTP/1.0"
    // GET만 허용 아니면(대소문자 무시 비교), 0(false)이면 같음으로 처리하여 에러가 안남
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not implemented",   // 501 에러 응답 전송
                    "Tiny does not implement this method"); // Tiny는 GET만 지원
        return;                                             // 처리 종료(이 연결은 곧 닫힘)
    }
    read_requesthdrs(&rio); // 이어지는 요청 헤더들을 (빈 줄(\r\n)까지) 줄 단위로 읽어서 소비

    /* Parse URI from GET request */               // GET 요청의 URI 해석
    is_static = parse_uri(uri, filename, cgiargs); // 정적/동적 판정 + 파일경로/CGI 인자 채우기
    /**
     * /home.html 같은 uri에는 .을 붙여 ./home.html로 만들고 cgiargs는 ""이 되며 1을 반환
     * /cgi-bin/adder?x=3&y=5 같은 uri에는 .을 붙여 ./cgi-bin/adder?x=3&y=5로 만들고 cgiargs는 "x=3&y=5"가 되며 0을
     * 반환. 경로에 cgi-bin이 있는지 없는지로 판단
     */
    if (stat(filename, &sbuf) < 0) { // 파일(또는 프로그램) 메타데이터 조회: 존재 여부/크기 등을 sbuf에 채움
        clienterror(fd, filename, "404", "Not found", // 못 찾으면 404
                    "Tiny couldn't find this file");
        return;
    }

    if (is_static) { /* Serve static content */
        // sbuf.st_mode: stat()가 채워주는 파일 모드 비트모스크(파일 종류 + 권한).
        // S_ISREG(sbuf.st_mode): 파일 종류 비트에서 “정규 파일(regular file)”인지 판별하는 매크로. 정규 파일이면 참.
        // S_IRUSR: “소유자(user) 읽기 권한” 비트 마스크.
        // S_IRUSR & sbuf.st_mode: 소유자 읽기 권한 비트가 켜져 있으면 0이 아닌 값(참), 꺼져 있으면 0(거짓). // 정적
        // 컨텐츠 제공 경로
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) { // 일반 파일인가? + 읽기 권한이 있는가?
            clienterror(fd, filename, "403", "Forbidden",            // 아니면 403 (읽을 수 없음)
                        "Tiny couldn't read the file");
            return;
        }
        // serve_static 동작 과정
        // 1.	get_filetype(filename, filetype)로 MIME 추정
        // 2.	Rio_writen으로 응답 헤더 전송
        //  - HTTP/1.0 200 OK
        //  - Server: Tiny Web Server
        //  - Connection: close
        //  - Content-length: <크기>
        //  - Content-type: <MIME>
        //  - \r\n(빈 줄)
        // 3.	파일을 open→mmap해서 바디를 정확히 <크기> 바이트 전송 (메모리에서 빠르게 읽기)
        // 4.	munmap (메모리 해제)
        //  - 결과: 브라우저는 home.html 내용을 받음.
        serve_static(fd, filename, sbuf.st_size); // OK: 응답 헤더 작성 후 파일 바디(sb.st_size 바이트) 전송
    } else { /* Serve dynamic content */          // 동적 컨텐츠(CGI) 제공 경로
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) { // 일반 파일인가? + 실행 권한이 있는가?
            clienterror(fd, filename, "403", "Forbidden",            // 아니면 403 (실행 불가)
                        "Tiny couldn't run the CGI program");
            return;
        }
        // serve_dynamic 동작 과정
        // 1.	상태줄/간단 서버 헤더를 먼저 보냄
        //  - HTTP/1.0 200 OK\r\n
        //  - Server: Tiny Web Server\r\n
        // 2.	fork() -> 자식에서
        //  - setenv("QUERY_STRING", cgiargs, 1)
        //  - dup2(fd, STDOUT_FILENO) 로 표준출력->소켓
        //  - execve(filename, argv, environ) 로 프로그램 실행
        // 3.	프로그램(예: adder)은 자신의 헤더(Content-Type, Content-length)와 바디를 printf로 출력
        // 4 - 부모는 waitpid로 자식을 회수
        //  - 결과: 브라우저는 동적 HTML(예: “3 + 5 = 8”)을 받음.
        serve_dynamic(fd, filename, cgiargs); // OK: fork/execve + dup2로 CGI 실행, 그 출력이 응답 바디가 됨
    }
    // 요청-응답 한 사이클 요약
    // 1. 정적
    // readline(요청줄) -> 헤더 소비 -> parse_uri -> stat
    // -> 권한검사(읽기) -> [헤더 작성] -> [파일 바디 N바이트 전송] -> 끝
    // 2. 동적
    // readline(요청줄) -> 헤더 소비 -> parse_uri -> stat
    // -> 권한검사(실행) -> [상태줄/Server 헤더] -> fork
    // child: setenv + dup2 + execve(CGI) -> CGI가 헤더+바디 출력
    // parent: waitpid
    // -> 끝

    // 반복형 서버이므로 하나를 처리하는 동안 다른 연결은 대기하게 됨
}

/**
 * 에러 응답을 만드는 함수
 * fd : 클라이언트와 연결된 소켓 FD
 * cause : 에러의 원인(ex : 요청한 파일 이름)
 * errnum : 상태 코드(ex : 404)
 * shortmsg : 짧은 이유 (ex : "Not Found")
 * longmsg : 설명 문장(ex : "Tiny couldn't find this file"
 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    // buf : 헤더 한 줄을 만들 때 쓰는 임시 버퍼
    // body : HTML 본문을 누적해서 만드는 버퍼
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    // body를 HTML 시작과 <title>로 초기화
    sprintf(body, "<html><title>Tiny Error</title>");
    // 기존 body 뒤에 <body>태그를 이어붙이고 배경을 흰색으로 지정 문자열 안의 "를 표현하기 위해 \"를 두 번 사용
    sprintf(body, "%s<body bgcolor=\"\"ffffff\"\">\r\n", body);
    // ex) 404: Not found 한 줄 추가 - 상태 코드와 짧은 사유를 보여줌
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    // ex) Tiny couldn't find this file: ./nope.html\r\n과 같은 자세한 설명을 <p>단락으로 추가
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    // 수평선과 서버 제목을 추가
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    // sprintf: 지정한 형식대로 문자열을 만들어 buf 메모리에 채워 넣기. 끝에 NUL 추가.
    // Rio_writen(fd, buf, n): buf의 앞에서부터 정확히 n바이트가 fd(소켓/파일)에 모두 쓰일 때까지 반복해서 write를 호출

    /* Print the HTTP response */
    // 상태줄 만들기. ex) HTTP/1.0 404  Not found\r\n
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    // 응답 바디가 html임을 알림.
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    // 바디의 바이트 길이를 정확히 기록하고 빈 줄로 헤더 종료를 알림
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    // 아까 만든 HTML 바디를 그대로 전송
    Rio_writen(fd, body, strlen(body));

    // doit 내부에서는 이 함수를 이런 식으로 호출함
    // clienterror(fd,
    //         "./nope.html",        // cause
    //         "404",                // errnum
    //         "Not found",          // shortmsg
    //         "Tiny couldn't find this file"); // longmsg
}

// 요청 헤더들을 줄 단위로 읽어서 버리는(소비하는) 함수
// rp는 이미 Rio_readinitb(&rio, fd)로 초기화된 RIO 상태
void read_requesthdrs(rio_t *rp) {
    // 한 줄을 담을 버퍼. 최대 MAXLINE-1 글자(널 종료를 포함해야하므로)를 담을 수 있음
    char buf[MAXLINE];
    // 첫 번째 헤더 줄을 \n까지 읽어 buf에 저장
    Rio_readlineb(rp, buf, MAXLINE);
    // buf가 빈 줄이 아니면 계속 반복. 빈 줄은 헤더의 끝이기 떄문
    while (strcmp(buf, "\r\n")) {
        // 다음 줄을 다시 읽어 buf에 저장
        Rio_readlineb(rp, buf, MAXLINE);
        // 방금 읽은 줄을 서버 콘솔에 그대로 찍음 - 이 구현에서 첫 번째 줄은 찍히지 않음
        printf("%s", buf);
    }
    return;
}

/**
 * HTTP URI 문자열을 받아서
 * 1. 정적 컨텐츠인지/동적 컨텐츠인지 판별하고(반환값 : 정적=1, 동적=0)
 * 2. 정적이면 filename에 디스크 파일 경로를, 동적이면 cgiargs에 쿼리 문자열을 채워 돌려줌
 * 인자로 들어온 uri 문자열을 직접 수정할 수 있다는 점(동적 분기엣 *ptr='\0')이 중요함
 */
int parse_uri(char *uri, char *filename, char *cgiargs) {
    // URI 안에서 특정 문자를 가리킬 작업용 포인터(동적 분기에서 ? 위치)
    char *ptr;

    if (!strstr(uri, "cgi-bin")) { // 정적 컨텐츠 분기
        // 정적 컨텐츠는 쿼리 인자 사용 없음 -> 빈 문자열
        strcpy(cgiargs, "");
        // 문서 루트를 .(현재 디렉터리)로 설정해 filename을 "."로 촉화
        strcpy(filename, ".");
        // 파일 경로를 . 뒤에 이어붙임
        // ex) .을 ./home.html로 변경
        strcat(filename, uri);
        // 요청이 디렉터리로 끝나면(마지막 문자가 /)
        if (uri[strlen(uri) - 1] == '/')
            // 기본 파일로 home.html을 이어붙임
            strcat(filename, "home.html");
        return 1; // 정적 의미를 반환(1)
    } else {      // 동적 컨텐츠 분기
        // ?의 위치를 찾아 포인터로 연결
        ptr = index(uri, '?');
        if (ptr) { // ?를 찾았다면 = 쿼리 문자열이 있다면
            // ? 다음부터 끝까지를 쿼리 문자열로 cgiargs에 복사
            // ex) "/cgi-bin/adder?x=1&y=2" -> cgiargs="x=1&y=2"
            strcpy(cgiargs, ptr + 1);
            // 원래 uri 문자열의 ?자리에 종료를 찍어 경로 부분만 남기도록 직접 자름
            // uri = "/cgi-bin/adder?x=1&y=2" -> "/cgi-bin/adder"
            // 인자로 받는 uri 원본이 직접 변한다는 것이 중요함
            *ptr = '\0';
        } else // 쿼리 문자열이 없으면
            // cgiargs는 그냥 빈 문자열로 빔
            strcpy(cgiargs, "");
        // 파일 경로의 설정. 정적과 마찬가지로 루트는 .임
        strcpy(filename, ".");
        strcat(filename, uri);
        return 0; // 동적을 반환(0)
    }
}

// 정적 파일을 클라이언트에 보내는 함수
/**
 * fd : 클라이언트와 연결된 소켓 fd
 * filename : 보낼 디스크 파일 경로(ex : ./home.html)
 * filesize : 바디로 보낼 정확한 바이트 수
 */
void serve_static(int fd, char *filename, int filesize) {
    // 디스크의 정적 파일을 열었을 때 얻는 파일 디스크립터
    // Open()의 결과를 담음.
    int srcfd;
    // *srcp : 메모리 매핑된 파일의 시작 주소. mmap으로 얻고 munmap으로 해제함
    // filetype : 응답 헤더에 넣을 MIME 타입 문자열 버퍼 ex)text/html
    // buf : HTTP 응답 헤더를 담아두는 문자열 버퍼. 여기 있는 것들을 Rio_write로 전송
    char *srcp, filetype[MAXLINE], buf[MAXBUF];

    /* Send response headers to client */
    // 확장자로 MIME 타입 결정
    get_filetype(filename, filetype);
    // 상태 줄부터 buf에 써넣음
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    // 기존 버퍼 뒤에 계속 이어붙임.
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    sprintf(buf, "%sConnection: close\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
    // 다 쓰고 늘 그랬듯 안전하게 전송.
    Rio_writen(fd, buf, strlen(buf));
    // 서버 콘솔(표준 출력)에 디버그용으로 방금 만든 헤더를 출력
    printf("Response headers:\n");
    printf("%s", buf);

    /* Send response body to client */ // 클라에게 파일 바디 전송
    // 정적 파일을 클라이언트에게 보내는 전형적인 Tiny 웹서버 패턴임
    // 읽기 전용으로 파일을 열기
    srcfd = Open(filename, O_RDONLY, 0);
    // 파일 내용을 메모리에 매핑
    // 인자 설명
    // addr=0(NULL): 커널이 적당한 매핑 주소를 선택.
    // length=filesize: 파일 크기만큼 매핑. 일반적으로 stat으로 얻은 크기를 사용.
    // prot=PROT_READ: 읽기 전용 접근(쓰기 금지).
    // flags=MAP_PRIVATE: 사본-쓰기(copy-on-write) 매핑. 여기서는 읽기만 하므로 사실상 원본은 변하지 않음.
    //  - 읽을 때는 파일 내용을 그대로 공유해 보지만, 쓰려고 하면 그 순간 “내 것만의 복사본”이 생기고 파일은 안 바뀜
    // 반대 개념 - MAP_SHARED: 쓰면 파일에도 반영(공유). 다른 프로세스와 내용이 공유됨.
    // fd=srcfd, offset=0: 파일의 처음부터 매핑.
    // 결과: srcp는 “파일 내용이 들어있는 연속 메모리”의 시작 주소를 가리킴.
    // 이 포인터를 write에 그대로 넘겨 전송 가능.
    srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    // 매핑 후 파일 디스크립터는 닫기. mmap으로 파일 내용을 매핑했기에 fd는 더이상 필요하지 않음
    Close(srcfd);
    // 매핑된 파일 내용을 소켓으로 정확히 filesize 바이트를 전송
    // fd는 클라이언트 소켓. srcp는 매핑 메모리, filesize는 전송해야 할 바이트 수
    Rio_writen(fd, srcp, filesize);
    // 메모리 매핑 해제. 전송 완료 후 사용한 자원을 정리하는 것
    Munmap(srcp, filesize);
    /**
     * 왜 mmap을 쓰는가?
     * 파일 내용을 파일을 메모리처럼 다룰 수 있음. 다수의 read 호출 대신 한 번의 mmap으로
     * 포인터만으로 파일을 전송할 수 있는 느낌
     * read->write로 한 줄씩 일일히 복사하는 방식보다 단순하고 빠름. 하지만 큰 파일이라면
     * 메모리 압박이 증가하기 때문에 비교적 작은 파일에서 이 방식을 사용하는 것이 좋음
     */
}

// 파일 이름의 확장자를 보고 MIME 타입을 정해주는 함수
void get_filetype(char *filename, char *filetype) {
    // strstr : 첫 번째 인자(filename) 안에 두 번째 인자(.html)이 포함되어있으면
    // 그 시작 위치 포인터를 반환 참으로 간주됨. 못 찾으면 NULL 반환
    // strcpy(dest, src): src 문자열을 널 종료문자 ‘\0’까지 포함해 dest로 복사.dest를 통째로 덮어씀.
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    else if (strstr(filename, ".mp4")) // mp4
        strcpy(filetype, "video/mp4");
    else if (strstr(filename, ".mpg") || strstr(filename, ".mpeg")) // mpg
        strcpy(filetype, "video/mpeg");
    else // 아무 것도 해당되지 않으면 그냥 text/plaian
        strcpy(filetype, "text/plain");
}

/**
 * 동적 컨텐츠(CGI)를 제공하는 함수
 * fd : 클라이언트 소켓 fd
 * filename : 실행할 CGI 프로그램 경로 ex) ./cgi-bin/adder
 * cgiargs : ? 뒤의 쿼리 문자열 ex) "x=4&y=5"
 */
void serve_dynamic(int fd, char *filename, char *cgiargs) {
    // buf : 응답 헤더 줄을 임시로 저장하는 버퍼
    // emptylist : execve에 넘길 인자 배열
    char buf[MAXLINE], *emptylist[] = {NULL};

    /* Return first part of HTTP response */
    // HTTP 상태줄 작성 후 소켓으로 전송
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    // 서버 식별 헤더 작성 후 전송
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) { // fork로 자식 프로세스 생성. 조건이 참이면 자식 경로(0) 반환
        // 이 조건문 안에서 실행되는 모든 코드는 전부 방금 fork한 자식 프로세스에서 실행됨
        // 환경변수 QUERY_STRING에 쿼리 문자열을 기록, 1은 기존 값이 있어도 덮어씀
        // CGI 프로그램은 getenv("QUERY_STRING")로 읽음
        setenv("QUERY_STRING", cgiargs, 1);
        // 표준 출력 fd(두 번째 인자. 1번 fd)를 소켓 fd(첫 번째 인자)로 교체함
        // 이제 CGI 프로그램이 printf, write로 찍는 모든 바이트가 네트워크로 클라에게 전달됨
        Dup2(fd, STDOUT_FILENO); /* Redirect stdout to client */
        // 현재 프로세스(자식)을 실행.
        // filename과 빈 배열, 환경 변수(쿼리스트링)를 전달.
        Execve(filename, emptylist, environ); /* Run CGI program */
    }
    // 부모 프로세스임가 자식이 끝날 때까지 대기하다가 자식이 끝나면 종료 상태를 회수함.
    // 좀비 프로세스(프로세스가 끝났는데 회수되지 않은 자식)를 방지하기 위함
    Wait(NULL); /* Parent waits for and reaps child */
}
